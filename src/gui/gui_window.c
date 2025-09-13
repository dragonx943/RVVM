/*
gui_window.c - GUI Window
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "gui_window.h"
#include "utils.h"

PUSH_OPTIMIZATION_SIZE

#if defined(USE_GUI)

/*
 * GUI backend
 */

static void gui_backend_free(gui_window_t* win)
{
    if (win) {
        if (win->bknd_cb) {
            if (win->bknd_cb->remove) {
                win->bknd_cb->remove(win);
            }
        }
        win->bknd_data = NULL;
    }
}

static bool gui_backend_init(gui_window_t* win)
{
    const char* gui = rvvm_getarg("gui");
#if defined(USE_WIN32_GUI)
    if ((!gui || rvvm_strcmp(gui, "win32")) && win32_window_init(win)) {
        return true;
    }
    gui_backend_free(win);
#endif
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
    rvvm_error("No suitable windowing backends found!");
    UNUSED(win);
    UNUSED(gui);
    return false;
}

/*
 * GUI Window
 */

static void gui_display_draw(rvvm_fbdev_t* fbdev)
{
    gui_window_t* win = rvvm_fbdev_get_display_data(fbdev);
    gui_window_draw(win);
}

static void gui_display_poll(rvvm_fbdev_t* fbdev)
{
    gui_window_t* win = rvvm_fbdev_get_display_data(fbdev);
    gui_window_poll(win);
}

static void gui_display_free(rvvm_fbdev_t* fbdev)
{
    gui_window_t* win = rvvm_fbdev_get_display_data(fbdev);
    gui_window_free(win);
}

static const rvvm_display_cb_t gui_display_cb = {
    .draw = gui_display_draw,
    .poll = gui_display_poll,
    .free = gui_display_free,
};

static const gui_event_cb_t gui_dummy_event_cb = ZERO_INIT;

gui_window_t* gui_window_init_bare(size_t vram_size, const rvvm_fb_t* fb)
{
    rvvm_fb_t tmp = {
        .width  = 640,
        .height = 480,
        .format = RVVM_RGB_XRGB8888,
    };
    gui_window_t* win = safe_new_obj(gui_window_t);

    // Normalize inputs
    if (rvvm_fb_size(fb)) {
        tmp = *fb;
    }
    if (vram_size < rvvm_fb_size(&tmp)) {
        vram_size = rvvm_fb_size(&tmp);
    }

    // Create fbdev
    win->fbdev = rvvm_fbdev_init();
    rvvm_fbdev_register_display(win->fbdev, &gui_display_cb);
    rvvm_fbdev_set_display_data(win->fbdev, win);
    rvvm_fbdev_set_vram(win->fbdev, NULL, vram_size);
    rvvm_fbdev_set_scanout(win->fbdev, &tmp);

    // Register dummy event callbacks
    gui_window_register(win, &gui_dummy_event_cb);
    return win;
}

gui_window_t* gui_window_init(size_t vram_size, const rvvm_fb_t* fb)
{
    gui_window_t* win = gui_window_init_bare(vram_size, fb);
    if (gui_backend_init(win)) {
        // Initialize scanout in VRAM
        rvvm_fb_t tmp = ZERO_INIT;
        rvvm_fbdev_get_scanout(win->fbdev, &tmp);
        tmp.buffer = rvvm_fbdev_get_vram(win->fbdev, NULL);
        rvvm_fbdev_set_scanout(win->fbdev, &tmp);
        return win;
    }
    gui_window_free(win);
    return NULL;
}

void gui_window_free(gui_window_t* win)
{
    if (win) {
        gui_backend_free(win);
        if (win->ev_cb->remove) {
            win->ev_cb->remove(win);
        }
        rvvm_fbdev_dec_ref(win->fbdev);
        safe_free(win);
    }
}

void gui_window_poll(gui_window_t* win)
{
    if (win) {
        if (win->ev_cb->update) {
            win->ev_cb->update(win);
        }
        if (win->bknd_cb->poll) {
            win->bknd_cb->poll(win);
        }
    }
}

void gui_window_draw(gui_window_t* win)
{
    if (win) {
        if (win->bknd_cb->draw) {
            win->bknd_cb->draw(win);
        }
    }
}

#else

gui_window_t* gui_window_init_bare(size_t vram_size, const rvvm_fb_t* fb)
{
    UNUSED(vram_size && fb);
    return NULL;
}

gui_window_t* gui_window_init(size_t vram_size, const rvvm_fb_t* fb)
{
    UNUSED(vram_size && fb);
    return NULL;
}

void gui_window_free(gui_window_t* win)
{
    UNUSED(win);
}

void gui_window_poll(gui_window_t* win)
{
    UNUSED(win);
}

void gui_window_draw(gui_window_t* win)
{
    UNUSED(win);
}

#endif

POP_OPTIMIZATION_SIZE
