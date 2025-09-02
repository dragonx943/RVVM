/*
win32window.c - Win32 GUI Window
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "feature_test.h"

// Use ANSI for Win9x compat, widechar otherwise
#undef UNICODE
#if !defined(HOST_TARGET_WIN9X)
#define UNICODE
#endif

#include "compiler.h"
#include "gui_window.h"

#if defined(HOST_TARGET_WIN32) || defined(HOST_TARGET_CYGWIN)

#include "utils.h"
#include "vma_ops.h"

#include <windows.h>

PUSH_OPTIMIZATION_SIZE

#define WINDOW_CLASS_NAME TEXT("LEKKIT_FB_WINDOW")

typedef struct {
    HWND hwnd;
    HDC  hdc;
} win32_win_t;

static ATOM window_atom = 0;

static const hid_key_t win32_key_to_hid_byte_map[] = {
    [0x41] = HID_KEY_A,
    [0x42] = HID_KEY_B,
    [0x43] = HID_KEY_C,
    [0x44] = HID_KEY_D,
    [0x45] = HID_KEY_E,
    [0x46] = HID_KEY_F,
    [0x47] = HID_KEY_G,
    [0x48] = HID_KEY_H,
    [0x49] = HID_KEY_I,
    [0x4A] = HID_KEY_J,
    [0x4B] = HID_KEY_K,
    [0x4C] = HID_KEY_L,
    [0x4D] = HID_KEY_M,
    [0x4E] = HID_KEY_N,
    [0x4F] = HID_KEY_O,
    [0x50] = HID_KEY_P,
    [0x51] = HID_KEY_Q,
    [0x52] = HID_KEY_R,
    [0x53] = HID_KEY_S,
    [0x54] = HID_KEY_T,
    [0x55] = HID_KEY_U,
    [0x56] = HID_KEY_V,
    [0x57] = HID_KEY_W,
    [0x58] = HID_KEY_X,
    [0x59] = HID_KEY_Y,
    [0x5A] = HID_KEY_Z,
    [0x30] = HID_KEY_0,
    [0x31] = HID_KEY_1,
    [0x32] = HID_KEY_2,
    [0x33] = HID_KEY_3,
    [0x34] = HID_KEY_4,
    [0x35] = HID_KEY_5,
    [0x36] = HID_KEY_6,
    [0x37] = HID_KEY_7,
    [0x38] = HID_KEY_8,
    [0x39] = HID_KEY_9,
    [0x0D] = HID_KEY_ENTER,
    [0x1B] = HID_KEY_ESC,
    [0x08] = HID_KEY_BACKSPACE,
    [0x09] = HID_KEY_TAB,
    [0x20] = HID_KEY_SPACE,
    [0xBD] = HID_KEY_MINUS,
    [0xBB] = HID_KEY_EQUAL,
    [0xDB] = HID_KEY_LEFTBRACE,
    [0xDD] = HID_KEY_RIGHTBRACE,
    [0xDC] = HID_KEY_BACKSLASH,
    [0xBA] = HID_KEY_SEMICOLON,
    [0xDE] = HID_KEY_APOSTROPHE,
    [0xC0] = HID_KEY_GRAVE,
    [0xBC] = HID_KEY_COMMA,
    [0xBE] = HID_KEY_DOT,
    [0xBF] = HID_KEY_SLASH,
    [0x14] = HID_KEY_CAPSLOCK,
    [0x70] = HID_KEY_F1,
    [0x71] = HID_KEY_F2,
    [0x72] = HID_KEY_F3,
    [0x73] = HID_KEY_F4,
    [0x74] = HID_KEY_F5,
    [0x75] = HID_KEY_F6,
    [0x76] = HID_KEY_F7,
    [0x77] = HID_KEY_F8,
    [0x78] = HID_KEY_F9,
    [0x79] = HID_KEY_F10,
    [0x7A] = HID_KEY_F11,
    [0x7B] = HID_KEY_F12,
    [0x2C] = HID_KEY_SYSRQ,
    [0x91] = HID_KEY_SCROLLLOCK,
    [0x13] = HID_KEY_PAUSE,
    [0x2D] = HID_KEY_INSERT,
    [0x24] = HID_KEY_HOME,
    [0x21] = HID_KEY_PAGEUP,
    [0x2E] = HID_KEY_DELETE,
    [0x23] = HID_KEY_END,
    [0x22] = HID_KEY_PAGEDOWN,
    [0x27] = HID_KEY_RIGHT,
    [0x25] = HID_KEY_LEFT,
    [0x28] = HID_KEY_DOWN,
    [0x26] = HID_KEY_UP,
    [0x90] = HID_KEY_NUMLOCK,
    [0x6F] = HID_KEY_KPSLASH,
    [0x6A] = HID_KEY_KPASTERISK,
    [0x6D] = HID_KEY_KPMINUS,
    [0x6B] = HID_KEY_KPPLUS,
    [0x6C] = HID_KEY_KPENTER,
    [0x61] = HID_KEY_KP1,
    [0x62] = HID_KEY_KP2,
    [0x63] = HID_KEY_KP3,
    [0x64] = HID_KEY_KP4,
    [0x65] = HID_KEY_KP5,
    [0x66] = HID_KEY_KP6,
    [0x67] = HID_KEY_KP7,
    [0x68] = HID_KEY_KP8,
    [0x69] = HID_KEY_KP9,
    [0x60] = HID_KEY_KP0,
    [0x6E] = HID_KEY_KPDOT,
    [0x5D] = HID_KEY_MENU,
    [0xE2] = HID_KEY_RO, // It's HID_KEY_102ND on Nordic keyboards (I have one),
                         // but Windows has no way to distinguish their VK keycodes
    [0xF2] = HID_KEY_KATAKANAHIRAGANA,
    [0x1C] = HID_KEY_HENKAN,
    [0x1D] = HID_KEY_MUHENKAN,
    [0x15] = HID_KEY_HANGEUL, // Actually KANA on Japanese NEC PC-9800
    [0x19] = HID_KEY_HANJA,
    [0x11] = HID_KEY_LEFTCTRL,
    [0x10] = HID_KEY_LEFTSHIFT,
    [0x12] = HID_KEY_LEFTALT,
    [0x5B] = HID_KEY_LEFTMETA,
    [0xA3] = HID_KEY_RIGHTCTRL,
    [0xA1] = HID_KEY_RIGHTSHIFT,
    [0xA5] = HID_KEY_RIGHTALT,
    [0x5C] = HID_KEY_RIGHTMETA,
};

static hid_key_t win32_key_to_hid(uint32_t win32_key)
{
    if (win32_key < sizeof(win32_key_to_hid_byte_map)) {
        return win32_key_to_hid_byte_map[win32_key];
    }
    rvvm_warn("Unmapped Win32 keycode %x", win32_key);
    return HID_KEY_NONE;
}

static LRESULT CALLBACK win32_wndproc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
        case WM_CLOSE:
            PostMessage(hwnd, WM_QUIT, wParam, lParam);
            return 0;
        case WM_KILLFOCUS:
            // This is an ugly fucking way to handle WM_KILLFOCUS via PeekMessage()
            PostMessage(hwnd, WM_KILLFOCUS, wParam, lParam);
            return 0;
    }

    if (uMsg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
        SetCursor(NULL);
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

static void win32_adjust_client_rect(uint32_t* width, uint32_t* height)
{
    RECT rect = {
        .right  = *width,
        .bottom = *height,
    };
    AdjustWindowRectEx(&rect, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, false, 0);
    *width  = rect.right - rect.left;
    *height = rect.bottom - rect.top;
}

static bool win32_window_set_scanout(gui_window_t* win, const fb_ctx_t* fb)
{
    win32_win_t* win32 = win->win_data;
    if (win32 && win32->hwnd) {
        if (fb->format != RGB_FMT_A8R8G8B8 && fb->format != RGB_FMT_R8G8B8 && fb->format != RGB_FMT_R5G6B5) {
            // Invalid format
            return false;
        }
        if (win->fb.width != fb->width || win->fb.height != fb->height) {
            uint32_t flags  = SWP_DRAWFRAME | SWP_NOMOVE | SWP_NOZORDER | SWP_SHOWWINDOW;
            uint32_t width  = fb->width;
            uint32_t height = fb->height;
            win32_adjust_client_rect(&width, &height);
            if (!SetWindowPos(win32->hwnd, NULL, 0, 0, width, height, flags)) {
                // Failed to resize window
                return false;
            }
        }
        win->fb = *fb;
    }
    return true;
}

static void win32_window_remove(gui_window_t* win)
{
    win32_win_t* win32 = win->win_data;
    if (win32) {
        win->win_data = NULL;
        if (win32->hdc) {
            ReleaseDC(win32->hwnd, win32->hdc);
        }
        if (win32->hwnd) {
            DestroyWindow(win32->hwnd);
        }
        safe_free(win32);
    }
}

static void win32_window_draw(gui_window_t* win)
{
    win32_win_t* win32 = win->win_data;
    if (win32 && win32->hdc && win->fb.buffer) {
        BITMAPINFO bmi = {
            .bmiHeader = {
                .biSize = sizeof(BITMAPINFOHEADER),
                .biWidth = framebuffer_stride(&win->fb) / rgb_format_bytes(win->fb.format),
                .biHeight = -win->fb.height,
                .biPlanes = 1,
                .biBitCount = rgb_format_bpp(win->fb.format),
            },
        };
        SetDIBitsToDevice(win32->hdc, 0, 0, win->fb.width, win->fb.height, 0, 0, 0, //
                          win->fb.height, win->fb.buffer, &bmi, DIB_RGB_COLORS);
    }
}

static void win32_window_poll(gui_window_t* win)
{
    win32_win_t* win32 = win->win_data;
    MSG          Msg   = {0};

    while (win32 && win32->hwnd && PeekMessage(&Msg, win32->hwnd, 0, 0, PM_REMOVE)) {
        switch (Msg.message) {
            case WM_MOUSEMOVE: {
                if (win->on_mouse_place) {
                    POINTS cur = MAKEPOINTS(Msg.lParam);
                    win->on_mouse_place(win, cur.x, cur.y);
                }
                break;
            }
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN: // For handling F10
                // Disable autorepeat keypresses
                if ((Msg.lParam & KF_REPEAT) == 0 && win->on_key_press) {
                    win->on_key_press(win, win32_key_to_hid(Msg.wParam));
                }
                break;
            case WM_KEYUP:
            case WM_SYSKEYUP:
                if ((Msg.lParam & KF_REPEAT) == 0 && win->on_key_release) {
                    win->on_key_release(win, win32_key_to_hid(Msg.wParam));
                }
                break;
            case WM_LBUTTONDOWN:
                if (win->on_mouse_press) {
                    win->on_mouse_press(win, HID_BTN_LEFT);
                }
                break;
            case WM_LBUTTONUP:
                if (win->on_mouse_release) {
                    win->on_mouse_release(win, HID_BTN_LEFT);
                }
                break;
            case WM_RBUTTONDOWN:
                if (win->on_mouse_press) {
                    win->on_mouse_press(win, HID_BTN_RIGHT);
                }
                break;
            case WM_RBUTTONUP:
                if (win->on_mouse_release) {
                    win->on_mouse_release(win, HID_BTN_RIGHT);
                }
                break;
            case WM_MBUTTONDOWN:
                if (win->on_mouse_press) {
                    win->on_mouse_press(win, HID_BTN_MIDDLE);
                }
                break;
            case WM_MBUTTONUP:
                if (win->on_mouse_release) {
                    win->on_mouse_release(win, HID_BTN_MIDDLE);
                }
                break;
            case WM_MOUSEWHEEL:
                if (win->on_mouse_scroll) {
                    win->on_mouse_scroll(win, -GET_WHEEL_DELTA_WPARAM(Msg.wParam) / WHEEL_DELTA);
                }
                break;
            case WM_QUIT:
                if (win->on_close) {
                    win->on_close(win);
                }
                break;
            case WM_KILLFOCUS:
                if (win->on_focus_lost) {
                    win->on_focus_lost(win);
                }
                break;
            default:
                DispatchMessage(&Msg);
                break;
        }
    }
}

static void win32_window_set_title(gui_window_t* win, const char* title)
{
    win32_win_t* win32 = win->win_data;
    if (win32 && win32->hwnd) {
#if defined(HOST_TARGET_WIN9X)
        SetWindowTextA(win32->hwnd, title);
#else
        void* title_u16 = utf8_to_utf16(title);
        SetWindowTextW(win32->hwnd, title_u16);
        free(title_u16);
#endif
    }
}

bool win32_window_init(gui_window_t* win)
{
    DO_ONCE_SCOPED {
        // Initialize window atom
        WNDCLASS wc = {
            .lpfnWndProc   = win32_wndproc,
            .hInstance     = GetModuleHandle(NULL),
            .lpszClassName = WINDOW_CLASS_NAME,
        };
        window_atom = RegisterClass(&wc);
    };

    if (!window_atom) {
        rvvm_warn("Failed to register window class!");
        return false;
    }

    win32_win_t* win32 = safe_new_obj(win32_win_t);

    win->win_data  = win32;
    win->draw      = win32_window_draw;
    win->poll      = win32_window_poll;
    win->remove    = win32_window_remove;
    win->set_title = win32_window_set_title;
    // TODO: win32_window_grab_input

    uint32_t width  = win->fb.width;
    uint32_t height = win->fb.height;
    win32_adjust_client_rect(&width, &height);

    // Create window
    win32->hwnd = CreateWindow(WINDOW_CLASS_NAME, TEXT(""), WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, width, height, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!win32->hwnd) {
        rvvm_error("Failed to create window!");
        return false;
    }

    win32->hdc = GetDC(win32->hwnd);

    // Allocate framebuffer
    win->fb.buffer = vma_alloc(NULL, framebuffer_size(&win->fb), VMA_RDWR);
    if (!win->fb.buffer) {
        rvvm_error("vma_alloc() failed!");
        return false;
    }

    win32_window_set_scanout(win, &win->fb);

    return true;
}

POP_OPTIMIZATION_SIZE

#else

bool win32_window_init(gui_window_t* win)
{
    UNUSED(win);
    return false;
}

#endif
