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
#include "state.h"

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
    /* gngeo's save_state embeds a screenshot + full state. It uses its own
     * path scheme (ROOTPATH/save/<game>.sta). We call it directly; the slot
     * maps to retro-go's saveSlot. */
    if (conf.game && save_state(conf.game, app->saveSlot))
        return true;
    return false;
}

static bool load_state_handler(const char *filename)
{
    if (conf.game && how_many_slot(conf.game) > 0)
    {
        if (load_state(conf.game, app->saveSlot))
            return true;
    }
    return false;
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

    if (app->bootFlags & RG_BOOT_RESUME)
        rg_emu_load_state(app->saveSlot);

    rg_system_set_tick_rate(60);

    neogeo_main_loop();

    screen_close();
    rg_system_shutdown();
}
