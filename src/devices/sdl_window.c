/*
sdl_window.c - SDL1 / SDL2 / SDL3 RVVM Window
Copyright (C) 2022  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "feature_test.h"

#include "dlib.h"
#include "gui_window.h"
#include "utils.h"
#include "vector.h"
#include "vma_ops.h"

PUSH_OPTIMIZATION_SIZE

#if !defined(USE_FULL_LINKING) /**/                                                                                    \
    && (defined(COMPILER_IS_MSVC) || defined(HOST_TARGET_EMSCRIPTEN) || defined(HOST_TARGET_REDOX))
// MSVC compiler, Emscripten and Redox OS can't handle dynamic library loading
#define USE_FULL_LINKING 1
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
#warning Disabling USE_SDL as <SDL.h> is unavailable
#undef USE_SDL

#endif

/*
 * SDL1 / SDL2 / SDL3 support glue
 */

#if defined(USE_SDL)

// Resolve symbols at runtime
#define SDL_DLIB_SYM(sym) static __typeof__(sym)* MACRO_CONCAT(sym, _dlib) = NULL;

#if USE_SDL == 1

#define SDL_LIB_NAME "SDL"

#if !defined(USE_FULL_LINKING)

SDL_DLIB_SYM(SDL_SetVideoMode)
SDL_DLIB_SYM(SDL_CreateRGBSurfaceFrom)
SDL_DLIB_SYM(SDL_UpperBlit)
SDL_DLIB_SYM(SDL_Flip)
SDL_DLIB_SYM(SDL_WM_GrabInput)
SDL_DLIB_SYM(SDL_WM_SetCaption)
SDL_DLIB_SYM(SDL_FreeSurface)

#define SDL_SetVideoMode         SDL_SetVideoMode_dlib
#define SDL_CreateRGBSurfaceFrom SDL_CreateRGBSurfaceFrom_dlib
#define SDL_UpperBlit            SDL_UpperBlit_dlib
#define SDL_Flip                 SDL_Flip_dlib
#define SDL_WM_GrabInput         SDL_WM_GrabInput_dlib
#define SDL_WM_SetCaption        SDL_WM_SetCaption_dlib
#define SDL_FreeSurface          SDL_FreeSurface_dlib

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

#else

#if USE_SDL == 3

#define SDL_LIB_NAME "SDL3"

#if !defined(USE_FULL_LINKING)

SDL_DLIB_SYM(SDL_HideCursor)
SDL_DLIB_SYM(SDL_RenderTexture)
SDL_DLIB_SYM(SDL_SetWindowRelativeMouseMode)
SDL_DLIB_SYM(SDL_SetWindowMouseGrab)
SDL_DLIB_SYM(SDL_SetWindowKeyboardGrab)

#define SDL_HideCursor                 SDL_HideCursor_dlib
#define SDL_RenderTexture              SDL_RenderTexture_dlib
#define SDL_SetWindowRelativeMouseMode SDL_SetWindowRelativeMouseMode_dlib
#define SDL_SetWindowMouseGrab         SDL_SetWindowMouseGrab_dlib
#define SDL_SetWindowKeyboardGrab      SDL_SetWindowKeyboardGrab_dlib

#endif

#elif USE_SDL == 2

#define SDL_LIB_NAME "SDL2"

#if SDL_MAJOR_VERSION == 2 && SDL_MINOR_VERSION < 14
// Support pre SDL 2.14
#define SDL_PIXELFORMAT_XRGB8888 SDL_PIXELFORMAT_RGB888
#define SDL_PIXELFORMAT_XBGR8888 SDL_PIXELFORMAT_BGR888
#endif

#if !defined(USE_FULL_LINKING)

SDL_DLIB_SYM(SDL_RenderCopy)
SDL_DLIB_SYM(SDL_SetWindowGrab)
SDL_DLIB_SYM(SDL_SetRelativeMouseMode)

#define SDL_RenderCopy           SDL_RenderCopy_dlib
#define SDL_SetWindowGrab        SDL_SetWindowGrab_dlib
#define SDL_SetRelativeMouseMode SDL_SetRelativeMouseMode_dlib

#endif

#endif

#if !defined(USE_FULL_LINKING)

