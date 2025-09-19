/*
sdl_window.c - SDL1 / SDL2 / SDL3 GUI Window
Copyright (C) 2022  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "feature_test.h"

#include "compiler.h"
#include "gui_window.h"

PUSH_OPTIMIZATION_SIZE

/*
 * Check library availability
 */

#if defined(COMPILER_IS_MSVC) || defined(HOST_TARGET_EMSCRIPTEN) || defined(HOST_TARGET_REDOX)
// MSVC compiler, Emscripten and Redox OS can't handle dynamic library loading
#undef USE_LIBS_PROBE
#endif

#if defined(USE_SDL) && USE_SDL == 1 && CHECK_INCLUDE(SDL/SDL.h, 1)
// SDL1 headers available system wide
#include <SDL/SDL.h>

#elif defined(USE_SDL) && USE_SDL == 2 && CHECK_INCLUDE(SDL2/SDL.h, 1)
// SDL2 headers available system wide
#include <SDL2/SDL.h>

#elif defined(USE_SDL) && USE_SDL == 3 && CHECK_INCLUDE(SDL3/SDL.h, 1)
// SDL3 headers available system wide
#include <SDL3/SDL.h>

#elif defined(USE_SDL) && CHECK_INCLUDE(SDL.h, 1)
// Specific SDL version provided by pkg-config
#include <SDL.h>
#undef USE_SDL
#define USE_SDL SDL_MAJOR_VERSION

#elif defined(USE_SDL)
// No SDL headers available
#pragma message("Disabling SDL support as <SDL.h> is unavailable")
#undef USE_SDL
#endif

/*
 * SDL1 / SDL2 / SDL3 support glue
 */

#if defined(USE_SDL)

#include "utils.h"
#include "vector.h"
#include "vma_ops.h"

#if defined(USE_LIBS_PROBE)

// Resolve symbols at runtime
#include "dlib.h"

#define SDL_DLIB_SYM(sym) static __typeof__(sym)* MACRO_CONCAT(sym, _dlib) = NULL;

SDL_DLIB_SYM(SDL_Init)
SDL_DLIB_SYM(SDL_PollEvent)
SDL_DLIB_SYM(SDL_QuitSubSystem)
SDL_DLIB_SYM(SDL_ShowCursor)

#define SDL_Init          SDL_Init_dlib
#define SDL_PollEvent     SDL_PollEvent_dlib
#define SDL_QuitSubSystem SDL_QuitSubSystem_dlib
#define SDL_ShowCursor    SDL_ShowCursor_dlib

#endif

#if USE_SDL >= 2

#if defined(USE_LIBS_PROBE)
SDL_DLIB_SYM(SDL_CreateRenderer)
SDL_DLIB_SYM(SDL_CreateTexture)
SDL_DLIB_SYM(SDL_CreateWindow)
SDL_DLIB_SYM(SDL_DestroyRenderer)
SDL_DLIB_SYM(SDL_DestroyTexture)
SDL_DLIB_SYM(SDL_DestroyWindow)
SDL_DLIB_SYM(SDL_GetCurrentDisplayMode)
SDL_DLIB_SYM(SDL_GetCurrentVideoDriver)
SDL_DLIB_SYM(SDL_GetWindowID)
SDL_DLIB_SYM(SDL_GetWindowPosition)
SDL_DLIB_SYM(SDL_RenderClear)
SDL_DLIB_SYM(SDL_RenderPresent)
SDL_DLIB_SYM(SDL_SetHint)
SDL_DLIB_SYM(SDL_SetWindowMinimumSize)
SDL_DLIB_SYM(SDL_SetWindowPosition)
SDL_DLIB_SYM(SDL_SetWindowSize)
SDL_DLIB_SYM(SDL_SetWindowTitle)
SDL_DLIB_SYM(SDL_UpdateTexture)

#define SDL_CreateRenderer        SDL_CreateRenderer_dlib
#define SDL_CreateTexture         SDL_CreateTexture_dlib
#define SDL_CreateWindow          SDL_CreateWindow_dlib
#define SDL_DestroyRenderer       SDL_DestroyRenderer_dlib
#define SDL_DestroyTexture        SDL_DestroyTexture_dlib
#define SDL_DestroyWindow         SDL_DestroyWindow_dlib
#define SDL_GetCurrentDisplayMode SDL_GetCurrentDisplayMode_dlib
#define SDL_GetCurrentVideoDriver SDL_GetCurrentVideoDriver_dlib
#define SDL_GetWindowID           SDL_GetWindowID_dlib
#define SDL_GetWindowPosition     SDL_GetWindowPosition_dlib
#define SDL_RenderClear           SDL_RenderClear_dlib
#define SDL_RenderPresent         SDL_RenderPresent_dlib
#define SDL_SetHint               SDL_SetHint_dlib
#define SDL_SetWindowMinimumSize  SDL_SetWindowMinimumSize_dlib
#define SDL_SetWindowPosition     SDL_SetWindowPosition_dlib
#define SDL_SetWindowSize         SDL_SetWindowSize_dlib
#define SDL_SetWindowTitle        SDL_SetWindowTitle_dlib
#define SDL_UpdateTexture         SDL_UpdateTexture_dlib
#endif

