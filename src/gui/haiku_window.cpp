/*
haiku_window.cpp - Haiku GUI Window
Copyright (C) 2022  X547 <github.com/X547>
                    LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "feature_test.h"

extern "C" {
#include "compiler.h"
#include "gui_window.h"
}

PUSH_OPTIMIZATION_SIZE

#if defined(HOST_TARGET_HAIKU)

extern "C" {
#include "utils.h"
}

#include <Application.h>
#include <Bitmap.h>
#include <Cursor.h>
#include <OS.h>
#include <View.h>
#include <Window.h>
#include <private/shared/AutoDeleter.h>

/*
 * C++ doesn't allow designated initializers like in win32window... Ugh
 * Luckily, Haiku has contiguous & small keycodes for a trivial array initializer
 *
 * Don't ever touch this table, or The Order will not take kindly.
 * Otherwise be prepared to suffer the Consequences...
 */
static const hid_key_t haiku_key_to_hid_byte_map[] = {
    // clang-format off
    HID_KEY_NONE,
    HID_KEY_ESC,
    HID_KEY_F1,
    HID_KEY_F2,
    HID_KEY_F3,
    HID_KEY_F4,
    HID_KEY_F5,
    HID_KEY_F6,
    HID_KEY_F7,
    HID_KEY_F8,
    HID_KEY_F9,
    HID_KEY_F10,
    HID_KEY_F11,
    HID_KEY_F12,
    HID_KEY_SYSRQ,
    HID_KEY_SCROLLLOCK,
    HID_KEY_PAUSE,
    HID_KEY_GRAVE,
    HID_KEY_1,
    HID_KEY_2,
    HID_KEY_3,
    HID_KEY_4,
    HID_KEY_5,
    HID_KEY_6,
    HID_KEY_7,
    HID_KEY_8,
    HID_KEY_9,
    HID_KEY_0,
    HID_KEY_MINUS,
    HID_KEY_EQUAL,
    HID_KEY_BACKSPACE,
    HID_KEY_INSERT,
    HID_KEY_HOME,
    HID_KEY_PAGEUP,
    HID_KEY_NUMLOCK,
    HID_KEY_KPSLASH,
    HID_KEY_KPASTERISK,
    HID_KEY_KPMINUS,
    HID_KEY_TAB,
    HID_KEY_Q,
    HID_KEY_W,
    HID_KEY_E,
    HID_KEY_R,
    HID_KEY_T,
    HID_KEY_Y,
    HID_KEY_U,
    HID_KEY_I,
    HID_KEY_O,
    HID_KEY_P,
    HID_KEY_LEFTBRACE,
    HID_KEY_RIGHTBRACE,
    HID_KEY_BACKSLASH,
    HID_KEY_DELETE,
    HID_KEY_END,
    HID_KEY_PAGEDOWN,
    HID_KEY_KP7,
    HID_KEY_KP8,
    HID_KEY_KP9,
    HID_KEY_KPPLUS,
    HID_KEY_CAPSLOCK,
    HID_KEY_A,
    HID_KEY_S,
    HID_KEY_D,
    HID_KEY_F,
    HID_KEY_G,
    HID_KEY_H,
    HID_KEY_J,
    HID_KEY_K,
    HID_KEY_L,
    HID_KEY_SEMICOLON,
    HID_KEY_APOSTROPHE,
    HID_KEY_ENTER,
    HID_KEY_KP4,
    HID_KEY_KP5,
    HID_KEY_KP6,
    HID_KEY_LEFTSHIFT,
    HID_KEY_Z,
    HID_KEY_X,
    HID_KEY_C,
    HID_KEY_V,
    HID_KEY_B,
    HID_KEY_N,
    HID_KEY_M,
    HID_KEY_COMMA,
    HID_KEY_DOT,
    HID_KEY_SLASH,
    HID_KEY_RIGHTSHIFT,
    HID_KEY_UP,
    HID_KEY_KP1,
    HID_KEY_KP2,
    HID_KEY_KP3,
    HID_KEY_KPENTER,
    HID_KEY_LEFTCTRL,
    HID_KEY_LEFTALT,
    HID_KEY_SPACE,
    HID_KEY_RIGHTALT,
    HID_KEY_RIGHTCTRL,
    HID_KEY_LEFT,
    HID_KEY_DOWN,
    HID_KEY_RIGHT,
    HID_KEY_KP0,
    HID_KEY_KPDOT,
    HID_KEY_LEFTMETA,
    HID_KEY_RIGHTMETA,
    HID_KEY_COMPOSE,
    HID_KEY_102ND,
    HID_KEY_YEN,
    HID_KEY_RO,
    HID_KEY_MUHENKAN,
    HID_KEY_HENKAN,
    HID_KEY_KATAKANAHIRAGANA,
    HID_KEY_NONE, // Haiku keycode 0x6f unused?
    HID_KEY_KPCOMMA,
    // 0xf0 hangul?
    // 0xf1 hanja?
    // clang-format on
};

