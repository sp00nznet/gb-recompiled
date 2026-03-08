/**
 * @file platform_sdl.cpp
 * @brief SDL2 platform implementation for GameBoy runtime with ImGui
 */

#include "platform_sdl.h"
#include "gbrt.h"   /* For GBPlatformCallbacks */
#include "ppu.h"
#include "gbrt_debug.h"

#ifdef GB_HAS_SDL2
#include <SDL.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

/* ============================================================================
 * SDL State
 * ========================================================================== */

static SDL_Window* g_window = NULL;
static SDL_Renderer* g_renderer = NULL;
static SDL_Texture* g_texture = NULL;
static int g_scale = 3;
static uint32_t g_last_frame_time = 0;
static SDL_AudioDeviceID g_audio_device = 0;
static bool g_vsync = true;

/* Menu State */
static bool g_show_menu = false;
static int g_speed_percent = 100;
static int g_palette_idx = 0;
static const char* g_palette_names[] = { "Original (Green)", "Black & White (Pocket)", "Amber (Plasma)" };
static const char* g_scale_names[] = { "1x (160x144)", "2x (320x288)", "3x (480x432)", "4x (640x576)", "5x (800x720)", "6x (960x864)", "7x (1120x1008)", "8x (1280x1152)" };
static const uint32_t g_palettes[][4] = {
    { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 }, // Original
    { 0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000 }, // B&W
    { 0xFFFFB000, 0xFFCB4F0E, 0xFF800000, 0xFF330000 }  // Amber
};

/* Joypad state - exported for gbrt.c to access */
uint8_t g_joypad_buttons = 0xFF;  /* Active low: Start, Select, B, A */
uint8_t g_joypad_dpad = 0xFF;     /* Active low: Down, Up, Left, Right */

/* ============================================================================
 * Automation State
 * ========================================================================== */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_SCRIPT_ENTRIES 100
typedef struct {
    uint32_t start_frame;
    uint32_t duration;
    uint8_t dpad;    /* Active LOW mask to apply (0 = Pressed) */
    uint8_t buttons; /* Active LOW mask to apply (0 = Pressed) */
} ScriptEntry;

static ScriptEntry g_input_script[MAX_SCRIPT_ENTRIES];
static int g_script_count = 0;

#define MAX_DUMP_FRAMES 100
static uint32_t g_dump_frames[MAX_DUMP_FRAMES];
static int g_dump_count = 0;
static char g_screenshot_prefix[64] = "screenshot";

/* Helper to parse button string "U,D,L,R,A,B,S,T" */
static void parse_buttons(const char* btn_str, uint8_t* dpad, uint8_t* buttons) {
    *dpad = 0xFF;
    *buttons = 0xFF;
    // Simple parser: check for existence of characters
    if (strchr(btn_str, 'U')) *dpad &= ~0x04;
    if (strchr(btn_str, 'D')) *dpad &= ~0x08;
    if (strchr(btn_str, 'L')) *dpad &= ~0x02;
    if (strchr(btn_str, 'R')) *dpad &= ~0x01;
    if (strchr(btn_str, 'A')) *buttons &= ~0x01;
    if (strchr(btn_str, 'B')) *buttons &= ~0x02;
    if (strchr(btn_str, 'S')) *buttons &= ~0x08; /* Start */
    if (strchr(btn_str, 'T')) *buttons &= ~0x04; /* Select (T for selecT) */
}

void gb_platform_set_input_script(const char* script) {
    // Format: frame:buttons:duration,...
    if (!script) return;
    
    char* copy = strdup(script);
    char* token = strtok(copy, ",");
    g_script_count = 0;
    
    while (token && g_script_count < MAX_SCRIPT_ENTRIES) {
        uint32_t frame = 0, duration = 0;
        char btn_buf[16] = {0};
        
        if (sscanf(token, "%u:%15[^:]:%u", &frame, btn_buf, &duration) == 3) {
            ScriptEntry* e = &g_input_script[g_script_count++];
            e->start_frame = frame;
            e->duration = duration;
            parse_buttons(btn_buf, &e->dpad, &e->buttons);
            printf("[AUTO] Added input: Frame %u, Btns '%s', Dur %u\n", frame, btn_buf, duration);
        }
        token = strtok(NULL, ",");
    }
    free(copy);
}