static const hid_key_t sdl_key_to_hid_byte_map[] = {
    [SDL_SCANCODE_A]              = HID_KEY_A,
    [SDL_SCANCODE_B]              = HID_KEY_B,
    [SDL_SCANCODE_C]              = HID_KEY_C,
    [SDL_SCANCODE_D]              = HID_KEY_D,
    [SDL_SCANCODE_E]              = HID_KEY_E,
    [SDL_SCANCODE_F]              = HID_KEY_F,
    [SDL_SCANCODE_G]              = HID_KEY_G,
    [SDL_SCANCODE_H]              = HID_KEY_H,
    [SDL_SCANCODE_I]              = HID_KEY_I,
    [SDL_SCANCODE_J]              = HID_KEY_J,
    [SDL_SCANCODE_K]              = HID_KEY_K,
    [SDL_SCANCODE_L]              = HID_KEY_L,
    [SDL_SCANCODE_M]              = HID_KEY_M,
    [SDL_SCANCODE_N]              = HID_KEY_N,
    [SDL_SCANCODE_O]              = HID_KEY_O,
    [SDL_SCANCODE_P]              = HID_KEY_P,
    [SDL_SCANCODE_Q]              = HID_KEY_Q,
    [SDL_SCANCODE_R]              = HID_KEY_R,
    [SDL_SCANCODE_S]              = HID_KEY_S,
    [SDL_SCANCODE_T]              = HID_KEY_T,
    [SDL_SCANCODE_U]              = HID_KEY_U,
    [SDL_SCANCODE_V]              = HID_KEY_V,
    [SDL_SCANCODE_W]              = HID_KEY_W,
    [SDL_SCANCODE_X]              = HID_KEY_X,
    [SDL_SCANCODE_Y]              = HID_KEY_Y,
    [SDL_SCANCODE_Z]              = HID_KEY_Z,
    [SDL_SCANCODE_0]              = HID_KEY_0,
    [SDL_SCANCODE_1]              = HID_KEY_1,
    [SDL_SCANCODE_2]              = HID_KEY_2,
    [SDL_SCANCODE_3]              = HID_KEY_3,
    [SDL_SCANCODE_4]              = HID_KEY_4,
    [SDL_SCANCODE_5]              = HID_KEY_5,
    [SDL_SCANCODE_6]              = HID_KEY_6,
    [SDL_SCANCODE_7]              = HID_KEY_7,
    [SDL_SCANCODE_8]              = HID_KEY_8,
    [SDL_SCANCODE_9]              = HID_KEY_9,
    [SDL_SCANCODE_RETURN]         = HID_KEY_ENTER,
    [SDL_SCANCODE_ESCAPE]         = HID_KEY_ESC,
    [SDL_SCANCODE_BACKSPACE]      = HID_KEY_BACKSPACE,
    [SDL_SCANCODE_TAB]            = HID_KEY_TAB,
    [SDL_SCANCODE_SPACE]          = HID_KEY_SPACE,
    [SDL_SCANCODE_MINUS]          = HID_KEY_MINUS,
    [SDL_SCANCODE_EQUALS]         = HID_KEY_EQUAL,
    [SDL_SCANCODE_LEFTBRACKET]    = HID_KEY_LEFTBRACE,
    [SDL_SCANCODE_RIGHTBRACKET]   = HID_KEY_RIGHTBRACE,
    [SDL_SCANCODE_BACKSLASH]      = HID_KEY_BACKSLASH,
    [SDL_SCANCODE_SEMICOLON]      = HID_KEY_SEMICOLON,
    [SDL_SCANCODE_APOSTROPHE]     = HID_KEY_APOSTROPHE,
    [SDL_SCANCODE_GRAVE]          = HID_KEY_GRAVE,
    [SDL_SCANCODE_COMMA]          = HID_KEY_COMMA,
    [SDL_SCANCODE_PERIOD]         = HID_KEY_DOT,
    [SDL_SCANCODE_SLASH]          = HID_KEY_SLASH,
    [SDL_SCANCODE_CAPSLOCK]       = HID_KEY_CAPSLOCK,
    [SDL_SCANCODE_F1]             = HID_KEY_F1,
    [SDL_SCANCODE_F2]             = HID_KEY_F2,
    [SDL_SCANCODE_F3]             = HID_KEY_F3,
    [SDL_SCANCODE_F4]             = HID_KEY_F4,
    [SDL_SCANCODE_F5]             = HID_KEY_F5,
    [SDL_SCANCODE_F6]             = HID_KEY_F6,
    [SDL_SCANCODE_F7]             = HID_KEY_F7,
    [SDL_SCANCODE_F8]             = HID_KEY_F8,
    [SDL_SCANCODE_F9]             = HID_KEY_F9,
    [SDL_SCANCODE_F10]            = HID_KEY_F10,
    [SDL_SCANCODE_F11]            = HID_KEY_F11,
    [SDL_SCANCODE_F12]            = HID_KEY_F12,
    [SDL_SCANCODE_SYSREQ]         = HID_KEY_SYSRQ,
    [SDL_SCANCODE_SCROLLLOCK]     = HID_KEY_SCROLLLOCK,
    [SDL_SCANCODE_PAUSE]          = HID_KEY_PAUSE,
    [SDL_SCANCODE_INSERT]         = HID_KEY_INSERT,
    [SDL_SCANCODE_HOME]           = HID_KEY_HOME,
    [SDL_SCANCODE_PAGEUP]         = HID_KEY_PAGEUP,
    [SDL_SCANCODE_DELETE]         = HID_KEY_DELETE,
    [SDL_SCANCODE_END]            = HID_KEY_END,
    [SDL_SCANCODE_PAGEDOWN]       = HID_KEY_PAGEDOWN,
    [SDL_SCANCODE_RIGHT]          = HID_KEY_RIGHT,
    [SDL_SCANCODE_LEFT]           = HID_KEY_LEFT,
    [SDL_SCANCODE_DOWN]           = HID_KEY_DOWN,
    [SDL_SCANCODE_UP]             = HID_KEY_UP,
    [SDL_SCANCODE_NUMLOCKCLEAR]   = HID_KEY_NUMLOCK,
    [SDL_SCANCODE_KP_DIVIDE]      = HID_KEY_KPSLASH,
    [SDL_SCANCODE_KP_MULTIPLY]    = HID_KEY_KPASTERISK,
    [SDL_SCANCODE_KP_MINUS]       = HID_KEY_KPMINUS,
    [SDL_SCANCODE_KP_PLUS]        = HID_KEY_KPPLUS,
    [SDL_SCANCODE_KP_ENTER]       = HID_KEY_KPENTER,
    [SDL_SCANCODE_KP_1]           = HID_KEY_KP1,
    [SDL_SCANCODE_KP_2]           = HID_KEY_KP2,
    [SDL_SCANCODE_KP_3]           = HID_KEY_KP3,
    [SDL_SCANCODE_KP_4]           = HID_KEY_KP4,
    [SDL_SCANCODE_KP_5]           = HID_KEY_KP5,
    [SDL_SCANCODE_KP_6]           = HID_KEY_KP6,
    [SDL_SCANCODE_KP_7]           = HID_KEY_KP7,
    [SDL_SCANCODE_KP_8]           = HID_KEY_KP8,
    [SDL_SCANCODE_KP_9]           = HID_KEY_KP9,
    [SDL_SCANCODE_KP_0]           = HID_KEY_KP0,
    [SDL_SCANCODE_KP_PERIOD]      = HID_KEY_KPDOT,
    [SDL_SCANCODE_APPLICATION]    = HID_KEY_COMPOSE,
    [SDL_SCANCODE_KP_EQUALS]      = HID_KEY_KPEQUAL,
    [SDL_SCANCODE_INTERNATIONAL1] = HID_KEY_RO,
    [SDL_SCANCODE_INTERNATIONAL2] = HID_KEY_KATAKANAHIRAGANA,
    [SDL_SCANCODE_INTERNATIONAL3] = HID_KEY_YEN,
    [SDL_SCANCODE_INTERNATIONAL4] = HID_KEY_HENKAN,
    [SDL_SCANCODE_INTERNATIONAL5] = HID_KEY_MUHENKAN,
    [SDL_SCANCODE_INTERNATIONAL6] = HID_KEY_KPJPCOMMA,
    [SDL_SCANCODE_LANG1]          = HID_KEY_HANGEUL,
    [SDL_SCANCODE_LANG2]          = HID_KEY_HANJA,
    [SDL_SCANCODE_LANG3]          = HID_KEY_KATAKANA,
    [SDL_SCANCODE_LANG4]          = HID_KEY_HIRAGANA,
    [SDL_SCANCODE_LANG5]          = HID_KEY_ZENKAKUHANKAKU,
    [SDL_SCANCODE_MENU]           = HID_KEY_MENU,
    [SDL_SCANCODE_LCTRL]          = HID_KEY_LEFTCTRL,
    [SDL_SCANCODE_LSHIFT]         = HID_KEY_LEFTSHIFT,
    [SDL_SCANCODE_LALT]           = HID_KEY_LEFTALT,
    [SDL_SCANCODE_LGUI]           = HID_KEY_LEFTMETA,
    [SDL_SCANCODE_RCTRL]          = HID_KEY_RIGHTCTRL,
    [SDL_SCANCODE_RSHIFT]         = HID_KEY_RIGHTSHIFT,
    [SDL_SCANCODE_RALT]           = HID_KEY_RIGHTALT,
    [SDL_SCANCODE_RGUI]           = HID_KEY_RIGHTMETA,
};

