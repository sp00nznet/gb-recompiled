/**
 * GameBoy Recompiled - ImGui Menu System
 *
 * xemu-inspired settings overlay + debug menu.
 * Adapted from the burnout3 recompilation menu system.
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
    bool show_settings;
    bool show_debug;
    int  settings_tab;  /* 0=File, 1=Graphics, 2=Sound, 3=Controller, 4=About */
    bool quit_requested;

    /* Graphics settings */
    int   scale;              /* 1-8 */
    bool  vsync;
    int   filter_mode;       /* 0=Nearest, 1=Linear */
    int   palette_idx;       /* 0=Original, 1=B&W, 2=Amber */

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
    false, false, 0, false,
    /* Graphics */
    3,     /* 3x scale */
    true,  /* vsync */
    0,     /* nearest */
    0,     /* original palette */
    /* Sound */
    1.0f,  /* master volume */
    false, /* mute */
    true, true, true, true, /* all channels */
    /* Speed */
    100,   /* 100% */
    true,  /* show fps */
    /* Debug cheats */
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

    colors[ImGuiCol_ScrollbarBg]        = ImVec4(0.10f, 0.10f, 0.10f, 0.50f);
    colors[ImGuiCol_ScrollbarGrab]      = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);

    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding       = 4.0f;
    style.WindowPadding     = ImVec2(12, 12);
    style.FramePadding      = ImVec2(8, 4);
    style.ItemSpacing       = ImVec2(8, 6);
    style.WindowBorderSize  = 1.0f;
}

/* ================================================================
 * Settings tabs
 * ================================================================ */

static const char *settings_tabs[] = {
    "File", "Graphics", "Sound", "Controller", "About"
};
static const int num_tabs = 5;

static void draw_settings_sidebar(void)
{
    ImGui::BeginChild("Sidebar", ImVec2(140, 0), true);

    for (int i = 0; i < num_tabs; i++) {
        bool selected = (g_menu.settings_tab == i);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.65f, 0.25f, 1.0f));
        }
        if (ImGui::Button(settings_tabs[i], ImVec2(-1, 36))) {
            g_menu.settings_tab = i;
        }
        if (selected) {
            ImGui::PopStyleColor(2);
        }
    }

    ImGui::EndChild();
}

static void draw_tab_file(GBContext* ctx)
{
    ImGui::Text("Link's Awakening DX - Recompiled");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Save Game", ImVec2(200, 0))) {
        if (ctx) {
            gb_context_save_ram(ctx);
            fprintf(stderr, "[MENU] Game saved\n");
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(saves SRAM to disk)");

    ImGui::Spacing();

    ImGui::SliderInt("Speed %", &g_menu.speed_percent, 10, 500);
    if (ImGui::Button("Reset Speed")) g_menu.speed_percent = 100;

    ImGui::Spacing();
    ImGui::Checkbox("Show FPS Counter", &g_menu.show_fps);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Quit", ImVec2(200, 0))) {
        g_menu.quit_requested = true;
    }
}

static void draw_tab_graphics(void)
{
    ImGui::Text("Display");
    ImGui::Separator();
    ImGui::Spacing();

    const char* scale_names[] = {
        "1x (160x144)", "2x (320x288)", "3x (480x432)", "4x (640x576)",
        "5x (800x720)", "6x (960x864)", "7x (1120x1008)", "8x (1280x1152)"
    };
    int scale_idx = g_menu.scale - 1;
    if (scale_idx < 0) scale_idx = 0;
    if (scale_idx > 7) scale_idx = 7;
    if (ImGui::Combo("Window Scale", &scale_idx, scale_names, 8)) {
        g_menu.scale = scale_idx + 1;
    }

    ImGui::Checkbox("V-Sync", &g_menu.vsync);

    ImGui::Spacing();
    ImGui::Text("Filtering");
    ImGui::Separator();
    ImGui::Spacing();

    const char* filter_names[] = { "Nearest (sharp pixels)", "Linear (smooth)" };
    ImGui::Combo("Texture Filter", &g_menu.filter_mode, filter_names, 2);

    ImGui::Spacing();
    ImGui::Text("Palette");
    ImGui::Separator();
    ImGui::Spacing();

    const char* palette_names[] = { "Original (CGB Color)", "Classic Green (DMG)", "Black & White (Pocket)", "Amber (Plasma)" };
    ImGui::Combo("Color Palette", &g_menu.palette_idx, palette_names, 4);
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Palette only affects DMG-mode colors.\nCGB palettes always use true color.");
}

static void draw_tab_sound(void)
{
    ImGui::Text("Volume");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::SliderFloat("Master Volume", &g_menu.master_volume, 0.0f, 1.0f, "%.0f%%");
    ImGui::Checkbox("Mute", &g_menu.mute);

    ImGui::Spacing();
    ImGui::Text("Channels");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Checkbox("CH1 - Pulse A", &g_menu.ch1_enabled);
    ImGui::Checkbox("CH2 - Pulse B", &g_menu.ch2_enabled);
    ImGui::Checkbox("CH3 - Wave", &g_menu.ch3_enabled);
    ImGui::Checkbox("CH4 - Noise", &g_menu.ch4_enabled);
}

