/**
 * @file platform_sdl.h
 * @brief SDL2 platform layer for GameBoy runtime
 */

#ifndef GB_PLATFORM_SDL_H
#define GB_PLATFORM_SDL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Joypad state variables
extern uint8_t g_joypad_buttons;
extern uint8_t g_joypad_dpad;

typedef struct GBContext GBContext;

/**
 * @brief Initialize SDL2 platform (window, renderer)
 * @param scale Window scale factor (1-4)
 * @return true on success
 */
bool gb_platform_init(int scale);

/**
 * @brief Register context with platform (sets up callbacks)
 */
void gb_platform_register_context(GBContext* ctx);

/**
 * @brief Shutdown SDL2 platform
 */
void gb_platform_shutdown(void);

/**
 * @brief Process SDL events
 * @return false if quit requested
 */
bool gb_platform_poll_events(GBContext* ctx);

/**
 * @brief Render frame to screen
 */
void gb_platform_render_frame(const uint32_t* framebuffer);

/**
 * @brief Get joypad state
 * @return Joypad byte (active low)
 */
uint8_t gb_platform_get_joypad(void);

/**
 * @brief Wait for vsync / frame timing
 */
void gb_platform_vsync(void);

/**
 * @brief Set window title
 */
void gb_platform_set_title(const char* title);

/**
 * @brief Override the directory where battery RAM (.sav), RTC sidecar
 *        (.rtc), bindings (bindings.cfg) and save-states live.
 *
 * When unset (the default), files are written next to the executable
 * via SDL_GetBasePath(). Useful when running multiple rom.exe instances
 * on the same machine (e.g. for link-cable multiplayer testing) so they
 * don't clobber each other's saves. The directory is created if it does
 * not exist.
 *
 * Pass NULL or "" to clear the override and fall back to the default.
 */
void gb_platform_set_save_dir(const char* path);

/**
 * @brief Save full program state to disk
 */
void gb_platform_save_state(GBContext* ctx);

/**
 * @brief Load full program state from disk
 */
void gb_platform_load_state(GBContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* GB_PLATFORM_SDL_H */
