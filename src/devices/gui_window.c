/*
gui_window.c - Framebuffer GUI Window
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "gui_window.h"
#include "mem_ops.h"
#include "utils.h"
#include "vma_ops.h"

PUSH_OPTIMIZATION_SIZE

#if defined(USE_GUI)

/*
 * RVVM GUI Window handlers
 */

typedef struct {
    rvvm_machine_t* machine;
    hid_keyboard_t* keyboard;
    hid_mouse_t*    mouse;

    uint32_t dirty;

    bool ctrl;
    bool alt;
    bool grab;
} rvvm_window_t;

static const uint8_t rvvm_logo_pix[] = {
    0xfc, 0x3f, 0xf0, 0x02, 0xcb, 0x0b, 0x2c, 0x3f, 0xf0, 0xcb, 0xf3, 0x03, 0x2f, 0xb0, 0xbc, 0xc0, 0xf2, 0xcf, 0xbf,
    0x3e, 0xf2, 0xf9, 0x01, 0xe7, 0x07, 0xac, 0xdf, 0xcf, 0xeb, 0x23, 0x9f, 0x1f, 0x70, 0x7e, 0xc0, 0xfa, 0x31, 0xbc,
    0x3e, 0x30, 0xe1, 0xc3, 0x86, 0x0f, 0x9b, 0x0f, 0xe0, 0xe7, 0xc3, 0x13, 0x3e, 0x6c, 0xf8, 0xb0, 0xf9, 0x00, 0x7e,
    0xfe, 0x0f, 0x81, 0xcf, 0x01, 0x3e, 0x87, 0x0f, 0xe0, 0xe3, 0xc3, 0x03, 0xf8, 0x1c, 0xe0, 0x73, 0xf8, 0x00, 0x3e,
    0xfd, 0xf8, 0x02, 0x7e, 0x00, 0xf8, 0x81, 0x2f, 0xd0, 0xdb, 0x8f, 0x2f, 0x20, 0x07, 0x80, 0x1c, 0xf8, 0x02, 0xbd,
    0xe1, 0xe4, 0x01, 0x71, 0x00, 0xc4, 0x41, 0x18, 0x10, 0x16, 0x4e, 0x1e, 0x10, 0x07, 0x40, 0x1c, 0x84, 0x01, 0x61,
    0x90, 0x84, 0x01, 0x51, 0x00, 0x44, 0x41, 0x10, 0x00, 0x04, 0x49, 0x18, 0x10, 0x05, 0x40, 0x14, 0x04, 0x01, 0x40,
    0x50, 0x40, 0x00, 0x50, 0x00, 0x40, 0x41, 0x00, 0x10, 0x00, 0x05, 0x04, 0x00, 0x05, 0x00, 0x14, 0x04, 0x00, 0x01,
    0x40, 0x00, 0x00, 0x40, 0x00, 0x00, 0x01, 0x00, 0x10, 0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x10, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00,
};

static void rvvm_window_update_title(gui_window_t* win)
{
    rvvm_window_t* rvvm = win->data;
    if (win->set_title) {
        if (rvvm->grab) {
            win->set_title(win, "RVVM - Press Ctrl+Alt+G to release grab");
        } else {
            win->set_title(win, "RVVM");
        }
    }
}

static void rvvm_window_grab_input(gui_window_t* win, bool grab)
{
    rvvm_window_t* rvvm = win->data;
    if (rvvm->grab != grab && win->grab_input) {
        rvvm->grab = grab;
        win->grab_input(win, grab);
        rvvm_window_update_title(win);
    }
}

static void rvvm_on_close(gui_window_t* win)
{
    rvvm_window_t* rvvm = win->data;
    if (rvvm_has_arg("poweroff_key")) {
        // Send poweroff request to the guest via keyboard key
        hid_keyboard_press(rvvm->keyboard, HID_KEY_POWER);
        hid_keyboard_release(rvvm->keyboard, HID_KEY_POWER);
    } else {
        rvvm_reset_machine(rvvm->machine, false);
    }
}

static void rvvm_on_focus_lost(gui_window_t* win)
{
    rvvm_window_t* rvvm = win->data;

    // Fix stuck buttons after lost focus (Alt+Tab, etc)
    for (hid_key_t key = 0x00; key < 0xFF; ++key) {
        hid_keyboard_release(rvvm->keyboard, key);
    }

    // Ungrab input
    rvvm_window_grab_input(win, false);
}

static void rvvm_handle_modkeys(rvvm_window_t* rvvm, hid_key_t key, bool pressed)
{
    switch (key) {
        case HID_KEY_LEFTALT:
        case HID_KEY_RIGHTALT:
            rvvm->alt = pressed;
            break;
        case HID_KEY_LEFTCTRL:
        case HID_KEY_RIGHTCTRL:
            rvvm->ctrl = pressed;
            break;
    }
}

