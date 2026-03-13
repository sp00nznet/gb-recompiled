/**
 * GameBoy Recompiled - ImGui Menu System
 *
 * Windows-style menu bar always visible at top of window.
 * File, Config, Graphics, Sound, Controller menus with dropdown items.
 * Debug window (F2) with game state, cheats, memory inspector.
 */

#include "imgui.h"
#include <stdio.h>
#include <string.h>

extern "C" {
#include "menu_gui.h"
#include "gbrt.h"
}

/* ================================================================
 * Menu state
 * ================================================================ */

static struct {
    bool quit_requested;

    /* Window visibility */
    bool show_debug;
    bool show_about;
    bool show_controller;

    /* Graphics settings */
    int   scale;              /* 1-8 */
    bool  vsync;
    int   filter_mode;       /* 0=Nearest, 1=Linear */
    int   palette_idx;       /* 0=Original, 1=Green, 2=B&W, 3=Amber */

    /* Sound settings */
    float master_volume;
    bool  mute;
    bool  ch1_enabled;
    bool  ch2_enabled;
    bool  ch3_enabled;
    bool  ch4_enabled;

    /* Speed */
    int   speed_percent;
    bool  show_fps;

    /* Debug cheats */
    bool  invincible;
    bool  unlimited_rupees;
    bool  unlimited_bombs;
    bool  unlimited_arrows;
    bool  unlimited_powder;

} g_menu = {
    false,
    /* Windows */
    false, false, false,
    /* Graphics */
    3, true, 0, 0,
    /* Sound */
    1.0f, false, true, true, true, true,
    /* Speed */
    100, true,
    /* Cheats */
    false, false, false, false, false,
};

/* ================================================================
 * Color theme (xemu-inspired dark green)
 * ================================================================ */

static void apply_theme(void)
{
    ImGuiStyle &style = ImGui::GetStyle();
    ImVec4 *colors = style.Colors;

    colors[ImGuiCol_WindowBg]           = ImVec4(0.10f, 0.10f, 0.10f, 0.94f);
    colors[ImGuiCol_PopupBg]            = ImVec4(0.08f, 0.08f, 0.08f, 0.96f);
    colors[ImGuiCol_Border]             = ImVec4(0.30f, 0.30f, 0.30f, 0.50f);
    colors[ImGuiCol_Text]               = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]       = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);

    colors[ImGuiCol_Header]             = ImVec4(0.20f, 0.55f, 0.20f, 0.80f);
    colors[ImGuiCol_HeaderHovered]      = ImVec4(0.25f, 0.65f, 0.25f, 0.80f);
    colors[ImGuiCol_HeaderActive]       = ImVec4(0.30f, 0.75f, 0.30f, 1.00f);

    colors[ImGuiCol_Button]             = ImVec4(0.20f, 0.55f, 0.20f, 0.65f);
    colors[ImGuiCol_ButtonHovered]      = ImVec4(0.25f, 0.65f, 0.25f, 0.80f);
    colors[ImGuiCol_ButtonActive]       = ImVec4(0.30f, 0.75f, 0.30f, 1.00f);

    colors[ImGuiCol_FrameBg]            = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]     = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgActive]      = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);

    colors[ImGuiCol_SliderGrab]         = ImVec4(0.30f, 0.70f, 0.30f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]   = ImVec4(0.35f, 0.80f, 0.35f, 1.00f);
    colors[ImGuiCol_CheckMark]          = ImVec4(0.30f, 0.80f, 0.30f, 1.00f);

    colors[ImGuiCol_Tab]                = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_TabHovered]         = ImVec4(0.25f, 0.60f, 0.25f, 0.80f);
    colors[ImGuiCol_TabActive]          = ImVec4(0.20f, 0.55f, 0.20f, 1.00f);
    colors[ImGuiCol_TabSelected]        = ImVec4(0.20f, 0.55f, 0.20f, 1.00f);

    colors[ImGuiCol_Separator]          = ImVec4(0.30f, 0.30f, 0.30f, 0.50f);
    colors[ImGuiCol_TitleBg]            = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive]      = ImVec4(0.15f, 0.40f, 0.15f, 1.00f);

    colors[ImGuiCol_MenuBarBg]          = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);

    colors[ImGuiCol_ScrollbarBg]        = ImVec4(0.10f, 0.10f, 0.10f, 0.50f);
    colors[ImGuiCol_ScrollbarGrab]      = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);

    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding       = 4.0f;
    style.WindowPadding     = ImVec2(8, 8);
    style.FramePadding      = ImVec2(6, 3);
    style.ItemSpacing       = ImVec2(6, 4);
    style.WindowBorderSize  = 1.0f;
}