static hid_key_t haiku_key_to_hid(uint32_t haiku_key)
{
    if (haiku_key < sizeof(haiku_key_to_hid_byte_map)) {
        return haiku_key_to_hid_byte_map[haiku_key];
    }
    rvvm_warn("Unmapped Haiku keycode %x", haiku_key);
    return HID_KEY_NONE;
}

class HaikuGUIView : public BView {
private:
    gui_window_t* m_win;

public:
    HaikuGUIView(BRect frame, gui_window_t* win);

    virtual ~HaikuGUIView(void) {}

    void AttachedToWindow(void) override;
    void Draw(BRect dirty) override;
    void MessageReceived(BMessage* msg) override;
    void WindowActivated(bool active) override;
};

class HaikuGUIWindow : public BWindow {
private:
    gui_window_t* m_win;
    HaikuGUIView* m_view;

    // Bitmap data area (VRAM)
    area_id m_area;
    void*   m_addr;

    // Current scanout bitmap handle
    ObjectDeleter<BBitmap> m_bitmap;

    // Scanout position
    BPoint m_pos;

    // Last scanout context
    rvvm_fb_t m_fb;

public:
    HaikuGUIWindow(BRect frame, const char* title, gui_window_t* win, area_id area, void* addr);

    virtual ~HaikuGUIWindow(void) {}

    bool QuitRequested(void) override;

    BBitmap* CreateBitmapFromScanout(const rvvm_fb_t* fb);

    void SetScanoutBitmap(const rvvm_fb_t* fb);

    BBitmap* GetScanoutBitmap(void);

    void SetScanoutPosition(BPoint pos);

    BPoint GetScanoutPosition(void);

    HaikuGUIView* GetView(void) const
    {
        return m_view;
    }

    area_id GetArea(void) const
    {
        return m_area;
    }
};

class HaikuGUIApp : public BApplication {
public:
    HaikuGUIApp(const char* signature) : BApplication(signature) {}

    virtual ~HaikuGUIApp(void) {}

    thread_id Run(void) override;
};

HaikuGUIView::HaikuGUIView(BRect frame, gui_window_t* win)
    : BView(frame, NULL, B_FOLLOW_ALL_SIDES, B_WILL_DRAW), m_win(win)
{
    SetViewColor(B_TRANSPARENT_COLOR);
    SetLowColor(0, 0, 0);
}

void HaikuGUIView::AttachedToWindow(void)
{
    BCursor cursor(B_CURSOR_ID_NO_CURSOR);
    SetViewCursor(&cursor);
}

void HaikuGUIView::Draw(BRect dirty)
{
    HaikuGUIWindow* hwin   = (HaikuGUIWindow*)gui_backend_get_data(m_win);
    BBitmap*        bitmap = hwin->GetScanoutBitmap();
    UNUSED(dirty);
    if (bitmap) {
        DrawBitmap(bitmap, hwin->GetScanoutPosition());
    }
}

