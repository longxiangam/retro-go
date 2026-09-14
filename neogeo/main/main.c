/*
 * neogeo/main/main.c — Retro-Go app entry for the GnGeo NeoGeo emulator.
 *
 * Architecture:
 *   - rg_system_init() sets up the retro-go framework (display/audio/input/gui).
 *   - The GnGeo core (components/gngeo) provides init_game() + neogeo_main_loop().
 *   - The platform adapter (components/gngeo/esp32_platform.c, rewritten for rg_*)
 *     bridges GnGeo's screen_update/handle_event/init_sdl_audio to retro-go's
 *     rg_display_submit / rg_input_read_gamepad / rg_audio_submit.
 *
 * The GnGeo main loop (emu.c:neogeo_main_loop) is kept unmodified. It calls
 * platform functions every frame; the platform adapter routes those to retro-go.
 */
#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu.h"
#include "memory.h"
#include "roms.h"
#include "screen.h"
#include "conf.h"
#include <zlib.h>
#include "state.h"

static void load_backup_ram(void);
static void save_backup_ram(void);

/* Platform helpers implemented in esp32_platform.c (the rg_* adapter) */
extern void esp32_init_conf(const char *game_name);
extern void esp32_set_rompath(const char *path);
extern void esp32_enable_sound(int enable);
extern void gn_loading_info(const char *msg);
extern void gn_set_loading_game(const char *name);

/* Returns the current visible framebuffer as an rg_surface for screenshots/redraw */
extern rg_surface_t *neogeo_get_display_surface(void);

#define AUDIO_SAMPLE_RATE 22050
#define NEO_VISIBLE_W 304
#define NEO_VISIBLE_H 224

static rg_app_t *app;
static char game_name[64];
static char rom_dir[256];

/* ── Retro-Go handlers ── */

static bool screenshot_handler(const char *filename, int width, int height)
{
    rg_surface_t *surf = neogeo_get_display_surface();
    if (surf)
        return rg_surface_save_image_file(surf, filename, width, height);
    return false;
}

static bool save_state_handler(const char *filename)
{
    /* Write the gngeo-format state directly to the framework's filename.
     * (gngeo's own save_state() writes to ROOTPATH/save/<game>.NNN which is
     * not where retro-go expects states.) */
    gzFile gzf = gzopen(filename, "wb");
    if (!gzf)
        return false;

    int flags = state_arch_flags();
    gzwrite(gzf, "GNGST3", 6);
    gzwrite(gzf, &flags, sizeof(int));
    gzwrite(gzf, state_img->pixels, 304 * 224 * 2);
    neogeo_mkstate(gzf, STWRITE);
    gzclose(gzf);
    return true;
}

static bool load_state_handler(const char *filename)
{
    /* Save pointers that gngeo's loader restores by re-assignment */
    Uint8 *ng_lo = memory.ng_lo;
    Uint8 *fix_game_usage = memory.fix_game_usage;
    Uint8 *bksw_unscramble = memory.bksw_unscramble;
    int *bksw_offset = memory.bksw_offset;

    gzFile gzf = gzopen(filename, "rb");
    if (!gzf)
        return false;

    char magic[8] = {0};
    int flags = 0;
    gzread(gzf, magic, 6);
    if (strcmp(magic, "GNGST3") != 0 && strcmp(magic, "GNGST2") != 0)
    {
        gzclose(gzf);
        return false;
    }
    if (strcmp(magic, "GNGST2") == 0)
        state_version = ST_VER2;
    if (strcmp(magic, "GNGST3") == 0)
        state_version = ST_VER3;

    gzread(gzf, &flags, sizeof(int));
    if (flags != state_arch_flags())
    {
        gzclose(gzf);
        return false;
    }

    gzread(gzf, state_img_tmp->pixels, 304 * 224 * 2);
    neogeo_mkstate(gzf, STREAD);
    gzclose(gzf);

    /* Refresh the Z80 banked windows: neogeo_mkstate restores z80_bank[]
     * regs but not the mamez80 window copies, which on a RESUME boot still
     * hold pre-load data.  Without this the sound driver reads the wrong
     * bank and goes silent. */
    {
        extern void cpu_z80_switchbank(unsigned char bank, unsigned short PortNo);
        extern unsigned short z80_bank[4];
        for (int b = 0; b < 4; b++)
            cpu_z80_switchbank((unsigned char)b, z80_bank[b]);
    }

    {
        extern int ym_trace_arm;
        extern int ym_trace_cnt;
        ym_trace_arm = 1;
        ym_trace_cnt = 0;
        printf("YM_TRACE ARMED\n");
    }

    /* Restore the pointers gngeo's loader preserves */
    memory.ng_lo = ng_lo;
    memory.fix_game_usage = fix_game_usage;
    memory.bksw_unscramble = bksw_unscramble;
    memory.bksw_offset = bksw_offset;
    cpu_68k_bankswitch(bankaddress);

    if (memory.current_vector == 0)
        memcpy(memory.rom.cpu_m68k.p, memory.rom.bios_m68k.p, 0x80);
    else
        memcpy(memory.rom.cpu_m68k.p, memory.game_vector, 0x80);

    if (memory.vid.currentpal)
    {
        current_pal = memory.vid.pal_neo[1];
        current_pc_pal = (Uint32 *)memory.vid.pal_host[1];
    }
    else
    {
        current_pal = memory.vid.pal_neo[0];
        current_pc_pal = (Uint32 *)memory.vid.pal_host[0];
    }

    if (memory.vid.currentfix)
    {
        current_fix = memory.rom.game_sfix.p;
        fix_usage = memory.fix_game_usage;
    }
    else
    {
        current_fix = memory.rom.bios_sfix.p;
        fix_usage = memory.fix_board_usage;
    }
    return true;
}

