/*
wayland-window.c - Wayland RVVM Window
Copyright (C) 2021  0xCatPKG <0xCatPKG@rvvm.dev>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/


#include "gui_window.h"
#include "compiler.h"

SOURCE_OPTIMIZATION_SIZE

#if defined(USE_WAYLAND) && !CHECK_INCLUDE(wayland-client-core.h, 0)
#undef USE_WAYLAND
#warning Disabling Wayland support as <wayland-client-core.h> is unavailable
#endif

#if defined(USE_WAYLAND) && !CHECK_INCLUDE(xkbcommon/xkbcommon.h, 0)
#undef USE_WAYLAND
#warning Disabling Wayland support as <xkbcommon/xkbcommon.h> is unavailable
#endif

#ifdef USE_WAYLAND

#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-client-core.h>

#if !defined(USE_FULL_LINKING)

// Resolve symbols at runtime
#define WAYLAND_DLIB_SYM(sym) static typeof(sym)* sym##_dlib = NULL;

// Functions
WAYLAND_DLIB_SYM(wl_display_connect)
WAYLAND_DLIB_SYM(wl_display_disconnect)
WAYLAND_DLIB_SYM(wl_display_dispatch)
WAYLAND_DLIB_SYM(wl_display_dispatch_pending)
WAYLAND_DLIB_SYM(wl_display_read_events)
WAYLAND_DLIB_SYM(wl_display_roundtrip)
WAYLAND_DLIB_SYM(wl_display_flush)
WAYLAND_DLIB_SYM(wl_display_prepare_read)
WAYLAND_DLIB_SYM(wl_proxy_get_version)
WAYLAND_DLIB_SYM(wl_proxy_marshal_flags)
WAYLAND_DLIB_SYM(wl_proxy_add_listener)
WAYLAND_DLIB_SYM(wl_proxy_set_user_data)
WAYLAND_DLIB_SYM(wl_proxy_get_user_data)
WAYLAND_DLIB_SYM(wl_proxy_destroy)

#define wl_display_connect wl_display_connect_dlib
#define wl_display_disconnect wl_display_disconnect_dlib
#define wl_display_dispatch_pending wl_display_dispatch_pending_dlib
#define wl_display_dispatch wl_display_dispatch_dlib
#define wl_display_roundtrip wl_display_roundtrip_dlib
#define wl_display_flush wl_display_flush_dlib
#define wl_proxy_get_version wl_proxy_get_version_dlib
#define wl_proxy_marshal_flags wl_proxy_marshal_flags_dlib
#define wl_proxy_add_listener wl_proxy_add_listener_dlib
#define wl_proxy_set_user_data wl_proxy_set_user_data_dlib
#define wl_proxy_get_user_data wl_proxy_get_user_data_dlib
#define wl_display_prepare_read wl_display_prepare_read_dlib
#define wl_display_read_events wl_display_read_events_dlib
#define wl_proxy_destroy wl_proxy_destroy_dlib

WAYLAND_DLIB_SYM(xkb_context_new)
WAYLAND_DLIB_SYM(xkb_context_unref)
WAYLAND_DLIB_SYM(xkb_keymap_new_from_string)
WAYLAND_DLIB_SYM(xkb_state_new)
WAYLAND_DLIB_SYM(xkb_state_key_get_one_sym)
WAYLAND_DLIB_SYM(xkb_state_unref)
WAYLAND_DLIB_SYM(xkb_keymap_unref)

#define xkb_context_new xkb_context_new_dlib
#define xkb_context_unref xkb_context_unref_dlib
#define xkb_keymap_new_from_string xkb_keymap_new_from_string_dlib
#define xkb_state_new xkb_state_new_dlib
#define xkb_state_key_get_one_sym xkb_state_key_get_one_sym_dlib
#define xkb_state_unref xkb_state_unref_dlib
#define xkb_keymap_unref xkb_keymap_unref_dlib

#endif

// Protocol autogen
#include "wayland_window.h"

// RVVM internal headers come after system headers because of safe_free()
#include "threading.h"
#include "spinlock.h"
#include "vma_ops.h"
#include "hashmap.h"
#include "utils.h"
#include "dlib.h"

// Min global versions
#define XDG_WM_BASE_MIN_VER 2
#define ZXDG_OUTPUT_MANAGER_V1_MIN_VER 2

// Required globals
static struct wl_display* display = NULL;
static struct wl_compositor* compositor = NULL;
static struct wl_shm* shm = NULL;
static struct xdg_wm_base* wm_base = NULL;

// Optional globals
static struct zxdg_output_manager_v1* xdg_output_manager = NULL;
static struct zxdg_decoration_manager_v1* xdg_decoration_manager = NULL;
static struct zwp_relative_pointer_manager_v1* relative_pointer_manager = NULL;
static struct zwp_pointer_constraints_v1* pointer_constraints = NULL;
static struct zwp_keyboard_shortcuts_inhibit_manager_v1 *keyboard_shortcuts_inhibit_manager = NULL;
static struct wp_tearing_control_manager_v1* tearing_control_manager = NULL;

struct xkb_context* xkb_context = NULL;

static uint32_t      wl_windows = 0;
static thread_ctx_t* wl_thread = NULL;

static hashmap_t globals = {0};
static hashmap_t outputs = {0};
static hashmap_t seats = {0};

typedef struct {
    struct wl_surface* surface;
    struct wl_buffer* buffer; // Linked to VM's framebuffer
    struct wl_output* output;

    // XDG namespace
    struct xdg_surface* xdg_surface;
    struct xdg_toplevel* xdg_toplevel;
    struct zxdg_toplevel_decoration_v1* xdg_decoration;

    // Tearing control
    struct wp_tearing_control_v1* tearing_control;

    // Pointer grab
    struct zwp_locked_pointer_v1* locked_pointer;

    // Keyboard grab
    struct zwp_keyboard_shortcuts_inhibitor_v1* keyboard_shortcuts_inhibitor;

    // Window scaling
    int32_t scale;

    // Serials
    uint32_t configure_serial;
    uint32_t last_enter_serial;
    uint32_t last_button_serial;
    uint32_t last_key_serial;

    bool grab;
} wayland_win_t;

// ----------------[Helper structs]--------------------

struct global_data {
    void* data;
    enum {
        GLOBAL_ANY,
        GLOBAL_TYPE_COMPOSITOR,
        GLOBAL_TYPE_SHM,
        GLOBAL_TYPE_SEAT,
        GLOBAL_TYPE_OUTPUT,
        GLOBAL_TYPE_WM_BASE,
    } global_type;
};

struct output_data {
    int32_t logical_size[2];
    int32_t physical_size[2];

    float scale;

    bool done;

    struct wl_output* output;
    struct zxdg_output_v1* xdg_output;

    int32_t refcounter;

};

struct seat_data {
    struct wl_seat* seat;
    struct wl_pointer* pointer;
    struct wl_keyboard* keyboard;
    struct wl_touch* touch;
};

#define WL_POINTER_LEFTBTN   0x01
#define WL_POINTER_RIGHTBTN  0x02
#define WL_POINTER_MIDDLEBTN 0x04

#define WL_POINTER_FRAME_MOTION  0x01
#define WL_POINTER_FRAME_BUTTON  0x02
#define WL_POINTER_FRAME_AXIS    0x04
#define WL_POINTER_FRAME_SURFACE 0x08

// From <linux/input-event-codes.h>
#define WL_BTN_LEFT   0x110
#define WL_BTN_RIGHT  0x111
#define WL_BTN_MIDDLE 0x112

struct pointer_data {
    struct wl_pointer* pointer;
    struct wl_surface* entered_surface;
    struct zwp_relative_pointer_v1* relative_source;

    int32_t x;
    int32_t y;

    struct {
        int32_t mask;
        int32_t x;
        int32_t y;
        int32_t axis_h;
        int32_t pressed;
        int32_t released;
        struct wl_surface* surface;

        int32_t enter_serial;
        int32_t interaction_serial;
    } frame;
};

struct keyboard_data {
    struct wl_keyboard* keyboard;
    struct wl_surface* entered_surface;
    struct xkb_keymap* keymap;
    struct xkb_state* state;
};

// ------------[Wayland core callbacks]---------------

// wl_shm
static void wl_shm_on_format(void* data, struct wl_shm* wl_shm, uint32_t format)
{
    UNUSED(data); UNUSED(wl_shm); UNUSED(format);
}

static const struct wl_shm_listener shm_listener = {
    .format = wl_shm_on_format,
};

// wl_output

static void wl_output_on_geometry(
    void* data, struct wl_output* output,
    int32_t x, int32_t y, int32_t pwidth, int32_t pheight,
    int32_t subpixel, const char* make, const char* model, int32_t transform
)
{
    struct global_data* global_data = data;
    UNUSED(output); UNUSED(x); UNUSED(y); UNUSED(subpixel); UNUSED(make); UNUSED(model); UNUSED(transform);
    if (global_data) {
        struct output_data* output_data = global_data->data;
        if (output_data) {
            output_data->physical_size[0] = pwidth;
            output_data->physical_size[1] = pheight;
        }
    }
}

static void wl_output_on_mode(
    void* data, struct wl_output* output,
    uint32_t flags, int32_t width, int32_t height, int32_t refresh
)
{
    UNUSED(data); UNUSED(output); UNUSED(flags); UNUSED(width); UNUSED(height); UNUSED(refresh);
}

static void wl_output_on_name_or_desc(void* data, struct wl_output* output, const char* name)
{
    UNUSED(data); UNUSED(output); UNUSED(name);
}

static void wl_output_on_done(void* data, struct wl_output* output)
{
    struct global_data* global_data = data;
    UNUSED(output);
    if (global_data) {
        struct output_data* output_data = global_data->data;
        if (output_data) {
            output_data->done = true;
        }
    }
}

static void wl_output_on_scale(void* data, struct wl_output* output, int32_t scale)
{
    struct global_data* global_data = data;
    UNUSED(output);
    if (global_data) {
        struct output_data* output_data = global_data->data;
        if (output_data) {
            output_data->scale = scale;
        }
    }
}

static const struct wl_output_listener output_listener = {
    .geometry = wl_output_on_geometry,
    .mode = wl_output_on_mode,
    .done = wl_output_on_done,
    .scale = wl_output_on_scale,
    .name = wl_output_on_name_or_desc,
    .description = wl_output_on_name_or_desc,
};

// zxdg_output_v1

static void xdg_output_on_logical_size(void* data, struct zxdg_output_v1* xdg_output, int32_t width, int32_t height)
{
    struct output_data* output_data = data;
    UNUSED(xdg_output);
    if (output_data) {
        output_data->logical_size[0] = width;
        output_data->logical_size[1] = height;
    }
}

static void xdg_output_on_logical_position(void* data, struct zxdg_output_v1* xdg_output, int32_t x, int32_t y)
{
    UNUSED(data); UNUSED(xdg_output); UNUSED(x); UNUSED(y);
}

static void xdg_output_on_done(void* data, struct zxdg_output_v1* xdg_output)
{
    struct output_data* output_data = data;
    UNUSED(xdg_output);
    if (output_data) {
        output_data->done = true;
    }
}

static void xdg_output_on_name(void* data, struct zxdg_output_v1* xdg_output, const char* name)
{
    UNUSED(data); UNUSED(xdg_output); UNUSED(name);
}

static void xdg_output_on_description(void* data, struct zxdg_output_v1* xdg_output, const char* desc)
{
    UNUSED(data); UNUSED(xdg_output); UNUSED(desc);
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_size = xdg_output_on_logical_size,
    .logical_position = xdg_output_on_logical_position,
    .done = xdg_output_on_done,
    .name = xdg_output_on_name,
    .description = xdg_output_on_description,
};

// wl_pointer

static void wl_pointer_on_enter(void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surface, wl_fixed_t x, wl_fixed_t y)
{
    struct pointer_data* pointer_data = data;
    UNUSED(pointer);
    if (pointer_data) {
        pointer_data->frame.mask |= WL_POINTER_FRAME_SURFACE | WL_POINTER_FRAME_MOTION;
        pointer_data->frame.surface = surface;
        pointer_data->frame.x = wl_fixed_to_int(x);
        pointer_data->frame.y = wl_fixed_to_int(y);
        pointer_data->frame.enter_serial = serial;
    }
}

static void wl_pointer_on_leave(void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surface)
{
    struct pointer_data* pointer_data = data;
    UNUSED(pointer); UNUSED(surface);
    if (pointer_data) {
        pointer_data->frame.mask |= WL_POINTER_FRAME_SURFACE;
        pointer_data->frame.surface = NULL;
        pointer_data->frame.enter_serial = serial;
    }
}

static void wl_pointer_on_motion(void* data, struct wl_pointer* pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    struct pointer_data* pointer_data = data;
    UNUSED(pointer); UNUSED(time);
    if (pointer_data) {
        pointer_data->frame.mask |= WL_POINTER_FRAME_MOTION;
        pointer_data->frame.x = wl_fixed_to_int(x);
        pointer_data->frame.y = wl_fixed_to_int(y);
    }
}

static void wl_pointer_on_button(void* data, struct wl_pointer* pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    struct pointer_data* pointer_data = data;
    UNUSED(pointer);
    UNUSED(time);
    if (pointer_data) {
        uint32_t frame_button = 0;
        pointer_data->frame.mask |= WL_POINTER_FRAME_BUTTON;

        if (button == WL_BTN_LEFT) {
            frame_button = WL_POINTER_LEFTBTN;
        } else if (button == WL_BTN_RIGHT) {
            frame_button = WL_POINTER_RIGHTBTN;
        } else if (button == WL_BTN_MIDDLE) {
            frame_button = WL_POINTER_MIDDLEBTN;
        }

        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            pointer_data->frame.pressed = frame_button;
            pointer_data->frame.released &= ~frame_button;
        } else {
            pointer_data->frame.released |= frame_button;
            pointer_data->frame.pressed &= ~frame_button;
        }

        pointer_data->frame.interaction_serial = serial;
    }
}

static void wl_pointer_on_axis(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    struct pointer_data* pointer_data = data;
    UNUSED(pointer); UNUSED(time);
    if (pointer_data && axis) {
        pointer_data->frame.mask |= WL_POINTER_FRAME_AXIS;
        pointer_data->frame.axis_h = wl_fixed_to_int(value);
    }
}

static void wl_pointer_on_frame(void* data, struct wl_pointer* pointer)
{
    struct pointer_data* pointer_data = data;
    gui_window_t* win = NULL;
    wayland_win_t* wl = NULL;
    UNUSED(pointer);
    if (pointer_data && pointer_data->entered_surface) {
        win = wl_surface_get_user_data(pointer_data->entered_surface);
    }
    if (win) {
        wl = win->win_data;
    }
    if (pointer_data->frame.mask & WL_POINTER_FRAME_SURFACE) {
        if (pointer_data->frame.surface) {
            win = wl_surface_get_user_data(pointer_data->frame.surface);
            if (win) {
                wl = win->win_data;
            }
            if (wl) {
                wl->last_enter_serial = pointer_data->frame.enter_serial;
                wl_pointer_set_cursor(pointer, pointer_data->frame.enter_serial, NULL, 0, 0);
                if (wl->locked_pointer) {
                    zwp_locked_pointer_v1_set_cursor_position_hint(wl->locked_pointer, pointer_data->frame.x, pointer_data->frame.y);
                }
            }
        }
        pointer_data->entered_surface = pointer_data->frame.surface;
    }
    if (wl) {
        if (pointer_data->frame.mask & WL_POINTER_FRAME_MOTION) {
            pointer_data->x = pointer_data->frame.x;
            pointer_data->y = pointer_data->frame.y;
            if (!wl->grab && win->on_mouse_place) {
                win->on_mouse_place(win, pointer_data->x, pointer_data->y);
            }
        }
        if (pointer_data->frame.mask & WL_POINTER_FRAME_BUTTON) {
            if (pointer_data->frame.pressed && win->on_mouse_press) {
                if (pointer_data->frame.pressed & WL_POINTER_LEFTBTN) {
                    win->on_mouse_press(win, HID_BTN_LEFT);
                } else if (pointer_data->frame.pressed & WL_POINTER_RIGHTBTN) {
                    win->on_mouse_press(win, HID_BTN_RIGHT);
                } else if (pointer_data->frame.pressed & WL_POINTER_MIDDLEBTN) {
                    win->on_mouse_press(win, HID_BTN_MIDDLE);
                }
            } else if (pointer_data->frame.released && win->on_mouse_release) {
                if (pointer_data->frame.released & WL_POINTER_LEFTBTN) {
                    win->on_mouse_release(win, HID_BTN_LEFT);
                } else if (pointer_data->frame.released & WL_POINTER_RIGHTBTN) {
                    win->on_mouse_release(win, HID_BTN_RIGHT);
                } else if (pointer_data->frame.released & WL_POINTER_MIDDLEBTN) {
                    win->on_mouse_release(win, HID_BTN_MIDDLE);
                }
            }
            wl->last_button_serial = pointer_data->frame.interaction_serial;
        }
        if ((pointer_data->frame.mask & WL_POINTER_FRAME_AXIS) && win->on_mouse_scroll) {
            win->on_mouse_scroll(win, pointer_data->frame.axis_h);
        }

        pointer_data->frame.mask = 0;
        pointer_data->frame.pressed = 0;
        pointer_data->frame.released = 0;
        pointer_data->frame.axis_h = 0;
        pointer_data->frame.surface = NULL;
        pointer_data->frame.enter_serial = 0;
        pointer_data->frame.interaction_serial = 0;
    }
}

static void wl_pointer_on_axis_source(void* data, struct wl_pointer* pointer, uint32_t source)
{
    UNUSED(data); UNUSED(pointer); UNUSED(source);
}

static void wl_pointer_on_axis_u32(void* data, struct wl_pointer* pointer, uint32_t a, uint32_t b)
{
    UNUSED(data); UNUSED(pointer); UNUSED(a); UNUSED(b);
}

static void wl_pointer_on_axis_i32(void* data, struct wl_pointer* pointer, uint32_t a, int32_t b)
{
    UNUSED(data); UNUSED(pointer); UNUSED(a); UNUSED(b);
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = wl_pointer_on_enter,
    .leave = wl_pointer_on_leave,
    .motion = wl_pointer_on_motion,
    .button = wl_pointer_on_button,
    .axis = wl_pointer_on_axis,
    .frame = wl_pointer_on_frame,
    .axis_source = wl_pointer_on_axis_source,
    .axis_stop = wl_pointer_on_axis_u32,
    .axis_discrete = wl_pointer_on_axis_i32,
    .axis_value120 = wl_pointer_on_axis_i32,
    .axis_relative_direction = wl_pointer_on_axis_u32,
};

// wl_keyboard

static void wl_keyboard_on_keymap(void* data, struct wl_keyboard* keyboard, uint32_t format, int32_t fd, uint32_t size)
{
    struct keyboard_data* keyboard_data = data;
    UNUSED(keyboard);

    if (keyboard_data) {
        if (!format) {
            rvvm_warn("Keymap format not supported");
            close(fd);
            return;
        }

        void* map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map == MAP_FAILED) {
            rvvm_error("Failed to mmap keymap from fd");
            close(fd);
            return;
        }

        struct xkb_keymap* keymap = xkb_keymap_new_from_string(xkb_context, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        munmap(map, size);
        close(fd);

        struct xkb_state* state = xkb_state_new(keymap);
        keyboard_data->keymap = keymap;
        keyboard_data->state = state;
    }
}

static void wl_keyboard_on_enter(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface, struct wl_array* keys)
{
    struct keyboard_data* keyboard_data = data;
    UNUSED(keyboard); UNUSED(serial); UNUSED(keys);
    if (keyboard_data) {
        keyboard_data->entered_surface = surface;
    }
}

static void wl_keyboard_on_leave(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface)
{
    struct keyboard_data* keyboard_data = data;
    UNUSED(keyboard); UNUSED(serial);
    if (keyboard_data && surface) {
        gui_window_t* win = wl_surface_get_user_data(surface);
        if (win) {
            wayland_win_t* wl = win->win_data;
            if (wl && win->on_focus_lost) {
                win->on_focus_lost(win);
            }
        }
        keyboard_data->entered_surface = NULL;
    }
}

static hid_key_t wayland_keysym_to_hid(int32_t keysym)
{
    switch (keysym) {
        case XKB_KEY_a:                    return HID_KEY_A;
        case XKB_KEY_b:                    return HID_KEY_B;
        case XKB_KEY_c:                    return HID_KEY_C;
        case XKB_KEY_d:                    return HID_KEY_D;
        case XKB_KEY_e:                    return HID_KEY_E;
        case XKB_KEY_f:                    return HID_KEY_F;
        case XKB_KEY_g:                    return HID_KEY_G;
        case XKB_KEY_h:                    return HID_KEY_H;
        case XKB_KEY_i:                    return HID_KEY_I;
        case XKB_KEY_j:                    return HID_KEY_J;
        case XKB_KEY_k:                    return HID_KEY_K;
        case XKB_KEY_l:                    return HID_KEY_L;
        case XKB_KEY_m:                    return HID_KEY_M;
        case XKB_KEY_n:                    return HID_KEY_N;
        case XKB_KEY_o:                    return HID_KEY_O;
        case XKB_KEY_p:                    return HID_KEY_P;
        case XKB_KEY_q:                    return HID_KEY_Q;
        case XKB_KEY_r:                    return HID_KEY_R;
        case XKB_KEY_s:                    return HID_KEY_S;
        case XKB_KEY_t:                    return HID_KEY_T;
        case XKB_KEY_u:                    return HID_KEY_U;
        case XKB_KEY_v:                    return HID_KEY_V;
        case XKB_KEY_w:                    return HID_KEY_W;
        case XKB_KEY_x:                    return HID_KEY_X;
        case XKB_KEY_y:                    return HID_KEY_Y;
        case XKB_KEY_z:                    return HID_KEY_Z;
        case XKB_KEY_0:                    return HID_KEY_0;
        case XKB_KEY_1:                    return HID_KEY_1;
        case XKB_KEY_2:                    return HID_KEY_2;
        case XKB_KEY_3:                    return HID_KEY_3;
        case XKB_KEY_4:                    return HID_KEY_4;
        case XKB_KEY_5:                    return HID_KEY_5;
        case XKB_KEY_6:                    return HID_KEY_6;
        case XKB_KEY_7:                    return HID_KEY_7;
        case XKB_KEY_8:                    return HID_KEY_8;
        case XKB_KEY_9:                    return HID_KEY_9;
        case XKB_KEY_Return:               return HID_KEY_ENTER;
        case XKB_KEY_Escape:               return HID_KEY_ESC;
        case XKB_KEY_BackSpace:            return HID_KEY_BACKSPACE;
        case XKB_KEY_Tab:                  return HID_KEY_TAB;
        case XKB_KEY_space:                return HID_KEY_SPACE;
        case XKB_KEY_minus:                return HID_KEY_MINUS;
        case XKB_KEY_equal:                return HID_KEY_EQUAL;
        case XKB_KEY_bracketleft:          return HID_KEY_LEFTBRACE;
        case XKB_KEY_bracketright:         return HID_KEY_RIGHTBRACE;
        case XKB_KEY_backslash:            return HID_KEY_BACKSLASH;
        case XKB_KEY_semicolon:            return HID_KEY_SEMICOLON;
        case XKB_KEY_apostrophe:           return HID_KEY_APOSTROPHE;
        case XKB_KEY_grave:                return HID_KEY_GRAVE;
        case XKB_KEY_comma:                return HID_KEY_COMMA;
        case XKB_KEY_period:               return HID_KEY_DOT;
        case XKB_KEY_slash:                return HID_KEY_SLASH;
        case XKB_KEY_Caps_Lock:            return HID_KEY_CAPSLOCK;
        case XKB_KEY_F1:                   return HID_KEY_F1;
        case XKB_KEY_F2:                   return HID_KEY_F2;
        case XKB_KEY_F3:                   return HID_KEY_F3;
        case XKB_KEY_F4:                   return HID_KEY_F4;
        case XKB_KEY_F5:                   return HID_KEY_F5;
        case XKB_KEY_F6:                   return HID_KEY_F6;
        case XKB_KEY_F7:                   return HID_KEY_F7;
        case XKB_KEY_F8:                   return HID_KEY_F8;
        case XKB_KEY_F9:                   return HID_KEY_F9;
        case XKB_KEY_F10:                  return HID_KEY_F10;
        case XKB_KEY_F11:                  return HID_KEY_F11;
        case XKB_KEY_F12:                  return HID_KEY_F12;
        case XKB_KEY_Print:
        case XKB_KEY_Sys_Req:              return HID_KEY_SYSRQ;
        case XKB_KEY_Scroll_Lock:          return HID_KEY_SCROLLLOCK;
        case XKB_KEY_Pause:                return HID_KEY_PAUSE;
        case XKB_KEY_Insert:               return HID_KEY_INSERT;
        case XKB_KEY_Home:                 return HID_KEY_HOME;
        case XKB_KEY_Page_Up:              return HID_KEY_PAGEUP;
        case XKB_KEY_Delete:               return HID_KEY_DELETE;
        case XKB_KEY_End:                  return HID_KEY_END;
        case XKB_KEY_Page_Down:            return HID_KEY_PAGEDOWN;
        case XKB_KEY_Right:                return HID_KEY_RIGHT;
        case XKB_KEY_Left:                 return HID_KEY_LEFT;
        case XKB_KEY_Down:                 return HID_KEY_DOWN;
        case XKB_KEY_Up:                   return HID_KEY_UP;
        case XKB_KEY_Num_Lock:             return HID_KEY_NUMLOCK;
        case XKB_KEY_KP_Divide:            return HID_KEY_KPSLASH;
        case XKB_KEY_asterisk:
        case XKB_KEY_KP_Multiply:          return HID_KEY_KPASTERISK;
        case XKB_KEY_KP_Subtract:          return HID_KEY_KPMINUS;
        case XKB_KEY_plus:
        case XKB_KEY_KP_Add:               return HID_KEY_KPPLUS;
        case XKB_KEY_KP_Enter:             return HID_KEY_KPENTER;
        case XKB_KEY_KP_End:
        case XKB_KEY_KP_1:                 return HID_KEY_KP1;
        case XKB_KEY_KP_Down:
        case XKB_KEY_KP_2:                 return HID_KEY_KP2;
        case XKB_KEY_KP_Next:
        case XKB_KEY_KP_3:                 return HID_KEY_KP3;
        case XKB_KEY_KP_Left:
        case XKB_KEY_KP_4:                 return HID_KEY_KP4;
        case XKB_KEY_KP_Begin:
        case XKB_KEY_KP_5:                 return HID_KEY_KP5;
        case XKB_KEY_KP_Right:
        case XKB_KEY_KP_6:                 return HID_KEY_KP6;
        case XKB_KEY_KP_Home:
        case XKB_KEY_KP_7:                 return HID_KEY_KP7;
        case XKB_KEY_KP_Up:
        case XKB_KEY_KP_8:                 return HID_KEY_KP8;
        case XKB_KEY_KP_Prior:
        case XKB_KEY_KP_9:                 return HID_KEY_KP9;
        case XKB_KEY_KP_Insert:
        case XKB_KEY_KP_0:                 return HID_KEY_KP0;
        case XKB_KEY_KP_Delete:
        case XKB_KEY_KP_Decimal:           return HID_KEY_KPDOT;
        case XKB_KEY_less:                 return HID_KEY_102ND;
        case XKB_KEY_Multi_key:            return HID_KEY_COMPOSE;
        case XKB_KEY_KP_Equal:             return HID_KEY_KPEQUAL;
        case XKB_KEY_F13:                  return HID_KEY_F13;
        case XKB_KEY_F14:                  return HID_KEY_F14;
        case XKB_KEY_F15:                  return HID_KEY_F15;
        case XKB_KEY_F16:                  return HID_KEY_F16;
        case XKB_KEY_F17:                  return HID_KEY_F17;
        case XKB_KEY_F18:                  return HID_KEY_F18;
        case XKB_KEY_F19:                  return HID_KEY_F19;
        case XKB_KEY_F20:                  return HID_KEY_F20;
        case XKB_KEY_F21:                  return HID_KEY_F21;
        case XKB_KEY_F22:                  return HID_KEY_F22;
        case XKB_KEY_F23:                  return HID_KEY_F23;
        case XKB_KEY_F24:                  return HID_KEY_F24;
        case XKB_KEY_Execute:              return HID_KEY_OPEN;
        case XKB_KEY_Help:                 return HID_KEY_HELP;
        case XKB_KEY_XF86ContextMenu:      return HID_KEY_PROPS;
        case XKB_KEY_Menu:                 return HID_KEY_MENU;
        case XKB_KEY_Select:               return HID_KEY_FRONT;
        case XKB_KEY_Cancel:               return HID_KEY_STOP;
        case XKB_KEY_Redo:                 return HID_KEY_AGAIN;
        case XKB_KEY_Undo:                 return HID_KEY_UNDO;
        case XKB_KEY_XF86Cut:              return HID_KEY_CUT;
        case XKB_KEY_XF86Copy:             return HID_KEY_COPY;
        case XKB_KEY_XF86Paste:            return HID_KEY_PASTE;
        case XKB_KEY_Find:                 return HID_KEY_FIND;
        case XKB_KEY_XF86AudioMute:        return HID_KEY_MUTE;
        case XKB_KEY_XF86AudioRaiseVolume: return HID_KEY_VOLUMEUP;
        case XKB_KEY_XF86AudioLowerVolume: return HID_KEY_VOLUMEDOWN;
        case XKB_KEY_KP_Separator:         return HID_KEY_KPCOMMA;
        case XKB_KEY_kana_RO:              return HID_KEY_RO;
        case XKB_KEY_Hiragana_Katakana:    return HID_KEY_KATAKANAHIRAGANA;
        case XKB_KEY_yen:                  return HID_KEY_YEN;
        case XKB_KEY_Henkan:               return HID_KEY_HENKAN;
        case XKB_KEY_Muhenkan:             return HID_KEY_MUHENKAN;
        // HID_KEY_KPJPCOMMA ?
        case XKB_KEY_Hangul:               return HID_KEY_HANGEUL;
        case XKB_KEY_Hangul_Hanja:         return HID_KEY_HANJA;
        case XKB_KEY_Katakana:             return HID_KEY_KATAKANA;
        case XKB_KEY_Hiragana:             return HID_KEY_HIRAGANA;
        case XKB_KEY_Zenkaku_Hankaku:      return HID_KEY_ZENKAKUHANKAKU;
        case XKB_KEY_Control_L:            return HID_KEY_LEFTCTRL;
        case XKB_KEY_Shift_L:              return HID_KEY_LEFTSHIFT;
        case XKB_KEY_Alt_L:                return HID_KEY_LEFTALT;
        case XKB_KEY_Super_L:              return HID_KEY_LEFTMETA;
        case XKB_KEY_Control_R:            return HID_KEY_RIGHTCTRL;
        case XKB_KEY_Shift_R:              return HID_KEY_RIGHTSHIFT;
        case XKB_KEY_Alt_R:                return HID_KEY_RIGHTALT;
        case XKB_KEY_Super_R:              return HID_KEY_RIGHTMETA;
    }
    if (keysym) {
        rvvm_warn("Unmapped XKB keycode %x", keysym);
    }
    return HID_KEY_NONE;
}

static void wl_keyboard_on_key(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    struct keyboard_data* keyboard_data = data;
    UNUSED(keyboard); UNUSED(time);
    if (keyboard_data && keyboard_data->keymap) {
        gui_window_t* win = NULL;
        wayland_win_t* wl = NULL;
        if (keyboard_data->entered_surface) {
            win = wl_surface_get_user_data(keyboard_data->entered_surface);
        }
        if (win) {
            wl = win->win_data;
        }
        if (wl) {
            int32_t keysym = xkb_state_key_get_one_sym(keyboard_data->state, key + 8);
            int32_t hid_key = wayland_keysym_to_hid(keysym);

            if (state == WL_KEYBOARD_KEY_STATE_PRESSED && win->on_key_press) {
                win->on_key_press(win, hid_key);
            } else if (state != WL_KEYBOARD_KEY_STATE_PRESSED && win->on_key_release) {
                win->on_key_release(win, hid_key);
            }

            wl->last_key_serial = serial;
        }
    }
}

static void wl_keyboard_on_modifiers(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group)
{
    UNUSED(data); UNUSED(keyboard); UNUSED(serial); UNUSED(mods_depressed); UNUSED(mods_latched); UNUSED(mods_locked); UNUSED(group);
}

static void wl_keyboard_on_repeat_info(void* data, struct wl_keyboard* keyboard, int32_t rate, int32_t delay)
{
    UNUSED(data); UNUSED(keyboard); UNUSED(rate); UNUSED(delay);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = wl_keyboard_on_keymap,
    .enter = wl_keyboard_on_enter,
    .leave = wl_keyboard_on_leave,
    .key = wl_keyboard_on_key,
    .modifiers = wl_keyboard_on_modifiers,
    .repeat_info = wl_keyboard_on_repeat_info,
};

// zwp_relative_pointer_v1

static void relative_pointer_on_relative_motion(void* data, struct zwp_relative_pointer_v1* relative_pointer, uint32_t utime_hi, uint32_t utime_lo, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel)
{
    struct pointer_data* pointer_data = data;
    UNUSED(relative_pointer); UNUSED(utime_hi); UNUSED(utime_lo); UNUSED(dx); UNUSED(dy);
    if (pointer_data && pointer_data->entered_surface) {
        gui_window_t* win = wl_surface_get_user_data(pointer_data->entered_surface);
        wayland_win_t* wl = NULL;
        if (win) {
            wl = win->win_data;
        }
        if (wl && wl->grab && win->on_mouse_move) {
            win->on_mouse_move(win, wl_fixed_to_int(dx_unaccel), wl_fixed_to_int(dy_unaccel));
        }
    }
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    .relative_motion = relative_pointer_on_relative_motion,
};

// zwp_locked_pointer_v1

static void zwp_locked_pointer_on_locked(void* data, struct zwp_locked_pointer_v1* locked_pointer)
{
    gui_window_t* win = data;
    UNUSED(locked_pointer);
    if (win) {
        wayland_win_t* wl = win->win_data;
        if (wl) {
            wl->grab = true;
        }
    }
}

static void zwp_locked_pointer_on_unlocked(void* data, struct zwp_locked_pointer_v1* locked_pointer)
{
    gui_window_t* win = data;
    UNUSED(locked_pointer);
    if (win) {
        wayland_win_t* wl = win->win_data;
        if (wl) {
            wl->grab = false;
        }
    }
}

static const struct zwp_locked_pointer_v1_listener locked_pointer_listener = {
    .locked = zwp_locked_pointer_on_locked,
    .unlocked = zwp_locked_pointer_on_unlocked,
};

// wl_seat

static void wl_seat_on_capabilities(void* data, struct wl_seat* seat, uint32_t capabilities)
{
    struct global_data* global_data = data;
    struct seat_data* seat_data = global_data->data;

    if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
        seat_data->pointer = wl_seat_get_pointer(seat);
        struct pointer_data* pointer_data = safe_new_obj(struct pointer_data);
        pointer_data->pointer = seat_data->pointer;
        wl_pointer_add_listener(pointer_data->pointer, &pointer_listener, pointer_data);
        wl_pointer_set_user_data(pointer_data->pointer, pointer_data);

        if (relative_pointer_manager) {
            pointer_data->relative_source = zwp_relative_pointer_manager_v1_get_relative_pointer(relative_pointer_manager, pointer_data->pointer);
            zwp_relative_pointer_v1_add_listener(pointer_data->relative_source, &relative_pointer_listener, pointer_data);
        }
    }

    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        struct wl_keyboard* kb = wl_seat_get_keyboard(seat);
        struct keyboard_data* keyboard_data = safe_new_obj(struct keyboard_data);
        keyboard_data->keyboard = kb;
        wl_keyboard_add_listener(kb, &keyboard_listener, keyboard_data);
        wl_keyboard_set_user_data(kb, keyboard_data);
        seat_data->keyboard = kb;

    }

    if (capabilities & WL_SEAT_CAPABILITY_TOUCH) {
        seat_data->touch = wl_seat_get_touch(seat);
    }
}

static void wl_seat_on_name(void* data, struct wl_seat* seat, const char* name)
{
    UNUSED(data); UNUSED(seat); UNUSED(name);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = wl_seat_on_capabilities,
    .name = wl_seat_on_name
};

// xdg_wm_base

static void xdg_wm_base_ping(void* data, struct xdg_wm_base* xdg_wm_base, uint32_t serial)
{
    UNUSED(data);
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

// xdg_surface

static void xdg_surface_on_configure(void* data, struct xdg_surface* xdg_surface, uint32_t serial)
{
    gui_window_t* win = data;
    wayland_win_t* wl = NULL;
    if (win) {
        wl = win->win_data;
    }
    if (wl && xdg_surface) {
        xdg_surface_ack_configure(xdg_surface, serial);

        if (!wl->configure_serial) {
            wl_surface_attach(wl->surface, wl->buffer, 0, 0);
            struct wl_region* region = wl_compositor_create_region(compositor);
            wl_region_add(region, 0, 0, win->fb.width, win->fb.height);
            wl_surface_set_opaque_region(wl->surface, region);
            wl_surface_commit(wl->surface);
            wl_region_destroy(region);
        }

        wl->configure_serial = serial;
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_on_configure,
};

// xdg_toplevel

static void xdg_toplevel_on_configure(void* data, struct xdg_toplevel* xdg_toplevel, int32_t width, int32_t height, struct wl_array* states)
{
    UNUSED(data); UNUSED(xdg_toplevel); UNUSED(width); UNUSED(height); UNUSED(states);
}

static void xdg_toplevel_on_close(void* data, struct xdg_toplevel* xdg_toplevel)
{
    gui_window_t* win = data;
    UNUSED(xdg_toplevel);
    if (win && win->on_close) {
        win->on_close(win);
    }
}

static void xdg_toplevel_on_configure_bounds(void* data, struct xdg_toplevel* xdg_toplevel, int32_t width, int32_t height)
{
    UNUSED(data); UNUSED(xdg_toplevel); UNUSED(width); UNUSED(height);
}

static void xdg_toplevel_on_wm_capabilities(void* data, struct xdg_toplevel* xdg_toplevel, struct wl_array* capabilities)
{
    UNUSED(data); UNUSED(xdg_toplevel); UNUSED(capabilities);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_on_configure,
    .close = xdg_toplevel_on_close,
    .configure_bounds = xdg_toplevel_on_configure_bounds,
    .wm_capabilities = xdg_toplevel_on_wm_capabilities,
};

// wl_surface

static void wl_surface_on_enter(void* data, struct wl_surface* wl_surface, struct wl_output* output)
{
    gui_window_t* win = data;
    wayland_win_t* wl = NULL;
    UNUSED(wl_surface);
    if (win) {
        wl = win->win_data;
    }
    if (wl && output) {
        if (wl->output) {
            struct global_data* global_data = wl_output_get_user_data(wl->output);
            struct output_data* output_data = global_data->data;
            output_data->refcounter--;
        }

        wl->output = output;

        struct global_data* global_data = wl_output_get_user_data(wl->output);
        struct output_data* output_data = global_data->data;
        output_data->refcounter++;
        if (!wl->scale) {
            wl->scale = 1;
        }
        if ((int32_t)(win->fb.width / wl->scale) >= output_data->logical_size[0]
         && (int32_t)(win->fb.height / wl->scale) >= output_data->logical_size[1]) {
            xdg_toplevel_set_fullscreen(wl->xdg_toplevel, wl->output);
        }
    }
}

static void wl_surface_on_leave(void* data, struct wl_surface* wl_surface, struct wl_output* output)
{
    UNUSED(wl_surface); UNUSED(output); UNUSED(data);
}

static void wl_surface_on_preferred_buffer_scale(void* data, struct wl_surface* wl_surface, int32_t scale)
{
    gui_window_t* win = data;
    wayland_win_t* wl = NULL;
    if (win) {
        wl = win->win_data;
    }
    if (wl && wl_surface) {
        wl_surface_set_buffer_scale(wl_surface, scale);
        wl->scale = scale;
        if (wl->output) {
            struct global_data* output_data = wl_output_get_user_data(wl->output);
            if (output_data) {
                struct output_data* output = output_data->data;
                if (output) {
                    if ((int32_t)(win->fb.width / scale) >= output->logical_size[0]
                     && (int32_t)(win->fb.height / scale) >= output->logical_size[1]) {
                        xdg_toplevel_set_fullscreen(wl->xdg_toplevel, wl->output);
                    }
                }
            }
        }
    }
}

static void wl_surface_on_preferred_buffer_transform(void* data, struct wl_surface* wl_surface, uint32_t transform)
{
    UNUSED(data);
    wl_surface_set_buffer_transform(wl_surface, transform);
}

static const struct wl_surface_listener surface_listener = {
    .enter = wl_surface_on_enter,
    .leave = wl_surface_on_leave,
    .preferred_buffer_scale = wl_surface_on_preferred_buffer_scale,
    .preferred_buffer_transform = wl_surface_on_preferred_buffer_transform,
};

// wl_registry

static void wl_registry_on_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    UNUSED(registry); UNUSED(data);

    struct global_data* global_data = safe_new_obj(struct global_data);
    struct wl_proxy* proxy = NULL;

    if (rvvm_strcmp(interface, "wl_compositor")) {
        proxy = wl_registry_bind(registry, name, &wl_compositor_interface, version);
        compositor = (struct wl_compositor*)proxy;
        global_data->global_type = GLOBAL_TYPE_COMPOSITOR;

    } else if (rvvm_strcmp(interface, "wl_shm")) {
        proxy = wl_registry_bind(registry, name, &wl_shm_interface, version);
        shm = (struct wl_shm*)proxy;
        global_data->global_type = GLOBAL_TYPE_SHM;
        wl_shm_add_listener(shm, &shm_listener, NULL);

    } else if (rvvm_strcmp(interface, "wl_output")) {
        proxy = wl_registry_bind(registry, name, &wl_output_interface, version);
        struct output_data* output_data = safe_new_obj(struct output_data);
        global_data->data = output_data;
        global_data->global_type = GLOBAL_TYPE_OUTPUT;
        output_data->output = (struct wl_output*)proxy;
        output_data->refcounter = 1;

        if (xdg_output_manager) {
            output_data->xdg_output = zxdg_output_manager_v1_get_xdg_output(xdg_output_manager, output_data->output);
            zxdg_output_v1_add_listener(output_data->xdg_output, &xdg_output_listener, output_data);
        }

        wl_output_add_listener(output_data->output, &output_listener, NULL);

    } else if (rvvm_strcmp(interface, "wl_seat")) {
        proxy = wl_registry_bind(registry, name, &wl_seat_interface, version);
        global_data->global_type = GLOBAL_TYPE_SEAT;

        struct seat_data* seat_data = safe_new_obj(struct seat_data);
        global_data->data = seat_data;
        seat_data->seat = (struct wl_seat*)proxy;
        hashmap_put(&seats, name, (size_t)(uintptr_t)seat_data);
        wl_seat_add_listener(seat_data->seat, &seat_listener, NULL);

    } else if (rvvm_strcmp(interface, xdg_wm_base_interface.name) && version >= XDG_WM_BASE_MIN_VER) {
        proxy = wl_registry_bind(registry, name, &xdg_wm_base_interface, version);
        wm_base = (struct xdg_wm_base*)proxy;
        global_data->global_type = GLOBAL_TYPE_WM_BASE;
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);

    } else if (rvvm_strcmp(interface, zxdg_output_manager_v1_interface.name) && version >= ZXDG_OUTPUT_MANAGER_V1_MIN_VER) {
        proxy = wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, version);
        xdg_output_manager = (struct zxdg_output_manager_v1*)proxy;
        global_data->global_type = GLOBAL_ANY;

    } else if (rvvm_strcmp(interface, zxdg_decoration_manager_v1_interface.name)) {
        proxy = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, version);
        xdg_decoration_manager = (struct zxdg_decoration_manager_v1*)proxy;
        global_data->global_type = GLOBAL_ANY;

    } else if (rvvm_strcmp(interface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name)) {
        proxy = wl_registry_bind(registry, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, version);
        keyboard_shortcuts_inhibit_manager = (struct zwp_keyboard_shortcuts_inhibit_manager_v1*)proxy;
        global_data->global_type = GLOBAL_ANY;

    } else if (rvvm_strcmp(interface, zwp_pointer_constraints_v1_interface.name)) {
        proxy = wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, version);
        pointer_constraints = (struct zwp_pointer_constraints_v1*)proxy;
        global_data->global_type = GLOBAL_ANY;

    } else if (rvvm_strcmp(interface, zwp_relative_pointer_manager_v1_interface.name)) {
        proxy = wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, version);
        relative_pointer_manager = (struct zwp_relative_pointer_manager_v1*)proxy;
        global_data->global_type = GLOBAL_ANY;

    } else if (rvvm_strcmp(interface, wp_tearing_control_manager_v1_interface.name)) {
        proxy = wl_registry_bind(registry, name, &wp_tearing_control_manager_v1_interface, version);
        tearing_control_manager = (struct wp_tearing_control_manager_v1*)proxy;
        global_data->global_type = GLOBAL_ANY;

    } else {
        safe_free(global_data);
        return;
    }

    hashmap_put(&globals, name, (size_t)(uintptr_t)global_data);
    wl_proxy_set_user_data(proxy, global_data);
}

static void wl_registry_on_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    size_t val = hashmap_get(&globals, name);
    UNUSED(data); UNUSED(registry);

    if (val) {
        struct global_data* global_data = (struct global_data*)(uintptr_t)val;
        if (global_data->global_type == GLOBAL_TYPE_OUTPUT) {
            struct output_data* output_data = global_data->data;
            output_data->refcounter--;
            if (output_data->refcounter <= 0) {
                if (output_data->xdg_output) {
                    zxdg_output_v1_destroy(output_data->xdg_output);
                }
                wl_output_destroy(output_data->output);
                safe_free(output_data);
            }
        } else if (global_data->global_type == GLOBAL_TYPE_SEAT) {
            struct seat_data* seat_data = global_data->data;
            if (seat_data->pointer) {
                struct pointer_data* pointer_data = wl_pointer_get_user_data(seat_data->pointer);
                if (pointer_data->relative_source) {
                    zwp_relative_pointer_v1_destroy(pointer_data->relative_source);
                }
                wl_pointer_destroy(seat_data->pointer);
            }
            if (seat_data->keyboard) {
                struct keyboard_data* keyboard_data = wl_keyboard_get_user_data(seat_data->keyboard);
                if (keyboard_data->keymap) {
                    xkb_keymap_unref(keyboard_data->keymap);
                }
                if (keyboard_data->state) {
                    xkb_state_unref(keyboard_data->state);
                }
                wl_keyboard_destroy(seat_data->keyboard);
            }
            if (seat_data->touch) {
                wl_touch_destroy(seat_data->touch);
            }
            safe_free(seat_data);
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = wl_registry_on_global,
    .global_remove = wl_registry_on_global_remove,
};

// ----------------------------------------------------

static void* wl_worker(void* arg)
{
    while (atomic_load_uint32_relax(&wl_windows)) {
        if (wl_display_dispatch(display) == -1) {
            rvvm_warn("Wayland connection broken.");
            break;
        }
    }
    return arg;
}

#define WAYLAND_DLIB_RESOLVE(lib, sym) \
do { \
    sym = dlib_resolve(lib, #sym); \
    libwayland_avail = libwayland_avail && !!sym; \
} while (0)

static bool wayland_init(void)
{
    bool libwayland_avail = true;
#if !defined(USE_FULL_LINKING)
    dlib_ctx_t* libwayland = dlib_open("wayland-client", DLIB_NAME_PROBE);
    dlib_ctx_t* libxkbcommon = dlib_open("xkbcommon", DLIB_NAME_PROBE);

    WAYLAND_DLIB_RESOLVE(libwayland, wl_display_connect);
    WAYLAND_DLIB_RESOLVE(libwayland, wl_display_disconnect);
    WAYLAND_DLIB_RESOLVE(libwayland, wl_display_dispatch);
    WAYLAND_DLIB_RESOLVE(libwayland, wl_display_dispatch_pending);
    WAYLAND_DLIB_RESOLVE(libwayland, wl_display_flush);
    WAYLAND_DLIB_RESOLVE(libwayland, wl_display_prepare_read);
    WAYLAND_DLIB_RESOLVE(libwayland, wl_display_roundtrip);
    WAYLAND_DLIB_RESOLVE(libwayland, wl_proxy_get_version);
    WAYLAND_DLIB_RESOLVE(libwayland, wl_proxy_marshal_flags);
    WAYLAND_DLIB_RESOLVE(libwayland, wl_proxy_add_listener);
    WAYLAND_DLIB_RESOLVE(libwayland, wl_proxy_set_user_data);
    WAYLAND_DLIB_RESOLVE(libwayland, wl_proxy_get_user_data);
    WAYLAND_DLIB_RESOLVE(libwayland, wl_display_read_events);
    WAYLAND_DLIB_RESOLVE(libwayland, wl_proxy_destroy);

    WAYLAND_DLIB_RESOLVE(libxkbcommon, xkb_context_new);
    WAYLAND_DLIB_RESOLVE(libxkbcommon, xkb_context_unref);
    WAYLAND_DLIB_RESOLVE(libxkbcommon, xkb_keymap_new_from_string);
    WAYLAND_DLIB_RESOLVE(libxkbcommon, xkb_keymap_unref);
    WAYLAND_DLIB_RESOLVE(libxkbcommon, xkb_state_new);
    WAYLAND_DLIB_RESOLVE(libxkbcommon, xkb_state_unref);
    WAYLAND_DLIB_RESOLVE(libxkbcommon, xkb_state_key_get_one_sym);

    dlib_close(libwayland);
    dlib_close(libxkbcommon);
#endif

    if (!libwayland_avail) {
        rvvm_info("Failed to load libwayland-client or libxkbcommon");
        return false;
    }

    hashmap_init(&globals, 16);
    hashmap_init(&seats, 1);
    hashmap_init(&outputs, 1);

    xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    display = wl_display_connect(NULL);
    if (!display) {
        rvvm_info("Wayland initialization failed: Failed to connect to Wayland display");
        return false;
    }

    struct wl_registry* registry = wl_display_get_registry(display);
    if (!registry) {
        rvvm_info("Wayland initialization failed: Failed to get Wayland registry");
        return false;
    }

    wl_registry_add_listener(registry, &registry_listener, NULL);

    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!compositor) {
        rvvm_info("Wayland initialization failed: wl_compositor global is never advertised.");
        return false;
    }

    if (!shm) {
        rvvm_info("Wayland initialization failed: wl_shm global is never advertised.");
        return false;
    }

    if (!wm_base) {
        rvvm_info("Wayland initialization failed: xdg_wm_base global of version >= 2 is never advertised.");
        return false;
    }

    return true;
}

static void wayland_window_draw(gui_window_t* win)
{
    wayland_win_t* wl = win->win_data;
    if (wl && wl->configure_serial) {
        wl_surface_attach(wl->surface, wl->buffer, 0, 0);
        wl_surface_damage_buffer(wl->surface, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(wl->surface);

        wl_display_flush(display);
    }
}

static void wayland_window_remove(gui_window_t* win)
{
    wayland_win_t* wl = win->win_data;
    if (wl) {
        bool shut = atomic_sub_uint32(&wl_windows, 1) == 1;

        if (wl->surface) {
            wl_surface_attach(wl->surface, NULL, 0, 0);
            wl_surface_commit(wl->surface);
        }
        wl_display_roundtrip(display);
        if (wl->xdg_decoration) {
            zxdg_toplevel_decoration_v1_destroy(wl->xdg_decoration);
        }
        if (wl->tearing_control) {
            wp_tearing_control_v1_destroy(wl->tearing_control);
        }
        if (wl->xdg_toplevel) {
            xdg_toplevel_destroy(wl->xdg_toplevel);
        }
        if (wl->xdg_surface) {
            xdg_surface_destroy(wl->xdg_surface);
            wl_surface_destroy(wl->surface);
        }
        if (wl->buffer) {
            wl_buffer_destroy(wl->buffer);
        }

        safe_free(wl);

        if (shut) {
            thread_join(wl_thread);
            wl_thread = NULL;
        }
    }
}

static void wayland_window_set_title(gui_window_t* win, const char* title)
{
    wayland_win_t* wl = win->win_data;
    if (wl && wl->xdg_toplevel) {
        xdg_toplevel_set_title(wl->xdg_toplevel, title);
    }
}

static void wayland_window_set_fullscreen(gui_window_t *win, bool fullscreen)
{
    wayland_win_t* wl = win->win_data;
    if (wl && wl->xdg_toplevel) {
        if (fullscreen) {
            xdg_toplevel_set_fullscreen(wl->xdg_toplevel, NULL);
        } else {
            xdg_toplevel_unset_fullscreen(wl->xdg_toplevel);
        }
    }
}

static void wayland_grab_input(gui_window_t *win, bool grab)
{
    wayland_win_t* wl = win->win_data;
    if (wl) {
        if (grab) {
            if (!wl->locked_pointer && pointer_constraints) {
                hashmap_foreach(&seats, key, seatptr) {
                    UNUSED(key);
                    struct seat_data* seat = (void*)(uintptr_t)seatptr;
                    if (seat->pointer) {
                        wl->locked_pointer = zwp_pointer_constraints_v1_lock_pointer(pointer_constraints, wl->surface, seat->pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
                        zwp_locked_pointer_v1_set_user_data(wl->locked_pointer, win);
                        zwp_locked_pointer_v1_add_listener(wl->locked_pointer, &locked_pointer_listener, win);
                        break;
                    }
                }

            }
            if (!wl->keyboard_shortcuts_inhibitor && keyboard_shortcuts_inhibit_manager) {
                hashmap_foreach(&seats, key, seatptr) {
                    UNUSED(key);
                    struct seat_data* seat = (void*)(uintptr_t)seatptr;
                    if (seat->keyboard) {
                        wl->keyboard_shortcuts_inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(keyboard_shortcuts_inhibit_manager, wl->surface, seat->seat);
                        break;
                    }
                }
            }
        } else {
            if (wl->locked_pointer) {
                zwp_locked_pointer_v1_destroy(wl->locked_pointer);
            }
            if (wl->keyboard_shortcuts_inhibitor) {
                zwp_keyboard_shortcuts_inhibitor_v1_destroy(wl->keyboard_shortcuts_inhibitor);
            }
            wl_display_roundtrip(display);
            wl->locked_pointer = NULL;
            wl->keyboard_shortcuts_inhibitor = NULL;
            wl->grab = false;
        }
    }
}

bool wayland_window_init(gui_window_t *win)
{
    static bool libwayland_avail = false;
    DO_ONCE(libwayland_avail = wayland_init());
    if (!libwayland_avail) {
        return false;
    }

    wayland_win_t* wl = safe_new_obj(wayland_win_t);

    win->win_data = wl;
    win->draw = wayland_window_draw;
    win->remove = wayland_window_remove;
    win->set_title = wayland_window_set_title;
    win->set_fullscreen = wayland_window_set_fullscreen;

    if (atomic_add_uint32(&wl_windows, 1) == 0) {
        wl_thread = thread_create(wl_worker, NULL);
    }

    int framebuffer_fd = vma_anon_memfd(framebuffer_size(&win->fb));
    if (framebuffer_fd < 0) {
        rvvm_error("Failed to create framebuffer memfd");
        return false;
    }

    struct wl_shm_pool* pool = wl_shm_create_pool(shm, framebuffer_fd, framebuffer_size(&win->fb));
    if (!pool) {
        close(framebuffer_fd);
        rvvm_error("Failed to create wl_shm_pool");
        return false;
    }

    void* framebuffer = mmap(NULL, framebuffer_size(&win->fb), PROT_READ | PROT_WRITE, MAP_SHARED, framebuffer_fd, 0);
    close(framebuffer_fd);
    if (framebuffer == MAP_FAILED) {
        wl_shm_pool_destroy(pool);
        rvvm_error("Failed to mmap framebuffer");
        return false;
    }

    win->fb.buffer = framebuffer;

    wl->buffer = wl_shm_pool_create_buffer(pool, 0, win->fb.width, win->fb.height, framebuffer_stride(&win->fb), WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);

    if (!wl->buffer) {
        rvvm_error("Failed to create wl_buffer");
        return false;
    }

    // Create surface
    wl->surface = wl_compositor_create_surface(compositor);

    if (!wl->surface) {
        rvvm_error("Failed to create wl_surface");
        return false;
    }

    wl_surface_add_listener(wl->surface, &surface_listener, wl);

    // XDG
    wl->xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, wl->surface);

    if (!wl->xdg_surface) {
        rvvm_error("Failed to create xdg_surface");
        return false;
    }

    wl->xdg_toplevel = xdg_surface_get_toplevel(wl->xdg_surface);

    if (!wl->xdg_toplevel) {
        rvvm_error("Failed to get xdg_toplevel");
        return false;
    }

    if (xdg_decoration_manager) {
        wl->xdg_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(xdg_decoration_manager, wl->xdg_toplevel);
        zxdg_toplevel_decoration_v1_set_mode(wl->xdg_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    } else {
        rvvm_error("Your Wayland compositor doesn't support XDG decorations!");
        return false;
    }

    if (tearing_control_manager) {
        wl->tearing_control = wp_tearing_control_manager_v1_get_tearing_control(tearing_control_manager, wl->surface);
        wp_tearing_control_v1_set_presentation_hint(wl->tearing_control, WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
    }

    xdg_surface_add_listener(wl->xdg_surface, &xdg_surface_listener, wl);
    xdg_toplevel_add_listener(wl->xdg_toplevel, &xdg_toplevel_listener, wl);

    xdg_toplevel_set_title(wl->xdg_toplevel, "RVVM");
    xdg_toplevel_set_app_id(wl->xdg_toplevel, "dev.rvvm.RVVM");
    xdg_toplevel_set_min_size(wl->xdg_toplevel, win->fb.width, win->fb.height);
    xdg_toplevel_set_max_size(wl->xdg_toplevel, win->fb.width, win->fb.height);

    wl_surface_set_user_data(wl->surface, win);
    xdg_surface_set_user_data(wl->xdg_surface, win);
    xdg_toplevel_set_user_data(wl->xdg_toplevel, win);

    wl_surface_commit(wl->surface);

    if (pointer_constraints && relative_pointer_manager) {
        win->grab_input = wayland_grab_input;
    }

    return true;
}

#else

bool wayland_window_init(gui_window_t* win)
{
    UNUSED(win);
    return false;
}

#endif