static void draw_tab_controller(void)
{
    ImGui::Text("Keyboard Controls");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Columns(2, "keybinds", false);
    ImGui::SetColumnWidth(0, 120);

    ImGui::Text("D-pad");    ImGui::NextColumn(); ImGui::Text("Arrow Keys / WASD"); ImGui::NextColumn();
    ImGui::Text("A Button");  ImGui::NextColumn(); ImGui::Text("Z / J");             ImGui::NextColumn();
    ImGui::Text("B Button");  ImGui::NextColumn(); ImGui::Text("X / K");             ImGui::NextColumn();
    ImGui::Text("Start");     ImGui::NextColumn(); ImGui::Text("Enter");             ImGui::NextColumn();
    ImGui::Text("Select");    ImGui::NextColumn(); ImGui::Text("Right Shift / Backspace"); ImGui::NextColumn();
    ImGui::Separator();
    ImGui::Text("Menu");      ImGui::NextColumn(); ImGui::Text("ESC");               ImGui::NextColumn();
    ImGui::Text("Debug");     ImGui::NextColumn(); ImGui::Text("F2");                ImGui::NextColumn();

    ImGui::Columns(1);

    ImGui::Spacing();
    ImGui::Text("Gamepad Controls");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Columns(2, "gamepad", false);
    ImGui::SetColumnWidth(0, 120);

    ImGui::Text("D-pad");    ImGui::NextColumn(); ImGui::Text("D-pad / Left Stick"); ImGui::NextColumn();
    ImGui::Text("A Button"); ImGui::NextColumn(); ImGui::Text("A (Xbox) / Cross");   ImGui::NextColumn();
    ImGui::Text("B Button"); ImGui::NextColumn(); ImGui::Text("B (Xbox) / Circle");  ImGui::NextColumn();
    ImGui::Text("Start");    ImGui::NextColumn(); ImGui::Text("Start / Options");     ImGui::NextColumn();
    ImGui::Text("Select");   ImGui::NextColumn(); ImGui::Text("Back / Share");        ImGui::NextColumn();

    ImGui::Columns(1);
}

static void draw_tab_about(void)
{
    ImGui::Text("Link's Awakening DX");
    ImGui::Text("Static Recompilation for Windows");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Build Information");
    ImGui::BulletText("Recompiler: gb-recompiled");
    ImGui::BulletText("Functions: 17,805 recompiled");
    ImGui::BulletText("Code: 4.2M lines of generated C");
    ImGui::BulletText("ROM: 1MB, 64 banks, MBC5");

    ImGui::Spacing();
    ImGui::Text("Original Game");
    ImGui::BulletText("Developer: Nintendo");
    ImGui::BulletText("Platform: Game Boy Color (1998)");
    ImGui::BulletText("CPU: Sharp SM83 @ 4/8 MHz");

    ImGui::Spacing();
    ImGui::Text("Credits");
    ImGui::BulletText("gb-recompiled by arcanite24");
    ImGui::BulletText("LADX-Disassembly contributors");
    ImGui::BulletText("SameBoy by LIJI32");
}