static bool reset_handler(bool hard)
{
    /* gngeo handles reset internally via the BIOS; a soft reset is enough */
    return true;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
    {
        rg_surface_t *surf = neogeo_get_display_surface();
        if (surf)
            rg_display_submit(surf, 0);
    }
}

static rg_gui_event_t raster_callback(rg_gui_option_t *option, rg_gui_event_t event)
{
    /* Optional: expose the per-game raster-mode toggle in the options menu */
    strcpy(option->value, conf.raster ? _("On") : _("Off"));
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        conf.raster = !conf.raster;
        strcpy(option->value, conf.raster ? _("On") : _("Off"));
    }
    return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Raster effects"), "-", RG_DIALOG_FLAG_NORMAL, &raster_callback};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

/* ── Backup RAM (SRAM) persistence ── */

static void load_backup_ram(void)
{
    char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
    size_t len = 0x10000;
    void *buf = NULL;
    if (rg_storage_read_file(path, &buf, &len, 0) && buf)
    {
        if (len > 0x10000) len = 0x10000;
        memcpy(memory.sram, buf, len);
        free(buf);
        RG_LOGI("Backup RAM loaded (%u bytes)", (unsigned)len);
    }
    free(path);
}

static void save_backup_ram(void)
{
    char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
    if (rg_storage_write_file(path, memory.sram, 0x10000, 0))
        RG_LOGI("Backup RAM saved");
    else
        RG_LOGE("Backup RAM save FAILED");
    free(path);
}

/* ── Entry point ── */

void app_main(void)
{
    const rg_handlers_t handlers = {
        .loadState = &load_state_handler,
        .saveState = &save_state_handler,
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
        .event = &event_handler,
        .options = &options_handler,
    };

    app = rg_system_init(AUDIO_SAMPLE_RATE, &handlers, NULL);

    const char *slash = strrchr(app->romPath, '/');
    const char *base = slash ? slash + 1 : app->romPath;
    strncpy(game_name, base, sizeof(game_name) - 1);
    game_name[sizeof(game_name) - 1] = '\0';
    char *dot = strrchr(game_name, '.');
    if (dot)
        *dot = '\0';

    if (slash)
    {
        size_t dirlen = slash - app->romPath;
        if (dirlen >= sizeof(rom_dir))
            dirlen = sizeof(rom_dir) - 1;
        memcpy(rom_dir, app->romPath, dirlen);
        rom_dir[dirlen] = '\0';
    }
    else
    {
        strcpy(rom_dir, RG_BASE_PATH_ROMS "/neogeo");
    }

    RG_LOGI("NeoGeo app: game='%s' rom_dir='%s' romPath='%s'", game_name, rom_dir, app->romPath);

    esp32_init_conf(game_name);
    esp32_set_rompath(rom_dir);
    esp32_enable_sound(1);

    gn_set_loading_game(game_name);
    gn_loading_info("Initializing...");

    if (screen_init() != 0)
        RG_PANIC("screen_init failed");

    gn_loading_info("Loading game...");

    if (init_game(game_name) != 1)
        RG_PANIC("init_game failed");

    RG_LOGI("Game loaded. P=%uKB S=%uKB Tiles=%uKB ADPCM-A=%uKB Z80=%uKB",
            (unsigned)(memory.rom.cpu_m68k.size / 1024),
            (unsigned)(memory.rom.game_sfix.size / 1024),
            (unsigned)(memory.rom.tiles.size / 1024),
            (unsigned)(memory.rom.adpcma.size / 1024),
            (unsigned)(memory.rom.cpu_z80.size / 1024));

    load_backup_ram();

    /* DIAG: defer the resume load until 10s into the run (frame 600) to
     * match path-A timing; neogeo_main_loop triggers it via the flag. */
    if (app->bootFlags & RG_BOOT_RESUME)
    {
        extern void neogeo_deferred_resume(int slot);
        neogeo_deferred_resume(app->saveSlot);
    }

    rg_system_set_tick_rate(60);

    neogeo_main_loop();

    save_backup_ram();

    screen_close();
    rg_system_shutdown();
}
