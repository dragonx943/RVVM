/*
gui_window.h - GUI Window
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_GUI_WINDOW_H
#define RVVM_GUI_WINDOW_H

#include "framebuffer.h"
#include "hid_api.h"

/*
 * GUI Window structure
 */

typedef struct gui_window_t gui_window_t;

struct gui_window_t {
    // Windowing backend private data
    void* win_data;

    // Window user private data
    void* data;

    // Video RAM (May be optionally allocated by backend)
    void*  vram;
    size_t vram_size;
    bool   vram_anon;

    // Scanout framebuffer
    fb_ctx_t fb;

    void (*draw)(gui_window_t* win);
    void (*poll)(gui_window_t* win);
    void (*remove)(gui_window_t* win);
    void (*grab_input)(gui_window_t* win, bool grab);
    void (*set_title)(gui_window_t* win, const char* title);
    bool (*set_scanout)(gui_window_t* win, const fb_ctx_t* fb);
    void (*set_fullscreen)(gui_window_t* win, bool fullscreen);

    void (*on_close)(gui_window_t* win);
    void (*on_remove)(gui_window_t* win);
    void (*on_scanout)(gui_window_t* win);
    void (*on_focus_lost)(gui_window_t* win);
    void (*on_key_press)(gui_window_t* win, hid_key_t key);
    void (*on_key_release)(gui_window_t* win, hid_key_t key);
    void (*on_mouse_press)(gui_window_t* win, hid_btns_t btns);
    void (*on_mouse_release)(gui_window_t* win, hid_btns_t btns);
    void (*on_mouse_place)(gui_window_t* win, int32_t x, int32_t y);
    void (*on_mouse_move)(gui_window_t* win, int32_t x, int32_t y);
    void (*on_mouse_scroll)(gui_window_t* win, int32_t offset);
};

/*
 * Internal use only
 */

bool haiku_window_init(gui_window_t* win);
bool wayland_window_init(gui_window_t* win);
bool x11_window_init(gui_window_t* win);
bool sdl_window_init(gui_window_t* win);
bool win32_window_init(gui_window_t* win);

/*
 * GUI abstractions (WIP)
 */

// Probe windowing backends and create a window
bool gui_window_init(gui_window_t* win, const fb_ctx_t* fb, size_t vram_size);

// Set window scanout framebuffer, preferably in VRAM
bool gui_window_set_scanout(gui_window_t* win, const fb_ctx_t* fb);

// Free GUI window
void gui_window_free(gui_window_t* win);

// Attach a framebuffer & HID mouse/keyboard to the VM. Returns false on failure.
PUBLIC bool gui_window_init_auto(rvvm_machine_t* machine, uint32_t width, uint32_t height);

#endif