SDL_DLIB_SYM(SDL_GetCurrentVideoDriver)
SDL_DLIB_SYM(SDL_SetHint)
SDL_DLIB_SYM(SDL_CreateWindow)
SDL_DLIB_SYM(SDL_CreateRenderer)
SDL_DLIB_SYM(SDL_CreateTexture)
SDL_DLIB_SYM(SDL_UpdateTexture)
SDL_DLIB_SYM(SDL_RenderPresent)
SDL_DLIB_SYM(SDL_GetWindowID)
SDL_DLIB_SYM(SDL_SetWindowSize)
SDL_DLIB_SYM(SDL_SetWindowTitle)
SDL_DLIB_SYM(SDL_DestroyTexture)
SDL_DLIB_SYM(SDL_DestroyRenderer)
SDL_DLIB_SYM(SDL_DestroyWindow)

#define SDL_GetCurrentVideoDriver SDL_GetCurrentVideoDriver_dlib
#define SDL_SetHint               SDL_SetHint_dlib
#define SDL_CreateWindow          SDL_CreateWindow_dlib
#define SDL_CreateRenderer        SDL_CreateRenderer_dlib
#define SDL_CreateTexture         SDL_CreateTexture_dlib
#define SDL_UpdateTexture         SDL_UpdateTexture_dlib
#define SDL_RenderPresent         SDL_RenderPresent_dlib
#define SDL_GetWindowID           SDL_GetWindowID_dlib
#define SDL_SetWindowSize         SDL_SetWindowSize_dlib
#define SDL_SetWindowTitle        SDL_SetWindowTitle_dlib
#define SDL_DestroyTexture        SDL_DestroyTexture_dlib
#define SDL_DestroyRenderer       SDL_DestroyRenderer_dlib
#define SDL_DestroyWindow         SDL_DestroyWindow_dlib

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

#if !defined(USE_FULL_LINKING)

SDL_DLIB_SYM(SDL_Init)
SDL_DLIB_SYM(SDL_ShowCursor)
SDL_DLIB_SYM(SDL_PollEvent)

#define SDL_Init       SDL_Init_dlib
#define SDL_ShowCursor SDL_ShowCursor_dlib
#define SDL_PollEvent  SDL_PollEvent_dlib

#endif

/*
 * SDL backend implementation
 */

typedef struct {
#if USE_SDL >= 2
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;
#else
    SDL_Surface* window;
    SDL_Surface* texture;
#endif
    uint32_t id;
    bool     grab;
} sdl_window_t;

static vector_t(gui_window_t*) sdl_windows = {0};

static hid_key_t sdl_key_to_hid(uint32_t sdl_key)
{
    if (sdl_key < sizeof(sdl_key_to_hid_byte_map)) {
        return sdl_key_to_hid_byte_map[sdl_key];
    }
    rvvm_warn("Unmapped " SDL_LIB_NAME " keycode %x", sdl_key);
    return HID_KEY_NONE;
}

static gui_window_t* sdl_find_window(uint32_t window_id)
{
    vector_foreach (sdl_windows, i) {
        gui_window_t* win = vector_at(sdl_windows, i);
        sdl_window_t* sdl = win->win_data;
        if (sdl->id == window_id) {
            return win;
        }
    }
    DO_ONCE(rvvm_warn("Invalid " SDL_LIB_NAME " window in event"));
    return NULL;
}

static void sdl_handle_keyboard(SDL_Event* event, bool press)
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
    if (win) {
        if (win && press && win->on_key_press) {
            win->on_key_press(win, key);
        } else if (win && !press && win->on_key_press) {
            win->on_key_release(win, key);
        }
    }
}

static void sdl_handle_mouse_moution(SDL_Event* event)
{
#if USE_SDL >= 2
    gui_window_t* win = sdl_find_window(event->motion.windowID);
#else
    gui_window_t* win = sdl_find_window(0);
#endif
    if (win && win->on_mouse_move) {
        sdl_window_t* sdl = win->win_data;
        if (sdl->grab) {
            win->on_mouse_move(win, event->motion.xrel, event->motion.yrel);
        } else {
            win->on_mouse_place(win, event->motion.x, event->motion.y);
        }
    }
}

static void sdl_handle_mouse_btn(SDL_Event* event, bool press)
{
#if USE_SDL >= 2
    gui_window_t* win = sdl_find_window(event->button.windowID);
#else
    gui_window_t* win = sdl_find_window(0);
#endif
    if (win) {
        switch (event->button.button) {
            case SDL_BUTTON_LEFT:
                if (press && win->on_mouse_press) {
                    win->on_mouse_press(win, HID_BTN_LEFT);
                } else if (!press && win->on_mouse_release) {
                    win->on_mouse_release(win, HID_BTN_LEFT);
                }
                return;
            case SDL_BUTTON_MIDDLE:
                if (press && win->on_mouse_press) {
                    win->on_mouse_press(win, HID_BTN_MIDDLE);
                } else if (!press && win->on_mouse_release) {
                    win->on_mouse_release(win, HID_BTN_MIDDLE);
                }
                return;
            case SDL_BUTTON_RIGHT:
                if (press && win->on_mouse_press) {
                    win->on_mouse_press(win, HID_BTN_RIGHT);
                } else if (!press && win->on_mouse_release) {
                    win->on_mouse_release(win, HID_BTN_RIGHT);
                }
                return;
#if USE_SDL == 1
            case SDL_BUTTON_WHEELUP:
                if (press && win->on_mouse_scroll) {
                    win->on_mouse_scroll(win, HID_SCROLL_UP);
                }
                return;
            case SDL_BUTTON_WHEELDOWN:
                if (press && win->on_mouse_scroll) {
                    win->on_mouse_scroll(win, HID_SCROLL_DOWN);
                }
                return;
#endif
        }
    }
}