void gb_platform_set_dump_frames(const char* frames) {
    if (!frames) return;
    char* copy = strdup(frames);
    char* token = strtok(copy, ",");
    g_dump_count = 0;
    while (token && g_dump_count < MAX_DUMP_FRAMES) {
        g_dump_frames[g_dump_count++] = (uint32_t)strtoul(token, NULL, 10);
        token = strtok(NULL, ",");
    }
    free(copy);
}

void gb_platform_set_screenshot_prefix(const char* prefix) {
    if (prefix) snprintf(g_screenshot_prefix, sizeof(g_screenshot_prefix), "%s", prefix);
}

static void save_ppm(const char* filename, const uint32_t* fb, int width, int height, int frame_count) {
    // Calculate simple hash
    uint32_t hash = 0;
    for (int k = 0; k < width * height; k++) {
        hash = (hash * 33) ^ fb[k];
    }
    printf("[AUTO] Frame %d hash: %08X\n", frame_count, hash);

    FILE* f = fopen(filename, "wb");
    if (!f) return;
    
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    
    uint8_t* row = (uint8_t*)malloc(width * 3);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t p = fb[y * width + x];
            row[x*3+0] = (p >> 16) & 0xFF; // R
            row[x*3+1] = (p >> 8) & 0xFF;  // G
            row[x*3+2] = (p >> 0) & 0xFF;  // B
        }
        fwrite(row, 1, width * 3, f);
    }
    
    free(row);
    fclose(f);
    printf("[AUTO] Saved screenshot: %s\n", filename);
}


static int g_frame_count = 0;

/* ============================================================================
 * Platform Functions
 * ========================================================================== */

void gb_platform_shutdown(void) {
    if (g_audio_device) {
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
    }
    
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (g_texture) {
        SDL_DestroyTexture(g_texture);
        g_texture = NULL;
    }
    if (g_renderer) {
        SDL_DestroyRenderer(g_renderer);
        g_renderer = NULL;
    }
    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = NULL;
    }
    SDL_Quit();
}

/* ============================================================================
 * Audio
 * ========================================================================== */

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_BUFFER_SIZE 4096 /* Samples (stereo frames) */


static int16_t g_audio_buffer[AUDIO_BUFFER_SIZE * 2]; /* *2 for stereo */
static int g_audio_write_pos = 0;
static int g_audio_read_pos = 0;

static void sdl_audio_callback(void* userdata, Uint8* stream, int len) {
    (void)userdata;
    int16_t* output = (int16_t*)stream;
    int samples_needed = len / sizeof(int16_t) / 2; /* Stereo frames */
    
    for (int i = 0; i < samples_needed; i++) {
        if (g_audio_read_pos != g_audio_write_pos) {
            output[i*2] = g_audio_buffer[g_audio_read_pos*2];
            output[i*2+1] = g_audio_buffer[g_audio_read_pos*2+1];
            g_audio_read_pos = (g_audio_read_pos + 1) % AUDIO_BUFFER_SIZE;
        } else {
            /* Buffer underrun - silence */
            output[i*2] = 0;
            output[i*2+1] = 0;
        }
    }
}

static void on_audio_sample(GBContext* ctx, int16_t left, int16_t right) {
    (void)ctx;
    int next_pos = (g_audio_write_pos + 1) % AUDIO_BUFFER_SIZE;
    if (next_pos != g_audio_read_pos) {
        g_audio_buffer[g_audio_write_pos*2] = left;
        g_audio_buffer[g_audio_write_pos*2+1] = right;
        g_audio_write_pos = next_pos;
    }
}