static void rvvm_on_key_press(gui_window_t* win, hid_key_t key)
{
    rvvm_window_t* rvvm = win->data;
    rvvm_handle_modkeys(rvvm, key, true);
    if (rvvm->ctrl && rvvm->alt) {
        switch (key) {
            case HID_KEY_G:
                // Grab mouse & keyboard
                rvvm_window_grab_input(win, !rvvm->grab);
                return;
        }
    }
    hid_keyboard_press(rvvm->keyboard, key);
}

static void rvvm_on_key_release(gui_window_t* win, hid_key_t key)
{
    rvvm_window_t* rvvm = win->data;
    rvvm_handle_modkeys(rvvm, key, false);
    hid_keyboard_release(rvvm->keyboard, key);
}

static void rvvm_on_mouse_press(gui_window_t* win, hid_btns_t btns)
{
    rvvm_window_t* rvvm = win->data;
    hid_mouse_press(rvvm->mouse, btns);
}

static void rvvm_on_mouse_release(gui_window_t* win, hid_btns_t btns)
{
    rvvm_window_t* rvvm = win->data;
    hid_mouse_release(rvvm->mouse, btns);
}

static void rvvm_on_mouse_place(gui_window_t* win, int32_t x, int32_t y)
{
    rvvm_window_t* rvvm = win->data;
    hid_mouse_place(rvvm->mouse, x, y);
}

static void rvvm_on_mouse_move(gui_window_t* win, int32_t x, int32_t y)
{
    rvvm_window_t* rvvm = win->data;
    hid_mouse_move(rvvm->mouse, x, y);
}

static void rvvm_on_mouse_scroll(gui_window_t* win, int32_t offset)
{
    rvvm_window_t* rvvm = win->data;
    hid_mouse_scroll(rvvm->mouse, offset);
}

static gui_window_t* rvvm_window_prepare(rvvm_machine_t* machine, uint32_t width, uint32_t height)
{
    gui_window_t*  win  = safe_new_obj(gui_window_t);
    rvvm_window_t* rvvm = safe_new_obj(rvvm_window_t);

    rvvm->machine  = machine;
    rvvm->keyboard = hid_keyboard_init_auto(machine);
    rvvm->mouse    = hid_mouse_init_auto(machine);

    hid_mouse_resolution(rvvm->mouse, width, height);

    win->data      = rvvm;
    win->fb.width  = width;
    win->fb.height = height;
    win->fb.format = RGB_FMT_A8R8G8B8;

    win->on_close         = rvvm_on_close;
    win->on_focus_lost    = rvvm_on_focus_lost;
    win->on_key_press     = rvvm_on_key_press;
    win->on_key_release   = rvvm_on_key_release;
    win->on_mouse_press   = rvvm_on_mouse_press;
    win->on_mouse_release = rvvm_on_mouse_release;
    win->on_mouse_place   = rvvm_on_mouse_place;
    win->on_mouse_move    = rvvm_on_mouse_move;
    win->on_mouse_scroll  = rvvm_on_mouse_scroll;

    return win;
}

/*
 * Framebuffer device hooks
 */

static bool rvvm_framebuffer_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    // Mark the framebuffer as dirty
    gui_window_t*  win  = dev->data;
    rvvm_window_t* rvvm = win->data;
    atomic_store_uint32_relax(&rvvm->dirty, 0);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static void rvvm_framebuffer_update(rvvm_mmio_dev_t* dev)
{
    gui_window_t* win = dev->data;
    if (win->poll) {
        win->poll(win);
    }
    if (win->draw) {
        rvvm_window_t* rvvm  = win->data;
        uint32_t       dirty = atomic_add_uint32(&rvvm->dirty, 1);
        if ((dirty < 2) || !(dirty & 0xF)) {
            win->draw(win);
        }
    }
}

static void rvvm_framebuffer_remove(rvvm_mmio_dev_t* dev)
{
    gui_window_t* win = dev->data;
    gui_window_free(win);
}

