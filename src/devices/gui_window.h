/*
gui_window.h - GUI Window interfaces
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _GUI_WINDOW_INTERFACES_H
#define _GUI_WINDOW_INTERFACES_H

#include "framebuffer.h"
#include "hid_api.h"

typedef struct gui_window_t gui_window_t;

// GUI Window callbacks
typedef struct {
    void (*remove)(gui_window_t* win);
    void (*update)(gui_window_t* win);
    void (*on_close)(gui_window_t* win);
    void (*on_focus_lost)(gui_window_t* win);
    void (*on_paste)(gui_window_t* win, const char* str);
    void (*on_key_press)(gui_window_t* win, hid_key_t key);
    void (*on_key_release)(gui_window_t* win, hid_key_t key);
    void (*on_mouse_press)(gui_window_t* win, hid_btns_t btns);
    void (*on_mouse_release)(gui_window_t* win, hid_btns_t btns);
    void (*on_mouse_place)(gui_window_t* win, int32_t x, int32_t y);
    void (*on_mouse_move)(gui_window_t* win, int32_t x, int32_t y);
    void (*on_mouse_scroll)(gui_window_t* win, int32_t offset);
} gui_event_cb_t;

// GUI Backend callbacks
typedef struct {
    void (*remove)(gui_window_t* win);
    void (*draw)(gui_window_t* win);
    void (*poll)(gui_window_t* win);
    void (*set_title)(gui_window_t* win, const char* title);
    void (*grab_input)(gui_window_t* win, bool grab);
    void (*hide_cursor)(gui_window_t* win, bool hide);
    void (*set_fullscreen)(gui_window_t* win, bool fullscreen);
} gui_backend_cb_t;

// GUI Window handle
struct gui_window_t {
    // Window backend private data
    void* win_data;

    // Window private data
    void* data;

    // Scanout framebuffer
    fb_ctx_t fb;

    // TODO: Video RAM (May be optionally allocated by backend)
    void*  vram;
    size_t vram_size;
    bool   vram_anon;

    // TODO: Rid of this and store gui_backend_cb_t*
    void (*draw)(gui_window_t* win);
    void (*poll)(gui_window_t* win);
    void (*remove)(gui_window_t* win);
    void (*set_title)(gui_window_t* win, const char* title);
    void (*grab_input)(gui_window_t* win, bool grab);
    bool (*set_scanout)(gui_window_t* win, const fb_ctx_t* fb);
    void (*set_fullscreen)(gui_window_t* win, bool fullscreen);

    // TODO: Rid of this and store gui_event_cb_t*
    void (*on_close)(gui_window_t* win);
    void (*on_focus_lost)(gui_window_t* win);
    void (*on_paste)(gui_window_t* win, const char* str);
    void (*on_key_press)(gui_window_t* win, hid_key_t key);
    void (*on_key_release)(gui_window_t* win, hid_key_t key);
    void (*on_mouse_press)(gui_window_t* win, hid_btns_t btns);
    void (*on_mouse_release)(gui_window_t* win, hid_btns_t btns);
    void (*on_mouse_place)(gui_window_t* win, int32_t x, int32_t y);
    void (*on_mouse_move)(gui_window_t* win, int32_t x, int32_t y);
    void (*on_mouse_scroll)(gui_window_t* win, int32_t offset);

    void (*on_remove)(gui_window_t* win);
    void (*on_scanout)(gui_window_t* win);
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
 * GUI API (TODO)
 */

// Probe windowing backends and create a window
bool gui_window_init(gui_window_t* win, const fb_ctx_t* fb, size_t vram_size);

// Set window scanout framebuffer, preferably in VRAM
bool gui_window_set_scanout(gui_window_t* win, const fb_ctx_t* fb);

// Free GUI window
void gui_window_free(gui_window_t* win);

// Attach a framebuffer & HID mouse/keyboard to the VM. Returns false on failure.
PUBLIC bool gui_window_init_auto(rvvm_machine_t* machine, uint32_t width, uint32_t height);

/*
 * GUI Backend interfaces
 */

// Register GUI backend callbacks
static inline void gui_backend_register(gui_window_t* win, const gui_backend_cb_t* cb)
{
    if (win && cb) {
        // TODO: Store gui_backend_cb_t* in gui_window_t
        win->draw           = cb->draw;
        win->poll           = cb->poll;
        win->remove         = cb->remove;
        win->set_title      = cb->set_title;
        win->grab_input     = cb->grab_input;
        win->set_fullscreen = cb->set_fullscreen;
    }
}

// Handle GUI backend private data
static inline void gui_backend_set_data(gui_window_t* win, void* data)
{
    if (win) {
        win->win_data = data;
    }
}

static inline void* gui_backend_get_data(gui_window_t* win)
{
    return win ? win->win_data : NULL;
}

// Set VRAM buffer (XShm / wl_shm), GUI API user must place scanout inside VRAM
static inline void gui_backend_set_vram(gui_window_t* win, void* vram, size_t vram_size)
{
    // TODO: Proper VRAM handling
    if (win && vram && vram_size >= framebuffer_size(&win->fb)) {
        win->fb.buffer = vram;
    }
}

// Get VRAM buffer
static inline void* gui_backend_get_vram(gui_window_t* win)
{
    // TODO: Proper VRAM handling
    return win ? win->fb.buffer : NULL;
}

// Get requested VRAM size
static inline size_t gui_backend_get_vram_size(gui_window_t* win)
{
    // TODO: Proper VRAM handling
    return win ? framebuffer_size(&win->fb) : 0;
}

// Get current scanout context
static inline const fb_ctx_t* gui_backend_get_scanout(gui_window_t* win)
{
    // TODO: Proper scanout handling
    return win ? &win->fb : 0;
}

static inline void gui_backend_on_close(gui_window_t* win)
{
    if (win && win->on_close) {
        win->on_close(win);
    }
}

static inline void gui_backend_on_focus_lost(gui_window_t* win)
{
    if (win && win->on_focus_lost) {
        win->on_focus_lost(win);
    }
}

static inline void gui_backend_on_paste(gui_window_t* win, const char* str)
{
    if (win && win->on_paste) {
        win->on_paste(win, str);
    }
}

static inline void gui_backend_on_key_press(gui_window_t* win, hid_key_t key)
{
    if (win && win->on_key_press && key) {
        win->on_key_press(win, key);
    }
}

static inline void gui_backend_on_key_release(gui_window_t* win, hid_key_t key)
{
    if (win && win->on_key_release && key) {
        win->on_key_release(win, key);
    }
}

static inline void gui_backend_on_mouse_press(gui_window_t* win, hid_btns_t btns)
{
    if (win && win->on_mouse_press && btns) {
        win->on_mouse_press(win, btns);
    }
}

static inline void gui_backend_on_mouse_release(gui_window_t* win, hid_btns_t btns)
{
    if (win && win->on_mouse_release && btns) {
        win->on_mouse_release(win, btns);
    }
}

static inline void gui_backend_on_mouse_place(gui_window_t* win, int32_t x, int32_t y)
{
    if (win && win->on_mouse_place) {
        win->on_mouse_place(win, x, y);
    }
}

static inline void gui_backend_on_mouse_move(gui_window_t* win, int32_t x, int32_t y)
{
    if (win && win->on_mouse_move && (x || y)) {
        win->on_mouse_move(win, x, y);
    }
}

static inline void gui_backend_on_mouse_scroll(gui_window_t* win, int32_t offset)
{
    if (win && win->on_mouse_scroll && offset) {
        win->on_mouse_scroll(win, offset);
    }
}

#endif
