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

SOURCE_OPTIMIZATION_SIZE

#ifdef USE_GUI

typedef struct {
    rvvm_machine_t* machine;
    hid_keyboard_t* keyboard;
    hid_mouse_t*    mouse;

    uint32_t dirty;

    bool force_redraw;
    bool ctrl;
    bool alt;
    bool grab;
} gui_window_data_t;

static const uint8_t rvvm_logo_pix[] = {
    0xfc, 0x3f, 0xf0, 0x02, 0xcb, 0x0b, 0x2c, 0x3f, 0xf0, 0xcb,
    0xf3, 0x03, 0x2f, 0xb0, 0xbc, 0xc0, 0xf2, 0xcf, 0xbf, 0x3e,
    0xf2, 0xf9, 0x01, 0xe7, 0x07, 0xac, 0xdf, 0xcf, 0xeb, 0x23,
    0x9f, 0x1f, 0x70, 0x7e, 0xc0, 0xfa, 0x31, 0xbc, 0x3e, 0x30,
    0xe1, 0xc3, 0x86, 0x0f, 0x9b, 0x0f, 0xe0, 0xe7, 0xc3, 0x13,
    0x3e, 0x6c, 0xf8, 0xb0, 0xf9, 0x00, 0x7e, 0xfe, 0x0f, 0x81,
    0xcf, 0x01, 0x3e, 0x87, 0x0f, 0xe0, 0xe3, 0xc3, 0x03, 0xf8,
    0x1c, 0xe0, 0x73, 0xf8, 0x00, 0x3e, 0xfd, 0xf8, 0x02, 0x7e,
    0x00, 0xf8, 0x81, 0x2f, 0xd0, 0xdb, 0x8f, 0x2f, 0x20, 0x07,
    0x80, 0x1c, 0xf8, 0x02, 0xbd, 0xe1, 0xe4, 0x01, 0x71, 0x00,
    0xc4, 0x41, 0x18, 0x10, 0x16, 0x4e, 0x1e, 0x10, 0x07, 0x40,
    0x1c, 0x84, 0x01, 0x61, 0x90, 0x84, 0x01, 0x51, 0x00, 0x44,
    0x41, 0x10, 0x00, 0x04, 0x49, 0x18, 0x10, 0x05, 0x40, 0x14,
    0x04, 0x01, 0x40, 0x50, 0x40, 0x00, 0x50, 0x00, 0x40, 0x41,
    0x00, 0x10, 0x00, 0x05, 0x04, 0x00, 0x05, 0x00, 0x14, 0x04,
    0x00, 0x01, 0x40, 0x00, 0x00, 0x40, 0x00, 0x00, 0x01, 0x00,
    0x10, 0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x10, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x10, 0x00, 0x40, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00,
};

static void gui_window_update_title(gui_window_t* win)
{
    gui_window_data_t* wdata = win->data;
    if (win->set_title) {
        if (wdata->grab) {
            win->set_title(win, "RVVM - Press Ctrl+Alt+G to release grab");
        } else {
            win->set_title(win, "RVVM");
        }
    }
}

static void gui_window_grab_input(gui_window_t* win, bool grab)
{
    gui_window_data_t* wdata = win->data;
    if (wdata->grab != grab && win->grab_input) {
        wdata->grab = grab;
        win->grab_input(win, wdata->grab);
        gui_window_update_title(win);
    }
}

static bool gui_window_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    // Mark the framebuffer as dirty
    gui_window_t* win = dev->data;
    gui_window_data_t* wdata = win->data;
    atomic_store_uint32_relax(&wdata->dirty, 0);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static void gui_window_free(gui_window_t* win)
{
    gui_window_grab_input(win, false);
    if (win->remove) {
        win->remove(win);
    }
    free(win->data);
    free(win);
}