void HaikuGUIView::MessageReceived(BMessage* msg)
{
    int32  key   = 0;
    int32  btns  = 0;
    BPoint point = {};
    float  wheel = 0.f;

    switch (msg->what) {
        case B_KEY_DOWN:
        case B_UNMAPPED_KEY_DOWN:
            msg->FindInt32("key", &key);
            gui_backend_on_key_press(m_win, haiku_key_to_hid(key));
            return;
        case B_KEY_UP:
        case B_UNMAPPED_KEY_UP:
            msg->FindInt32("key", &key);
            gui_backend_on_key_release(m_win, haiku_key_to_hid(key));
            return;
        case B_MOUSE_DOWN:
            SetMouseEventMask(B_POINTER_EVENTS);
            msg->FindInt32("buttons", &btns);
            gui_backend_on_mouse_press(m_win, btns);
            return;
        case B_MOUSE_UP:
            msg->FindInt32("buttons", &btns);
            gui_backend_on_mouse_release(m_win, ~btns);
            return;
        case B_MOUSE_MOVED:
            msg->FindPoint("where", &point);
            gui_backend_on_mouse_place(m_win, (int32_t)point.x, (int32_t)point.y);
            return;
        case B_MOUSE_WHEEL_CHANGED:
            msg->FindFloat("be:wheel_delta_y", &wheel);
            gui_backend_on_mouse_scroll(m_win, (int32_t)wheel);
            return;
    }

    BView::MessageReceived(msg);
}

void HaikuGUIView::WindowActivated(bool active)
{
    if (!active) {
        gui_backend_on_focus_lost(m_win);
    }
}

HaikuGUIWindow::HaikuGUIWindow(BRect frame, const char* title, gui_window_t* win, area_id area, void* addr)
    : BWindow(frame, title, B_TITLED_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL, B_NOT_ZOOMABLE | B_NOT_RESIZABLE), //
      m_win(win), m_area(area), m_addr(addr)
{
    gui_backend_set_data(win, this);
    m_view = new HaikuGUIView(frame.OffsetToCopy(B_ORIGIN), win);
    AddChild(m_view);
    m_view->MakeFocus();
}

bool HaikuGUIWindow::QuitRequested(void)
{
    gui_backend_on_close(m_win);
    return false;
}

BBitmap* HaikuGUIWindow::CreateBitmapFromScanout(const rvvm_fb_t* fb)
{
    if (rvvm_fb_buffer(fb)) {
        BRect       fb_rect(0, 0, rvvm_fb_width(fb) - 1, rvvm_fb_height(fb) - 1);
        size_t      fb_off = ((size_t)rvvm_fb_buffer(fb)) - ((size_t)m_addr);
        color_space fb_fmt = B_RGB32;
        switch (rvvm_fb_format(fb)) {
            case RVVM_RGB_XRGB1555:
                fb_fmt = B_RGB15;
                break;
            case RVVM_RGB_RGB565:
                fb_fmt = B_RGB16;
                break;
            case RVVM_RGB_RGB888:
                fb_fmt = B_RGB24;
                break;
            case RVVM_RGB_XRGB8888:
                fb_fmt = B_RGB32;
                break;
            case RVVM_RGB_BGR888:
                fb_fmt = B_RGB24_BIG;
                break;
            case RVVM_RGB_BGRX8888:
                fb_fmt = B_RGB32_BIG;
                break;
            default:
                // TODO: XBGR8888, RGBX8888, XRGB2101010, XBGR2101010 pixel formats?
                return NULL;
        }
        return new BBitmap(m_area, fb_off, fb_rect, 0, fb_fmt, rvvm_fb_stride(fb));
    }
    return NULL;
}

void HaikuGUIWindow::SetScanoutBitmap(const rvvm_fb_t* fb)
{
    // Recreate bitmap if needed
    if (!rvvm_fb_same_scanout(fb, &m_fb)) {
        BBitmap* new_bitmap = CreateBitmapFromScanout(fb);
        if (new_bitmap) {
            m_bitmap.SetTo(new_bitmap);
            m_fb = *fb;
        }
    }
}

