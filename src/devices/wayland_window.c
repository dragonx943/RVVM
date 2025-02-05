/*
wayland-window.c - Wayland RVVM Window
Copyright (C) 2021  0xCatPKG <0xCatPKG@rvvm.dev>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <linux/input-event-codes.h>

#include "gui_window.h"
#include "dlib.h"
#include "vma_ops.h"
#include "utils.h"
#include "compiler.h"
#include "hashmap.h"
#include "atomics.h"
#include "threading.h"

#define WAYLAND_DYNAMIC_LOADING

// Resolve symbols at runtime
#define WAYLAND_DLIB_SYM(sym) static typeof(sym)* sym##_dlib = NULL;
#define XKB_DLIB_SYM(sym) static typeof(sym)* sym##_dlib = NULL;

#if !(CHECK_INCLUDE(wayland-client-core.h, 1) && CHECK_INCLUDE(xkbcommon/xkbcommon.h, 1))
// No Wayland and/or XKB headers found
#undef USE_WAYLAND
#warning Disabling Wayland support as <wayland-client.h> and/or <xkbcommon/xkbcommon.h> is unavailable
#endif

#ifdef USE_WAYLAND

#include <wayland-client-core.h>
#include <xkbcommon/xkbcommon.h>

#ifdef WAYLAND_DYNAMIC_LOADING

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

XKB_DLIB_SYM(xkb_context_new)
XKB_DLIB_SYM(xkb_context_unref)
XKB_DLIB_SYM(xkb_keymap_new_from_string)
XKB_DLIB_SYM(xkb_state_new)
XKB_DLIB_SYM(xkb_state_key_get_one_sym)
XKB_DLIB_SYM(xkb_state_unref)
XKB_DLIB_SYM(xkb_keymap_unref)

#define xkb_context_new xkb_context_new_dlib
#define xkb_context_unref xkb_context_unref_dlib
#define xkb_keymap_new_from_string xkb_keymap_new_from_string_dlib
#define xkb_state_new xkb_state_new_dlib
#define xkb_state_key_get_one_sym xkb_state_key_get_one_sym_dlib
#define xkb_state_unref xkb_state_unref_dlib
#define xkb_keymap_unref xkb_keymap_unref_dlib

#endif

thread_ctx_t* wayland_evloop_thread = NULL;

// Autogen start

#include "wayland_window.h"

// Autogen end

// Min global versions
#define XDG_WM_BASE_MIN_VER 2
#define ZXDG_OUTPUT_MANAGER_V1_MIN_VER 2

// Required globals
static struct wl_display *display = NULL;
static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL;
static struct xdg_wm_base* wm_base = NULL;

// Optional globals
static struct zxdg_output_manager_v1* xdg_output_manager = NULL;
static struct zxdg_decoration_manager_v1* xdg_decoration_manager = NULL;
static struct zwp_relative_pointer_manager_v1* relative_pointer_manager = NULL;
static struct zwp_pointer_constraints_v1* pointer_constraints = NULL;
static struct zwp_keyboard_shortcuts_inhibit_manager_v1 *keyboard_shortcuts_inhibit_manager = NULL;
static struct wp_tearing_control_manager_v1* tearing_control_manager = NULL;

struct xkb_context* xkb_context = NULL;

static hashmap_t globals;
static hashmap_t outputs;
static hashmap_t seats;

typedef struct {
    gui_window_t* win;

    struct wl_surface* surface;
    struct wl_buffer* buffer; // Linked to VM's framebuffer

    int32_t scale;

    struct wl_output* output;

    // XDG namespace

    struct xdg_surface* xdg_surface;
    struct xdg_toplevel* xdg_toplevel;
    struct zxdg_toplevel_decoration_v1* xdg_decoration;

    // Tearing control
    struct wp_tearing_control_v1* tearing_control;

    // Serials
    uint32_t configure_serial;
    uint32_t last_enter_serial;
    uint32_t last_button_serial;
    uint32_t last_key_serial;

    // Pointer
    struct zwp_locked_pointer_v1 *locked_pointer;

    // Keyboard
    struct zwp_keyboard_shortcuts_inhibitor_v1 *keyboard_shortcuts_inhibitor;

    bool grabbed;
} wayland_data_t;

#define WAYLAND_DLIB_RESOLVE(lib, sym) \
do { \
    sym = dlib_resolve(lib, #sym);\
    if (sym == NULL) { \
        rvvm_warn("Failed to resolve symbol %s", #sym); \
        return false; \
    } \
} while (0)

#define XKB_DLIB_RESOLVE(lib, sym) \
do { \
    sym = dlib_resolve(lib, #sym);\
    if (sym == NULL) { \
        rvvm_warn("Failed to resolve symbol %s", #sym); \
        return false; \
    } \
} while (0)

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

#define WL_POINTER_LEFTBTN 0x01
#define WL_POINTER_RIGHTBTN 0x02
#define WL_POINTER_MIDDLEBTN 0x04

#define WL_POINTER_FRAME_MOTION 0x01
#define WL_POINTER_FRAME_BUTTON 0x02
#define WL_POINTER_FRAME_AXIS 0x04
#define WL_POINTER_FRAME_SURFACE 0x08
struct pointer_data {
    struct wl_pointer* pointer;
    struct wl_surface* entered_surface;

    int32_t x;
    int32_t y;

    struct zwp_relative_pointer_v1 *relative_source;

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
    UNUSED(data);
    UNUSED(wl_shm);
    UNUSED(format);
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
    struct output_data* output_data = global_data->data;
    UNUSED(output);
    UNUSED(x);
    UNUSED(y);
    UNUSED(subpixel);
    UNUSED(make);
    UNUSED(model);
    UNUSED(transform);

    output_data->physical_size[0] = pwidth;
    output_data->physical_size[1] = pheight;
}

static void wl_output_on_mode(
    void* data, struct wl_output* output,
    uint32_t flags, int32_t width, int32_t height, int32_t refresh
)
{
    UNUSED(data);
    UNUSED(output);
    UNUSED(flags);
    UNUSED(width);
    UNUSED(height);
    UNUSED(refresh);
}

static void wl_output_on_name (void* data, struct wl_output* output, const char* name)
{
    UNUSED(data);
    UNUSED(output);
    UNUSED(name);
}

static void wl_output_on_description(void* data, struct wl_output* output, const char* desc)
{
    UNUSED(data);
    UNUSED(output);
    UNUSED(desc);
}

static void wl_output_on_done(void* data, struct wl_output* output)
{
    struct global_data* global_data = data;
    struct output_data* output_data = global_data->data;
    UNUSED(output);
    output_data->done = true;
}

static void wl_output_on_scale(void* data, struct wl_output* output, int32_t scale)
{
    struct global_data* global_data = data;
    struct output_data* output_data = global_data->data;
    UNUSED(output);
    output_data->scale = scale;
}

static const struct wl_output_listener output_listener = {
    .geometry = wl_output_on_geometry,
    .mode = wl_output_on_mode,
    .done = wl_output_on_done,
    .scale = wl_output_on_scale,
    .name = wl_output_on_name,
    .description = wl_output_on_description,
};

// zxdg_output_v1

static void xdg_output_on_logical_size(void* data, struct zxdg_output_v1* xdg_output, int32_t width, int32_t height)
{
    struct output_data* output_data = data;
    UNUSED(xdg_output);
    output_data->logical_size[0] = width;
    output_data->logical_size[1] = height;
}

static void xdg_output_on_logical_position(void* data, struct zxdg_output_v1* xdg_output, int32_t x, int32_t y)
{
    UNUSED(data);
    UNUSED(xdg_output);
    UNUSED(x);
    UNUSED(y);
}

static void xdg_output_on_done(void* data, struct zxdg_output_v1* xdg_output)
{
    struct output_data* output_data = data;
    UNUSED(xdg_output);
    output_data->done = true;
}

static void xdg_output_on_name(void* data, struct zxdg_output_v1* xdg_output, const char* name)
{
    UNUSED(data);
    UNUSED(xdg_output);
    UNUSED(name);
}

static void xdg_output_on_description(void* data, struct zxdg_output_v1* xdg_output, const char* desc)
{
    UNUSED(data);
    UNUSED(xdg_output);
    UNUSED(desc);
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
    pointer_data->frame.mask |= WL_POINTER_FRAME_SURFACE | WL_POINTER_FRAME_MOTION;
    pointer_data->frame.surface = surface;
    pointer_data->frame.x = wl_fixed_to_int(x);
    pointer_data->frame.y = wl_fixed_to_int(y);
    pointer_data->frame.enter_serial = serial;
}

static void wl_pointer_on_leave(void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surface)
{
    struct pointer_data* pointer_data = data;
    UNUSED(pointer);
    UNUSED(surface);
    pointer_data->frame.mask |= WL_POINTER_FRAME_SURFACE;
    pointer_data->frame.surface = NULL;
    pointer_data->frame.enter_serial = serial;
}

static void wl_pointer_on_motion(void* data, struct wl_pointer* pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    struct pointer_data* pointer_data = data;
    UNUSED(pointer);
    UNUSED(time);
    pointer_data->frame.mask |= WL_POINTER_FRAME_MOTION;
    pointer_data->frame.x = wl_fixed_to_int(x);
    pointer_data->frame.y = wl_fixed_to_int(y);
}

static void wl_pointer_on_button(void* data, struct wl_pointer* pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    struct pointer_data* pointer_data = data;
    UNUSED(pointer);
    UNUSED(time);
    pointer_data->frame.mask |= WL_POINTER_FRAME_BUTTON;

    int32_t frame_button = 0;
    if (button == BTN_LEFT) {
        frame_button = WL_POINTER_LEFTBTN;
    } else if (button == BTN_RIGHT) {
        frame_button = WL_POINTER_RIGHTBTN;
    } else if (button == BTN_MIDDLE) {
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

static void wl_pointer_on_axis(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    if (axis) return;
    struct pointer_data* pointer_data = data;
    UNUSED(pointer);
    UNUSED(time);
    pointer_data->frame.mask |= WL_POINTER_FRAME_AXIS;
    pointer_data->frame.axis_h = wl_fixed_to_int(value);
}

static void wl_pointer_on_frame(void* data, struct wl_pointer* pointer)
{
    struct pointer_data* pointer_data = data;
    UNUSED(pointer);
    wayland_data_t* wayland_data = NULL;
    if (pointer_data->entered_surface) {
        wayland_data = wl_surface_get_user_data(pointer_data->entered_surface);
    }

    if (pointer_data->frame.mask & WL_POINTER_FRAME_SURFACE) {
        if (pointer_data->frame.surface) {
            wayland_data = wl_surface_get_user_data(pointer_data->frame.surface);
            wayland_data->last_enter_serial = pointer_data->frame.enter_serial;
            wl_pointer_set_cursor(pointer, pointer_data->frame.enter_serial, NULL, 0, 0);
            if (wayland_data->locked_pointer) {
                zwp_locked_pointer_v1_set_cursor_position_hint(wayland_data->locked_pointer, pointer_data->frame.x, pointer_data->frame.y);
            }
        }
        pointer_data->entered_surface = pointer_data->frame.surface;
    }
    if (pointer_data->frame.mask & WL_POINTER_FRAME_MOTION) {
        pointer_data->x = pointer_data->frame.x;
        pointer_data->y = pointer_data->frame.y;
        if (wayland_data) {
            if (!wayland_data->grabbed) wayland_data->win->on_mouse_place(wayland_data->win, pointer_data->x, pointer_data->y);
        }
    }
    if (pointer_data->frame.mask & WL_POINTER_FRAME_BUTTON) {
        if (wayland_data) {
            if (pointer_data->frame.pressed) {
                if (pointer_data->frame.pressed & WL_POINTER_LEFTBTN) {
                    wayland_data->win->on_mouse_press(wayland_data->win, HID_BTN_LEFT);
                } else if (pointer_data->frame.pressed & WL_POINTER_RIGHTBTN) {
                    wayland_data->win->on_mouse_press(wayland_data->win, HID_BTN_RIGHT);
                } else if (pointer_data->frame.pressed & WL_POINTER_MIDDLEBTN) {
                    wayland_data->win->on_mouse_press(wayland_data->win, HID_BTN_MIDDLE);
                }
            } else if (pointer_data->frame.released) {
                if (pointer_data->frame.released & WL_POINTER_LEFTBTN) {
                    wayland_data->win->on_mouse_release(wayland_data->win, HID_BTN_LEFT);
                } else if (pointer_data->frame.released & WL_POINTER_RIGHTBTN) {
                    wayland_data->win->on_mouse_release(wayland_data->win, HID_BTN_RIGHT);
                } else if (pointer_data->frame.released & WL_POINTER_MIDDLEBTN) {
                    wayland_data->win->on_mouse_release(wayland_data->win, HID_BTN_MIDDLE);
                }
            }
            wayland_data->last_button_serial = pointer_data->frame.interaction_serial;
        }
    }
    if (pointer_data->frame.mask & WL_POINTER_FRAME_AXIS) {
        if (wayland_data) {
            wayland_data->win->on_mouse_scroll(wayland_data->win, pointer_data->frame.axis_h);
        }
    }

    pointer_data->frame.mask = 0;
    pointer_data->frame.pressed = 0;
    pointer_data->frame.released = 0;
    pointer_data->frame.axis_h = 0;
    pointer_data->frame.surface = NULL;
    pointer_data->frame.enter_serial = 0;
    pointer_data->frame.interaction_serial = 0;

}

static void wl_pointer_on_axis_source(void* data, struct wl_pointer* pointer, uint32_t source)
{
    UNUSED(data);
    UNUSED(pointer);
    UNUSED(source);
}

static void wl_pointer_on_axis_stop(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis)
{
    UNUSED(data);
    UNUSED(pointer);
    UNUSED(time);
    UNUSED(axis);
}

static void wl_pointer_on_axis_discrete(void* data, struct wl_pointer* pointer, uint32_t axis, int32_t discrete)
{
    UNUSED(data);
    UNUSED(pointer);
    UNUSED(axis);
    UNUSED(discrete);
}

static void wl_pointer_on_axis_value120(void* data, struct wl_pointer* pointer, uint32_t axis, int32_t value)
{
    UNUSED(data);
    UNUSED(pointer);
    UNUSED(axis);
    UNUSED(value);
}

static void wl_pointer_on_axis_relative_direction(void* data, struct wl_pointer* pointer, uint32_t axis, uint32_t direction)
{
    UNUSED(data);
    UNUSED(pointer);
    UNUSED(axis);
    UNUSED(direction);
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = wl_pointer_on_enter,
    .leave = wl_pointer_on_leave,
    .motion = wl_pointer_on_motion,
    .button = wl_pointer_on_button,
    .axis = wl_pointer_on_axis,
    .frame = wl_pointer_on_frame,
    .axis_source = wl_pointer_on_axis_source,
    .axis_stop = wl_pointer_on_axis_stop,
    .axis_discrete = wl_pointer_on_axis_discrete,
    .axis_value120 = wl_pointer_on_axis_value120,
    .axis_relative_direction = wl_pointer_on_axis_relative_direction,
};

// wl_keyboard

static void wl_keyboard_on_keymap(void* data, struct wl_keyboard* keyboard, uint32_t format, int32_t fd, uint32_t size)
{
    UNUSED(keyboard);
    UNUSED(format);
    UNUSED(size);

    if (!format) {
        rvvm_warn("Keymap format not supported");
        close(fd);
        return;
    }

    struct keyboard_data* keyboard_data = data;
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

static void wl_keyboard_on_enter(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface, struct wl_array* keys)
{
    UNUSED(keyboard);
    UNUSED(serial);
    UNUSED(keys);
    struct keyboard_data* keyboard_data = data;
    keyboard_data->entered_surface = surface;
}

static void wl_keyboard_on_leave(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface)
{
    UNUSED(keyboard);
    UNUSED(serial);
    struct keyboard_data* keyboard_data = data;
    wayland_data_t* wayland_data = wl_surface_get_user_data(surface);
    keyboard_data->entered_surface = NULL;
    wayland_data->win->on_focus_lost(wayland_data->win);
}

static void wl_keyboard_on_key(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    struct keyboard_data* keyboard_data = data;
    if (!keyboard_data->keymap) return;
    UNUSED(keyboard);
    UNUSED(time);
    wayland_data_t* wayland_data = NULL;
    if (keyboard_data->entered_surface) {
        wayland_data = wl_surface_get_user_data(keyboard_data->entered_surface);
    }
    if (!wayland_data) return;

    int32_t keysym = xkb_state_key_get_one_sym(keyboard_data->state, key + 8);
    int32_t hid_key;
    switch (keysym) {
        case XKB_KEY_Escape: hid_key = HID_KEY_ESC; break;
        case XKB_KEY_a: hid_key = HID_KEY_A; break;
        case XKB_KEY_b: hid_key = HID_KEY_B; break;
        case XKB_KEY_c: hid_key = HID_KEY_C; break;
        case XKB_KEY_d: hid_key = HID_KEY_D; break;
        case XKB_KEY_e: hid_key = HID_KEY_E; break;
        case XKB_KEY_f: hid_key = HID_KEY_F; break;
        case XKB_KEY_g: hid_key = HID_KEY_G; break;
        case XKB_KEY_h: hid_key = HID_KEY_H; break;
        case XKB_KEY_i: hid_key = HID_KEY_I; break;
        case XKB_KEY_j: hid_key = HID_KEY_J; break;
        case XKB_KEY_k: hid_key = HID_KEY_K; break;
        case XKB_KEY_l: hid_key = HID_KEY_L; break;
        case XKB_KEY_m: hid_key = HID_KEY_M; break;
        case XKB_KEY_n: hid_key = HID_KEY_N; break;
        case XKB_KEY_o: hid_key = HID_KEY_O; break;
        case XKB_KEY_p: hid_key = HID_KEY_P; break;
        case XKB_KEY_q: hid_key = HID_KEY_Q; break;
        case XKB_KEY_r: hid_key = HID_KEY_R; break;
        case XKB_KEY_s: hid_key = HID_KEY_S; break;
        case XKB_KEY_t: hid_key = HID_KEY_T; break;
        case XKB_KEY_u: hid_key = HID_KEY_U; break;
        case XKB_KEY_v: hid_key = HID_KEY_V; break;
        case XKB_KEY_w: hid_key = HID_KEY_W; break;
        case XKB_KEY_x: hid_key = HID_KEY_X; break;
        case XKB_KEY_y: hid_key = HID_KEY_Y; break;
        case XKB_KEY_z: hid_key = HID_KEY_Z; break;
        case XKB_KEY_0: hid_key = HID_KEY_0; break;
        case XKB_KEY_1: hid_key = HID_KEY_1; break;
        case XKB_KEY_2: hid_key = HID_KEY_2; break;
        case XKB_KEY_3: hid_key = HID_KEY_3; break;
        case XKB_KEY_4: hid_key = HID_KEY_4; break;
        case XKB_KEY_5: hid_key = HID_KEY_5; break;
        case XKB_KEY_6: hid_key = HID_KEY_6; break;
        case XKB_KEY_7: hid_key = HID_KEY_7; break;
        case XKB_KEY_8: hid_key = HID_KEY_8; break;
        case XKB_KEY_9: hid_key = HID_KEY_9; break;
        case XKB_KEY_Return: hid_key = HID_KEY_ENTER; break;
        case XKB_KEY_BackSpace: hid_key = HID_KEY_BACKSPACE; break;
        case XKB_KEY_Tab: hid_key = HID_KEY_TAB; break;
        case XKB_KEY_space: hid_key = HID_KEY_SPACE; break;
        case XKB_KEY_F1: hid_key = HID_KEY_F1; break;
        case XKB_KEY_F2: hid_key = HID_KEY_F2; break;
        case XKB_KEY_F3: hid_key = HID_KEY_F3; break;
        case XKB_KEY_F4: hid_key = HID_KEY_F4; break;
        case XKB_KEY_F5: hid_key = HID_KEY_F5; break;
        case XKB_KEY_F6: hid_key = HID_KEY_F6; break;
        case XKB_KEY_F7: hid_key = HID_KEY_F7; break;
        case XKB_KEY_F8: hid_key = HID_KEY_F8; break;
        case XKB_KEY_F9: hid_key = HID_KEY_F9; break;
        case XKB_KEY_F10: hid_key = HID_KEY_F10; break;
        case XKB_KEY_F11: hid_key = HID_KEY_F11; break;
        case XKB_KEY_F12: hid_key = HID_KEY_F12; break;
        case XKB_KEY_minus: hid_key = HID_KEY_MINUS; break;
        case XKB_KEY_equal: hid_key = HID_KEY_EQUAL; break;
        case XKB_KEY_bracketleft: hid_key = HID_KEY_LEFTBRACE; break;
        case XKB_KEY_bracketright: hid_key = HID_KEY_RIGHTBRACE; break;
        case XKB_KEY_backslash: hid_key = HID_KEY_BACKSLASH; break;
        case XKB_KEY_semicolon: hid_key = HID_KEY_SEMICOLON; break;
        case XKB_KEY_apostrophe: hid_key = HID_KEY_APOSTROPHE; break;
        case XKB_KEY_grave: hid_key = HID_KEY_GRAVE; break;
        case XKB_KEY_comma: hid_key = HID_KEY_COMMA; break;
        case XKB_KEY_period: hid_key = HID_KEY_DOT; break;
        case XKB_KEY_slash: hid_key = HID_KEY_SLASH; break;
        case XKB_KEY_Caps_Lock: hid_key = HID_KEY_CAPSLOCK; break;
        case XKB_KEY_Shift_L: hid_key = HID_KEY_LEFTSHIFT; break;
        case XKB_KEY_Shift_R: hid_key = HID_KEY_RIGHTSHIFT; break;
        case XKB_KEY_Control_L: hid_key = HID_KEY_LEFTCTRL; break;
        case XKB_KEY_Control_R: hid_key = HID_KEY_RIGHTCTRL; break;
        case XKB_KEY_Alt_L: hid_key = HID_KEY_LEFTALT; break;
        case XKB_KEY_Alt_R: hid_key = HID_KEY_RIGHTALT; break;
        case XKB_KEY_Super_L: hid_key = HID_KEY_LEFTMETA; break;
        case XKB_KEY_Super_R: hid_key = HID_KEY_RIGHTMETA; break;
        case XKB_KEY_Sys_Req: hid_key = HID_KEY_SYSRQ; break;
        case XKB_KEY_Scroll_Lock: hid_key = HID_KEY_SCROLLLOCK; break;
        case XKB_KEY_Pause: hid_key = HID_KEY_PAUSE; break;
        case XKB_KEY_Insert: hid_key = HID_KEY_INSERT; break;
        case XKB_KEY_Home: hid_key = HID_KEY_HOME; break;
        case XKB_KEY_Page_Up: hid_key = HID_KEY_PAGEUP; break;
        case XKB_KEY_Delete: hid_key = HID_KEY_DELETE; break;
        case XKB_KEY_End: hid_key = HID_KEY_END; break;
        case XKB_KEY_Page_Down: hid_key = HID_KEY_PAGEDOWN; break;
        case XKB_KEY_Up: hid_key = HID_KEY_UP; break;
        case XKB_KEY_Down: hid_key = HID_KEY_DOWN; break;
        case XKB_KEY_Left: hid_key = HID_KEY_LEFT; break;
        case XKB_KEY_Right: hid_key = HID_KEY_RIGHT; break;
        case XKB_KEY_Num_Lock: hid_key = HID_KEY_NUMLOCK; break;
        case XKB_KEY_KP_Divide: hid_key = HID_KEY_KPSLASH; break;
        case XKB_KEY_KP_Multiply: hid_key = HID_KEY_KPASTERISK; break;
        case XKB_KEY_KP_Subtract: hid_key = HID_KEY_KPMINUS; break;
        case XKB_KEY_KP_Add: hid_key = HID_KEY_KPPLUS; break;
        case XKB_KEY_KP_Enter: hid_key = HID_KEY_KPENTER; break;
        case XKB_KEY_KP_0: hid_key = HID_KEY_KP0; break;
        case XKB_KEY_KP_1: hid_key = HID_KEY_KP1; break;
        case XKB_KEY_KP_2: hid_key = HID_KEY_KP2; break;
        case XKB_KEY_KP_3: hid_key = HID_KEY_KP3; break;
        case XKB_KEY_KP_4: hid_key = HID_KEY_KP4; break;
        case XKB_KEY_KP_5: hid_key = HID_KEY_KP5; break;
        case XKB_KEY_KP_6: hid_key = HID_KEY_KP6; break;
        case XKB_KEY_KP_7: hid_key = HID_KEY_KP7; break;
        case XKB_KEY_KP_8: hid_key = HID_KEY_KP8; break;
        case XKB_KEY_KP_9: hid_key = HID_KEY_KP9; break;
        case XKB_KEY_KP_Decimal: hid_key = HID_KEY_KPDOT; break;
        case XKB_KEY_KP_Equal: hid_key = HID_KEY_KPEQUAL; break;
        case XKB_KEY_Multi_key: hid_key = HID_KEY_COMPOSE; break;
        case XKB_KEY_F13: hid_key = HID_KEY_F13; break;
        case XKB_KEY_F14: hid_key = HID_KEY_F14; break;
        case XKB_KEY_F15: hid_key = HID_KEY_F15; break;
        case XKB_KEY_F16: hid_key = HID_KEY_F16; break;
        case XKB_KEY_F17: hid_key = HID_KEY_F17; break;
        case XKB_KEY_F18: hid_key = HID_KEY_F18; break;
        case XKB_KEY_F19: hid_key = HID_KEY_F19; break;
        case XKB_KEY_F20: hid_key = HID_KEY_F20; break;
        case XKB_KEY_F21: hid_key = HID_KEY_F21; break;
        case XKB_KEY_F22: hid_key = HID_KEY_F22; break;
        case XKB_KEY_F23: hid_key = HID_KEY_F23; break;
        case XKB_KEY_F24: hid_key = HID_KEY_F24; break;
        case XKB_KEY_Execute: hid_key = HID_KEY_OPEN; break;
        case XKB_KEY_Help: hid_key = HID_KEY_HELP; break;
        case XKB_KEY_Menu: hid_key = HID_KEY_MENU; break;
        case XKB_KEY_Select: hid_key = HID_KEY_FRONT; break;
        case XKB_KEY_Cancel: hid_key = HID_KEY_STOP; break;
        case XKB_KEY_Redo: hid_key = HID_KEY_AGAIN; break;
        case XKB_KEY_Undo: hid_key = HID_KEY_UNDO; break;
        // case XKB_KEY_Cut: hid_key = HID_KEY_CUT; break;
        // case XKB_KEY_Copy: hid_key = HID_KEY_COPY; break;
        // case XKB_KEY_Paste: hid_key = HID_KEY_PASTE; break;
        case XKB_KEY_Find: hid_key = HID_KEY_FIND; break;
        // case XKB_KEY_Mute: hid_key = HID_KEY_MUTE; break;
        // case XKB_KEY_Volume_Mute: hid_key = HID_KEY_MUTE; break;
        // case XKB_KEY_Volume_Down: hid_key = HID_KEY_VOLUMEDOWN; break;
        // case XKB_KEY_Volume_Up: hid_key = HID_KEY_VOLUMEUP; break;
        case XKB_KEY_KP_Separator: hid_key = HID_KEY_KPCOMMA; break;
        case XKB_KEY_Romaji: hid_key = HID_KEY_RO; break;
        case XKB_KEY_Hiragana_Katakana: hid_key = HID_KEY_KATAKANAHIRAGANA; break;
        case XKB_KEY_yen: hid_key = HID_KEY_YEN; break;
        case XKB_KEY_Henkan: hid_key = HID_KEY_HENKAN; break;
        case XKB_KEY_Muhenkan: hid_key = HID_KEY_MUHENKAN; break;
        case XKB_KEY_Hangul: hid_key = HID_KEY_HANGEUL; break;
        case XKB_KEY_Hangul_Hanja: hid_key = HID_KEY_HANJA; break;
        case XKB_KEY_Katakana: hid_key = HID_KEY_KATAKANA; break;
        case XKB_KEY_Hiragana: hid_key = HID_KEY_HIRAGANA; break;
        case XKB_KEY_Zenkaku_Hankaku: hid_key = HID_KEY_Z; break;
        default: hid_key = HID_KEY_NONE; break;
    }

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        wayland_data->win->on_key_press(wayland_data->win, hid_key);
    } else {
        wayland_data->win->on_key_release(wayland_data->win, hid_key);
    }

    wayland_data->last_key_serial = serial;
}

static void wl_keyboard_on_modifiers(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group)
{
    UNUSED(data);
    UNUSED(keyboard);
    UNUSED(serial);
    UNUSED(mods_depressed);
    UNUSED(mods_latched);
    UNUSED(mods_locked);
    UNUSED(group);
}

static void wl_keyboard_on_repeat_info(void* data, struct wl_keyboard* keyboard, int32_t rate, int32_t delay)
{
    UNUSED(data);
    UNUSED(keyboard);
    UNUSED(rate);
    UNUSED(delay);
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
    UNUSED(relative_pointer);
    UNUSED(utime_hi);
    UNUSED(utime_lo);
    UNUSED(dx);
    UNUSED(dy);

    if (!pointer_data->entered_surface) return;
    wayland_data_t* wayland_data = wl_surface_get_user_data(pointer_data->entered_surface);
    if (!wayland_data->grabbed) return;
    wayland_data->win->on_mouse_move(wayland_data->win, wl_fixed_to_int(dx_unaccel), wl_fixed_to_int(dy_unaccel));
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    .relative_motion = relative_pointer_on_relative_motion,
};

// zwp_locked_pointer_v1

static void zwp_locked_pointer_on_locked(void* data, struct zwp_locked_pointer_v1* locked_pointer)
{
    wayland_data_t* wayland_data = data;
    UNUSED(locked_pointer);
    wayland_data->grabbed = true;
}

static void zwp_locked_pointer_on_unlocked(void* data, struct zwp_locked_pointer_v1* locked_pointer)
{
    wayland_data_t* wayland_data = data;
    UNUSED(locked_pointer);
    wayland_data->grabbed = false;
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
    UNUSED(data);
    UNUSED(seat);
    UNUSED(name);
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
    wayland_data_t* wayland_data = data;
    xdg_surface_ack_configure(xdg_surface, serial);

    if (!wayland_data->configure_serial) {
        wl_surface_attach(wayland_data->surface, wayland_data->buffer, 0, 0);
        struct wl_region *region = wl_compositor_create_region(compositor);
        wl_region_add(region, 0, 0, wayland_data->win->fb.width, wayland_data->win->fb.height);
        wl_surface_set_opaque_region(wayland_data->surface, region);
        wl_surface_commit(wayland_data->surface);
        wl_region_destroy(region);
    }

    wayland_data->configure_serial = serial;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_on_configure,
};

// xdg_toplevel

static void xdg_toplevel_on_configure(void* data, struct xdg_toplevel* xdg_toplevel, int32_t width, int32_t height, struct wl_array* states)
{
    UNUSED(data);
    UNUSED(xdg_toplevel);
    UNUSED(width);
    UNUSED(height);
    UNUSED(states);
}

static void xdg_toplevel_on_close(void* data, struct xdg_toplevel* xdg_toplevel)
{
    UNUSED(xdg_toplevel);
    wayland_data_t* wayland_data = data;
    wayland_data->win->on_close(wayland_data->win);
}

static void xdg_toplevel_on_configure_bounds(void* data, struct xdg_toplevel* xdg_toplevel, int32_t width, int32_t height)
{
    UNUSED(data);
    UNUSED(xdg_toplevel);
    UNUSED(width);
    UNUSED(height);
}

static void xdg_toplevel_on_wm_capabilities(void* data, struct xdg_toplevel* xdg_toplevel, struct wl_array* capabilities)
{
    UNUSED(data);
    UNUSED(xdg_toplevel);
    UNUSED(capabilities);
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
    UNUSED(wl_surface);

    wayland_data_t* wayland_data = data;

    if (wayland_data->output) {
        struct global_data* global_data = wl_output_get_user_data(wayland_data->output);
        struct output_data* output_data = global_data->data;
        output_data->refcounter--;
    }

    wayland_data->output = output;

    struct global_data* global_data = wl_output_get_user_data(wayland_data->output);
    struct output_data* output_data = global_data->data;
    output_data->refcounter++;

    if (!wayland_data->scale) wayland_data->scale = 1;

    if ((int32_t)(wayland_data->win->fb.width / wayland_data->scale) >= output_data->logical_size[0] && (int32_t)(wayland_data->win->fb.height / wayland_data->scale) >= output_data->logical_size[1]) {
        xdg_toplevel_set_fullscreen(wayland_data->xdg_toplevel, wayland_data->output);
    }
}

static void wl_surface_on_leave(void* data, struct wl_surface* wl_surface, struct wl_output* output)
{
    UNUSED(wl_surface);
    UNUSED(output);
    UNUSED(data);
}

static void wl_surface_on_preferred_buffer_scale(void* data, struct wl_surface* wl_surface, int32_t scale)
{
    wayland_data_t* wayland_data = data;
    wl_surface_set_buffer_scale(wl_surface, scale);
    wayland_data->scale = scale;

    if (!wayland_data->output) {
        return;
    }

    struct global_data* output_data = wl_output_get_user_data(wayland_data->output);
    struct output_data* output = output_data->data;

    if ((int32_t)(wayland_data->win->fb.width / scale) >= output->logical_size[0] && (int32_t)(wayland_data->win->fb.height / scale) >= output->logical_size[1]) {
        xdg_toplevel_set_fullscreen(wayland_data->xdg_toplevel, wayland_data->output);
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
    UNUSED(registry);
    UNUSED(data);

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
    UNUSED(data);
    UNUSED(registry);
    UNUSED(name);

    size_t val = hashmap_get(&globals, name);
    if (!val) return;

    struct global_data* global_data = (struct global_data*)(uintptr_t)val;
    if (global_data->global_type == GLOBAL_TYPE_OUTPUT) {
        struct output_data* output_data = global_data->data;
        output_data->refcounter--;
        if (output_data->refcounter <= 0) {
            if (output_data->xdg_output) zxdg_output_v1_destroy(output_data->xdg_output);
            wl_output_destroy(output_data->output);
            safe_free(output_data);
        }
    } else if (global_data->global_type == GLOBAL_TYPE_SEAT) {
        struct seat_data* seat_data = global_data->data;
        if (seat_data->pointer) {
            struct pointer_data* pointer_data = wl_pointer_get_user_data(seat_data->pointer);
            if (pointer_data->relative_source) zwp_relative_pointer_v1_destroy(pointer_data->relative_source);
            wl_pointer_destroy(seat_data->pointer);
        }
        if (seat_data->keyboard) {
            struct keyboard_data* keyboard_data = wl_keyboard_get_user_data(seat_data->keyboard);
            if (keyboard_data->keymap) xkb_keymap_unref(keyboard_data->keymap);
            if (keyboard_data->state) xkb_state_unref(keyboard_data->state);
            wl_keyboard_destroy(seat_data->keyboard);
        }
        if (seat_data->touch) wl_touch_destroy(seat_data->touch);
        safe_free(seat_data);
    }

}

static const struct wl_registry_listener registry_listener = {
    .global = wl_registry_on_global,
    .global_remove = wl_registry_on_global_remove,
};

// ----------------------------------------------------

void* wayland_eventloop(void* data)
{
    UNUSED(data);
    while (wl_display_dispatch(display) != -1);
    rvvm_warn("Wayland connection broken.");
    return NULL;
}

static bool wayland_init(void)
{

    hashmap_init(&globals, 16);
    hashmap_init(&seats, 1);
    hashmap_init(&outputs, 1);
#ifdef WAYLAND_DYNAMIC_LOADING
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

    XKB_DLIB_RESOLVE(libxkbcommon, xkb_context_new);
    XKB_DLIB_RESOLVE(libxkbcommon, xkb_context_unref);
    XKB_DLIB_RESOLVE(libxkbcommon, xkb_keymap_new_from_string);
    XKB_DLIB_RESOLVE(libxkbcommon, xkb_keymap_unref);
    XKB_DLIB_RESOLVE(libxkbcommon, xkb_state_new);
    XKB_DLIB_RESOLVE(libxkbcommon, xkb_state_unref);
    XKB_DLIB_RESOLVE(libxkbcommon, xkb_state_key_get_one_sym);

    dlib_close(libwayland);
    dlib_close(libxkbcommon);
#endif

    xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    rvvm_info("Initializing wayland backend");
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

    rvvm_info("Wayland backend initialized.");
    rvvm_info("Available features:");
    rvvm_info(" - Decorations: %s", xdg_decoration_manager ? "Available" : "Not available");
    rvvm_info(" - Pointer input grab: %s", (pointer_constraints && relative_pointer_manager) ? "Available" : "Not available");
    rvvm_info(" - Keyboard input grab: %s", keyboard_shortcuts_inhibit_manager ? "Available" : "Not available");
    rvvm_info(" - Tearing control hints: %s", tearing_control_manager ? "Available" : "Not available");

    wayland_evloop_thread = thread_create(wayland_eventloop, NULL);
    if (!wayland_evloop_thread) {
        return false;
    }

    return true;
}

void wayland_window_draw(gui_window_t *win)
{
    wayland_data_t *wayland = win->win_data;
    if (!wayland->configure_serial) {
        return;
    }

    wl_surface_attach(wayland->surface, wayland->buffer, 0, 0);
    wl_surface_damage_buffer(wayland->surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(wayland->surface);

    wl_display_flush(display);
}

void wayland_window_remove(gui_window_t *win)
{
    wayland_data_t *wayland = win->win_data;

    wl_surface_attach(wayland->surface, NULL, 0, 0);
    wl_surface_commit(wayland->surface);
    wl_display_roundtrip(display);
    if (wayland->xdg_decoration) zxdg_toplevel_decoration_v1_destroy(wayland->xdg_decoration);
    if (wayland->tearing_control) wp_tearing_control_v1_destroy(wayland->tearing_control);
    xdg_toplevel_destroy(wayland->xdg_toplevel);
    xdg_surface_destroy(wayland->xdg_surface);
    wl_surface_destroy(wayland->surface);
    wl_buffer_destroy(wayland->buffer);

    safe_free(wayland);
}

void wayland_window_set_title(gui_window_t *win, const char *title)
{
    wayland_data_t *wayland = win->win_data;
    xdg_toplevel_set_title(wayland->xdg_toplevel, title);
}

void wayland_window_set_fullscreen(gui_window_t *win, bool fullscreen)
{
    wayland_data_t *wayland = win->win_data;
    if (fullscreen) xdg_toplevel_set_fullscreen(wayland->xdg_toplevel, NULL);
    else xdg_toplevel_unset_fullscreen(wayland->xdg_toplevel);
}

void wayland_grab_input(gui_window_t *win, bool grab)
{
    wayland_data_t* wayland_data = win->win_data;
    if (grab) {
        if (!wayland_data->locked_pointer && pointer_constraints) {
            hashmap_foreach(&seats, key, seatptr) {
                UNUSED(key);
                struct seat_data* seat = (void*)(uintptr_t)seatptr;
                if (seat->pointer) {
                    wayland_data->locked_pointer = zwp_pointer_constraints_v1_lock_pointer(pointer_constraints, wayland_data->surface, seat->pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
                    zwp_locked_pointer_v1_set_user_data(wayland_data->locked_pointer, wayland_data);
                    zwp_locked_pointer_v1_add_listener(wayland_data->locked_pointer, &locked_pointer_listener, wayland_data);
                    break;
                }
            }

        }
        if (!wayland_data->keyboard_shortcuts_inhibitor && keyboard_shortcuts_inhibit_manager) {
            hashmap_foreach(&seats, key, seatptr) {
                UNUSED(key);
                struct seat_data* seat = (void*)(uintptr_t)seatptr;
                if (seat->keyboard) {
                    wayland_data->keyboard_shortcuts_inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(keyboard_shortcuts_inhibit_manager, wayland_data->surface, seat->seat);
                    break;
                }
            }
        }
    } else {
        if (wayland_data->locked_pointer) {
            zwp_locked_pointer_v1_destroy(wayland_data->locked_pointer);
        }
        if (wayland_data->keyboard_shortcuts_inhibitor) {
            zwp_keyboard_shortcuts_inhibitor_v1_destroy(wayland_data->keyboard_shortcuts_inhibitor);
        }
        wl_display_roundtrip(display);
        wayland_data->locked_pointer = NULL;
        wayland_data->keyboard_shortcuts_inhibitor = NULL;
        wayland_data->grabbed = false;
    }
}

bool wayland_window_init(gui_window_t *win)
{
    static bool libwayland_avail = false;
    DO_ONCE(libwayland_avail = wayland_init());
    if (!libwayland_avail) {
        return false;
    }

    wayland_data_t *wayland = safe_new_obj(wayland_data_t);
    wayland->win = win;

    int framebuffer_fd = vma_anon_memfd(framebuffer_size(&win->fb));
    void *framebuffer = mmap(NULL, framebuffer_size(&win->fb), PROT_READ | PROT_WRITE, MAP_SHARED, framebuffer_fd, 0);
    win->fb.buffer = framebuffer;

    struct wl_shm_pool* pool = wl_shm_create_pool(shm, framebuffer_fd, framebuffer_size(&win->fb));
    close(framebuffer_fd);

    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, win->fb.width, win->fb.height, framebuffer_stride(&win->fb), WL_SHM_FORMAT_XRGB8888);
    wayland->buffer = buffer;
    wl_shm_pool_destroy(pool);

    // Create surface
    struct wl_surface* surface = wl_compositor_create_surface(compositor);
    wayland->surface = surface;
    wl_surface_add_listener(surface, &surface_listener, wayland);

    // XDG
    struct xdg_surface* xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    wayland->xdg_surface = xdg_surface;

    struct xdg_toplevel* xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
    wayland->xdg_toplevel = xdg_toplevel;

    if (xdg_decoration_manager) {
        struct zxdg_toplevel_decoration_v1* xdg_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(xdg_decoration_manager, xdg_toplevel);
        wayland->xdg_decoration = xdg_decoration;
        zxdg_toplevel_decoration_v1_set_mode(xdg_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }

    if (tearing_control_manager) {
        struct wp_tearing_control_v1* tearing_control = wp_tearing_control_manager_v1_get_tearing_control(tearing_control_manager, surface);
        wp_tearing_control_v1_set_presentation_hint(tearing_control, WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
        wayland->tearing_control = tearing_control;
    }

    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, wayland);
    xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, wayland);

    xdg_toplevel_set_title(xdg_toplevel, "RVVM");
    xdg_toplevel_set_app_id(xdg_toplevel, "dev.rvvm.RVVM");
    xdg_toplevel_set_min_size(xdg_toplevel, win->fb.width, win->fb.height);
    xdg_toplevel_set_max_size(xdg_toplevel, win->fb.width, win->fb.height);

    wl_surface_set_user_data(surface, wayland);
    xdg_surface_set_user_data(xdg_surface, wayland);
    xdg_toplevel_set_user_data(xdg_toplevel, wayland);

    wl_surface_commit(surface);

    win->win_data = wayland;
    win->draw = wayland_window_draw;
    win->remove = wayland_window_remove;
    win->set_title = wayland_window_set_title;
    win->set_fullscreen = wayland_window_set_fullscreen;

    if (pointer_constraints && relative_pointer_manager) {
        win->grab_input = wayland_grab_input;
    }

    return true;
}

#else

bool wayland_window_init(gui_window_t *win)
{
    UNUSED(win);
    return false;
}

#endif