#endif

#if USE_SDL == 1

#define SDL_LIB_NAME "SDL"

#if defined(USE_LIBS_PROBE)
SDL_DLIB_SYM(SDL_CreateRGBSurfaceFrom)
SDL_DLIB_SYM(SDL_Flip)
SDL_DLIB_SYM(SDL_FreeSurface)
SDL_DLIB_SYM(SDL_SetVideoMode)
SDL_DLIB_SYM(SDL_UpperBlit)
SDL_DLIB_SYM(SDL_WM_GrabInput)
SDL_DLIB_SYM(SDL_WM_SetCaption)

#define SDL_CreateRGBSurfaceFrom SDL_CreateRGBSurfaceFrom_dlib
#define SDL_Flip                 SDL_Flip_dlib
#define SDL_FreeSurface          SDL_FreeSurface_dlib
#define SDL_SetVideoMode         SDL_SetVideoMode_dlib
#define SDL_UpperBlit            SDL_UpperBlit_dlib
#define SDL_WM_GrabInput         SDL_WM_GrabInput_dlib
#define SDL_WM_SetCaption        SDL_WM_SetCaption_dlib
#endif

static const hid_key_t sdl_key_to_hid_byte_map[] = {
    [SDLK_a]            = HID_KEY_A,
    [SDLK_b]            = HID_KEY_B,
    [SDLK_c]            = HID_KEY_C,
    [SDLK_d]            = HID_KEY_D,
    [SDLK_e]            = HID_KEY_E,
    [SDLK_f]            = HID_KEY_F,
    [SDLK_g]            = HID_KEY_G,
    [SDLK_h]            = HID_KEY_H,
    [SDLK_i]            = HID_KEY_I,
    [SDLK_j]            = HID_KEY_J,
    [SDLK_k]            = HID_KEY_K,
    [SDLK_l]            = HID_KEY_L,
    [SDLK_m]            = HID_KEY_M,
    [SDLK_n]            = HID_KEY_N,
    [SDLK_o]            = HID_KEY_O,
    [SDLK_p]            = HID_KEY_P,
    [SDLK_q]            = HID_KEY_Q,
    [SDLK_r]            = HID_KEY_R,
    [SDLK_s]            = HID_KEY_S,
    [SDLK_t]            = HID_KEY_T,
    [SDLK_u]            = HID_KEY_U,
    [SDLK_v]            = HID_KEY_V,
    [SDLK_w]            = HID_KEY_W,
    [SDLK_x]            = HID_KEY_X,
    [SDLK_y]            = HID_KEY_Y,
    [SDLK_z]            = HID_KEY_Z,
    [SDLK_0]            = HID_KEY_0,
    [SDLK_1]            = HID_KEY_1,
    [SDLK_2]            = HID_KEY_2,
    [SDLK_3]            = HID_KEY_3,
    [SDLK_4]            = HID_KEY_4,
    [SDLK_5]            = HID_KEY_5,
    [SDLK_6]            = HID_KEY_6,
    [SDLK_7]            = HID_KEY_7,
    [SDLK_8]            = HID_KEY_8,
    [SDLK_9]            = HID_KEY_9,
    [SDLK_RETURN]       = HID_KEY_ENTER,
    [SDLK_ESCAPE]       = HID_KEY_ESC,
    [SDLK_BACKSPACE]    = HID_KEY_BACKSPACE,
    [SDLK_TAB]          = HID_KEY_TAB,
    [SDLK_SPACE]        = HID_KEY_SPACE,
    [SDLK_MINUS]        = HID_KEY_MINUS,
    [SDLK_EQUALS]       = HID_KEY_EQUAL,
    [SDLK_LEFTBRACKET]  = HID_KEY_LEFTBRACE,
    [SDLK_RIGHTBRACKET] = HID_KEY_RIGHTBRACE,
    [SDLK_BACKSLASH]    = HID_KEY_BACKSLASH,
    [SDLK_SEMICOLON]    = HID_KEY_SEMICOLON,
    [SDLK_QUOTE]        = HID_KEY_APOSTROPHE,
    [SDLK_BACKQUOTE]    = HID_KEY_GRAVE,
    [SDLK_COMMA]        = HID_KEY_COMMA,
    [SDLK_PERIOD]       = HID_KEY_DOT,
    [SDLK_SLASH]        = HID_KEY_SLASH,
    [SDLK_CAPSLOCK]     = HID_KEY_CAPSLOCK,
    [SDLK_LCTRL]        = HID_KEY_LEFTCTRL,
    [SDLK_LSHIFT]       = HID_KEY_LEFTSHIFT,
    [SDLK_LALT]         = HID_KEY_LEFTALT,
    [SDLK_LMETA]        = HID_KEY_LEFTMETA,
    [SDLK_RCTRL]        = HID_KEY_RIGHTCTRL,
    [SDLK_RSHIFT]       = HID_KEY_RIGHTSHIFT,
    [SDLK_RALT]         = HID_KEY_RIGHTALT,
    [SDLK_RMETA]        = HID_KEY_RIGHTMETA,
    [SDLK_F1]           = HID_KEY_F1,
    [SDLK_F2]           = HID_KEY_F2,
    [SDLK_F3]           = HID_KEY_F3,
    [SDLK_F4]           = HID_KEY_F4,
    [SDLK_F5]           = HID_KEY_F5,
    [SDLK_F6]           = HID_KEY_F6,
    [SDLK_F7]           = HID_KEY_F7,
    [SDLK_F8]           = HID_KEY_F8,
    [SDLK_F9]           = HID_KEY_F9,
    [SDLK_F10]          = HID_KEY_F10,
    [SDLK_F11]          = HID_KEY_F11,
    [SDLK_F12]          = HID_KEY_F12,
    [SDLK_SYSREQ]       = HID_KEY_SYSRQ,
    [SDLK_SCROLLOCK]    = HID_KEY_SCROLLLOCK,
    [SDLK_PAUSE]        = HID_KEY_PAUSE,
    [SDLK_INSERT]       = HID_KEY_INSERT,
    [SDLK_HOME]         = HID_KEY_HOME,
    [SDLK_PAGEUP]       = HID_KEY_PAGEUP,
    [SDLK_DELETE]       = HID_KEY_DELETE,
    [SDLK_END]          = HID_KEY_END,
    [SDLK_PAGEDOWN]     = HID_KEY_PAGEDOWN,
    [SDLK_RIGHT]        = HID_KEY_RIGHT,
    [SDLK_LEFT]         = HID_KEY_LEFT,
    [SDLK_DOWN]         = HID_KEY_DOWN,
    [SDLK_UP]           = HID_KEY_UP,
    [SDLK_NUMLOCK]      = HID_KEY_NUMLOCK,
    [SDLK_KP_DIVIDE]    = HID_KEY_KPSLASH,
    [SDLK_KP_MULTIPLY]  = HID_KEY_KPASTERISK,
    [SDLK_KP_MINUS]     = HID_KEY_KPMINUS,
    [SDLK_KP_PLUS]      = HID_KEY_KPPLUS,
    [SDLK_KP_ENTER]     = HID_KEY_KPENTER,
    [SDLK_KP1]          = HID_KEY_KP1,
    [SDLK_KP2]          = HID_KEY_KP2,
    [SDLK_KP3]          = HID_KEY_KP3,
    [SDLK_KP4]          = HID_KEY_KP4,
    [SDLK_KP5]          = HID_KEY_KP5,
    [SDLK_KP6]          = HID_KEY_KP6,
    [SDLK_KP7]          = HID_KEY_KP7,
    [SDLK_KP8]          = HID_KEY_KP8,
    [SDLK_KP9]          = HID_KEY_KP9,
    [SDLK_KP0]          = HID_KEY_KP0,
    [SDLK_KP_PERIOD]    = HID_KEY_KPDOT,
    [SDLK_MENU]         = HID_KEY_MENU,
#if defined(HOST_TARGET_EMSCRIPTEN)
    // I dunno why, I don't want to know why,
    // but some Emscripten SDL keycodes are plain wrong..
    [0xbb] = HID_KEY_EQUAL,
    [0xbd] = HID_KEY_MINUS,
#endif
};