#if USE_SDL >= 2

static void sdl_handle_mouse_scroll(SDL_Event* event)
{
    gui_window_t* win = sdl_find_window(event->wheel.windowID);
    if (win && win->on_mouse_scroll) {
        win->on_mouse_scroll(win, event->wheel.y);
    }
}

#endif

static void sdl_handle_window(SDL_Event* event, bool close)
{
#if USE_SDL >= 2
    gui_window_t* win = sdl_find_window(event->window.windowID);
#else
    gui_window_t* win = sdl_find_window(0);
    UNUSED(event);
#endif
    if (win && !close && win->on_focus_lost) {
        win->on_focus_lost(win);
    } else if (win && close && win->on_close) {
        win->on_close(win);
    }
}

static bool sdl_set_scanout(gui_window_t* win, const fb_ctx_t* fb)
{
    sdl_window_t* sdl = win->win_data;
#if USE_SDL >= 2
    SDL_Texture* texture = NULL;
#else
    SDL_Surface* texture = NULL;
#endif

    if (fb) {
        // Create new texture with requested pixel format
#if USE_SDL >= 2
        switch (fb->format) {
            case RGB_FMT_R5G6B5:
                texture = SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_RGB565, //
                                            SDL_TEXTUREACCESS_STREAMING, fb->width, fb->height);
                break;
            case RGB_FMT_R8G8B8:
                texture = SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_BGR24, //
                                            SDL_TEXTUREACCESS_STREAMING, fb->width, fb->height);
                break;
            case RGB_FMT_A8R8G8B8:
                texture = SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_XRGB8888, //
                                            SDL_TEXTUREACCESS_STREAMING, fb->width, fb->height);
                break;
            case RGB_FMT_A8B8G8R8:
                texture = SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_XBGR8888, //
                                            SDL_TEXTUREACCESS_STREAMING, fb->width, fb->height);
                break;
        }
#else
        switch (fb->format) {
            case RGB_FMT_R5G6B5:
                texture = SDL_CreateRGBSurfaceFrom(fb->buffer, fb->width, fb->height, 16, //
                                                   framebuffer_stride(fb), 0xF800, 0x7E0, 0x1F, 0);
                break;
            case RGB_FMT_R8G8B8:
                texture = SDL_CreateRGBSurfaceFrom(fb->buffer, fb->width, fb->height, 24, //
                                                   framebuffer_stride(fb), 0xFF0000, 0xFF00, 0xFF, 0);
                break;
            case RGB_FMT_A8R8G8B8:
                texture = SDL_CreateRGBSurfaceFrom(fb->buffer, fb->width, fb->height, 32, //
                                                   framebuffer_stride(fb), 0xFF0000, 0xFF00, 0xFF, 0);
                break;
            case RGB_FMT_A8B8G8R8:
                texture = SDL_CreateRGBSurfaceFrom(fb->buffer, fb->width, fb->height, 32, //
                                                   framebuffer_stride(fb), 0xFF, 0xFF00, 0xFF0000, 0);
                break;
        }
#endif
    }

    if (texture || !fb) {
        if (sdl->texture) {
            // Destroy previous texture
#if USE_SDL >= 2
            SDL_DestroyTexture(sdl->texture);
#else
            SDL_FreeSurface(sdl->texture);
#endif
        }
        sdl->texture = texture;
        if (texture && fb) {
            // Update window size or recreate it
#if USE_SDL >= 2
            if (sdl->window && (fb->width != win->fb.width || fb->height != win->fb.height)) {
                SDL_SetWindowSize(sdl->window, fb->width, fb->height);
            }
#else
            if (!sdl->window || fb->width != win->fb.width || fb->height != win->fb.height) {
                sdl->window = SDL_SetVideoMode(fb->width, fb->height, rgb_format_bpp(fb->format), SDL_ANYFORMAT);
            }
#endif
            win->fb = *fb;
        }
    }

    return sdl->window && texture;
}