/* ================================================================
 * WRAM helpers
 * ================================================================ */

/* Link's Awakening DX memory map (key addresses) */
#define LADX_GAMEPLAY_TYPE    0xDB95
#define LADX_GAMEPLAY_SUBTYPE 0xDB96
#define LADX_LINK_HEALTH      0xDB5A
#define LADX_LINK_MAX_HEALTH  0xDB5B
#define LADX_RUPEES_HIGH      0xDB5D
#define LADX_RUPEES_LOW       0xDB5E
#define LADX_BOMBS            0xDB4D
#define LADX_ARROWS           0xDB45
#define LADX_POWDER           0xDB4C
#define LADX_MAP_ROOM         0xD401
#define LADX_IS_INDOOR        0xD402
#define LADX_DUNGEON_IDX      0xDB83
#define LADX_KEYS             0xDB86

static uint8_t read_wram(GBContext* ctx, uint16_t addr) {
    if (!ctx || !ctx->wram) return 0;
    if (addr >= 0xC000 && addr < 0xE000)
        return ctx->wram[addr - 0xC000];
    return 0;
}

static void write_wram(GBContext* ctx, uint16_t addr, uint8_t value) {
    if (!ctx || !ctx->wram) return;
    if (addr >= 0xC000 && addr < 0xE000)
        ctx->wram[addr - 0xC000] = value;
}

/* ================================================================
 * Main menu bar (always visible)
 * ================================================================ */