BBitmap* HaikuGUIWindow::GetScanoutBitmap(void)
{
    return m_bitmap.Get();
}

void HaikuGUIWindow::SetScanoutPosition(BPoint pos)
{
    m_pos = pos;
}

BPoint HaikuGUIWindow::GetScanoutPosition(void)
{
    return m_pos;
}

// Override BApplication::Run() to run a background BLooper thread instead of blocking
thread_id HaikuGUIApp::Run(void)
{
    return BLooper::Run();
}

static bool haiku_global_init(void)
{
    DO_ONCE_SCOPED {
        if (!be_app) {
            be_app = new HaikuGUIApp("application/x-vnd.RVVM");
        }
        if (be_app) {
            // Run a BLooper event thread
            // NOTE: Do not quit it, it's part of Be toolkit anyways
            be_app->Lock();
            be_app->Run();
        } else {
            rvvm_error("Failed to create BApplication");
        }
    }
    return !!be_app;
}

static void haiku_window_free(gui_window_t* win)
{
    HaikuGUIWindow* hwin = (HaikuGUIWindow*)gui_backend_get_data(win);
    if (hwin) {
        HaikuGUIView* view = hwin->GetView();
        area_id       area = hwin->GetArea();
        view->LockLooper();
        // BWindow::Quit() also deletes BWindow
        hwin->Quit();
        delete_area(area);
    }
    gui_backend_set_data(win, NULL);
}

static void haiku_window_draw(gui_window_t* win, const rvvm_fb_t* fb, uint32_t x, uint32_t y)
{
    HaikuGUIWindow* hwin = (HaikuGUIWindow*)gui_backend_get_data(win);
    HaikuGUIView*   view = hwin->GetView();
    // NOTE: Is this doing a synchronous flip or just notifies app thread?
    // GUI user expects sync draw for future ack (interrupt) on flip
    view->LockLooper();
    hwin->SetScanoutBitmap(fb);
    hwin->SetScanoutPosition(BPoint(x, y));
    view->Invalidate();
    view->UnlockLooper();
}

static void haiku_window_set_title(gui_window_t* win, const char* title)
{
    HaikuGUIWindow* hwin = (HaikuGUIWindow*)gui_backend_get_data(win);
    hwin->SetTitle(title);
}

static void haiku_window_set_win_size(gui_window_t* win, uint32_t w, uint32_t h)
{
    HaikuGUIWindow* hwin = (HaikuGUIWindow*)gui_backend_get_data(win);
    hwin->ResizeTo(w - 1, h - 1);
}

// TODO: haiku_window_grab_input() - no implementation in SDL either :O
static const gui_backend_cb_t haiku_window_cb = {
    .free         = haiku_window_free,
    .draw         = haiku_window_draw,
    .set_title    = haiku_window_set_title,
    .set_win_size = haiku_window_set_win_size,
};

bool haiku_window_init(gui_window_t* win)
{
    gui_backend_register(win, &haiku_window_cb);

    // Perform global init
    if (!haiku_global_init()) {
        return false;
    }

    // Create VRAM area
    size_t  vram_size = gui_backend_get_vram_size(win);
    void*   vram_addr = NULL;
    area_id vram_area = create_area("vram_area", &vram_addr, B_ANY_ADDRESS, vram_size, //
                                    B_NO_LOCK, B_CLONEABLE_AREA | B_READ_AREA | B_WRITE_AREA);
    if (vram_addr) {
        gui_backend_set_vram(win, vram_addr, vram_size);
    } else {
        rvvm_error("Failed to allocate VRAM area");
        return false;
    }

    HaikuGUIWindow* hwin = new HaikuGUIWindow( //
        BRect(0, 0, gui_window_width(win) - 1, gui_window_height(win) - 1), "", win, vram_area, vram_addr);
    hwin->CenterOnScreen();
    hwin->Show();

    return true;
}

#else

bool haiku_window_init(gui_window_t* win)
{
    UNUSED(win);
    return false;
}

#endif

POP_OPTIMIZATION_SIZE
