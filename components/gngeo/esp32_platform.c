/*
 * esp32_platform.c — Retro-Go platform adapter for GnGeo
 *
 * Replaces the original RetroESP32-P4 odroid_xxx/st7701_lcd/app_common glue with
 * retro-go's rg_xxx framework.  All GnGeo core functions (screen_init, screen_update,
 * handle_event, init_sdl_audio, etc.) are preserved with the same signatures so
 * emu.c / video.c / memory.c compile unmodified.
 *
 * Removed (handled by retro-go natively):
 *   - Custom loading screen font + progress bar  -> rg_gui / ESP_LOGI
 *   - Sidebar button overlays (MENU/VOL)         -> not needed (hardware buttons)
 *   - Volume overlay (neo_show_volume)           -> retro-go options menu
 *   - In-game menu (neo_show_menu)               -> rg_gui_game_menu()
 *   - Async video task (core 1 LCD push)         -> rg_display handles DMA
 *   - Audio submit task                          -> rg_audio_submit handles DMA
 *
 * Preserved:
 *   - SDL_Surface shim (screen/buffer/sprbuf -> PSRAM pixel buffers)
 *   - Z80 sound task on core 1 (YM2610 synthesis)
 *   - Triple-buffered play_buffer rotation
 *   - All conf/message/debug stubs
 *   - neogeo controller register mapping (update_p1_key etc.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "SDL.h"
#include "emu.h"
#include "screen.h"
#include "conf.h"
#include "sound.h"
#include "event.h"
#include "menu.h"
#include "messages.h"
#include "video.h"
#include "memory.h"
#include "state.h"
#include "roms.h"
#include "debug.h"
#include "frame_skip.h"

#include <rg_system.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

/* stb_zlib for uncompress() implementation */
#include "stb_zlib.h"

static const char *TAG = "gngeo_rg";

/* ──────────────────────────────────────────────────────
 * Screen globals (declared in screen.h)
 * ────────────────────────────────────────────────────── */
static SDL_PixelFormat screen_fmt = { .BitsPerPixel = 16, .BytesPerPixel = 2,
    .Rmask = 0xF800, .Gmask = 0x07E0, .Bmask = 0x001F, .Amask = 0 };
static SDL_PixelFormat buf_fmt = { .BitsPerPixel = 16, .BytesPerPixel = 2,
    .Rmask = 0xF800, .Gmask = 0x07E0, .Bmask = 0x001F, .Amask = 0 };

static SDL_Surface screen_surface;
static SDL_Surface buffer_surface;
static SDL_Surface sprbuf_surface;

SDL_Surface *screen = NULL;
SDL_Surface *buffer = NULL;
SDL_Surface *sprbuf = NULL;
SDL_Surface *fps_buf = NULL;
SDL_Surface *scan = NULL;
SDL_Surface *fontbuf = NULL;
SDL_Rect visible_area;

int yscreenpadding = 0;
Uint8 interpolation = 0;
Uint8 nblitter = 0;
Uint8 neffect = 0;
Uint8 scale = 1;
Uint8 fullscreen = 0;

/* Video globals referenced by emu.c / video.c */
int pending_save_state = 0;
int pending_load_state = 0;
int show_fps = 0;
int autoframeskip = 0;
int sleep_idle = 0;
int show_keysym = 0;
char input_buf[256];

/* emu.h globals */
Uint8 key[SDLK_LAST];
Uint8 *joy_button[2];
Sint32 *joy_axe[2];
Uint32 joy_numaxes[2];

/* state_img for save state screenshot capture */
static SDL_Surface state_img_surface;
static SDL_Surface state_img_tmp_surface;
SDL_Surface *state_img = &state_img_surface;
Uint8 state_version = 3;
char fps_str[32];
struct gngeo_conf conf;

/* ──────────────────────────────────────────────────────
 * Retro-Go display surfaces (double-buffered)
 * ────────────────────────────────────────────────────── */
#define NEO_SCREEN_W 320
#define NEO_SCREEN_H 224
#define NEO_VIS_W 304
#define NEO_VIS_H 224
#define NEO_FB_W NEO_VIS_W
#define NEO_FB_H NEO_VIS_H

static uint16_t *screen_pixels;
static uint16_t *buffer_pixels;
static uint16_t *sprbuf_pixels;

static rg_surface_t display_surfaces[2];
static uint16_t *display_pixels[2];
static int display_write_idx = 0;

