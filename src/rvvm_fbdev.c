/*
rvvm_fb.c - Framebuffer device handling
Copyright (C) 2020-2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <rvvm/rvvm_fb.h>

#include "atomics.h"
#include "compiler.h"
#include "spinlock.h"
#include "utils.h"

PUSH_OPTIMIZATION_SIZE

struct rvvm_fbdev {
    // Framebuffer device private data
    void* dev_data;

    // Framebuffer device callbacks
    const rvvm_fb_dev_cb_t* dev_cb;

    // Display private data
    void* display_data;

    // Display callbacks
    const rvvm_display_cb_t* display_cb;

    // VRAM region
    void*  vram;
    size_t vram_size;

    // Current video mode (Scanout)
    rvvm_fb_t fb;

    // Video mode switch lock
    spinlock_t lock;

    // Flip completion is tracked
    uint32_t flip_track;

    // Dirty state is tracked
    uint32_t dirty_track;

    // Dirty counter
    uint32_t dirty;

    // Reference count, starts at 0
    uint32_t ref;
};

RVVM_PUBLIC rvvm_fbdev_t* rvvm_fbdev_init(void)
{
    rvvm_fbdev_t* fbdev = safe_new_obj(rvvm_fbdev_t);
    return fbdev;
}

RVVM_PUBLIC void rvvm_fbdev_inc_ref(rvvm_fbdev_t* fbdev)
{
    if (fbdev) {
        atomic_add_uint32(&fbdev->ref, 1);
    }
}

RVVM_PUBLIC bool rvvm_fbdev_dec_ref(rvvm_fbdev_t* fbdev)
{
    if (fbdev && !atomic_sub_uint32(&fbdev->ref, 1)) {
        if (fbdev->display_cb && fbdev->display_cb->free) {
            fbdev->display_cb->free(fbdev);
        }
        safe_free(fbdev);
        return true;
    }
    return false;
}

RVVM_PUBLIC void rvvm_fbdev_register_dev(rvvm_fbdev_t* fbdev, const rvvm_fb_dev_cb_t* cb)
{
    if (fbdev) {
        fbdev->dev_cb = cb;
    }
}

RVVM_PUBLIC void rvvm_fbdev_set_dev_data(rvvm_fbdev_t* fbdev, void* data)
{
    if (fbdev) {
        fbdev->dev_data = data;
    }
}

RVVM_PUBLIC void* rvvm_fbdev_get_dev_data(rvvm_fbdev_t* fbdev)
{
    return fbdev ? fbdev->dev_data : NULL;
}

RVVM_PUBLIC void rvvm_fbdev_register_display(rvvm_fbdev_t* fbdev, const rvvm_display_cb_t* cb)
{
    if (fbdev) {
        fbdev->display_cb = cb;
    }
}

RVVM_PUBLIC void rvvm_fbdev_set_display_data(rvvm_fbdev_t* fbdev, void* data)
{
    if (fbdev) {
        fbdev->display_data = data;
    }
}

RVVM_PUBLIC void* rvvm_fbdev_get_display_data(rvvm_fbdev_t* fbdev)
{
    return fbdev ? fbdev->display_data : NULL;
}

RVVM_PUBLIC bool rvvm_fbdev_set_vram(rvvm_fbdev_t* fbdev, void* vram, size_t size)
{
    if (fbdev) {
        fbdev->vram      = vram;
        fbdev->vram_size = size;
        return true;
    }
    return false;
}

RVVM_PUBLIC void* rvvm_fbdev_get_vram(rvvm_fbdev_t* fbdev, size_t* size)
{
    if (fbdev) {
        if (size) {
            *size = fbdev->vram_size;
        }
        return fbdev->vram;
    }
    return NULL;
}

RVVM_PUBLIC bool rvvm_fbdev_set_scanout(rvvm_fbdev_t* fbdev, const rvvm_fb_t* fb)
{
    if (fbdev && fb) {
        bool ret = true;
        scoped_spin_lock (&fbdev->lock) {
            ret = ret && ((size_t)rvvm_fb_buffer(fb)) >= ((size_t)fbdev->vram);
            ret = ret
               && (((size_t)rvvm_fb_buffer(fb)) + rvvm_fb_size(fb)) //
                      <= (((size_t)fbdev->vram) + fbdev->vram_size);
            ret = ret || !fbdev->vram;
            if (ret) {
                fbdev->fb = *fb;
            }
        }
        atomic_store_uint32_relax(&fbdev->dirty, 0);
        return ret;
    }
    return false;
}

RVVM_PUBLIC bool rvvm_fbdev_get_scanout(rvvm_fbdev_t* fbdev, rvvm_fb_t* fb)
{
    if (fb) {
        *fb = (rvvm_fb_t)ZERO_INIT;
    }
    if (fbdev && fb) {
        scoped_spin_lock (&fbdev->lock) {
            *fb = fbdev->fb;
        }
        return true;
    }
    return false;
}

RVVM_PUBLIC void rvvm_fbdev_dirty(rvvm_fbdev_t* fbdev)
{
    // Enable dirty tracking
    if (fbdev) {
        atomic_store_uint32_relax(&fbdev->dirty_track, 1);
        atomic_store_uint32_relax(&fbdev->dirty, 0);
    }
}

RVVM_PUBLIC bool rvvm_fbdev_is_dirty(rvvm_fbdev_t* fbdev)
{
    if (fbdev && atomic_load_uint32_relax(&fbdev->dirty_track)) {
        // Dirty tracking is enabled
        uint32_t dirty = atomic_add_uint32(&fbdev->dirty, 1);
        return dirty < 2 || !(dirty & 0xF);
    }
    return true;
}

RVVM_PUBLIC void rvvm_fbdev_update(rvvm_fbdev_t* fbdev)
{
    if (fbdev) {
        if (fbdev->display_cb) {
            // Poll for input
            if (fbdev->display_cb->poll) {
                fbdev->display_cb->poll(fbdev);
            }
            // Redraw if scanout is dirty
            if (fbdev->display_cb->draw && rvvm_fbdev_is_dirty(fbdev)) {
                fbdev->display_cb->draw(fbdev);
            }
        }
        if (fbdev->dev_cb) {
            // Acknowledge flip if manual flip tracking isn't used
            if (fbdev->dev_cb->flip_done && !atomic_load_uint32_relax(&fbdev->flip_track)) {
                fbdev->dev_cb->flip_done(fbdev);
            }
        }
    }
}

RVVM_PUBLIC void rvvm_fbdev_flip_done(rvvm_fbdev_t* fbdev)
{
    // Enable flip tracking, invoke callback
    if (fbdev && fbdev->dev_cb && fbdev->dev_cb->flip_done) {
        atomic_store_uint32_relax(&fbdev->flip_track, 1);
        fbdev->dev_cb->flip_done(fbdev);
    }
}

POP_OPTIMIZATION_SIZE