static void draw_settings_menu(GBContext* ctx)
{
    ImGuiIO& io = ImGui::GetIO();
    float menu_w = 580.0f;
    float menu_h = 420.0f;
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - menu_w) * 0.5f,
                                    (io.DisplaySize.y - menu_h) * 0.5f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(menu_w, menu_h), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoCollapse;

    if (ImGui::Begin("Settings", &g_menu.show_settings, flags)) {
        draw_settings_sidebar();
        ImGui::SameLine();
        ImGui::BeginChild("Content", ImVec2(0, 0), true);
        switch (g_menu.settings_tab) {
        case 0: draw_tab_file(ctx);       break;
        case 1: draw_tab_graphics();      break;
        case 2: draw_tab_sound();         break;
        case 3: draw_tab_controller();    break;
        case 4: draw_tab_about();         break;
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

/* ================================================================
 * Debug menu
 * ================================================================ */

/* Link's Awakening DX memory map (key addresses) */
#define LADX_GAMEPLAY_TYPE    0xDB95  /* Current gameplay state */
#define LADX_GAMEPLAY_SUBTYPE 0xDB96
#define LADX_LINK_POS_X       0xD100  /* Link X position (high byte) */
#define LADX_LINK_POS_Y       0xD101  /* Link Y position (high byte) */
#define LADX_LINK_HEALTH      0xDB5A  /* Current health (hearts * 8) */
#define LADX_LINK_MAX_HEALTH  0xDB5B  /* Max health */
#define LADX_RUPEES_HIGH      0xDB5D  /* Rupees high byte */
#define LADX_RUPEES_LOW       0xDB5E  /* Rupees low byte */
#define LADX_BOMBS            0xDB4D  /* Bomb count */
#define LADX_ARROWS           0xDB45  /* Arrow count */
#define LADX_POWDER           0xDB4C  /* Magic powder count */
#define LADX_MAP_ROOM         0xD401  /* Current room index */
#define LADX_IS_INDOOR        0xD402  /* 0=overworld, 1=indoor */
#define LADX_DUNGEON_IDX      0xDB83  /* Current dungeon number */
#define LADX_KEYS             0xDB86  /* Keys in current dungeon */

static uint8_t read_wram(GBContext* ctx, uint16_t addr) {
    if (!ctx || !ctx->wram) return 0;
    if (addr >= 0xC000 && addr < 0xE000) {
        return ctx->wram[addr - 0xC000];
    }
    return 0;
}

static void write_wram(GBContext* ctx, uint16_t addr, uint8_t value) {
    if (!ctx || !ctx->wram) return;
    if (addr >= 0xC000 && addr < 0xE000) {
        ctx->wram[addr - 0xC000] = value;
    }
}

static void draw_debug_menu(GBContext* ctx)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 360.0f, 10.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350.0f, 550.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Debug", &g_menu.show_debug)) {

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

                /* Health bar */
                float health_frac = max_health > 0 ? (float)health / max_health : 0;
                ImGui::ProgressBar(health_frac, ImVec2(-1, 0), "");

                ImGui::Text("Rupees: %d", rupees);
                ImGui::Text("Bombs: %d  Arrows: %d  Powder: %d", bombs, arrows, powder);
                ImGui::Text("Keys: %d", keys);
            }

            if (ImGui::CollapsingHeader("Cheats")) {
                if (ImGui::Checkbox("Invincible", &g_menu.invincible)) {
                    /* Will be applied each frame */
                }
                if (ImGui::Checkbox("Max Rupees", &g_menu.unlimited_rupees)) {}
                if (ImGui::Checkbox("Max Bombs", &g_menu.unlimited_bombs)) {}
                if (ImGui::Checkbox("Max Arrows", &g_menu.unlimited_arrows)) {}
                if (ImGui::Checkbox("Max Magic Powder", &g_menu.unlimited_powder)) {}

                /* Apply cheats */
                if (g_menu.invincible) {
                    uint8_t max_hp = read_wram(ctx, LADX_LINK_MAX_HEALTH);
                    if (max_hp > 0) write_wram(ctx, LADX_LINK_HEALTH, max_hp);
                }
                if (g_menu.unlimited_rupees) {
                    write_wram(ctx, LADX_RUPEES_HIGH, 0x03);
                    write_wram(ctx, LADX_RUPEES_LOW, 0xE7); /* 999 */
                }
                if (g_menu.unlimited_bombs) {
                    write_wram(ctx, LADX_BOMBS, 99);
                }
                if (g_menu.unlimited_arrows) {
                    write_wram(ctx, LADX_ARROWS, 99);
                }
                if (g_menu.unlimited_powder) {
                    write_wram(ctx, LADX_POWDER, 99);
                }
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
                        ctx->io[0xFF & 0x7F] /* IE is at FFFF but stored differently */,
                        ctx->io[0x0F], ctx->io[0x00]);
                    ImGui::Text("VBK=%02X  SVBK=%02X KEY1=%02X",
                        ctx->io[0x4F], ctx->io[0x70], ctx->io[0x4D]);
                }
            }
        }
    }
    ImGui::End();
}

/* ================================================================
 * Public API
 * ================================================================ */

extern "C" void menu_gui_init(void)
{
    apply_theme();
}

extern "C" void menu_gui_draw(GBContext* ctx)
{
    /* Settings menu */
    if (g_menu.show_settings) {
        draw_settings_menu(ctx);
    }

    /* Debug menu */
    if (g_menu.show_debug) {
        draw_debug_menu(ctx);
    }

    /* FPS overlay when no menu is open */
    if (!g_menu.show_settings && !g_menu.show_debug && g_menu.show_fps) {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.35f);
        ImGuiWindowFlags overlay_flags = ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        if (ImGui::Begin("##Overlay", NULL, overlay_flags)) {
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            ImGui::Text("Press ESC for Menu");
        }
        ImGui::End();
    }
}

extern "C" void menu_gui_toggle_settings(void)
{
    g_menu.show_settings = !g_menu.show_settings;
}

extern "C" void menu_gui_toggle_debug(void)
{
    g_menu.show_debug = !g_menu.show_debug;
}

extern "C" int menu_gui_is_active(void)
{
    return (g_menu.show_settings || g_menu.show_debug) ? 1 : 0;
}

extern "C" int menu_gui_get_scale(void)
{
    return g_menu.scale;
}

extern "C" void menu_gui_set_scale(int scale)
{
    g_menu.scale = scale;
}

extern "C" int menu_gui_get_speed_percent(void)
{
    return g_menu.speed_percent;
}

extern "C" int menu_gui_get_palette_idx(void)
{
    return g_menu.palette_idx;
}

extern "C" int menu_gui_get_vsync(void)
{
    return g_menu.vsync ? 1 : 0;
}

extern "C" float menu_gui_get_master_volume(void)
{
    if (g_menu.mute) return 0.0f;
    return g_menu.master_volume;
}

extern "C" int menu_gui_get_show_fps(void)
{
    return g_menu.show_fps ? 1 : 0;
}

extern "C" int menu_gui_quit_requested(void)
{
    return g_menu.quit_requested ? 1 : 0;
}