rg_surface_t *neogeo_get_display_surface(void)
{
    /* Return the last submitted surface (for screenshots/redraw) */
    return &display_surfaces[display_write_idx ^ 1];
}

/* ──────────────────────────────────────────────────────
 * zlib uncompress via stb_zlib
 * ────────────────────────────────────────────────────── */
int uncompress(uint8_t *dest, unsigned long *destLen,
               const uint8_t *source, unsigned long sourceLen) {
    zbuf z;
    memset(&z, 0, sizeof(z));
    z.zbuffer = (uint8 *)source;
    z.zbuffer_end = (uint8 *)source + sourceLen;
    int result = stbi_zlib_decode_noheader_stream(&z, (char *)dest, (int)*destLen);
    if (result < 0) return -1;
    *destLen = result;
    return 0;
}

/* ──────────────────────────────────────────────────────
 * Screen functions
 * ────────────────────────────────────────────────────── */
int screen_init(void) {
    RG_LOGI("screen_init: %dx%d RGB565 (visible %dx%d)",
            NEO_SCREEN_W + 32, NEO_SCREEN_H + 32, NEO_VIS_W, NEO_VIS_H);

    /* Allocate GnGeo internal buffers in PSRAM */
    screen_pixels = heap_caps_calloc(1, (NEO_SCREEN_W + 32) * (NEO_SCREEN_H + 32) * 2, MALLOC_CAP_SPIRAM);
    buffer_pixels = heap_caps_calloc(1, (NEO_SCREEN_W + 32) * (NEO_SCREEN_H + 32) * 2, MALLOC_CAP_SPIRAM);
    sprbuf_pixels = heap_caps_calloc(1, (NEO_SCREEN_W + 32) * (NEO_SCREEN_H + 32) * 2, MALLOC_CAP_SPIRAM);
    if (!screen_pixels || !buffer_pixels || !sprbuf_pixels) {
        RG_LOGE("Failed to allocate screen buffers");
        return -1;
    }

    /* SDL_Surface wrappers (gngeo core reads screen/buffer/sprbuf) */
    memset(&screen_surface, 0, sizeof(screen_surface));
    screen_surface.format = &screen_fmt;
    screen_surface.w = NEO_SCREEN_W + 32;
    screen_surface.h = NEO_SCREEN_H + 32;
    screen_surface.pitch = (NEO_SCREEN_W + 32) * 2;
    screen_surface.pixels = screen_pixels;
    screen = &screen_surface;

    memset(&buffer_surface, 0, sizeof(buffer_surface));
    buffer_surface.format = &buf_fmt;
    buffer_surface.w = NEO_SCREEN_W + 32;
    buffer_surface.h = NEO_SCREEN_H + 32;
    buffer_surface.pitch = (NEO_SCREEN_W + 32) * 2;
    buffer_surface.pixels = buffer_pixels;
    buffer = &buffer_surface;

    memset(&sprbuf_surface, 0, sizeof(sprbuf_surface));
    sprbuf_surface.format = &buf_fmt;
    sprbuf_surface.w = NEO_SCREEN_W + 32;
    sprbuf_surface.h = NEO_SCREEN_H + 32;
    sprbuf_surface.pitch = (NEO_SCREEN_W + 32) * 2;
    sprbuf_surface.pixels = sprbuf_pixels;
    sprbuf = &sprbuf_surface;

    /* Visible area — standard Neo Geo viewport */
    visible_area.x = 16;
    visible_area.y = 16;
    visible_area.w = NEO_VIS_W;
    visible_area.h = NEO_VIS_H;

    fps_buf = NULL;
    scan = NULL;
    fontbuf = NULL;

    /* Retro-Go double-buffered display surfaces */
    for (int i = 0; i < 2; i++) {
        display_pixels[i] = heap_caps_calloc(1, NEO_FB_W * NEO_FB_H * 2, MALLOC_CAP_SPIRAM);
        if (!display_pixels[i]) {
            RG_LOGE("Failed to allocate display surface %d", i);
            return -1;
        }
        memset(&display_surfaces[i], 0, sizeof(rg_surface_t));
        display_surfaces[i].width = NEO_FB_W;
        display_surfaces[i].height = NEO_FB_H;
        display_surfaces[i].stride = NEO_FB_W * 2;
        display_surfaces[i].offset = 0;
        display_surfaces[i].format = RG_PIXEL_565_LE;
        display_surfaces[i].data = display_pixels[i];
    }
    display_write_idx = 0;

    /* Allocate state_img / state_img_tmp for save states */
    state_img_surface.w = NEO_VIS_W;
    state_img_surface.h = NEO_VIS_H;
    state_img_surface.pitch = NEO_VIS_W * 2;
    state_img_surface.pixels = heap_caps_calloc(1, NEO_VIS_W * NEO_VIS_H * 2, MALLOC_CAP_SPIRAM);
    state_img_surface.format = &buf_fmt;

    state_img_tmp_surface.w = NEO_VIS_W;
    state_img_tmp_surface.h = NEO_VIS_H;
    state_img_tmp_surface.pitch = NEO_VIS_W * 2;
    state_img_tmp_surface.pixels = heap_caps_calloc(1, NEO_VIS_W * NEO_VIS_H * 2, MALLOC_CAP_SPIRAM);
    state_img_tmp_surface.format = &buf_fmt;

    extern SDL_Surface *state_img_tmp;
    state_img_tmp = &state_img_tmp_surface;

    RG_LOGI("Screen init done. Free PSRAM: %u KB",
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return 0;
}

int screen_reinit(void) {
    if (!screen_pixels) return screen_init();
    return 0;
}

int screen_resize(int w, int h) { (void)w; (void)h; return 0; }

void screen_update(void) {
    if (!buffer_pixels) return;

    const int src_stride = NEO_SCREEN_W + 32;
    uint16_t *dst = display_pixels[display_write_idx];

    for (int y = 0; y < NEO_VIS_H; y++) {
        memcpy(&dst[y * NEO_VIS_W],
               &buffer_pixels[(visible_area.y + y) * src_stride + visible_area.x],
               NEO_VIS_W * sizeof(uint16_t));
    }

    rg_display_submit(&display_surfaces[display_write_idx], 0);
    display_write_idx ^= 1;
}

void screen_close(void) {
    if (screen_pixels) { heap_caps_free(screen_pixels); screen_pixels = NULL; }
    if (buffer_pixels) { heap_caps_free(buffer_pixels); buffer_pixels = NULL; }
    if (sprbuf_pixels) { heap_caps_free(sprbuf_pixels); sprbuf_pixels = NULL; }
    for (int i = 0; i < 2; i++) {
        if (display_pixels[i]) { heap_caps_free(display_pixels[i]); display_pixels[i] = NULL; }
    }
    if (state_img_surface.pixels) { heap_caps_free(state_img_surface.pixels); state_img_surface.pixels = NULL; }
    if (state_img_tmp_surface.pixels) { heap_caps_free(state_img_tmp_surface.pixels); state_img_tmp_surface.pixels = NULL; }
}

void screen_fullscreen(void) {}
void init_sdl(void) { screen_init(); }

/* ──────────────────────────────────────────────────────
 * Sound — Z80 + YM2610 on core 1, audio via rg_audio_submit
 * ────────────────────────────────────────────────────── */
#include "timer.h"

static SDL_AudioSpec desired_spec, obtain_spec;
SDL_AudioSpec *desired = &desired_spec;
SDL_AudioSpec *obtain = &obtain_spec;

/* Triple-buffered play_buffer: YM2610 renders into one, rg_audio plays another */
Uint16 *play_buffer = NULL;
static Uint16 *play_buf_a = NULL;
static Uint16 *play_buf_b = NULL;
static Uint16 *play_buf_c = NULL;
static int play_buf_idx = 0;

static int audio_samples_per_frame = 367;

/* Z80 task on core 1 */
typedef enum {
    Z80_CMD_NMI,
    Z80_CMD_FRAME,
    Z80_CMD_STOP,
} z80_cmd_t;

static QueueHandle_t z80_cmd_queue = NULL;
static SemaphoreHandle_t z80_frame_done_sem = NULL;
static volatile bool z80_task_running = false;

static int z80_nb_interlace = 16;
static Uint32 z80_timeslice_interlace = 0;

static void z80_sound_task(void *arg) {
    (void)arg;
    extern void cpu_z80_run(int nbcycle);
    extern void cpu_z80_nmi(void);
    extern void YM2610Update_stream(int length);

    z80_cmd_t cmd;

    while (z80_task_running) {
        if (xQueueReceive(z80_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (cmd) {
            case Z80_CMD_NMI:
                cpu_z80_nmi();
                cpu_z80_run(300);
                break;

            case Z80_CMD_FRAME: {
                /* Drain pending NMI commands first */
                z80_cmd_t pending;
                while (xQueueReceive(z80_cmd_queue, &pending, 0) == pdTRUE) {
                    if (pending == Z80_CMD_NMI) {
                        cpu_z80_nmi();
                        cpu_z80_run(300);
                    } else if (pending == Z80_CMD_STOP) {
                        z80_task_running = false;
                        goto z80_exit;
                    }
                }

                /* Run Z80 for one frame */
                for (int i = 0; i < z80_nb_interlace; i++) {
                    cpu_z80_run(z80_timeslice_interlace);
                    my_timer();
                }

                /* Synthesize audio */
                YM2610Update_stream(audio_samples_per_frame);

                /* Submit to retro-go audio (handles DMA/buffering internally) */
                rg_audio_submit((rg_audio_frame_t *)play_buffer, audio_samples_per_frame);

                /* Rotate triple buffer */
                play_buf_idx = (play_buf_idx + 1) % 3;
                static Uint16 *bufs[3];
                bufs[0] = play_buf_a; bufs[1] = play_buf_b; bufs[2] = play_buf_c;
                play_buffer = bufs[play_buf_idx];

                xSemaphoreGive(z80_frame_done_sem);
                break;
            }

            case Z80_CMD_STOP:
                z80_task_running = false;
                break;
            }
        }
    }
z80_exit:
    xSemaphoreGive(z80_frame_done_sem);
    vTaskDelete(NULL);
}

int init_sdl_audio(void) {
    RG_LOGI("init_sdl_audio: sample_rate=%d", conf.sample_rate);

    audio_samples_per_frame = conf.sample_rate / 60;

    /* Allocate triple play_buffers in PSRAM */
    if (!play_buf_a) {
        play_buf_a = heap_caps_calloc(16384, sizeof(Uint16), MALLOC_CAP_SPIRAM);
        play_buf_b = heap_caps_calloc(16384, sizeof(Uint16), MALLOC_CAP_SPIRAM);
        play_buf_c = heap_caps_calloc(16384, sizeof(Uint16), MALLOC_CAP_SPIRAM);
        if (!play_buf_a || !play_buf_b || !play_buf_c) {
            RG_LOGE("Failed to allocate play_buffers");
            return -1;
        }
        play_buffer = play_buf_a;
        play_buf_idx = 0;
    }

    desired->freq = conf.sample_rate;
    desired->format = AUDIO_S16;
    desired->channels = 2;
    desired->samples = audio_samples_per_frame;
    desired->size = audio_samples_per_frame * 2 * sizeof(int16_t);
    desired->callback = NULL;
    desired->userdata = NULL;
    memcpy(obtain, desired, sizeof(SDL_AudioSpec));

    /* rg_audio_init was already called by rg_system_init().
     * Update sample rate if different. */
    if (rg_audio_get_sample_rate() != conf.sample_rate) {
        rg_audio_set_sample_rate(conf.sample_rate);
    }

    /* Compute Z80 timeslice */
    int z80_overclk = CF_VAL(cf_get_item_by_name("z80clock"));
    Uint32 z80_ts = (z80_overclk == 0 ? 73333 : 73333 + (z80_overclk * 73333 / 100));
    z80_timeslice_interlace = z80_ts / (float)z80_nb_interlace;

    /* Create Z80 command queue + completion semaphore */
    z80_cmd_queue = xQueueCreate(16, sizeof(z80_cmd_t));
    z80_frame_done_sem = xSemaphoreCreateBinary();

    /* Create Z80 sound task on core 1 */
    z80_task_running = true;
    static StaticTask_t z80_tcb;
    static StackType_t z80_stack[4096];
    xTaskCreateStaticPinnedToCore(z80_sound_task, "neo_z80", 4096,
                                  NULL, 7, z80_stack, &z80_tcb, 1);

    RG_LOGI("Audio ready: %d Hz, %d samples/frame, Z80 on core 1",
            conf.sample_rate, audio_samples_per_frame);
    return 0;
}

void close_sdl_audio(void) {
    if (z80_task_running && z80_cmd_queue) {
        z80_cmd_t cmd = Z80_CMD_STOP;
        xQueueSend(z80_cmd_queue, &cmd, pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    z80_task_running = false;
}

void pause_audio(int on) {
    rg_audio_set_mute(on);
}

/* Called from 68K store handler: queue an NMI for the Z80 task */
void esp32_z80_queue_nmi(void) {
    if (z80_cmd_queue) {
        z80_cmd_t cmd = Z80_CMD_NMI;
        xQueueSend(z80_cmd_queue, &cmd, 0);
    }
}

/* Called from emu loop: kick off Z80 frame on core 1 */
void esp32_z80_start_frame(void) {
    if (z80_cmd_queue) {
        z80_cmd_t cmd = Z80_CMD_FRAME;
        xQueueSend(z80_cmd_queue, &cmd, portMAX_DELAY);
    }
}

/* Called from emu loop: wait for Z80 frame to complete */
void esp32_z80_wait_frame(void) {
    if (z80_frame_done_sem) {
        xSemaphoreTake(z80_frame_done_sem, pdMS_TO_TICKS(50));
    }
}

/* Legacy stub — no longer called from emu loop */
void esp32_audio_submit_frame(void) {}

/* ──────────────────────────────────────────────────────
 * Event / Input — retro-go gamepad -> gngeo joy_state
 * ────────────────────────────────────────────────────── */
int init_event(void) { return 0; }
void reset_event(void) {}
int wait_event(void) { return 0; }

JOYMAP *jmap = NULL;
Uint8 joy_state[2][GN_MAX_KEY];
static uint32_t gp_prev = 0;

int handle_event(void) {
    uint32_t joypad = rg_input_read_gamepad();

    /* Map retro-go keys to gngeo P1 controls */
    joy_state[0][GN_UP]          = (joypad & RG_KEY_UP)     ? 1 : 0;
    joy_state[0][GN_DOWN]        = (joypad & RG_KEY_DOWN)   ? 1 : 0;
    joy_state[0][GN_LEFT]        = (joypad & RG_KEY_LEFT)   ? 1 : 0;
    joy_state[0][GN_RIGHT]       = (joypad & RG_KEY_RIGHT)  ? 1 : 0;
    /* NeoGeo A/B/C/D -> retro-go A/B/X/Y (4-button layout) */
    joy_state[0][GN_A]           = (joypad & RG_KEY_A)      ? 1 : 0;
    joy_state[0][GN_B]           = (joypad & RG_KEY_B)      ? 1 : 0;
    joy_state[0][GN_C]           = (joypad & RG_KEY_X)      ? 1 : 0;
    joy_state[0][GN_D]           = (joypad & RG_KEY_Y)      ? 1 : 0;
    joy_state[0][GN_START]       = (joypad & RG_KEY_START)  ? 1 : 0;
    joy_state[0][GN_SELECT_COIN] = (joypad & RG_KEY_SELECT) ? 1 : 0;

    /* In-game menu: MENU key -> retro-go's unified game menu */
    if ((joypad & RG_KEY_MENU) && !(gp_prev & RG_KEY_MENU)) {
        rg_gui_game_menu();
    }

    /* Options menu: OPTION key */
    if ((joypad & RG_KEY_OPTION) && !(gp_prev & RG_KEY_OPTION)) {
        rg_gui_options_menu();
    }

    gp_prev = joypad;
    return 0; /* never request quit — rg_gui_game_menu handles exit via rg_system_switch_app */
}

/*
 * Neo Geo controller registers (active-low: 0 = pressed, 1 = released)
 * intern_p1/p2:  bit0=UP, 1=DOWN, 2=LEFT, 3=RIGHT, 4=A, 5=B, 6=C, 7=D
 * intern_start:  bit0=P1 START, bit2=P2 START, bit3=P1 SELECT, upper=0x8F idle
 * intern_coin:   bit0=COIN1, bit1=COIN2, bit2=SERVICE, 0x07 idle
 */
void update_p1_key(void) {
    Uint8 val = 0xFF;
    if (joy_state[0][GN_UP])    val &= ~(1 << 0);
    if (joy_state[0][GN_DOWN])  val &= ~(1 << 1);
    if (joy_state[0][GN_LEFT])  val &= ~(1 << 2);
    if (joy_state[0][GN_RIGHT]) val &= ~(1 << 3);
    if (joy_state[0][GN_A])     val &= ~(1 << 4);
    if (joy_state[0][GN_B])     val &= ~(1 << 5);
    if (joy_state[0][GN_C])     val &= ~(1 << 6);
    if (joy_state[0][GN_D])     val &= ~(1 << 7);
    memory.intern_p1 = val;
}

void update_p2_key(void) {
    memory.intern_p2 = 0xFF;
}

void update_coin(void) {
    int coin_pressed = joy_state[0][GN_SELECT_COIN];
    Uint8 val = 0x3F;
    if (coin_pressed) val &= ~(1 << 0);
    memory.intern_coin = val;
}

void update_start(void) {
    Uint8 val = 0xFF;
    if (joy_state[0][GN_START]) val &= ~(1 << 0);
    memory.intern_start = val;
}

/* ──────────────────────────────────────────────────────
 * Menu / progress stubs
 * ────────────────────────────────────────────────────── */
Uint32 run_menu(void) {
    return 0; /* 0 = resume; menu handled by rg_gui_game_menu in handle_event */
}

int gn_init_skin(void) { return 0; }

static int pbar_total = 1;
static int pbar_pos = 0;
static char pbar_msg[64] = "";

static void loading_screen_refresh(void) {
    int disp_w = rg_display_get_width();
    rg_display_clear(C_BLACK);
    if (pbar_msg[0])
        rg_gui_draw_text(RG_GUI_CENTER, 90, disp_w, pbar_msg, C_CYAN, C_BLACK, RG_TEXT_ALIGN_CENTER);
    int bar_w = disp_w - 80, bar_h = 20;
    int bar_x = (disp_w - bar_w) / 2;
    int bar_y = 140;
    rg_gui_draw_rect(bar_x, bar_y, bar_w, bar_h, 2, C_WHITE, C_DARK_GRAY);
    if (pbar_total > 0 && pbar_pos > 0) {
        int fill = (int)((int64_t)pbar_pos * bar_w / pbar_total);
        if (fill > bar_w) fill = bar_w;
        rg_gui_draw_rect(bar_x, bar_y, fill, bar_h, 0, C_NONE, C_GREEN);
    }
    rg_display_force_redraw();
}

void gn_reset_pbar(void) { pbar_pos = 0; }
void gn_init_pbar(char *name, int size) {
    RG_LOGI("Loading: %s (size=%d)", name ? name : "?", size);
    pbar_total = size > 0 ? size : 1;
    pbar_pos = 0;
    loading_screen_refresh();
}
void gn_update_pbar(int pos) {
    int old_pct = pbar_total > 0 ? (int)((int64_t)pbar_pos * 100 / pbar_total) : 0;
    pbar_pos = pos;
    int new_pct = pbar_total > 0 ? (int)((int64_t)pbar_pos * 100 / pbar_total) : 0;
    if (new_pct != old_pct)
        loading_screen_refresh();
}
void gn_terminate_pbar(void) {
    RG_LOGI("Loading complete");
    pbar_pos = pbar_total;
    loading_screen_refresh();
}
void gn_loading_info(const char *msg) {
    if (msg) {
        RG_LOGI("gngeo: %s", msg);
        strncpy(pbar_msg, msg, sizeof(pbar_msg) - 1);
        pbar_msg[sizeof(pbar_msg) - 1] = '\0';
        loading_screen_refresh();
    }
}
void gn_set_loading_game(const char *name) {
    if (name) RG_LOGI("gngeo: starting %s", name);
}
void gn_popup_error(char *name, char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (name) printf("[ERROR] %s: ", name);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
int gn_popup_question(char *name, char *fmt, ...) {
    (void)name; (void)fmt;
    return 0;
}

/* ──────────────────────────────────────────────────────
 * Conf (configuration) stubs
 * ────────────────────────────────────────────────────── */
static CONF_ITEM cf_rompath   = { .name = "rompath",   .type = CFT_STRING, .data.dt_str = { .str = "/sd/roms/neogeo" } };
static CONF_ITEM cf_biospath  = { .name = "biospath",  .type = CFT_STRING, .data.dt_str = { .str = "/sd/roms/neogeo" } };
static CONF_ITEM cf_blitter   = { .name = "blitter",   .type = CFT_STRING, .data.dt_str = { .str = "soft" } };
static CONF_ITEM cf_effect    = { .name = "effect",    .type = CFT_STRING, .data.dt_str = { .str = "none" } };
static CONF_ITEM cf_transpack = { .name = "transpack", .type = CFT_STRING, .data.dt_str = { .str = "" } };
static CONF_ITEM cf_system    = { .name = "system",    .type = CFT_STRING, .data.dt_str = { .str = "arcade" } };
static CONF_ITEM cf_country   = { .name = "country",   .type = CFT_STRING, .data.dt_str = { .str = "usa" } };
static CONF_ITEM cf_68kclock  = { .name = "68kclock",  .type = CFT_INT,    .data.dt_int = { .val = 0 } };
static CONF_ITEM cf_z80clock  = { .name = "z80clock",  .type = CFT_INT,    .data.dt_int = { .val = 0 } };
static CONF_ITEM cf_debug     = { .name = "debug",     .type = CFT_BOOLEAN,.data.dt_bool = { .boolean = 0 } };
static CONF_ITEM cf_raster    = { .name = "raster",    .type = CFT_BOOLEAN,.data.dt_bool = { .boolean = 0 } };
static CONF_ITEM cf_sound     = { .name = "sound",     .type = CFT_BOOLEAN,.data.dt_bool = { .boolean = 1 } };
static CONF_ITEM cf_interp    = { .name = "interpolation", .type = CFT_INT,.data.dt_int = { .val = 0 } };
static CONF_ITEM cf_scale     = { .name = "scale",     .type = CFT_INT,    .data.dt_int = { .val = 1 } };
static CONF_ITEM cf_fullscreen = { .name = "fullscreen", .type = CFT_BOOLEAN, .data.dt_bool = { .boolean = 0 } };
static CONF_ITEM cf_autoframeskip = { .name = "autoframeskip", .type = CFT_BOOLEAN, .data.dt_bool = { .boolean = 1 } };
static CONF_ITEM cf_showfps   = { .name = "showfps",   .type = CFT_BOOLEAN,.data.dt_bool = { .boolean = 0 } };
static CONF_ITEM cf_sleepidle = { .name = "sleepidle", .type = CFT_BOOLEAN,.data.dt_bool = { .boolean = 0 } };
static CONF_ITEM cf_screen320 = { .name = "screen320", .type = CFT_BOOLEAN,.data.dt_bool = { .boolean = 0 } };
static CONF_ITEM cf_pal       = { .name = "pal",       .type = CFT_BOOLEAN,.data.dt_bool = { .boolean = 0 } };
static CONF_ITEM cf_sample_rate = { .name = "samplerate", .type = CFT_INT, .data.dt_int = { .val = 22050 } };
static CONF_ITEM cf_libglpath = { .name = "libglpath", .type = CFT_STRING, .data.dt_str = { .str = "" } };
static CONF_ITEM cf_datafile  = { .name = "datafile",  .type = CFT_STRING, .data.dt_str = { .str = "/sd/roms/neogeo/gngeo_data.zip" } };
static CONF_ITEM cf_bench     = { .name = "bench",     .type = CFT_BOOLEAN,.data.dt_bool = { .boolean = 0 } };
static CONF_ITEM cf_dump      = { .name = "dump",      .type = CFT_BOOLEAN,.data.dt_bool = { .boolean = 0 } };

static CONF_ITEM *cf_items[] = {
    &cf_rompath, &cf_biospath, &cf_blitter, &cf_effect, &cf_transpack,
    &cf_system, &cf_country, &cf_68kclock, &cf_z80clock, &cf_debug,
    &cf_raster, &cf_sound, &cf_interp, &cf_scale, &cf_fullscreen,
    &cf_autoframeskip, &cf_showfps, &cf_sleepidle, &cf_screen320,
    &cf_pal, &cf_sample_rate, &cf_libglpath, &cf_datafile,
    &cf_bench, &cf_dump,
    NULL
};

CONF_ITEM *cf_get_item_by_name(const char *name) {
    for (int i = 0; cf_items[i]; i++) {
        if (strcmp(cf_items[i]->name, name) == 0)
            return cf_items[i];
    }
    static CONF_ITEM cf_dummy = { .name = "dummy", .type = CFT_INT, .data.dt_int = { .val = 0 } };
    RG_LOGW("cf_get_item_by_name: '%s' not found", name);
    return &cf_dummy;
}

void cf_init(void) {}
void cf_reset_to_default(void) {}
int cf_open_file(char *filename) { (void)filename; return 0; }
int cf_save_file(char *filename, int flags) { (void)filename; (void)flags; return 0; }
int cf_save_option(char *filename, char *optname, int flags) { (void)filename; (void)optname; (void)flags; return 0; }

void cf_create_bool_item(const char *name, const char *help, char so, int def) { (void)name; (void)help; (void)so; (void)def; }
void cf_create_action_item(const char *name, const char *help, char so, int (*action)(struct CONF_ITEM *self)) { (void)name; (void)help; (void)so; (void)action; }
void cf_create_action_arg_item(const char *name, const char *help, const char *ha, char so, int (*action)(struct CONF_ITEM *self)) { (void)name; (void)help; (void)ha; (void)so; (void)action; }
void cf_create_string_item(const char *name, const char *help, const char *ha, char so, const char *def) { (void)name; (void)help; (void)ha; (void)so; (void)def; }
void cf_create_int_item(const char *name, const char *help, const char *ha, char so, int def) { (void)name; (void)help; (void)ha; (void)so; (void)def; }
void cf_create_array_item(const char *name, const char *help, const char *ha, char so, int size, int *def) { (void)name; (void)help; (void)ha; (void)so; (void)size; (void)def; }
void cf_create_str_array_item(const char *name, const char *help, const char *ha, char so, char *def) { (void)name; (void)help; (void)ha; (void)so; (void)def; }
void cf_init_cmd_line(void) {}
int cf_get_non_opt_index(int argc, char *argv[]) { (void)argc; (void)argv; return 0; }

/* ──────────────────────────────────────────────────────
 * Messages stubs
 * ────────────────────────────────────────────────────── */
void draw_message(const char *string) {
    if (string) RG_LOGI("MSG: %s", string);
}
void stop_message(int param) { (void)param; }
void SDL_textout(SDL_Surface *dest, int x, int y, const char *string) {
    (void)dest; (void)x; (void)y; (void)string;
}
void text_input(const char *message, int x, int y, char *string, int size) {
    (void)message; (void)x; (void)y;
    if (string && size > 0) string[0] = '\0';
}
void error_box(char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* ──────────────────────────────────────────────────────
 * Debug stubs
 * ────────────────────────────────────────────────────── */
int dbg_step = 0;
void show_bt(void) {}
void add_bt(Uint32 pc) { (void)pc; }
int check_bp(int pc) { (void)pc; return 0; }
void add_bp(int pc) { (void)pc; }
void del_bp(int pc) { (void)pc; }
int dbg_68k_run(Uint32 nbcycle) { (void)nbcycle; return 0; }

/* ──────────────────────────────────────────────────────
 * RGB2YUV table
 * ────────────────────────────────────────────────────── */
RGB2YUV *rgb2yuv = NULL;
void init_rgb2yuv_table(void) {
    if (!rgb2yuv) {
        rgb2yuv = heap_caps_calloc(65536, sizeof(RGB2YUV), MALLOC_CAP_SPIRAM);
    }
}

/* ──────────────────────────────────────────────────────
 * ESP32-specific helpers
 * ────────────────────────────────────────────────────── */
void esp32_set_rompath(const char *path) {
    strncpy(cf_rompath.data.dt_str.str, path, CF_MAXSTRLEN - 1);
    strncpy(cf_biospath.data.dt_str.str, path, CF_MAXSTRLEN - 1);
}

void esp32_enable_sound(int enable) {
    cf_sound.data.dt_bool.boolean = enable;
    conf.sound = enable;
}

void esp32_init_conf(const char *game_name) {
    memset(&conf, 0, sizeof(conf));
    conf.game = (char *)game_name;
    conf.x_start = 16;
    conf.y_start = 16;
    conf.res_x = NEO_VIS_W;
    conf.res_y = NEO_VIS_H;
    conf.sample_rate = 22050;
    conf.sound = 1;
    conf.vsync = 0;
    conf.debug = 0;
    conf.system = SYS_ARCADE;
    conf.country = CTY_USA;
    conf.autoframeskip = 1;
    conf.show_fps = 0;
    conf.sleep_idle = 0;
    conf.screen320 = 0;
    conf.pal = 0;

    /* Per-game raster mode for games needing mid-frame IRQ2 */
    static const char *raster_games[] = {
        "ridhero", "lbowling", "ssideki", "ssideki2", "ssideki3",
        "ssideki4", "turfmast", "ironclad", NULL
    };
    conf.raster = 0;
    if (game_name) {
        for (const char **g = raster_games; *g; g++) {
            if (strcmp(game_name, *g) == 0) {
                conf.raster = 1;
                RG_LOGI("Raster mode ON for %s", game_name);
                break;
            }
        }
    }
}