static void sdl_window_draw(gui_window_t* win)
{
    sdl_window_t* sdl = win->win_data;
    if (sdl && sdl->window && sdl->texture) {
#if USE_SDL == 3
        SDL_UpdateTexture(sdl->texture, NULL, win->fb.buffer, framebuffer_stride(&win->fb));
        SDL_RenderTexture(sdl->renderer, sdl->texture, NULL, NULL);
        SDL_RenderPresent(sdl->renderer);
#elif USE_SDL == 2
        SDL_UpdateTexture(sdl->texture, NULL, win->fb.buffer, framebuffer_stride(&win->fb));
        SDL_RenderCopy(sdl->renderer, sdl->texture, NULL, NULL);
        SDL_RenderPresent(sdl->renderer);
#else
        SDL_BlitSurface(sdl->texture, NULL, sdl->window, NULL);
        SDL_Flip(sdl->window);
#endif
    }
}

static void sdl_window_poll(gui_window_t* win)
{
    SDL_Event event = {0};
    UNUSED(win);

    while (SDL_PollEvent(&event)) {
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

static void sdl_window_grab_input(gui_window_t* win, bool grab)
{
    sdl_window_t* sdl = win->win_data;
    if (sdl->grab != grab && sdl->window) {
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

static void sdl_window_set_title(gui_window_t* win, const char* title)
{
    sdl_window_t* sdl = win->win_data;
    if (sdl->window) {
#if USE_SDL >= 2
        SDL_SetWindowTitle(sdl->window, title);
#else
        SDL_WM_SetCaption(title, NULL);
#endif
    }
}

static void sdl_window_remove(gui_window_t* win)
{
    sdl_window_t* sdl = win->win_data;
    if (sdl) {
        win->win_data = NULL;
        // Free texture
        sdl_set_scanout(win, NULL);
        // Free framebuffer VMA
        vma_free(win->fb.buffer, framebuffer_size(&win->fb));
        // Free renderer/window context
#if USE_SDL >= 2
        if (sdl->renderer) {
            SDL_DestroyRenderer(sdl->renderer);
        }
        if (sdl->window) {
            SDL_DestroyWindow(sdl->window);
        }
#else
        if (sdl->window) {
            SDL_FreeSurface(sdl->window);
        }
#endif
        vector_foreach_back (sdl_windows, i) {
            if (vector_at(sdl_windows, i) == win) {
                vector_erase(sdl_windows, i);
                break;
            }
        }
        free(sdl);
    }
}

#define SDL_DLIB_RESOLVE(lib, sym)                                                                                     \
    do {                                                                                                               \
        sym          = dlib_resolve(lib, #sym);                                                                        \
        libsdl_avail = libsdl_avail && !!sym;                                                                          \
    } while (0)

static bool sdl_init(void)
{
    bool libsdl_avail = true;
#if !defined(USE_FULL_LINKING)
    dlib_ctx_t* libsdl = dlib_open(SDL_LIB_NAME, DLIB_NAME_PROBE);

#if USE_SDL >= 2
    SDL_DLIB_RESOLVE(libsdl, SDL_GetCurrentVideoDriver);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetHint);
    SDL_DLIB_RESOLVE(libsdl, SDL_UpdateTexture);
    SDL_DLIB_RESOLVE(libsdl, SDL_RenderPresent);
    SDL_DLIB_RESOLVE(libsdl, SDL_GetWindowID);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetWindowSize);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetWindowTitle);
    SDL_DLIB_RESOLVE(libsdl, SDL_DestroyTexture);
    SDL_DLIB_RESOLVE(libsdl, SDL_DestroyRenderer);
    SDL_DLIB_RESOLVE(libsdl, SDL_DestroyWindow);
    SDL_DLIB_RESOLVE(libsdl, SDL_CreateWindow);
    SDL_DLIB_RESOLVE(libsdl, SDL_CreateRenderer);
    SDL_DLIB_RESOLVE(libsdl, SDL_CreateTexture);
#endif

#if USE_SDL == 3
    SDL_DLIB_RESOLVE(libsdl, SDL_HideCursor);
    SDL_DLIB_RESOLVE(libsdl, SDL_RenderTexture);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetWindowMouseGrab);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetWindowKeyboardGrab);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetWindowRelativeMouseMode);
#endif

#if USE_SDL == 2
    SDL_DLIB_RESOLVE(libsdl, SDL_RenderCopy);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetWindowGrab);
    SDL_DLIB_RESOLVE(libsdl, SDL_SetRelativeMouseMode);