#elif USE_SDL == 2

#define SDL_LIB_NAME "SDL2"

#if SDL_MAJOR_VERSION == 2 && SDL_MINOR_VERSION < 14
// Support pre SDL 2.14
#define SDL_PIXELFORMAT_XRGB8888 SDL_PIXELFORMAT_RGB888
#define SDL_PIXELFORMAT_XBGR8888 SDL_PIXELFORMAT_BGR888
#endif

#if defined(USE_LIBS_PROBE)
SDL_DLIB_SYM(SDL_RenderCopy)
SDL_DLIB_SYM(SDL_SetRelativeMouseMode)
SDL_DLIB_SYM(SDL_SetWindowGrab)

#define SDL_RenderCopy           SDL_RenderCopy_dlib
#define SDL_SetRelativeMouseMode SDL_SetRelativeMouseMode_dlib
#define SDL_SetWindowGrab        SDL_SetWindowGrab_dlib
#endif

#elif USE_SDL == 3

#define SDL_LIB_NAME "SDL3"

#if defined(USE_LIBS_PROBE)
SDL_DLIB_SYM(SDL_HideCursor)
SDL_DLIB_SYM(SDL_RenderTexture)
SDL_DLIB_SYM(SDL_SetWindowKeyboardGrab)
SDL_DLIB_SYM(SDL_SetWindowMouseGrab)
SDL_DLIB_SYM(SDL_SetWindowRelativeMouseMode)

#define SDL_HideCursor                 SDL_HideCursor_dlib
#define SDL_RenderTexture              SDL_RenderTexture_dlib
#define SDL_SetWindowKeyboardGrab      SDL_SetWindowKeyboardGrab_dlib
#define SDL_SetWindowMouseGrab         SDL_SetWindowMouseGrab_dlib
#define SDL_SetWindowRelativeMouseMode SDL_SetWindowRelativeMouseMode_dlib
#endif

#endif

/*
 * SDL backend implementation
 */

// D'oh the frickin endianness issues!
#if defined(HOST_BIG_ENDIAN)
#define SDL_PIXELFORMAT_BGRX32 SDL_PIXELFORMAT_BGRX8888
#define SDL_PIXELFORMAT_RGBX32 SDL_PIXELFORMAT_RGBX8888
#define SDL_PIXELFORMAT_XRGB32 SDL_PIXELFORMAT_XRGB8888
#define SDL_PIXELFORMAT_XBGR32 SDL_PIXELFORMAT_XBGR8888
#else
#define SDL_PIXELFORMAT_BGRX32 SDL_PIXELFORMAT_XRGB8888
#define SDL_PIXELFORMAT_RGBX32 SDL_PIXELFORMAT_XBGR8888
#define SDL_PIXELFORMAT_XRGB32 SDL_PIXELFORMAT_BGRX8888
#define SDL_PIXELFORMAT_XBGR32 SDL_PIXELFORMAT_RGBX8888
#endif

#define SDL_PIXELFORMAT_XRGB1555    0x15130F02U
#define SDL_PIXELFORMAT_XRGB2101010 0x16172004U
#define SDL_PIXELFORMAT_XBGR2101010 0x16572004U

#if USE_SDL == 3
#define SDL_INIT_SUCCESS 1
#else
#define SDL_INIT_SUCCESS 0
#endif

#if USE_SDL == 3

static inline int SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, //
                                 const SDL_Rect* src, const SDL_Rect* dst)
{
    SDL_FRect fsrc = ZERO_INIT;
    SDL_FRect fdst = ZERO_INIT;
    if (src) {
        SDL_RectToFRect(src, &fsrc);
    }
    if (dst) {
        SDL_RectToFRect(dst, &fdst);
    }
    return SDL_RenderTexture(renderer, texture, src ? &fsrc : NULL, dst ? &fdst : NULL) ? 0 : -1;
}

#endif

typedef struct {
#if USE_SDL >= 2
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;
#else
    SDL_Surface* window;
    SDL_Surface* texture;
#endif

    // Last scanout context
    rvvm_fb_t fb;

    // Window ID
    uint32_t id;

    // Input grab flag
    bool grab;
} sdl_window_t;