static void gui_window_update(rvvm_mmio_dev_t* dev)
{
    gui_window_t* win = dev->data;
    if (win->poll) {
        win->poll(win);
    }

    if (win->draw) {
        gui_window_data_t* wdata = win->data;
        if (wdata->force_redraw) {
            win->draw(win);
        } else {
            uint32_t dirty = atomic_add_uint32(&wdata->dirty, 1);
            if ((dirty < 2) || !(dirty & 0xF)) {
                win->draw(win);
            }
        }
    }
}

static void gui_window_remove(rvvm_mmio_dev_t* dev)
{
    gui_window_t* win = dev->data;
    gui_window_free(win);
}

static void gui_window_reset(rvvm_mmio_dev_t* dev)
{
    // Draw RVVM logo before guest takes over
    // Never ask why or how this works :D
    gui_window_t* win = dev->data;
    fb_ctx_t* fb = &win->fb;
    size_t bytes = rgb_format_bytes(fb->format);
    size_t stride = framebuffer_stride(fb);
    uint32_t pos_x = fb->width / 2 - 152;
    uint32_t pos_y = fb->height / 2 - 80;

    for (uint32_t y = 0; y < fb->height; ++y) {
        size_t tmp_stride = stride * y;
        for (uint32_t x=0; x < fb->width; ++x) {
            uint8_t pix = 0;
            if (x >= pos_x && x - pos_x < 304 && y >= pos_y && y - pos_y < 160) {
                uint32_t pos = ((y - pos_y) >> 3) * 38 + ((x - pos_x) >> 3);
                pix = ((rvvm_logo_pix[pos >> 2] >> ((pos & 0x3) << 1)) & 0x3) << 6;
            }
            memset(((uint8_t*)fb->buffer) + tmp_stride + (x * bytes), pix, bytes);
        }
    }
}

static const rvvm_mmio_type_t gui_window_dev_type = {
    .name = "gui_window",
    .remove = gui_window_remove,
    .update = gui_window_update,
    .reset = gui_window_reset,
};

static void gui_on_close(gui_window_t* win)
{
    gui_window_data_t* wdata = win->data;
    if (rvvm_has_arg("poweroff_key")) {
        // Send poweroff request to the guest via keyboard key
        hid_keyboard_press(wdata->keyboard, HID_KEY_POWER);
        hid_keyboard_release(wdata->keyboard, HID_KEY_POWER);
    } else {
        rvvm_reset_machine(wdata->machine, false);
    }
}

static void gui_on_focus_lost(gui_window_t* win)
{
    gui_window_data_t* wdata = win->data;

    // Fix stuck buttons after lost focus (Alt+Tab, etc)
    for (hid_key_t key = 0; key < 255; ++key) {
        hid_keyboard_release(wdata->keyboard, key);
    }

    // Ungrab input
    gui_window_grab_input(win, false);
}

static void gui_handle_modkeys(gui_window_data_t* wdata, hid_key_t key, bool pressed)
{
    switch (key) {
        case HID_KEY_LEFTALT:
        case HID_KEY_RIGHTALT:
            wdata->alt = pressed;
            break;
        case HID_KEY_LEFTCTRL:
        case HID_KEY_RIGHTCTRL:
            wdata->ctrl = pressed;
            break;
    }
}

static void gui_on_key_press(gui_window_t* win, hid_key_t key)
{
    gui_window_data_t* wdata = win->data;
    gui_handle_modkeys(wdata, key, true);
    if (wdata->ctrl && wdata->alt) {
        switch (key) {
            case HID_KEY_G:
                // Grab mouse & keyboard
                gui_window_grab_input(win, !wdata->grab);
                return;
        }
    }
    hid_keyboard_press(wdata->keyboard, key);
}

static void gui_on_key_release(gui_window_t* win, hid_key_t key)
{
    gui_window_data_t* wdata = win->data;
    gui_handle_modkeys(wdata, key, false);
    hid_keyboard_release(wdata->keyboard, key);
}