static void draw_menu_bar(GBContext* ctx)
{
    if (ImGui::BeginMainMenuBar()) {
        /* ---- File ---- */
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Game")) {
                if (ctx) {
                    gb_context_save_ram(ctx);
                    fprintf(stderr, "[MENU] Game saved\n");
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) {
                g_menu.quit_requested = true;
            }
            ImGui::EndMenu();
        }

        /* ---- Config ---- */
        if (ImGui::BeginMenu("Config")) {
            ImGui::MenuItem("Show FPS", NULL, &g_menu.show_fps);
            ImGui::Separator();
            ImGui::SliderInt("Speed %", &g_menu.speed_percent, 10, 500);
            if (ImGui::MenuItem("Reset Speed")) g_menu.speed_percent = 100;
            ImGui::Separator();
            ImGui::MenuItem("Debug Window", "F2", &g_menu.show_debug);
            ImGui::EndMenu();
        }

        /* ---- Graphics ---- */
        if (ImGui::BeginMenu("Graphics")) {
            if (ImGui::BeginMenu("Window Scale")) {
                const char* scale_labels[] = {
                    "1x (160x144)", "2x (320x288)", "3x (480x432)", "4x (640x576)",
                    "5x (800x720)", "6x (960x864)", "7x (1120x1008)", "8x (1280x1152)"
                };
                for (int i = 0; i < 8; i++) {
                    if (ImGui::MenuItem(scale_labels[i], NULL, g_menu.scale == (i + 1))) {
                        g_menu.scale = i + 1;
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::MenuItem("V-Sync", NULL, &g_menu.vsync);
            ImGui::Separator();

            if (ImGui::BeginMenu("Texture Filter")) {
                if (ImGui::MenuItem("Nearest (sharp)", NULL, g_menu.filter_mode == 0))
                    g_menu.filter_mode = 0;
                if (ImGui::MenuItem("Linear (smooth)", NULL, g_menu.filter_mode == 1))
                    g_menu.filter_mode = 1;
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Color Palette")) {
                const char* pal_names[] = { "Original (CGB Color)", "Classic Green (DMG)",
                                            "Black & White (Pocket)", "Amber (Plasma)" };
                for (int i = 0; i < 4; i++) {
                    if (ImGui::MenuItem(pal_names[i], NULL, g_menu.palette_idx == i))
                        g_menu.palette_idx = i;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        /* ---- Sound ---- */
        if (ImGui::BeginMenu("Sound")) {
            ImGui::SliderFloat("Volume", &g_menu.master_volume, 0.0f, 1.0f, "%.0f%%");
            ImGui::MenuItem("Mute", NULL, &g_menu.mute);
            ImGui::Separator();
            ImGui::MenuItem("CH1 - Pulse A", NULL, &g_menu.ch1_enabled);
            ImGui::MenuItem("CH2 - Pulse B", NULL, &g_menu.ch2_enabled);
            ImGui::MenuItem("CH3 - Wave",    NULL, &g_menu.ch3_enabled);
            ImGui::MenuItem("CH4 - Noise",   NULL, &g_menu.ch4_enabled);
            ImGui::EndMenu();
        }

        /* ---- Controller ---- */
        if (ImGui::BeginMenu("Controller")) {
            ImGui::MenuItem("Show Key Bindings", NULL, &g_menu.show_controller);
            ImGui::EndMenu();
        }

        /* ---- Help ---- */
        if (ImGui::BeginMenu("Help")) {
            ImGui::MenuItem("About", NULL, &g_menu.show_about);
            ImGui::EndMenu();
        }

        /* FPS in menu bar */
        if (g_menu.show_fps) {
            float bar_w = ImGui::GetWindowWidth();
            char fps_text[32];
            snprintf(fps_text, sizeof(fps_text), "%.1f FPS", ImGui::GetIO().Framerate);
            float text_w = ImGui::CalcTextSize(fps_text).x;
            ImGui::SameLine(bar_w - text_w - 10);
            ImGui::TextDisabled("%s", fps_text);
        }

        ImGui::EndMainMenuBar();
    }
}

/* ================================================================
 * Debug window
 * ================================================================ */

static void draw_debug_window(GBContext* ctx)
{
    ImGui::SetNextWindowSize(ImVec2(350, 500), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Debug", &g_menu.show_debug)) {
        ImGuiIO& io = ImGui::GetIO();

        if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("%.1f FPS (%.3f ms/frame)",
                io.Framerate, 1000.0f / io.Framerate);

            static float fps_history[120] = {};
            static int fps_idx = 0;
            fps_history[fps_idx] = io.Framerate;
            fps_idx = (fps_idx + 1) % 120;
            ImGui::PlotLines("FPS", fps_history, 120, fps_idx, NULL, 0.0f, 120.0f, ImVec2(0, 60));
        }

        if (ctx) {
            if (ImGui::CollapsingHeader("CPU Registers", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("AF=%04X  BC=%04X  DE=%04X  HL=%04X",
                    ctx->af, ctx->bc, ctx->de, ctx->hl);
                ImGui::Text("SP=%04X  PC=%04X  Bank=%d",
                    ctx->sp, ctx->pc, ctx->rom_bank);
                ImGui::Text("Flags: Z=%d N=%d H=%d C=%d  IME=%d",
                    ctx->f_z, ctx->f_n, ctx->f_h, ctx->f_c, ctx->ime);
                ImGui::Text("Halted=%d  Cycles=%u", ctx->halted, ctx->cycles);
            }

            if (ImGui::CollapsingHeader("Game State", ImGuiTreeNodeFlags_DefaultOpen)) {
                uint8_t gp_type = read_wram(ctx, LADX_GAMEPLAY_TYPE);
                uint8_t gp_sub  = read_wram(ctx, LADX_GAMEPLAY_SUBTYPE);
                uint8_t room    = read_wram(ctx, LADX_MAP_ROOM);
                uint8_t indoor  = read_wram(ctx, LADX_IS_INDOOR);
                uint8_t dungeon = read_wram(ctx, LADX_DUNGEON_IDX);

                const char* state_names[] = {
                    "World", "Inventory", "File Select", "Name Entry",
                    "Intro", "Ending", "Photo", "Color Dungeon Prompt",
                    "Map Transition", "Dungeon", "Side-Scroll", "Game Over"
                };
                const char* state_name = (gp_type < 12) ? state_names[gp_type] : "Unknown";

                ImGui::Text("GameplayType: %02X (%s)", gp_type, state_name);
                ImGui::Text("SubType: %02X", gp_sub);
                ImGui::Text("Room: %02X  %s  Dungeon: %d", room,
                    indoor ? "Indoor" : "Overworld", dungeon);
            }

            if (ImGui::CollapsingHeader("Link Stats")) {
                uint8_t health     = read_wram(ctx, LADX_LINK_HEALTH);
                uint8_t max_health = read_wram(ctx, LADX_LINK_MAX_HEALTH);
                uint8_t rupees_h   = read_wram(ctx, LADX_RUPEES_HIGH);
                uint8_t rupees_l   = read_wram(ctx, LADX_RUPEES_LOW);
                uint8_t bombs      = read_wram(ctx, LADX_BOMBS);
                uint8_t arrows     = read_wram(ctx, LADX_ARROWS);
                uint8_t powder     = read_wram(ctx, LADX_POWDER);
                uint8_t keys       = read_wram(ctx, LADX_KEYS);
                int rupees = (rupees_h << 8) | rupees_l;

                ImGui::Text("Health: %d/%d (%.1f/%.1f hearts)",
                    health, max_health, health / 8.0f, max_health / 8.0f);
                float health_frac = max_health > 0 ? (float)health / max_health : 0;
                ImGui::ProgressBar(health_frac, ImVec2(-1, 0), "");
                ImGui::Text("Rupees: %d", rupees);
                ImGui::Text("Bombs: %d  Arrows: %d  Powder: %d", bombs, arrows, powder);
                ImGui::Text("Keys: %d", keys);
            }

            if (ImGui::CollapsingHeader("Cheats")) {
                ImGui::Checkbox("Invincible", &g_menu.invincible);
                ImGui::Checkbox("Max Rupees", &g_menu.unlimited_rupees);
                ImGui::Checkbox("Max Bombs", &g_menu.unlimited_bombs);
                ImGui::Checkbox("Max Arrows", &g_menu.unlimited_arrows);
                ImGui::Checkbox("Max Magic Powder", &g_menu.unlimited_powder);

                if (g_menu.invincible) {
                    uint8_t max_hp = read_wram(ctx, LADX_LINK_MAX_HEALTH);
                    if (max_hp > 0) write_wram(ctx, LADX_LINK_HEALTH, max_hp);
                }
                if (g_menu.unlimited_rupees) {
                    write_wram(ctx, LADX_RUPEES_HIGH, 0x03);
                    write_wram(ctx, LADX_RUPEES_LOW, 0xE7);
                }
                if (g_menu.unlimited_bombs)  write_wram(ctx, LADX_BOMBS, 99);
                if (g_menu.unlimited_arrows) write_wram(ctx, LADX_ARROWS, 99);
                if (g_menu.unlimited_powder) write_wram(ctx, LADX_POWDER, 99);
            }

            if (ImGui::CollapsingHeader("Memory Inspector")) {
                static int inspect_addr = 0xC000;
                ImGui::InputInt("Address", &inspect_addr, 16, 256, ImGuiInputTextFlags_CharsHexadecimal);

                if (inspect_addr >= 0xC000 && inspect_addr < 0xDFF0) {
                    uint16_t addr = (uint16_t)inspect_addr;
                    ImGui::Text("%04X: %02X %02X %02X %02X  %02X %02X %02X %02X",
                        addr,
                        read_wram(ctx, addr), read_wram(ctx, addr+1),
                        read_wram(ctx, addr+2), read_wram(ctx, addr+3),
                        read_wram(ctx, addr+4), read_wram(ctx, addr+5),
                        read_wram(ctx, addr+6), read_wram(ctx, addr+7));
                    ImGui::Text("%04X: %02X %02X %02X %02X  %02X %02X %02X %02X",
                        addr+8,
                        read_wram(ctx, addr+8), read_wram(ctx, addr+9),
                        read_wram(ctx, addr+10), read_wram(ctx, addr+11),
                        read_wram(ctx, addr+12), read_wram(ctx, addr+13),
                        read_wram(ctx, addr+14), read_wram(ctx, addr+15));
                } else if (inspect_addr >= 0xFF00 && inspect_addr < 0xFF80 && ctx->io) {
                    uint16_t off = inspect_addr - 0xFF00;
                    ImGui::Text("%04X: %02X %02X %02X %02X  %02X %02X %02X %02X",
                        inspect_addr,
                        ctx->io[off], ctx->io[off+1], ctx->io[off+2], ctx->io[off+3],
                        ctx->io[off+4], ctx->io[off+5], ctx->io[off+6], ctx->io[off+7]);
                } else {
                    ImGui::TextColored(ImVec4(1,0.5f,0.5f,1), "Address out of WRAM/IO range");
                }
            }

            if (ImGui::CollapsingHeader("IO Registers")) {
                if (ctx->io) {
                    ImGui::Text("LCDC=%02X STAT=%02X SCY=%02X SCX=%02X LY=%02X",
                        ctx->io[0x40], ctx->io[0x41], ctx->io[0x42], ctx->io[0x43], ctx->io[0x44]);
                    ImGui::Text("BGP=%02X  OBP0=%02X OBP1=%02X WY=%02X  WX=%02X",
                        ctx->io[0x47], ctx->io[0x48], ctx->io[0x49], ctx->io[0x4A], ctx->io[0x4B]);
                    ImGui::Text("IE=%02X   IF=%02X   JOYP=%02X",
                        ctx->io[0x7F], ctx->io[0x0F], ctx->io[0x00]);
                    ImGui::Text("VBK=%02X  SVBK=%02X KEY1=%02X",
                        ctx->io[0x4F], ctx->io[0x70], ctx->io[0x4D]);
                }
            }
        }
    }
    ImGui::End();
}

/* ================================================================
 * Controller window
 * ================================================================ */

static void draw_controller_window(void)
{
    ImGui::SetNextWindowSize(ImVec2(340, 300), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Key Bindings", &g_menu.show_controller)) {
        ImGui::Text("Keyboard");
        ImGui::Separator();
        ImGui::Columns(2, "kb", false);
        ImGui::SetColumnWidth(0, 100);
        ImGui::Text("D-pad");   ImGui::NextColumn(); ImGui::Text("Arrows / WASD"); ImGui::NextColumn();
        ImGui::Text("A");       ImGui::NextColumn(); ImGui::Text("Z / J");         ImGui::NextColumn();
        ImGui::Text("B");       ImGui::NextColumn(); ImGui::Text("X / K");         ImGui::NextColumn();
        ImGui::Text("Start");   ImGui::NextColumn(); ImGui::Text("Enter");         ImGui::NextColumn();
        ImGui::Text("Select");  ImGui::NextColumn(); ImGui::Text("RShift / Backspace"); ImGui::NextColumn();
        ImGui::Columns(1);

        ImGui::Spacing();
        ImGui::Text("Gamepad");
        ImGui::Separator();
        ImGui::Columns(2, "gp", false);
        ImGui::SetColumnWidth(0, 100);
        ImGui::Text("D-pad");   ImGui::NextColumn(); ImGui::Text("D-pad / L-Stick"); ImGui::NextColumn();
        ImGui::Text("A");       ImGui::NextColumn(); ImGui::Text("A / Cross");        ImGui::NextColumn();
        ImGui::Text("B");       ImGui::NextColumn(); ImGui::Text("B / Circle");       ImGui::NextColumn();
        ImGui::Text("Start");   ImGui::NextColumn(); ImGui::Text("Start / Options");  ImGui::NextColumn();
        ImGui::Text("Select");  ImGui::NextColumn(); ImGui::Text("Back / Share");     ImGui::NextColumn();
        ImGui::Columns(1);
    }
    ImGui::End();
}

/* ================================================================
 * About window
 * ================================================================ */

static void draw_about_window(void)
{
    ImGui::SetNextWindowSize(ImVec2(340, 280), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About", &g_menu.show_about)) {
        ImGui::Text("Link's Awakening DX");
        ImGui::Text("Static Recompilation for Windows");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::BulletText("Recompiler: gb-recompiled");
        ImGui::BulletText("Functions: 17,805 recompiled");
        ImGui::BulletText("Code: 4.2M lines of generated C");
        ImGui::BulletText("ROM: 1MB, 64 banks, MBC5");
        ImGui::Spacing();
        ImGui::BulletText("Developer: Nintendo (1998)");
        ImGui::BulletText("CPU: Sharp SM83 @ 4/8 MHz");
        ImGui::Spacing();
        ImGui::Text("Credits");
        ImGui::BulletText("gb-recompiled by arcanite24");
        ImGui::BulletText("LADX-Disassembly contributors");
        ImGui::BulletText("SameBoy by LIJI32");
    }
    ImGui::End();
}

/* ================================================================
 * Public API
 * ================================================================ */

extern "C" void menu_gui_init(void)
{
    apply_theme();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL; /* Don't save layout */
}

extern "C" void menu_gui_draw(GBContext* ctx)
{
    /* Always-visible menu bar */
    draw_menu_bar(ctx);

    /* Floating windows opened from menu */
    if (g_menu.show_debug)      draw_debug_window(ctx);
    if (g_menu.show_about)      draw_about_window();
    if (g_menu.show_controller) draw_controller_window();
}

extern "C" void menu_gui_toggle_settings(void)
{
    /* ESC no longer toggles a settings popup — it's always the menu bar.
       Could use ESC to close any open floating windows. */
    if (g_menu.show_debug || g_menu.show_about || g_menu.show_controller) {
        g_menu.show_debug = false;
        g_menu.show_about = false;
        g_menu.show_controller = false;
    }
}

extern "C" void menu_gui_toggle_debug(void)
{
    g_menu.show_debug = !g_menu.show_debug;
}

extern "C" int menu_gui_is_active(void)
{
    /* Menu bar is always active but doesn't block input.
       Only block when a floating window has focus. */
    return (g_menu.show_debug || g_menu.show_about || g_menu.show_controller) ? 1 : 0;
}

extern "C" int menu_gui_get_scale(void)       { return g_menu.scale; }
extern "C" void menu_gui_set_scale(int scale)  { g_menu.scale = scale; }
extern "C" int menu_gui_get_speed_percent(void) { return g_menu.speed_percent; }
extern "C" int menu_gui_get_palette_idx(void)  { return g_menu.palette_idx; }
extern "C" int menu_gui_get_vsync(void)        { return g_menu.vsync ? 1 : 0; }
extern "C" int menu_gui_get_show_fps(void)     { return g_menu.show_fps ? 1 : 0; }
extern "C" int menu_gui_quit_requested(void)   { return g_menu.quit_requested ? 1 : 0; }

extern "C" float menu_gui_get_master_volume(void)
{
    if (g_menu.mute) return 0.0f;
    return g_menu.master_volume;
}