// Check whether this thread owns any SDL windows to fix MacOS issues
#if defined(THREAD_LOCAL)
static THREAD_LOCAL bool sdl_thread_ok = false;
#else
static bool sdl_thread_ok = false;
#endif

static vector_t(gui_window_t*) sdl_windows = {0};

/*
 * SDL Keycode to HID
 */

static hid_key_t sdl_key_to_hid(uint32_t sdl_key)
{
    if (sdl_key < sizeof(sdl_key_to_hid_byte_map)) {
        return sdl_key_to_hid_byte_map[sdl_key];
    }
    rvvm_warn("Unmapped " SDL_LIB_NAME " keycode %x", sdl_key);
    return HID_KEY_NONE;
}

/*
 * SDL Event handling
 */

static gui_window_t* sdl_find_window(uint32_t window_id)
{
    vector_foreach (sdl_windows, i) {
        gui_window_t* win = vector_at(sdl_windows, i);
        sdl_window_t* sdl = gui_backend_get_data(win);
        if (sdl->id == window_id) {
            return win;
        }
    }
    return NULL;
}

static void sdl_handle_keyboard(const SDL_Event* event, bool press)
{
#if USE_SDL == 3
    gui_window_t* win = sdl_find_window(event->key.windowID);
    hid_key_t     key = sdl_key_to_hid(event->key.scancode);
#elif USE_SDL == 2
    gui_window_t* win = sdl_find_window(event->key.windowID);
    hid_key_t     key = sdl_key_to_hid(event->key.keysym.scancode);
#else
    gui_window_t* win = sdl_find_window(0);
    hid_key_t     key = sdl_key_to_hid(event->key.keysym.sym);
#endif
    if (press) {
        gui_backend_on_key_press(win, key);
    } else {
        gui_backend_on_key_release(win, key);
    }
}

static void sdl_handle_mouse_moution(const SDL_Event* event)
{
#if USE_SDL >= 2
    gui_window_t* win = sdl_find_window(event->motion.windowID);
#else
    gui_window_t* win = sdl_find_window(0);
#endif
    sdl_window_t* sdl = gui_backend_get_data(win);
    if (sdl && sdl->grab) {
        gui_backend_on_mouse_move(win, (int)event->motion.xrel, (int)event->motion.yrel);
    } else {
        gui_backend_on_mouse_place(win, (int)event->motion.x, (int)event->motion.y);
    }
}

static void sdl_handle_mouse_btn(const SDL_Event* event, bool press)
{
#if USE_SDL >= 2
    gui_window_t* win = sdl_find_window(event->button.windowID);
#else
    gui_window_t* win = sdl_find_window(0);
#endif
    hid_btns_t btns = 0;
    switch (event->button.button) {
        case SDL_BUTTON_LEFT:
            btns = HID_BTN_LEFT;
            break;
        case SDL_BUTTON_MIDDLE:
            btns = HID_BTN_MIDDLE;
            break;
        case SDL_BUTTON_RIGHT:
            btns = HID_BTN_RIGHT;
            break;
#if USE_SDL == 1
        case SDL_BUTTON_WHEELUP:
            if (press) {
                gui_backend_on_mouse_scroll(win, HID_SCROLL_UP);
            }
            return;
        case SDL_BUTTON_WHEELDOWN:
            if (press) {
                gui_backend_on_mouse_scroll(win, HID_SCROLL_DOWN);
            }
            return;
#endif
    }
    if (press) {
        gui_backend_on_mouse_press(win, btns);
    } else {
        gui_backend_on_mouse_release(win, btns);
    }
}

static void sdl_handle_window(const SDL_Event* event, bool close)
{
#if USE_SDL >= 2
    gui_window_t* win = sdl_find_window(event->window.windowID);
#else
    gui_window_t* win = sdl_find_window(0);
    UNUSED(event);
#endif
    if (close) {
        gui_backend_on_close(win);
    } else {
        gui_backend_on_focus_lost(win);
    }
}

#if USE_SDL >= 2

static void sdl_handle_mouse_scroll(const SDL_Event* event)
{
    gui_window_t* win = sdl_find_window(event->wheel.windowID);
    gui_backend_on_mouse_scroll(win, (int)event->wheel.y);
}

static void sdl_handle_window_resize(const SDL_Event* event)
{
    gui_window_t* win = sdl_find_window(event->window.windowID);
    gui_backend_on_resize(win, event->window.data1, event->window.data2);
}

#endif

/*
 * SDL framebuffer texture formats handling
 */

static void* sdl_create_texture(sdl_window_t* sdl, const rvvm_fb_t* fb)
{
#if USE_SDL >= 2
    switch (rvvm_fb_format(fb)) {
        case RVVM_RGB_XRGB1555:
            return SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_XRGB1555, //
                                     SDL_TEXTUREACCESS_STREAMING, rvvm_fb_width(fb), rvvm_fb_height(fb));
        case RVVM_RGB_RGB565:
            return SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_RGB565, //
                                     SDL_TEXTUREACCESS_STREAMING, rvvm_fb_width(fb), rvvm_fb_height(fb));
        case RVVM_RGB_RGB888:
            return SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_BGR24, //
                                     SDL_TEXTUREACCESS_STREAMING, rvvm_fb_width(fb), rvvm_fb_height(fb));
        case RVVM_RGB_XRGB8888:
            return SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_BGRX32, //
                                     SDL_TEXTUREACCESS_STREAMING, rvvm_fb_width(fb), rvvm_fb_height(fb));
        case RVVM_RGB_BGR888:
            return SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_RGB24, //
                                     SDL_TEXTUREACCESS_STREAMING, rvvm_fb_width(fb), rvvm_fb_height(fb));
        case RVVM_RGB_XBGR8888:
            return SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_RGBX32, //
                                     SDL_TEXTUREACCESS_STREAMING, rvvm_fb_width(fb), rvvm_fb_height(fb));
        case RVVM_RGB_BGRX8888:
            return SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_XRGB32, //
                                     SDL_TEXTUREACCESS_STREAMING, rvvm_fb_width(fb), rvvm_fb_height(fb));
        case RVVM_RGB_RGBX8888:
            return SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_XBGR32, //
                                     SDL_TEXTUREACCESS_STREAMING, rvvm_fb_width(fb), rvvm_fb_height(fb));
        case RVVM_RGB_XRGB2101010:
            return SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_XRGB2101010, //
                                     SDL_TEXTUREACCESS_STREAMING, rvvm_fb_width(fb), rvvm_fb_height(fb));
        case RVVM_RGB_XBGR2101010:
            return SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_XBGR2101010, //
                                     SDL_TEXTUREACCESS_STREAMING, rvvm_fb_width(fb), rvvm_fb_height(fb));
    }