static void gui_on_mouse_press(gui_window_t* win, hid_btns_t btns)
{
    gui_window_data_t* wdata = win->data;
    hid_mouse_press(wdata->mouse, btns);
}

static void gui_on_mouse_release(gui_window_t* win, hid_btns_t btns)
{
    gui_window_data_t* wdata = win->data;
    hid_mouse_release(wdata->mouse, btns);
}

static void gui_on_mouse_place(gui_window_t* win, int32_t x, int32_t y)
{
    gui_window_data_t* wdata = win->data;
    hid_mouse_place(wdata->mouse, x, y);
}

static void gui_on_mouse_move(gui_window_t* win, int32_t x, int32_t y)
{
    gui_window_data_t* wdata = win->data;
    hid_mouse_move(wdata->mouse, x, y);
}

static void gui_on_mouse_scroll(gui_window_t* win, int32_t offset)
{
    gui_window_data_t* wdata = win->data;
    hid_mouse_scroll(wdata->mouse, offset);
}

bool gui_window_init_backend(gui_window_t* win)
{
    const char* gui = rvvm_getarg("gui");
#if defined(USE_WIN32_GUI)
    if (!gui || rvvm_strcmp(gui, "win32")) {
        if (win32_window_init(win)) return true;
    }
#endif
#if defined(USE_HAIKU_GUI)
    if (!gui || rvvm_strcmp(gui, "haiku")) {
        if (haiku_window_init(win)) return true;
    }
#endif
#if defined(USE_WAYLAND)
    if (!gui || rvvm_strcmp(gui, "wayland")) {
        if (wayland_window_init(win)) return true;
    }
#endif
#if defined(USE_X11)
    if (!gui || rvvm_strcmp(gui, "x11")) {
        if (x11_window_init(win)) return true;
    }
#endif
#if defined(USE_SDL)
    if (!gui || rvvm_strcmp(gui, "sdl")) {
        if (sdl_window_init(win)) return true;
    }
#endif
    rvvm_error("No suitable windowing backends found!");
    UNUSED(win);
    UNUSED(gui);
    return false;
}

PUBLIC bool gui_window_init_auto(rvvm_machine_t* machine, uint32_t width, uint32_t height)
{
    gui_window_t* win = safe_new_obj(gui_window_t);
    gui_window_data_t* wdata = safe_new_obj(gui_window_data_t);

    wdata->machine = machine;
    wdata->keyboard = hid_keyboard_init_auto(machine);
    wdata->mouse = hid_mouse_init_auto(machine);
    wdata->force_redraw = rvvm_has_arg("force_redraw");

    hid_mouse_resolution(wdata->mouse, width, height);

    win->data = wdata;
    win->fb.width = width;
    win->fb.height = height;
    win->fb.format = RGB_FMT_A8R8G8B8;

    win->on_close = gui_on_close;
    win->on_focus_lost = gui_on_focus_lost;
    win->on_key_press = gui_on_key_press;
    win->on_key_release = gui_on_key_release;
    win->on_mouse_press = gui_on_mouse_press;
    win->on_mouse_release = gui_on_mouse_release;
    win->on_mouse_place = gui_on_mouse_place;
    win->on_mouse_move = gui_on_mouse_move;
    win->on_mouse_scroll = gui_on_mouse_scroll;

    if (!gui_window_init_backend(win)) {
        gui_window_free(win);
        return false;
    }

    gui_window_update_title(win);

    rvvm_mmio_dev_t* fb = framebuffer_init_auto(machine, &win->fb);
    if (!fb) {
        gui_window_free(win);
        return false;
    }

    fb->data = win;
    fb->type = &gui_window_dev_type;

    if (!wdata->force_redraw) {
        fb->write = gui_window_write;
    }

    return true;
}

#else

PUBLIC bool gui_window_init_auto(rvvm_machine_t* machine, uint32_t width, uint32_t height)
{
    UNUSED(machine);
    UNUSED(width);
    UNUSED(height);
    return false;
}

#endif