static void rvvm_framebuffer_reset(rvvm_mmio_dev_t* dev)
{
    // Draw RVVM logo before guest takes over
    // Never ask why or how this works :D
    gui_window_t* win    = dev->data;
    fb_ctx_t*     fb     = &win->fb;
    size_t        bytes  = rgb_format_bytes(fb->format);
    size_t        stride = framebuffer_stride(fb);
    uint32_t      pos_x  = fb->width / 2 - 152;
    uint32_t      pos_y  = fb->height / 2 - 80;

    for (uint32_t y = 0; y < fb->height; ++y) {
        size_t tmp_stride = stride * y;
        for (uint32_t x = 0; x < fb->width; ++x) {
            uint8_t pix = 0;
            if (x >= pos_x && x - pos_x < 304 && y >= pos_y && y - pos_y < 160) {
                uint32_t pos = ((y - pos_y) >> 3) * 38 + ((x - pos_x) >> 3);
                pix          = ((rvvm_logo_pix[pos >> 2] >> ((pos & 0x3) << 1)) & 0x3) << 6;
            }
            memset(((uint8_t*)fb->buffer) + tmp_stride + (x * bytes), pix, bytes);
        }
    }
}

static const rvvm_mmio_type_t rvvm_framebuffer_dev_type = {
    .name   = "framebuffer",
    .remove = rvvm_framebuffer_remove,
    .update = rvvm_framebuffer_update,
    .reset  = rvvm_framebuffer_reset,
};

/*
 * GUI abstraction layer
 */

static inline void gui_backend_free(gui_window_t* win)
{
    if (win && win->remove) {
        win->remove(win);
    }
}

static bool gui_window_init_backend(gui_window_t* win)
{
    const char* gui = rvvm_getarg("gui");
#if defined(USE_HAIKU_GUI)
    if ((!gui || rvvm_strcmp(gui, "haiku")) && haiku_window_init(win)) {
        return true;
    }
    gui_backend_free(win);
#endif
#if defined(USE_WAYLAND)
    if ((!gui || rvvm_strcmp(gui, "wayland")) && wayland_window_init(win)) {
        return true;
    }
    gui_backend_free(win);
#endif
#if defined(USE_X11)
    if ((!gui || rvvm_strcmp(gui, "x11")) && x11_window_init(win)) {
        return true;
    }
    gui_backend_free(win);
#endif
#if defined(USE_SDL)
    if ((!gui || rvvm_strcmp(gui, "sdl")) && sdl_window_init(win)) {
        return true;
    }
    gui_backend_free(win);
#endif
#if defined(USE_WIN32_GUI)
    if ((!gui || rvvm_strcmp(gui, "win32")) && win32_window_init(win)) {
        return true;
    }
    gui_backend_free(win);
#endif
    rvvm_error("No suitable windowing backends found!");
    UNUSED(win);
    UNUSED(gui);
    return false;
}

bool gui_window_init(gui_window_t* win, const fb_ctx_t* fb, size_t vram_size)
{
    vram_size = EVAL_MAX(vram_size, framebuffer_size(fb));
    win->fb   = *fb;
    if (gui_window_init_backend(win)) {
        if (!win->fb.buffer) {
            win->fb.buffer = vma_alloc(NULL, framebuffer_size(&win->fb), VMA_RDWR);
        }
        if (!win->vram) {
            win->vram      = win->fb.buffer;
            win->vram_anon = true;
        }
        if (win->vram) {
            win->fb.buffer = win->vram;
            gui_window_set_scanout(win, &win->fb);
            return true;
        }
    }
    gui_window_free(win);
    return false;
}

bool gui_window_set_scanout(gui_window_t* win, const fb_ctx_t* fb)
{
    if (win && (!win->set_scanout || win->set_scanout(win, fb))) {
        win->fb = *fb;
        return true;
    }
    return false;
}

void gui_window_free(gui_window_t* win)
{
    if (win) {
        if (win->remove) {
            win->remove(win);
        } else {
            safe_free(win->win_data);
        }
        if (win->on_remove) {
            win->on_remove(win);
        } else {
            safe_free(win->data);
        }
        if (win->vram_anon) {
            vma_free(win->vram, win->vram_size);
        }
        safe_free(win);
    }
}

bool gui_window_init_auto(rvvm_machine_t* machine, uint32_t width, uint32_t height)
{
    fb_ctx_t scanout = {
        .width  = width,
        .height = height,
        .format = RGB_FMT_A8R8G8B8,
    };
    gui_window_t* win = rvvm_window_prepare(machine, width, height);

    if (!gui_window_init(win, &scanout, 0)) {
        return false;
    }

    rvvm_window_update_title(win);

    // Create framebuffer device
    rvvm_mmio_dev_t* fb = framebuffer_init_auto(machine, &win->fb);
    if (!fb) {
        gui_window_free(win);
        return false;
    }

    // Hook into framebuffer device
    fb->data  = win;
    fb->type  = &rvvm_framebuffer_dev_type;
    fb->write = rvvm_framebuffer_write;

    return true;
}

#endif

POP_OPTIMIZATION_SIZE