#else
    UNUSED(sdl);
    switch (rvvm_fb_format(fb)) {
        case RVVM_RGB_XRGB1555:
            return SDL_CreateRGBSurfaceFrom(rvvm_fb_buffer(fb), rvvm_fb_width(fb), rvvm_fb_height(fb), //
                                            15, rvvm_fb_stride(fb), 0x7C00U, 0x3E0U, 0x1FU, 0);
        case RVVM_RGB_RGB565:
            return SDL_CreateRGBSurfaceFrom(rvvm_fb_buffer(fb), rvvm_fb_width(fb), rvvm_fb_height(fb), //
                                            16, rvvm_fb_stride(fb), 0xF800U, 0x7E0U, 0x1FU, 0);
        case RVVM_RGB_RGB888:
            return SDL_CreateRGBSurfaceFrom(rvvm_fb_buffer(fb), rvvm_fb_width(fb), rvvm_fb_height(fb), //
                                            24, rvvm_fb_stride(fb), 0xFF0000U, 0xFF00U, 0xFFU, 0);
        case RVVM_RGB_XRGB8888:
            return SDL_CreateRGBSurfaceFrom(rvvm_fb_buffer(fb), rvvm_fb_width(fb), rvvm_fb_height(fb), //
                                            32, rvvm_fb_stride(fb), 0xFF0000U, 0xFF00U, 0xFFU, 0);
        case RVVM_RGB_BGR888:
            return SDL_CreateRGBSurfaceFrom(rvvm_fb_buffer(fb), rvvm_fb_width(fb), rvvm_fb_height(fb), //
                                            24, rvvm_fb_stride(fb), 0xFFU, 0xFF00U, 0xFF0000U, 0);
        case RVVM_RGB_XBGR8888:
            return SDL_CreateRGBSurfaceFrom(rvvm_fb_buffer(fb), rvvm_fb_width(fb), rvvm_fb_height(fb), //
                                            32, rvvm_fb_stride(fb), 0xFFU, 0xFF00U, 0xFF0000U, 0);
        case RVVM_RGB_BGRX8888:
            return SDL_CreateRGBSurfaceFrom(rvvm_fb_buffer(fb), rvvm_fb_width(fb), rvvm_fb_height(fb), //
                                            32, rvvm_fb_stride(fb), 0xFF00U, 0xFF0000U, 0xFF000000U, 0);
        case RVVM_RGB_RGBX8888:
            return SDL_CreateRGBSurfaceFrom(rvvm_fb_buffer(fb), rvvm_fb_width(fb), rvvm_fb_height(fb), //
                                            32, rvvm_fb_stride(fb), 0xFF000000U, 0xFF0000U, 0xFF00U, 0);
        case RVVM_RGB_XRGB2101010:
            return SDL_CreateRGBSurfaceFrom(rvvm_fb_buffer(fb), rvvm_fb_width(fb), rvvm_fb_height(fb), //
                                            32, rvvm_fb_stride(fb), 0x3FF00000U, 0xFFC00U, 0x3FFU, 0);
        case RVVM_RGB_XBGR2101010:
            return SDL_CreateRGBSurfaceFrom(rvvm_fb_buffer(fb), rvvm_fb_width(fb), rvvm_fb_height(fb), //
                                            32, rvvm_fb_stride(fb), 0x3FFU, 0xFFC00U, 0x3FF00000U, 0);
    }
#endif
    return NULL;
}

/*
 * SDL Window handling
 */

static void sdl_window_free(gui_window_t* win)
{
    sdl_window_t* sdl = gui_backend_get_data(win);
    gui_backend_set_data(win, NULL);
    if (sdl_thread_ok && sdl) {
        // Remove from window list
        vector_foreach_back (sdl_windows, i) {
            if (vector_at(sdl_windows, i) == win) {
                vector_erase(sdl_windows, i);
                break;
            }
        }
        // Free SDL objects
#if USE_SDL >= 2
        if (sdl->texture) {
            SDL_DestroyTexture(sdl->texture);
        }
        if (sdl->renderer) {
            SDL_DestroyRenderer(sdl->renderer);
        }
        if (sdl->window) {
            SDL_DestroyWindow(sdl->window);
        }
#else
        if (sdl->texture) {
            SDL_FreeSurface(sdl->texture);
        }
#endif
        if (!vector_size(sdl_windows)) {
            // Bye, SDL!
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            sdl_thread_ok = false;
        }
        // Free VRAM VMA
        vma_free(gui_backend_get_vram(win), gui_backend_get_vram_size(win));
        safe_free(sdl);
    } else if (sdl) {
        rvvm_warn("sdl_window_free() called from non-GUI thread!");
    }
}