#endif

#if USE_SDL == 1
    SDL_DLIB_RESOLVE(libsdl, SDL_SetVideoMode);
    SDL_DLIB_RESOLVE(libsdl, SDL_CreateRGBSurfaceFrom);
    SDL_DLIB_RESOLVE(libsdl, SDL_UpperBlit);
    SDL_DLIB_RESOLVE(libsdl, SDL_Flip);
    SDL_DLIB_RESOLVE(libsdl, SDL_WM_GrabInput);
    SDL_DLIB_RESOLVE(libsdl, SDL_WM_SetCaption);
    SDL_DLIB_RESOLVE(libsdl, SDL_FreeSurface);
#endif

    SDL_DLIB_RESOLVE(libsdl, SDL_Init);
    SDL_DLIB_RESOLVE(libsdl, SDL_ShowCursor);
    SDL_DLIB_RESOLVE(libsdl, SDL_PollEvent);

    dlib_close(libsdl);
#endif

    if (!libsdl_avail) {
        rvvm_error("Failed to load lib" SDL_LIB_NAME ", check your installation");
        return false;
    }

#if defined(HOST_TARGET_LINUX)
    // Force X11 over Wayland
    // SDL Wayland backend doesn't support software rendering,
    // and Nvidia driver isn't compatible with isolation (See #178)
    setenv("SDL_VIDEODRIVER", "x11,wayland,kmsdrm,directfb,fbcon", true);
    setenv("SDL_VIDEO_DRIVER", "x11,wayland,kmsdrm,directfb,fbcon", true);
#endif

#if USE_SDL == 3
    if (!SDL_Init(SDL_INIT_VIDEO)) {
#else
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
#endif
        rvvm_error("Failed to initialize " SDL_LIB_NAME);
        return false;
    }

#if USE_SDL >= 2 && defined(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR)
    if (rvvm_strcmp(SDL_GetCurrentVideoDriver(), "x11")) {
        // Prevent messing with the compositor
        SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
        // Force software rendering, because it's actually faster in our case,
        // and because Nvidia driver isn't compatible with isolation (See #178)
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    }
#endif

#if USE_SDL >= 2 && defined(SDL_HINT_GRAB_KEYBOARD)
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
#endif

    return true;
}

bool sdl_window_init(gui_window_t* win)
{
    static bool sdl_avail = false;
    DO_ONCE(sdl_avail = sdl_init());
    if (!sdl_avail) {
        return false;
    }

#if USE_SDL == 1
    if (vector_size(sdl_windows)) {
        rvvm_error("SDL1 supports only a single window");
        return false;
    }
#endif

    sdl_window_t* sdl = safe_new_obj(sdl_window_t);

    win->win_data   = sdl;
    win->draw       = sdl_window_draw;
    win->poll       = sdl_window_poll;
    win->remove     = sdl_window_remove;
    win->set_title  = sdl_window_set_title;
    win->grab_input = sdl_window_grab_input;

#if USE_SDL == 3
    sdl->window = SDL_CreateWindow("", win->fb.width, win->fb.height, 0);
#elif USE_SDL == 2
    sdl->window = SDL_CreateWindow("", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, //
                                   win->fb.width, win->fb.height, SDL_WINDOW_SHOWN);
#endif

#if USE_SDL >= 2
    if (!sdl->window) {
        rvvm_error("SDL_CreateWindow() failed!");
        return false;
    }
    sdl->id = SDL_GetWindowID(sdl->window);
#endif

#if USE_SDL == 3
    sdl->renderer = SDL_CreateRenderer(sdl->window, NULL);
#elif USE_SDL == 2
    sdl->renderer = SDL_CreateRenderer(sdl->window, -1, SDL_RENDERER_SOFTWARE);
    if (!sdl->renderer) {
        sdl->renderer = SDL_CreateRenderer(sdl->window, -1, 0);
    }
#endif

#if USE_SDL >= 2
    if (!sdl->renderer) {
        rvvm_error("SDL_CreateRenderer() failed!");
        return false;
    }
#endif

    win->fb.buffer = vma_alloc(NULL, framebuffer_size(&win->fb), VMA_RDWR);
    if (!win->fb.buffer) {
        rvvm_error("vma_alloc() failed!");
        return false;
    }

    if (!sdl_set_scanout(win, &win->fb)) {
        rvvm_error("sdl_set_scanout() failed!");
        return false;
    }

    vector_push_back(sdl_windows, win);

#if USE_SDL == 3
    SDL_HideCursor();
#else
    SDL_ShowCursor(SDL_DISABLE);
#endif

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
