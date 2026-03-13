/**
 * GameBoy Recompiled - ImGui Menu System
 *
 * xemu-inspired settings overlay + debug menu.
 * Toggle with ESC (settings) or F2 (debug).
 */

#ifndef GBRT_MENU_GUI_H
#define GBRT_MENU_GUI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct GBContext GBContext;

/* Initialize menu (call after ImGui context is created) */
void menu_gui_init(void);

/* Draw all active menu windows. Call between ImGui::NewFrame() and ImGui::Render() */
void menu_gui_draw(GBContext* ctx);

/* Toggle visibility */
void menu_gui_toggle_settings(void);
void menu_gui_toggle_debug(void);

/* Returns 1 if any menu is currently visible (should block game input) */
int menu_gui_is_active(void);

/* Accessors for settings that platform_sdl needs */
int   menu_gui_get_scale(void);
void  menu_gui_set_scale(int scale);
int   menu_gui_get_speed_percent(void);
int   menu_gui_get_palette_idx(void);
int   menu_gui_get_vsync(void);
float menu_gui_get_master_volume(void);
int   menu_gui_get_show_fps(void);

/* Signal quit requested */
int   menu_gui_quit_requested(void);

#ifdef __cplusplus
}
#endif

#endif /* GBRT_MENU_GUI_H */