static void sdl_window_poll(gui_window_t* win)
{
    SDL_Event event = {0};
    UNUSED(win);

    while (sdl_thread_ok && SDL_PollEvent(&event)) {
        switch (event.type) {
#if USE_SDL == 3
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                sdl_handle_keyboard(&event, event.type == SDL_EVENT_KEY_DOWN);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                sdl_handle_mouse_moution(&event);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                sdl_handle_mouse_scroll(&event);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                sdl_handle_mouse_btn(&event, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                sdl_handle_window(&event, event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED);
                break;
            case SDL_EVENT_WINDOW_RESIZED:
                sdl_handle_window_resize(&event);
                break;
#else
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                sdl_handle_keyboard(&event, event.type == SDL_KEYDOWN);
                break;
            case SDL_MOUSEMOTION:
                sdl_handle_mouse_moution(&event);
                break;
#if USE_SDL == 2
            case SDL_MOUSEWHEEL:
                sdl_handle_mouse_scroll(&event);
                break;
#endif
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                sdl_handle_mouse_btn(&event, event.type == SDL_MOUSEBUTTONDOWN);
                break;
#if USE_SDL == 2
            case SDL_WINDOWEVENT:
                switch (event.window.event) {
                    case SDL_WINDOWEVENT_FOCUS_LOST:
                    case SDL_WINDOWEVENT_CLOSE:
                        sdl_handle_window(&event, event.window.event == SDL_WINDOWEVENT_CLOSE);
                        break;
                    case SDL_WINDOWEVENT_RESIZED:
                        sdl_handle_window_resize(&event);
                        break;
                }
                break;
#else
            case SDL_ACTIVEEVENT:
                if (event.active.state == SDL_APPINPUTFOCUS && !event.active.gain) {
                    sdl_handle_window(&event, false);
                }
                break;
            case SDL_QUIT:
                sdl_handle_window(&event, true);
                break;
#endif
#endif
        }
    }
}

static void sdl_window_draw(gui_window_t* win, const rvvm_fb_t* fb, uint32_t x, uint32_t y)
{
    SDL_Rect dst = {
        .x = x,
        .y = y,
        .w = rvvm_fb_width(fb),
        .h = rvvm_fb_height(fb),
    };
    sdl_window_t* sdl = gui_backend_get_data(win);
    if (sdl_thread_ok && sdl->window) {
        // Recreate texture if needed
#if USE_SDL >= 2
        if (!rvvm_fb_same_layout(fb, &sdl->fb)) {
            SDL_Texture* new_tex = sdl_create_texture(sdl, fb);
            if (new_tex) {
                if (sdl->texture) {
                    SDL_DestroyTexture(sdl->texture);
                }
                sdl->texture = new_tex;
            }
        }
#else
        if (!rvvm_fb_same_scanout(fb, &sdl->fb)) {
            SDL_Surface* new_tex = sdl_create_texture(sdl, fb);
            if (new_tex) {
                if (sdl->texture) {
                    SDL_FreeSurface(sdl->texture);
                }
                sdl->texture = new_tex;
            }
        }
#endif
        // Render to window
#if USE_SDL >= 2
        SDL_RenderClear(sdl->renderer);
        SDL_UpdateTexture(sdl->texture, NULL, rvvm_fb_buffer(fb), rvvm_fb_stride(fb));
        SDL_RenderCopy(sdl->renderer, sdl->texture, NULL, &dst);
        SDL_RenderPresent(sdl->renderer);
#else
        SDL_BlitSurface(sdl->texture, NULL, sdl->window, &dst);
        SDL_Flip(sdl->window);
#endif
        sdl->fb = *fb;
    }
}

static void sdl_window_set_title(gui_window_t* win, const char* title)
{
    sdl_window_t* sdl = gui_backend_get_data(win);
    if (sdl_thread_ok && sdl->window) {
#if USE_SDL >= 2
        SDL_SetWindowTitle(sdl->window, title);
#else
        SDL_WM_SetCaption(title, NULL);
#endif
    }
}

static void sdl_window_grab_input(gui_window_t* win, bool grab)
{
    sdl_window_t* sdl = gui_backend_get_data(win);
    if (sdl_thread_ok && sdl->grab != grab && sdl->window) {
        sdl->grab = grab;
#if USE_SDL == 3
        SDL_SetWindowMouseGrab(sdl->window, grab);
        SDL_SetWindowKeyboardGrab(sdl->window, grab);
        SDL_SetWindowRelativeMouseMode(sdl->window, grab);
#elif USE_SDL == 2
        SDL_SetWindowGrab(sdl->window, grab);
        SDL_SetRelativeMouseMode(grab);
#else
        SDL_WM_GrabInput(grab ? SDL_GRAB_ON : SDL_GRAB_OFF);
#endif
    }
}

static void sdl_window_hide_cursor(gui_window_t* win, bool hide)
{
    sdl_window_t* sdl = gui_backend_get_data(win);
    if (sdl_thread_ok && sdl->window) {
#if USE_SDL == 3
        if (hide) {
            SDL_HideCursor();
        } else {
            SDL_ShowCursor();
        }
#else
        SDL_ShowCursor(hide ? SDL_DISABLE : SDL_ENABLE);
#endif
    }
}

static void sdl_window_set_win_size(gui_window_t* win, uint32_t w, uint32_t h)
{
    sdl_window_t* sdl = gui_backend_get_data(win);
    if (sdl_thread_ok && sdl->window) {
#if USE_SDL >= 2
        SDL_SetWindowSize(sdl->window, w, h);
#else
        SDL_Surface* new_win = SDL_SetVideoMode(w, h, 0, SDL_SWSURFACE | SDL_ANYFORMAT);
        if (new_win) {
            sdl->window = new_win;
        }
#endif
    }
}

#if USE_SDL >= 2

static void sdl_window_set_min_size(gui_window_t* win, uint32_t w, uint32_t h)
{
    sdl_window_t* sdl = gui_backend_get_data(win);
    if (sdl_thread_ok && sdl->window) {
        SDL_SetWindowMinimumSize(sdl->window, w, h);
    }
}

static void sdl_window_get_position(gui_window_t* win, int32_t* x, int32_t* y)
{
    sdl_window_t* sdl = gui_backend_get_data(win);
    if (sdl_thread_ok && sdl->window) {
        SDL_GetWindowPosition(sdl->window, x, y);
    }
}

static void sdl_window_set_position(gui_window_t* win, int32_t x, int32_t y)
{
    sdl_window_t* sdl = gui_backend_get_data(win);
    if (sdl_thread_ok && sdl->window) {
        SDL_SetWindowPosition(sdl->window, x, y);
    }
}

static void sdl_window_get_scr_size(gui_window_t* win, uint32_t* w, uint32_t* h)
{
    sdl_window_t* sdl = gui_backend_get_data(win);
    if (sdl_thread_ok && sdl->window) {
#if USE_SDL == 3
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(1);
        if (mode) {
            *w = mode->w;
            *h = mode->h;
        }
#else
        SDL_DisplayMode mode = ZERO_INIT;
        if (!SDL_GetCurrentDisplayMode(0, &mode)) {
            *w = mode.w;
            *h = mode.h;
        }
#endif
    }
}

#endif

/*
 * Dynamic probe of libSDL at runtime
 */

#if defined(USE_LIBS_PROBE)

#define SDL_DLIB_RESOLVE(lib, sym)                                                                                     \
    do {                                                                                                               \
        sym          = dlib_resolve(lib, #sym);                                                                        \
        libsdl_avail = libsdl_avail && !!sym;                                                                          \
    } while (0)

static bool sdl_init_libs(void)
{
    bool libsdl_avail = true;

    dlib_ctx_t* libsdl = dlib_open(SDL_LIB_NAME, DLIB_NAME_PROBE);

    SDL_DLIB_RESOLVE(libsdl, SDL_Init);
    SDL_DLIB_RESOLVE(libsdl, SDL_PollEvent);
    SDL_DLIB_RESOLVE(libsdl, SDL_QuitSubSystem);
    SDL_DLIB_RESOLVE(libsdl, SDL_ShowCursor);

#if USE_SDL >= 2
    SDL_DLIB_RESOLVE(libsdl, SDL_CreateRenderer);
    SDL_DLIB_RESOLVE(libsdl, SDL_CreateTexture);
    SDL_DLIB_RESOLVE(libsdl, SDL_CreateWindow);
    SDL_DLIB_RESOLVE(libsdl, SDL_DestroyRenderer);
    SDL_DLIB_RESOLVE(libsdl, SDL_DestroyTexture);
    SDL_DLIB_RESOLVE(libsdl, SDL_DestroyWindow);
    SDL_DLIB_RESOLVE(libsdl, SDL_GetCurrentDisplayMode);
    SDL_DLIB_RESOLVE(libsdl, SDL_GetCurrentVideoDriver);
    SDL_DLIB_RESOLVE(libsdl, SDL_GetWindowID);
    SDL_DLIB_RESOLVE(libsdl, SDL_GetWindowPosition);
    SDL_DLIB_RESOLVE(libsdl, SDL_RenderClear);
    SDL_DLIB_RESOLVE(libsdl, SDL_RenderPresent);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetHint);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetWindowMinimumSize);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetWindowPosition);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetWindowSize);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetWindowTitle);
    SDL_DLIB_RESOLVE(libsdl, SDL_UpdateTexture);