bool gb_platform_init(int scale) {
    g_scale = scale;
    if (g_scale < 1) g_scale = 1;
    if (g_scale > 8) g_scale = 8;
    
    fprintf(stderr, "[SDL] Initializing SDL...\n");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "[SDL] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    fprintf(stderr, "[SDL] SDL initialized.\n");
    
    /* Initialize Audio */
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = sdl_audio_callback;
    want.userdata = NULL;
    
    g_audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio_device == 0) {
        fprintf(stderr, "[SDL] Failed to open audio: %s\n", SDL_GetError());
    } else {
        fprintf(stderr, "[SDL] Audio initialized: %d Hz, %d channels\n", have.freq, have.channels);
        SDL_PauseAudioDevice(g_audio_device, 0); /* Start playing */
    }
    
    fprintf(stderr, "[SDL] Creating window...\n");
    g_window = SDL_CreateWindow(
        "GameBoy Recompiled",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        GB_SCREEN_WIDTH * g_scale,
        GB_SCREEN_HEIGHT * g_scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    
    if (!g_window) {
        fprintf(stderr, "[SDL] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    fprintf(stderr, "[SDL] Window created.\n");
    
    fprintf(stderr, "[SDL] Creating renderer...\n");
    g_renderer = SDL_CreateRenderer(g_window, -1, 
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        
    if (!g_renderer) {
        fprintf(stderr, "[SDL] Hardware renderer failed (flags=0x%x), trying software fallback...\n", 
                SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
        
    if (!g_renderer) {
        fprintf(stderr, "[SDL] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return false;
    }
    
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplSDL2_InitForSDLRenderer(g_window, g_renderer);
    ImGui_ImplSDLRenderer2_Init(g_renderer);

    g_texture = SDL_CreateTexture(
        g_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        GB_SCREEN_WIDTH,
        GB_SCREEN_HEIGHT
    );
    
    if (!g_texture) {
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return false;
    }
    
    g_last_frame_time = SDL_GetTicks();
    
    return true;
}

bool gb_platform_poll_events(GBContext* ctx) {
    SDL_Event event;
    uint8_t joyp = ctx ? ctx->io[0x00] : 0xFF;
    bool dpad_selected = !(joyp & 0x10);
    bool buttons_selected = !(joyp & 0x20);
    
    while (SDL_PollEvent(&event)) {
         ImGui_ImplSDL2_ProcessEvent(&event);
         if (event.type == SDL_QUIT) return false;
         if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(g_window))
            return false;

        switch (event.type) {
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                bool pressed = (event.type == SDL_KEYDOWN);
                bool trigger = false;
                
                switch (event.key.keysym.scancode) {
                    /* D-pad */
                    case SDL_SCANCODE_UP:
                    case SDL_SCANCODE_W:
                        if (pressed) { g_joypad_dpad &= ~0x04; if (dpad_selected) trigger = true; }
                        else g_joypad_dpad |= 0x04;
                        break;
                    case SDL_SCANCODE_DOWN:
                    case SDL_SCANCODE_S:
                        if (pressed) { g_joypad_dpad &= ~0x08; if (dpad_selected) trigger = true; }
                        else g_joypad_dpad |= 0x08;
                        break;
                    case SDL_SCANCODE_LEFT:
                    case SDL_SCANCODE_A:
                        if (pressed) { g_joypad_dpad &= ~0x02; if (dpad_selected) trigger = true; }
                        else g_joypad_dpad |= 0x02;
                        break;
                    case SDL_SCANCODE_RIGHT:
                    case SDL_SCANCODE_D:
                        if (pressed) { g_joypad_dpad &= ~0x01; if (dpad_selected) trigger = true; }
                        else g_joypad_dpad |= 0x01;
                        break;
                    
                    /* Buttons */
                    case SDL_SCANCODE_Z:
                    case SDL_SCANCODE_J:
                        if (pressed) { g_joypad_buttons &= ~0x01; if (buttons_selected) trigger = true; } /* A */
                        else g_joypad_buttons |= 0x01;
                        break;
                    case SDL_SCANCODE_X:
                    case SDL_SCANCODE_K:
                        if (pressed) { g_joypad_buttons &= ~0x02; if (buttons_selected) trigger = true; } /* B */
                        else g_joypad_buttons |= 0x02;
                        break;
                    case SDL_SCANCODE_RSHIFT:
                    case SDL_SCANCODE_BACKSPACE:
                        if (pressed) { g_joypad_buttons &= ~0x04; if (buttons_selected) trigger = true; } /* Select */
                        else g_joypad_buttons |= 0x04;
                        break;
                    case SDL_SCANCODE_RETURN:
                        if (pressed) { g_joypad_buttons &= ~0x08; if (buttons_selected) trigger = true; } /* Start */
                        else g_joypad_buttons |= 0x08;
                        break;
                    
                    case SDL_SCANCODE_ESCAPE:
                        if (pressed) {
                            g_show_menu = !g_show_menu;
                        }
                        return true; // Don't block
                        
                    default:
                        break;
                }
                
                if (trigger && ctx && event.key.repeat == 0) {
                    ctx->io[0x0F] |= 0x10; /* Request Joypad Interrupt */
                    /* Also wake up HALT state immediately if needed, though handle_interrupts does it */
                    if (ctx->halted) ctx->halted = 0;
                }
                break;
            }
            
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    /* Handle resize if needed or just let SDL/ImGui handle it */
                }
                break;
        }
    }
    
    /* Handle Automation Inputs */
    for (int i = 0; i < g_script_count; i++) {
        ScriptEntry* e = &g_input_script[i];
        if (g_frame_count >= e->start_frame && g_frame_count < (e->start_frame + e->duration)) {
             // Apply inputs (ANDing masks)
             g_joypad_dpad &= e->dpad;
             g_joypad_buttons &= e->buttons;
             
             // Check triggers
             bool trigger = false;
             if ((~e->dpad & 0x0F) && dpad_selected) trigger = true;
             if ((~e->buttons & 0x0F) && buttons_selected) trigger = true;
             
                if (trigger && ctx) {
                    /* Only trigger on initial press, not repeats or continuous hold */
                     if (g_frame_count == e->start_frame) {
                        ctx->io[0x0F] |= 0x10;
                        if (ctx->halted) ctx->halted = 0;
                     }
                }
        }
    }

    return true;
}



void gb_platform_render_frame(const uint32_t* framebuffer) {
    if (!g_texture || !g_renderer || !framebuffer) {
        DBG_FRAME("Platform render_frame: SKIPPED (null: texture=%d, renderer=%d, fb=%d)",
                  g_texture == NULL, g_renderer == NULL, framebuffer == NULL);
        return;
    }
    
    g_frame_count++;
    
    /* Handle Screenshot Dumping */
    for (int i = 0; i < g_dump_count; i++) {
        if (g_dump_frames[i] == (uint32_t)g_frame_count) {
             char filename[128];
             snprintf(filename, sizeof(filename), "%s_%05d.ppm", g_screenshot_prefix, g_frame_count);
             save_ppm(filename, framebuffer, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT, g_frame_count);
        }
    }
    
    /* Debug: check framebuffer content on first few frames */
    if (g_frame_count <= 3) {
        /* Check if framebuffer has any non-white pixels */
        bool has_content = false;
        uint32_t white = 0xFFE0F8D0;  /* DMG palette color 0 */
        for (int i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; i++) {
            if (framebuffer[i] != white) {
                has_content = true;
                break;
            }
        }
        DBG_FRAME("Platform frame %d - has_content=%d, first_pixel=0x%08X",
                  g_frame_count, has_content, framebuffer[0]);
    }
    
    if (g_frame_count % 60 == 0) {
        char title[64];
        snprintf(title, sizeof(title), "GameBoy Recompiled - Frame %d", g_frame_count);
        SDL_SetWindowTitle(g_window, title);
    }
    
    /* Update texture */
    void* pixels;
    int pitch;
    SDL_LockTexture(g_texture, NULL, &pixels, &pitch);
    
    const uint32_t* src = framebuffer;
    uint32_t* dst = (uint32_t*)pixels;
    
    if (g_palette_idx == 0) {
        memcpy(dst, src, GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT * sizeof(uint32_t));
    } else {
        uint32_t original_palette[4] = { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 };
        
        for (int i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; i++) {
            uint32_t c = src[i];
            int color_idx = -1;
            if (c == original_palette[0]) color_idx = 0;
            else if (c == original_palette[1]) color_idx = 1;
            else if (c == original_palette[2]) color_idx = 2;
            else if (c == original_palette[3]) color_idx = 3;
            
            if (color_idx >= 0) {
                dst[i] = g_palettes[g_palette_idx][color_idx];
            } else {
                dst[i] = c; 
            }
        }
    }
    
    SDL_UnlockTexture(g_texture);
    
    /* Clear and render */
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);

    // ImGui Frame
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    if (g_show_menu) {
        ImGui::Begin("GameBoy Recompiled", &g_show_menu);
        ImGui::Text("Performance: %.1f FPS", ImGui::GetIO().Framerate);
        int scale_idx = g_scale - 1;
        if (ImGui::Combo("Resolution", &scale_idx, g_scale_names, IM_ARRAYSIZE(g_scale_names))) {
            g_scale = scale_idx + 1;
            SDL_SetWindowSize(g_window, GB_SCREEN_WIDTH * g_scale, GB_SCREEN_HEIGHT * g_scale);
            SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }

        if (ImGui::Checkbox("V-Sync", &g_vsync)) {
            SDL_RenderSetVSync(g_renderer, g_vsync ? 1 : 0);
        }

        ImGui::SliderInt("Speed %", &g_speed_percent, 10, 500);
        if (ImGui::Button("Reset Speed")) g_speed_percent = 100;
        ImGui::Combo("Palette", &g_palette_idx, g_palette_names, IM_ARRAYSIZE(g_palette_names));

        if (ImGui::Button("Reset to Defaults")) {
            g_scale = 3;
            g_speed_percent = 100;
            SDL_SetWindowSize(g_window, GB_SCREEN_WIDTH * g_scale, GB_SCREEN_HEIGHT * g_scale);
            SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            g_palette_idx = 0;
        }

        if (ImGui::Button("Quit")) {
            SDL_Event quit_event;
            quit_event.type = SDL_QUIT;
            SDL_PushEvent(&quit_event);
        }
        ImGui::End();
    } else {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.35f); 
        if (ImGui::Begin("Overlay", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            ImGui::Text("Press ESC for Menu");
            ImGui::End();
        }
    }

    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_renderer);

    SDL_RenderPresent(g_renderer);
}

uint8_t gb_platform_get_joypad(void) {
    /* Return combined state based on P1 register selection */
    /* Caller should AND with the appropriate selection bits */
    return g_joypad_buttons & g_joypad_dpad;
}

void gb_platform_vsync(void) {
    /* Target 59.7 FPS * Speed Multiplier */
    const uint32_t base_frame_time_ms = 16;
    uint32_t scaled_frame_time = (base_frame_time_ms * 100) / (g_speed_percent > 0 ? g_speed_percent : 1);
    
    uint32_t current_time = SDL_GetTicks();
    uint32_t elapsed = current_time - g_last_frame_time;
    
    if (elapsed < scaled_frame_time) {
        SDL_Delay(scaled_frame_time - elapsed);
    }
    
    g_last_frame_time = SDL_GetTicks();
}

void gb_platform_set_title(const char* title) {
    if (g_window) {
        SDL_SetWindowTitle(g_window, title);
    }
}

/* ============================================================================
 * Save Data
 * ========================================================================== */

static void sdl_get_save_path(char* buffer, size_t size, const char* rom_name) {
    char* base_path = SDL_GetBasePath();
    if (base_path) {
        // Extract just the filename from rom_name to avoid path traversal issues
        const char* base_name = strrchr(rom_name, '/');
#ifdef _WIN32
        const char* base_name_win = strrchr(rom_name, '\\');
        if (base_name_win > base_name) base_name = base_name_win;
#endif
        if (base_name) {
            base_name++; // Skip separator
        } else {
            base_name = rom_name;
        }

        snprintf(buffer, size, "%s%s.sav", base_path, base_name);
        SDL_free(base_path);
    } else {
        // Fallback to CWD if SDL_GetBasePath fails
        snprintf(buffer, size, "%s.sav", rom_name);
    }
}

static bool sdl_load_battery_ram(GBContext* ctx, const char* rom_name, void* data, size_t size) {
    (void)ctx;
    char filename[512];
    sdl_get_save_path(filename, sizeof(filename), rom_name);
    
    FILE* f = fopen(filename, "rb");
    if (!f) return false;
    
    size_t read = fread(data, 1, size, f);
    fclose(f);
    
    return read == size;
}

static bool sdl_save_battery_ram(GBContext* ctx, const char* rom_name, const void* data, size_t size) {
    (void)ctx;
    char filename[512];
    sdl_get_save_path(filename, sizeof(filename), rom_name);
    
    FILE* f = fopen(filename, "wb");
    if (!f) return false;
    
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    
    return written == size;
}

void gb_platform_register_context(GBContext* ctx) {
    GBPlatformCallbacks callbacks = {
        .on_audio_sample = on_audio_sample,
        .load_battery_ram = sdl_load_battery_ram,
        .save_battery_ram = sdl_save_battery_ram
    };
    gb_set_platform_callbacks(ctx, &callbacks);
}

#else  /* !GB_HAS_SDL2 */

/* Stub implementations when SDL2 is not available */

bool gb_platform_init(int scale) {
    (void)scale;
    return false;
}

void gb_platform_shutdown(void) {}

bool gb_platform_poll_events(GBContext* ctx) {
    (void)ctx;
    return true;
}

void gb_platform_render_frame(const uint32_t* framebuffer) {
    (void)framebuffer;
}

uint8_t gb_platform_get_joypad(void) {
    return 0xFF;
}

void gb_platform_vsync(void) {}

void gb_platform_set_title(const char* title) {
    (void)title;
}

void gb_platform_register_context(GBContext* ctx) { (void)ctx; }

#endif /* GB_HAS_SDL2 */