#endif

#if USE_SDL == 1
    SDL_DLIB_RESOLVE(libsdl, SDL_CreateRGBSurfaceFrom);
    SDL_DLIB_RESOLVE(libsdl, SDL_Flip);
    SDL_DLIB_RESOLVE(libsdl, SDL_FreeSurface);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetVideoMode);
    SDL_DLIB_RESOLVE(libsdl, SDL_UpperBlit);
    SDL_DLIB_RESOLVE(libsdl, SDL_WM_GrabInput);
    SDL_DLIB_RESOLVE(libsdl, SDL_WM_SetCaption);
#elif USE_SDL == 2
    SDL_DLIB_RESOLVE(libsdl, SDL_RenderCopy);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetRelativeMouseMode);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetWindowGrab);
#elif USE_SDL == 3
    SDL_DLIB_RESOLVE(libsdl, SDL_HideCursor);
    SDL_DLIB_RESOLVE(libsdl, SDL_RenderTexture);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetWindowKeyboardGrab);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetWindowMouseGrab);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetWindowRelativeMouseMode);
#endif

    dlib_close(libsdl);
    return libsdl_avail;
}

#endif

static bool sdl_global_init(void)
{
#if defined(USE_LIBS_PROBE)
    static bool sdl_avail = false;
    DO_ONCE_SCOPED {
        // Load libSDL
        sdl_avail = sdl_init_libs();
        if (!sdl_avail) {
            rvvm_error("Failed to load lib" SDL_LIB_NAME ", check your installation");
        }
    }
    if (!sdl_avail) {
        return false;
    }
#endif
    // Perform SDL Video subsystem init
    if (!vector_size(sdl_windows)) {
#if defined(HOST_TARGET_LINUX)
        // Force X11 over Wayland
        // SDL Wayland backend doesn't support software rendering,
        // and Nvidia driver isn't compatible with isolation (See #178)
        setenv("SDL_VIDEODRIVER", "x11,wayland,kmsdrm,directfb,fbcon", true);
        setenv("SDL_VIDEO_DRIVER", "x11,wayland,kmsdrm,directfb,fbcon", true);
#endif
        if (SDL_Init(SDL_INIT_VIDEO) != SDL_INIT_SUCCESS) {
            rvvm_error("Failed to initialize " SDL_LIB_NAME);
            return false;
        }
        // Mark that this thread initialized video subsystem
        sdl_thread_ok = true;
        // Set SDL hints
#if USE_SDL >= 2 && defined(SDL_HINT_GRAB_KEYBOARD)
        SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
#endif
#if USE_SDL >= 2 && defined(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR)
        if (rvvm_strcmp(SDL_GetCurrentVideoDriver(), "x11")) {
            // Prevent messing with the compositor
            SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#if defined(SDL_HINT_FRAMEBUFFER_ACCELERATION) && defined(SDL_HINT_RENDER_DRIVER)
            // Force software rendering, because it's actually faster in our case,
            // and because Nvidia driver isn't compatible with isolation (See #178)
            SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
#endif
        }
#endif
#if USE_SDL == 1
    } else {
        rvvm_error("SDL1 supports only a single window");
        return false;
#endif
    }
    return sdl_thread_ok;
}

static const gui_backend_cb_t sdl_window_cb = {
    .free         = sdl_window_free,
    .poll         = sdl_window_poll,
    .draw         = sdl_window_draw,
    .set_title    = sdl_window_set_title,
    .grab_input   = sdl_window_grab_input,
    .hide_cursor  = sdl_window_hide_cursor,
    .set_win_size = sdl_window_set_win_size,
#if USE_SDL >= 2
    .set_min_size = sdl_window_set_min_size,
    .get_position = sdl_window_get_position,
    .set_position = sdl_window_set_position,
    .get_scr_size = sdl_window_get_scr_size,
#endif
};

bool sdl_window_init(gui_window_t* win)
{
    // Register SDL backend
    sdl_window_t* sdl = safe_new_obj(sdl_window_t);

    gui_backend_register(win, &sdl_window_cb);
    gui_backend_set_data(win, sdl);

    if (!sdl_global_init()) {
        return false;
    }

    // Create SDL window
#if USE_SDL == 3
    sdl->window = SDL_CreateWindow("", gui_window_width(win), gui_window_height(win), //
                                   SDL_WINDOW_RESIZABLE);
#elif USE_SDL == 2
    sdl->window = SDL_CreateWindow("", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, //
                                   gui_window_width(win), gui_window_height(win),      //
                                   SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
#else
    sdl->window = SDL_SetVideoMode(gui_window_width(win), gui_window_height(win), 0, //
                                   SDL_SWSURFACE | SDL_ANYFORMAT);
#endif
    if (!sdl->window) {
        rvvm_error("Failed to create " SDL_LIB_NAME " Window");
        return false;
    }

#if USE_SDL >= 2
    // Obtain window ID for handling multi-window input events
    sdl->id = SDL_GetWindowID(sdl->window);
#endif

#if USE_SDL >= 2
#if USE_SDL == 3
    // Create SDL renderer
    sdl->renderer = SDL_CreateRenderer(sdl->window, NULL);
#elif USE_SDL == 2
    sdl->renderer = SDL_CreateRenderer(sdl->window, -1, SDL_RENDERER_SOFTWARE);
    if (!sdl->renderer) {
        sdl->renderer = SDL_CreateRenderer(sdl->window, -1, 0);
    }
#endif
    if (!sdl->renderer) {
        rvvm_error("Failed to create " SDL_LIB_NAME " Renderer");
        return false;
    }
#endif

    // Allocate VRAM (Probably better done in GUI middleware later)
    size_t vram_size = gui_backend_get_vram_size(win);
    void*  vram      = vma_alloc(NULL, vram_size, VMA_RDWR);
    if (vram) {
        gui_backend_set_vram(win, vram, vram_size);
    } else {
        rvvm_error("vma_alloc() failed!");
        return false;
    }

    sdl_window_hide_cursor(win, true);

    vector_push_back(sdl_windows, win);

    return true;
}

#else

bool sdl_window_init(gui_window_t* win)
{
    UNUSED(win);
    return false;
}

#endif

POP_OPTIMIZATION_SIZE
