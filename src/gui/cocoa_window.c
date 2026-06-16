/*
cocoa_window.c - Cocoa (macOS / AppKit) GUI Window
Copyright (C) 2026  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "feature_test.h"

#include "compiler.h"
#include "gui_window.h"
#include "utils.h"
#include "vma_ops.h"

PUSH_OPTIMIZATION_SIZE

#if defined(USE_GUI) && defined(USE_COCOA_GUI) && defined(HOST_TARGET_APPLE)

/*
 * Pure-C AppKit backend: no Objective-C source, no ObjC compiler, no build
 * system changes beyond linking the frameworks. We drive AppKit entirely
 * through the Objective-C runtime (objc_getClass / sel_registerName /
 * objc_msgSend) and synthesize the two needed subclasses (RVVMView : NSView,
 * RVVMWindow : NSWindow) at runtime via objc_allocateClassPair. All the
 * drawing is plain CoreGraphics C API, so only the AppKit glue is "runtime".
 *
 * AppKit insists on running on the main thread, and RVVM conveniently drives
 * the display poll()/draw() callbacks from rvvm_run_eventloop() on the main
 * thread. So instead of blocking in [NSApp run], we manually pump the event
 * queue from poll() and blit the framebuffer from draw().
 */

#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/runtime.h>

// Foundation/AppKit scalar typedefs (LP64: macOS is the only target here)
typedef unsigned long NSUInteger;
typedef long          NSInteger;

// AppKit enum constants we use, hardcoded so we needn't pull in any headers
#define NSApplicationActivationPolicyRegular 0L

#define NSWindowStyleMaskTitled         (1UL << 0)
#define NSWindowStyleMaskClosable       (1UL << 1)
#define NSWindowStyleMaskMiniaturizable (1UL << 2)
#define NSWindowStyleMaskResizable      (1UL << 3)
#define NSWindowStyleMaskFullScreen     (1UL << 14)

#define NSBackingStoreBuffered 2UL

#define NSViewWidthSizable  (1UL << 1)
#define NSViewHeightSizable (1UL << 4)

#define NSEventMaskAny (~0UL)

// NSDefaultRunLoopMode is an extern NSString* const; the symbol is resolved at
// link time from Foundation, so we just forward-declare it as an id.
extern id NSDefaultRunLoopMode;

// libobjc autorelease pool primitives (public C ABI, declared here to avoid
// dragging in <objc/NSObjCRuntime.h>)
extern void* objc_autoreleasePoolPush(void);
extern void  objc_autoreleasePoolPop(void* pool);

// Type encoding for NSRect / CGRect (used when registering struct-arg methods)
#define ENC_RECT "{CGRect={CGPoint=dd}{CGSize=dd}}"

// BOOL encoding must match the runtime's BOOL (bool on arm64, signed char on x86_64)
#if defined(OBJC_BOOL_IS_BOOL) && OBJC_BOOL_IS_BOOL
#define ENC_BOOL "B"
#else
#define ENC_BOOL "c"
#endif

// Class / selector shorthand
#define C(name) ((id)objc_getClass(name))
#define S(name) sel_registerName(name)

/*
 * objc_msgSend must be called through a function pointer cast to the EXACT
 * prototype of the method - the variadic declaration in <objc/message.h> has
 * the wrong ABI on arm64. These typed wrappers keep the casts in one place.
 */
static inline id msg(id self, SEL op)
{
    return ((id (*)(id, SEL))objc_msgSend)(self, op);
}
static inline void msgv(id self, SEL op)
{
    ((void (*)(id, SEL))objc_msgSend)(self, op);
}
static inline void msgv_o(id self, SEL op, id a)
{
    ((void (*)(id, SEL, id))objc_msgSend)(self, op, a);
}
static inline void msgv_b(id self, SEL op, BOOL a)
{
    ((void (*)(id, SEL, BOOL))objc_msgSend)(self, op, a);
}
static inline void msgv_l(id self, SEL op, NSInteger a)
{
    ((void (*)(id, SEL, NSInteger))objc_msgSend)(self, op, a);
}
static inline void msgv_u(id self, SEL op, NSUInteger a)
{
    ((void (*)(id, SEL, NSUInteger))objc_msgSend)(self, op, a);
}
static inline void msgv_sz(id self, SEL op, CGSize a)
{
    ((void (*)(id, SEL, CGSize))objc_msgSend)(self, op, a);
}
static inline void msgv_rect_b(id self, SEL op, CGRect r, BOOL b)
{
    ((void (*)(id, SEL, CGRect, BOOL))objc_msgSend)(self, op, r, b);
}
static inline NSInteger msg_l(id self, SEL op)
{
    return ((NSInteger (*)(id, SEL))objc_msgSend)(self, op);
}
static inline NSUInteger msg_u(id self, SEL op)
{
    return ((NSUInteger (*)(id, SEL))objc_msgSend)(self, op);
}
static inline double msg_dbl(id self, SEL op)
{
    return ((double (*)(id, SEL))objc_msgSend)(self, op);
}
static inline BOOL msg_bool(id self, SEL op)
{
    return ((BOOL (*)(id, SEL))objc_msgSend)(self, op);
}
static inline unsigned short msg_ushort(id self, SEL op)
{
    return ((unsigned short (*)(id, SEL))objc_msgSend)(self, op);
}

// CGPoint is two doubles (16 bytes) - returned in registers on every ABI, so
// plain objc_msgSend is correct.
static inline CGPoint msg_point(id self, SEL op)
{
    return ((CGPoint (*)(id, SEL))objc_msgSend)(self, op);
}
static inline CGPoint msg_convert_point(id self, SEL op, CGPoint p, id view)
{
    return ((CGPoint (*)(id, SEL, CGPoint, id))objc_msgSend)(self, op, p, view);
}

/*
 * CGRect (NSRect) is 32 bytes. On x86_64 SysV it is returned via the hidden
 * struct-pointer ABI, i.e. objc_msgSend_stret. On arm64 struct returns go
 * through the unified objc_msgSend and objc_msgSend_stret does not exist.
 */
static inline CGRect msg_rect(id self, SEL op)
{
#if defined(__x86_64__)
    return ((CGRect (*)(id, SEL))objc_msgSend_stret)(self, op);
#else
    return ((CGRect (*)(id, SEL))objc_msgSend)(self, op);
#endif
}

// Device-dependent modifier flag bits (NX_DEVICE*KEYMASK), used to tell apart
// left/right modifier keys, which the public NSEventModifierFlag* masks can't.
#define COCOA_MOD_LCTRL  0x00000001u
#define COCOA_MOD_LSHIFT 0x00000002u
#define COCOA_MOD_RSHIFT 0x00000004u
#define COCOA_MOD_LMETA  0x00000008u
#define COCOA_MOD_RMETA  0x00000010u
#define COCOA_MOD_LALT   0x00000020u
#define COCOA_MOD_RALT   0x00000040u
#define COCOA_MOD_RCTRL  0x00002000u
#define COCOA_MOD_CAPS   0x00010000u // NSEventModifierFlagCapsLock

/*
 * macOS virtual keycode (kVK_*) -> USB HID usage translation.
 * Keycodes are layout-independent positions, so this table is fixed.
 */
static hid_key_t cocoa_key_to_hid(uint16_t key)
{
    switch (key) {
        // clang-format off
        case 0x00: return HID_KEY_A;
        case 0x0B: return HID_KEY_B;
        case 0x08: return HID_KEY_C;
        case 0x02: return HID_KEY_D;
        case 0x0E: return HID_KEY_E;
        case 0x03: return HID_KEY_F;
        case 0x05: return HID_KEY_G;
        case 0x04: return HID_KEY_H;
        case 0x22: return HID_KEY_I;
        case 0x26: return HID_KEY_J;
        case 0x28: return HID_KEY_K;
        case 0x25: return HID_KEY_L;
        case 0x2E: return HID_KEY_M;
        case 0x2D: return HID_KEY_N;
        case 0x1F: return HID_KEY_O;
        case 0x23: return HID_KEY_P;
        case 0x0C: return HID_KEY_Q;
        case 0x0F: return HID_KEY_R;
        case 0x01: return HID_KEY_S;
        case 0x11: return HID_KEY_T;
        case 0x20: return HID_KEY_U;
        case 0x09: return HID_KEY_V;
        case 0x0D: return HID_KEY_W;
        case 0x07: return HID_KEY_X;
        case 0x10: return HID_KEY_Y;
        case 0x06: return HID_KEY_Z;

        case 0x12: return HID_KEY_1;
        case 0x13: return HID_KEY_2;
        case 0x14: return HID_KEY_3;
        case 0x15: return HID_KEY_4;
        case 0x17: return HID_KEY_5;
        case 0x16: return HID_KEY_6;
        case 0x1A: return HID_KEY_7;
        case 0x1C: return HID_KEY_8;
        case 0x19: return HID_KEY_9;
        case 0x1D: return HID_KEY_0;

        case 0x24: return HID_KEY_ENTER;
        case 0x35: return HID_KEY_ESC;
        case 0x33: return HID_KEY_BACKSPACE;
        case 0x30: return HID_KEY_TAB;
        case 0x31: return HID_KEY_SPACE;
        case 0x1B: return HID_KEY_MINUS;
        case 0x18: return HID_KEY_EQUAL;
        case 0x21: return HID_KEY_LEFTBRACE;
        case 0x1E: return HID_KEY_RIGHTBRACE;
        case 0x2A: return HID_KEY_BACKSLASH;
        case 0x29: return HID_KEY_SEMICOLON;
        case 0x27: return HID_KEY_APOSTROPHE;
        case 0x32: return HID_KEY_GRAVE;
        case 0x2B: return HID_KEY_COMMA;
        case 0x2F: return HID_KEY_DOT;
        case 0x2C: return HID_KEY_SLASH;
        case 0x39: return HID_KEY_CAPSLOCK;
        case 0x0A: return HID_KEY_102ND; // ISO § / ± key

        case 0x7A: return HID_KEY_F1;
        case 0x78: return HID_KEY_F2;
        case 0x63: return HID_KEY_F3;
        case 0x76: return HID_KEY_F4;
        case 0x60: return HID_KEY_F5;
        case 0x61: return HID_KEY_F6;
        case 0x62: return HID_KEY_F7;
        case 0x64: return HID_KEY_F8;
        case 0x65: return HID_KEY_F9;
        case 0x6D: return HID_KEY_F10;
        case 0x67: return HID_KEY_F11;
        case 0x6F: return HID_KEY_F12;
        case 0x69: return HID_KEY_F13;
        case 0x6B: return HID_KEY_F14;
        case 0x71: return HID_KEY_F15;
        case 0x6A: return HID_KEY_F16;
        case 0x40: return HID_KEY_F17;
        case 0x4F: return HID_KEY_F18;
        case 0x50: return HID_KEY_F19;
        case 0x5A: return HID_KEY_F20;

        case 0x72: return HID_KEY_INSERT; // Help
        case 0x73: return HID_KEY_HOME;
        case 0x74: return HID_KEY_PAGEUP;
        case 0x75: return HID_KEY_DELETE; // Forward delete
        case 0x77: return HID_KEY_END;
        case 0x79: return HID_KEY_PAGEDOWN;
        case 0x7B: return HID_KEY_LEFT;
        case 0x7C: return HID_KEY_RIGHT;
        case 0x7D: return HID_KEY_DOWN;
        case 0x7E: return HID_KEY_UP;

        case 0x47: return HID_KEY_NUMLOCK; // Keypad clear
        case 0x4B: return HID_KEY_KPSLASH;
        case 0x43: return HID_KEY_KPASTERISK;
        case 0x4E: return HID_KEY_KPMINUS;
        case 0x45: return HID_KEY_KPPLUS;
        case 0x4C: return HID_KEY_KPENTER;
        case 0x53: return HID_KEY_KP1;
        case 0x54: return HID_KEY_KP2;
        case 0x55: return HID_KEY_KP3;
        case 0x56: return HID_KEY_KP4;
        case 0x57: return HID_KEY_KP5;
        case 0x58: return HID_KEY_KP6;
        case 0x59: return HID_KEY_KP7;
        case 0x5B: return HID_KEY_KP8;
        case 0x5C: return HID_KEY_KP9;
        case 0x52: return HID_KEY_KP0;
        case 0x41: return HID_KEY_KPDOT;
        case 0x51: return HID_KEY_KPEQUAL;

        case 0x4A: return HID_KEY_MUTE;
        case 0x48: return HID_KEY_VOLUMEUP;
        case 0x49: return HID_KEY_VOLUMEDOWN;

        case 0x5D: return HID_KEY_YEN;              // JIS ¥
        case 0x5E: return HID_KEY_RO;               // JIS _
        case 0x68: return HID_KEY_KATAKANAHIRAGANA; // JIS Kana
        case 0x66: return HID_KEY_MUHENKAN;         // JIS Eisu

        // Modifiers (also delivered via flagsChanged: handling)
        case 0x3B: return HID_KEY_LEFTCTRL;
        case 0x3E: return HID_KEY_RIGHTCTRL;
        case 0x38: return HID_KEY_LEFTSHIFT;
        case 0x3C: return HID_KEY_RIGHTSHIFT;
        case 0x3A: return HID_KEY_LEFTALT;
        case 0x3D: return HID_KEY_RIGHTALT;
        case 0x37: return HID_KEY_LEFTMETA;
        case 0x36: return HID_KEY_RIGHTMETA;
        // clang-format on
        default:
            return HID_KEY_NONE;
    }
}

typedef struct {
    id              window; // RVVMWindow*
    id              view;   // RVVMView*
    gui_window_t*   win;
    CGColorSpaceRef colorspace;

    // Last scanout context (set from draw(), read from RVVMView drawRect:)
    const void* buffer;
    uint32_t    width;
    uint32_t    height;
    uint32_t    stride;
    uint32_t    pos_x;
    uint32_t    pos_y;

    bool cursor_hidden;
    bool grab;
} cocoa_window_t;

// Synthesized classes and the offset of their `ctx` ivar (cached at register time)
static Class     g_view_class;
static Class     g_win_class;
static ptrdiff_t g_view_ctx_off;
static ptrdiff_t g_win_ctx_off;
static id        g_app;

static cocoa_window_t* view_ctx(id self)
{
    return *(cocoa_window_t**)(void*)((char*)self + g_view_ctx_off);
}

static cocoa_window_t* win_ctx(id self)
{
    return *(cocoa_window_t**)(void*)((char*)self + g_win_ctx_off);
}

/*
 * RVVMView method implementations (NSView subclass)
 */

// Shared YES-returning IMP for isOpaque / acceptsFirstResponder /
// acceptsFirstMouse: / canBecomeKeyWindow / canBecomeMainWindow.
// The trailing event arg of acceptsFirstMouse: is simply ignored.
static BOOL imp_return_yes(id self, SEL _cmd)
{
    UNUSED(self);
    UNUSED(_cmd);
    return YES;
}

static void imp_drawRect(id self, SEL _cmd, CGRect dirty)
{
    UNUSED(_cmd);
    UNUSED(dirty);
    cocoa_window_t* cocoa = view_ctx(self);

    id           gctx = msg(C("NSGraphicsContext"), S("currentContext"));
    CGContextRef ctx  = ((CGContextRef (*)(id, SEL))objc_msgSend)(gctx, S("CGContext"));
    CGRect       bnds = msg_rect(self, S("bounds"));
    CGFloat      vh   = bnds.size.height;

    // Letterbox background
    CGContextSetRGBFillColor(ctx, 0.0, 0.0, 0.0, 1.0);
    CGContextFillRect(ctx, bnds);

    if (!cocoa->buffer || !cocoa->width || !cocoa->height) {
        return;
    }

    // Wrap the live VRAM scanout into a CGImage without copying
    size_t            len  = (size_t)cocoa->stride * cocoa->height;
    CGDataProviderRef prov = CGDataProviderCreateWithData(NULL, cocoa->buffer, len, NULL);
    // XRGB8888 is a8r8g8b8 in a word, i.e. B,G,R,X bytes in memory (little endian)
    CGImageRef img = CGImageCreate(cocoa->width, cocoa->height, 8, 32, cocoa->stride, cocoa->colorspace,
                                   kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, //
                                   prov, NULL, false, kCGRenderingIntentDefault);
    if (img) {
        // The view is non-flipped, so CoreGraphics user space is bottom-left
        // origin and CGContextDrawImage already maps the scanout's first (top)
        // row to the top of the dest rect - no CTM flip needed. Translate the
        // top-left letterbox offset into the bottom-left rect origin CG wants.
        CGFloat rect_y = vh - (CGFloat)cocoa->pos_y - (CGFloat)cocoa->height;
        CGContextDrawImage(ctx, CGRectMake(cocoa->pos_x, rect_y, cocoa->width, cocoa->height), img);
        CGImageRelease(img);
    }
    CGDataProviderRelease(prov);
}

// Report absolute placement in scanout space (HID tablet)
static void cocoa_report_mouse_place(id self, cocoa_window_t* cocoa, id event)
{
    CGPoint loc  = msg_point(event, S("locationInWindow"));
    CGPoint p    = msg_convert_point(self, S("convertPoint:fromView:"), loc, nil);
    CGRect  bnds = msg_rect(self, S("bounds"));
    // Convert from AppKit bottom-left to scanout top-left, minus letterbox offset
    int32_t x = (int32_t)p.x - (int32_t)cocoa->pos_x;
    int32_t y = (int32_t)(bnds.size.height - p.y) - (int32_t)cocoa->pos_y;
    x         = EVAL_MAX(EVAL_MIN(x, (int32_t)cocoa->width - 1), 0);
    y         = EVAL_MAX(EVAL_MIN(y, (int32_t)cocoa->height - 1), 0);
    gui_backend_on_mouse_place(cocoa->win, x, y);
}

static void imp_mousePlace(id self, SEL _cmd, id event)
{
    UNUSED(_cmd);
    cocoa_report_mouse_place(self, view_ctx(self), event);
}

static void imp_mouseDown(id self, SEL _cmd, id event)
{
    UNUSED(_cmd);
    cocoa_window_t* cocoa = view_ctx(self);
    cocoa_report_mouse_place(self, cocoa, event);
    gui_backend_on_mouse_press(cocoa->win, HID_BTN_LEFT);
}

static void imp_mouseUp(id self, SEL _cmd, id event)
{
    UNUSED(_cmd);
    cocoa_window_t* cocoa = view_ctx(self);
    cocoa_report_mouse_place(self, cocoa, event);
    gui_backend_on_mouse_release(cocoa->win, HID_BTN_LEFT);
}

static void imp_rightMouseDown(id self, SEL _cmd, id event)
{
    UNUSED(_cmd);
    cocoa_window_t* cocoa = view_ctx(self);
    cocoa_report_mouse_place(self, cocoa, event);
    gui_backend_on_mouse_press(cocoa->win, HID_BTN_RIGHT);
}

static void imp_rightMouseUp(id self, SEL _cmd, id event)
{
    UNUSED(_cmd);
    cocoa_window_t* cocoa = view_ctx(self);
    cocoa_report_mouse_place(self, cocoa, event);
    gui_backend_on_mouse_release(cocoa->win, HID_BTN_RIGHT);
}

static void imp_otherMouseDown(id self, SEL _cmd, id event)
{
    UNUSED(_cmd);
    if (msg_l(event, S("buttonNumber")) == 2) {
        gui_backend_on_mouse_press(view_ctx(self)->win, HID_BTN_MIDDLE);
    }
}

static void imp_otherMouseUp(id self, SEL _cmd, id event)
{
    UNUSED(_cmd);
    if (msg_l(event, S("buttonNumber")) == 2) {
        gui_backend_on_mouse_release(view_ctx(self)->win, HID_BTN_MIDDLE);
    }
}

static void imp_scrollWheel(id self, SEL _cmd, id event)
{
    UNUSED(_cmd);
    cocoa_window_t* cocoa = view_ctx(self);
    double          dy    = msg_dbl(event, S("deltaY"));
    if (dy > 0.0) {
        gui_backend_on_mouse_scroll(cocoa->win, HID_SCROLL_UP);
    } else if (dy < 0.0) {
        gui_backend_on_mouse_scroll(cocoa->win, HID_SCROLL_DOWN);
    }
}

static void imp_keyDown(id self, SEL _cmd, id event)
{
    UNUSED(_cmd);
    if (!msg_bool(event, S("isARepeat"))) {
        gui_backend_on_key_press(view_ctx(self)->win, cocoa_key_to_hid(msg_ushort(event, S("keyCode"))));
    }
}

static void imp_keyUp(id self, SEL _cmd, id event)
{
    UNUSED(_cmd);
    gui_backend_on_key_release(view_ctx(self)->win, cocoa_key_to_hid(msg_ushort(event, S("keyCode"))));
}

static void imp_flagsChanged(id self, SEL _cmd, id event)
{
    UNUSED(_cmd);
    cocoa_window_t* cocoa = view_ctx(self);
    // Modifier keys arrive here; press state is derived from device-dependent bits
    static const struct {
        uint32_t  mask;
        hid_key_t key;
    } mods[] = {
        {COCOA_MOD_LCTRL,  HID_KEY_LEFTCTRL  },
        {COCOA_MOD_RCTRL,  HID_KEY_RIGHTCTRL },
        {COCOA_MOD_LSHIFT, HID_KEY_LEFTSHIFT },
        {COCOA_MOD_RSHIFT, HID_KEY_RIGHTSHIFT},
        {COCOA_MOD_LALT,   HID_KEY_LEFTALT   },
        {COCOA_MOD_RALT,   HID_KEY_RIGHTALT  },
        {COCOA_MOD_LMETA,  HID_KEY_LEFTMETA  },
        {COCOA_MOD_RMETA,  HID_KEY_RIGHTMETA },
    };
    uint32_t flags = (uint32_t)msg_u(event, S("modifierFlags"));
    for (size_t i = 0; i < STATIC_ARRAY_SIZE(mods); ++i) {
        if (flags & mods[i].mask) {
            gui_backend_on_key_press(cocoa->win, mods[i].key);
        } else {
            gui_backend_on_key_release(cocoa->win, mods[i].key);
        }
    }
    // Caps Lock reports toggle state, not key state - emit a tap on each change
    if (flags & COCOA_MOD_CAPS) {
        gui_backend_on_key_press(cocoa->win, HID_KEY_CAPSLOCK);
    } else {
        gui_backend_on_key_release(cocoa->win, HID_KEY_CAPSLOCK);
    }
}

/*
 * RVVMWindow method implementations (NSWindow subclass, also its own delegate)
 */
static BOOL imp_windowShouldClose(id self, SEL _cmd, id sender)
{
    UNUSED(_cmd);
    UNUSED(sender);
    gui_backend_on_close(win_ctx(self)->win);
    // RVVM decides what closing means (reset / poweroff), don't destroy here
    return NO;
}

static void imp_windowDidResignKey(id self, SEL _cmd, id notification)
{
    UNUSED(_cmd);
    UNUSED(notification);
    gui_backend_on_focus_lost(win_ctx(self)->win);
}

/*
 * Build the RVVMView / RVVMWindow classes at runtime. Class names are process
 * global, so this must happen exactly once.
 */
static void cocoa_register_classes(void)
{
    g_view_class = objc_allocateClassPair((Class)objc_getClass("NSView"), "RVVMView", 0);
    class_addIvar(g_view_class, "ctx", sizeof(void*), 3 /* log2(alignof(void*)) */, "^v");
    class_addMethod(g_view_class, S("isOpaque"), (IMP)imp_return_yes, ENC_BOOL "@:");
    class_addMethod(g_view_class, S("acceptsFirstResponder"), (IMP)imp_return_yes, ENC_BOOL "@:");
    class_addMethod(g_view_class, S("acceptsFirstMouse:"), (IMP)imp_return_yes, ENC_BOOL "@:@");
    class_addMethod(g_view_class, S("drawRect:"), (IMP)imp_drawRect, "v@:" ENC_RECT);
    class_addMethod(g_view_class, S("mouseMoved:"), (IMP)imp_mousePlace, "v@:@");
    class_addMethod(g_view_class, S("mouseDragged:"), (IMP)imp_mousePlace, "v@:@");
    class_addMethod(g_view_class, S("rightMouseDragged:"), (IMP)imp_mousePlace, "v@:@");
    class_addMethod(g_view_class, S("otherMouseDragged:"), (IMP)imp_mousePlace, "v@:@");
    class_addMethod(g_view_class, S("mouseDown:"), (IMP)imp_mouseDown, "v@:@");
    class_addMethod(g_view_class, S("mouseUp:"), (IMP)imp_mouseUp, "v@:@");
    class_addMethod(g_view_class, S("rightMouseDown:"), (IMP)imp_rightMouseDown, "v@:@");
    class_addMethod(g_view_class, S("rightMouseUp:"), (IMP)imp_rightMouseUp, "v@:@");
    class_addMethod(g_view_class, S("otherMouseDown:"), (IMP)imp_otherMouseDown, "v@:@");
    class_addMethod(g_view_class, S("otherMouseUp:"), (IMP)imp_otherMouseUp, "v@:@");
    class_addMethod(g_view_class, S("scrollWheel:"), (IMP)imp_scrollWheel, "v@:@");
    class_addMethod(g_view_class, S("keyDown:"), (IMP)imp_keyDown, "v@:@");
    class_addMethod(g_view_class, S("keyUp:"), (IMP)imp_keyUp, "v@:@");
    class_addMethod(g_view_class, S("flagsChanged:"), (IMP)imp_flagsChanged, "v@:@");
    objc_registerClassPair(g_view_class);
    g_view_ctx_off = ivar_getOffset(class_getInstanceVariable(g_view_class, "ctx"));

    g_win_class = objc_allocateClassPair((Class)objc_getClass("NSWindow"), "RVVMWindow", 0);
    class_addIvar(g_win_class, "ctx", sizeof(void*), 3, "^v");
    class_addMethod(g_win_class, S("canBecomeKeyWindow"), (IMP)imp_return_yes, ENC_BOOL "@:");
    class_addMethod(g_win_class, S("canBecomeMainWindow"), (IMP)imp_return_yes, ENC_BOOL "@:");
    class_addMethod(g_win_class, S("windowShouldClose:"), (IMP)imp_windowShouldClose, ENC_BOOL "@:@");
    class_addMethod(g_win_class, S("windowDidResignKey:"), (IMP)imp_windowDidResignKey, "v@:@");
    objc_registerClassPair(g_win_class);
    g_win_ctx_off = ivar_getOffset(class_getInstanceVariable(g_win_class, "ctx"));
}

static bool cocoa_global_init(void)
{
    DO_ONCE_SCOPED {
        cocoa_register_classes();

        g_app = msg(C("NSApplication"), S("sharedApplication"));
        // Foreground app so the window can receive focus & key events without a bundle
        msgv_l(g_app, S("setActivationPolicy:"), NSApplicationActivationPolicyRegular);
        msgv(g_app, S("finishLaunching"));
        msgv_b(g_app, S("activateIgnoringOtherApps:"), YES);
    }
    return g_app != nil;
}

// Convert a top-left origin screen point into AppKit's bottom-left frame origin
static CGRect cocoa_frame_from_topleft(id window, int32_t x, int32_t y)
{
    CGRect frame   = msg_rect(window, S("frame"));
    CGRect screen  = msg_rect(msg(C("NSScreen"), S("mainScreen")), S("frame"));
    frame.origin.x = x;
    frame.origin.y = screen.size.height - y - frame.size.height;
    return frame;
}

static void cocoa_window_free(gui_window_t* win)
{
    cocoa_window_t* cocoa = gui_backend_get_data(win);
    if (cocoa) {
        if (cocoa->window) {
            msgv_o(cocoa->window, S("setDelegate:"), nil);
            msgv(cocoa->window, S("close"));
            msgv(cocoa->window, S("release"));
        }
        if (cocoa->colorspace) {
            CGColorSpaceRelease(cocoa->colorspace);
        }
        if (cocoa->cursor_hidden) {
            msgv(C("NSCursor"), S("unhide"));
        }
        // Free VRAM VMA
        vma_free(gui_backend_get_vram(win), gui_backend_get_vram_size(win));
        safe_free(cocoa);
    }
    gui_backend_set_data(win, NULL);
}

static void cocoa_window_poll(gui_window_t* win)
{
    UNUSED(win);
    void* pool = objc_autoreleasePoolPush();
    id    event;
    while ((event = ((id (*)(id, SEL, NSUInteger, id, id, BOOL))objc_msgSend)( //
                g_app, S("nextEventMatchingMask:untilDate:inMode:dequeue:"),   //
                NSEventMaskAny, msg(C("NSDate"), S("distantPast")), NSDefaultRunLoopMode, YES))) {
        msgv_o(g_app, S("sendEvent:"), event);
    }
    objc_autoreleasePoolPop(pool);
}

static void cocoa_window_draw(gui_window_t* win, const rvvm_fb_t* fb, uint32_t x, uint32_t y)
{
    cocoa_window_t* cocoa = gui_backend_get_data(win);

    cocoa->buffer = rvvm_fb_buffer(fb);
    cocoa->width  = rvvm_fb_width(fb);
    cocoa->height = rvvm_fb_height(fb);
    cocoa->stride = rvvm_fb_stride(fb);
    cocoa->pos_x  = x;
    cocoa->pos_y  = y;

    // GUI user expects a synchronous flip
    msgv_b(cocoa->view, S("setNeedsDisplay:"), YES);
    msgv(cocoa->view, S("displayIfNeeded"));
}

static void cocoa_window_set_title(gui_window_t* win, const char* title)
{
    cocoa_window_t* cocoa = gui_backend_get_data(win);
    void*           pool  = objc_autoreleasePoolPush();
    id str = ((id (*)(id, SEL, const char*))objc_msgSend)(C("NSString"), S("stringWithUTF8String:"), title);
    msgv_o(cocoa->window, S("setTitle:"), str);
    objc_autoreleasePoolPop(pool);
}

static void cocoa_window_grab_input(gui_window_t* win, bool grab)
{
    cocoa_window_t* cocoa = gui_backend_get_data(win);
    cocoa->grab           = grab;
    // Detach the hardware cursor so relative input isn't clipped to the screen
    CGAssociateMouseAndMouseCursorPosition(!grab);
    CGDisplayHideCursor(kCGDirectMainDisplay);
    if (!grab) {
        CGDisplayShowCursor(kCGDirectMainDisplay);
    }
}

static void cocoa_window_hide_cursor(gui_window_t* win, bool hide)
{
    cocoa_window_t* cocoa = gui_backend_get_data(win);
    if (cocoa->cursor_hidden != hide) {
        cocoa->cursor_hidden = hide;
        msgv(C("NSCursor"), hide ? S("hide") : S("unhide"));
    }
}

static void cocoa_window_set_win_size(gui_window_t* win, uint32_t w, uint32_t h)
{
    cocoa_window_t* cocoa = gui_backend_get_data(win);
    msgv_sz(cocoa->window, S("setContentSize:"), CGSizeMake(w, h));
}

static void cocoa_window_set_min_size(gui_window_t* win, uint32_t w, uint32_t h)
{
    cocoa_window_t* cocoa = gui_backend_get_data(win);
    msgv_sz(cocoa->window, S("setContentMinSize:"), CGSizeMake(w, h));
}

static void cocoa_window_get_position(gui_window_t* win, int32_t* x, int32_t* y)
{
    cocoa_window_t* cocoa  = gui_backend_get_data(win);
    CGRect          frame  = msg_rect(cocoa->window, S("frame"));
    CGRect          screen = msg_rect(msg(C("NSScreen"), S("mainScreen")), S("frame"));
    *x                     = (int32_t)frame.origin.x;
    *y                     = (int32_t)(screen.size.height - frame.origin.y - frame.size.height);
}

static void cocoa_window_set_position(gui_window_t* win, int32_t x, int32_t y)
{
    cocoa_window_t* cocoa = gui_backend_get_data(win);
    msgv_rect_b(cocoa->window, S("setFrame:display:"), cocoa_frame_from_topleft(cocoa->window, x, y), NO);
}

static void cocoa_window_get_scr_size(gui_window_t* win, uint32_t* w, uint32_t* h)
{
    UNUSED(win);
    CGRect frame = msg_rect(msg(C("NSScreen"), S("mainScreen")), S("visibleFrame"));
    *w           = (uint32_t)frame.size.width;
    *h           = (uint32_t)frame.size.height;
}

static void cocoa_window_set_fullscreen(gui_window_t* win, bool fullscreen)
{
    cocoa_window_t* cocoa  = gui_backend_get_data(win);
    bool            is_now = (msg_u(cocoa->window, S("styleMask")) & NSWindowStyleMaskFullScreen) != 0;
    if (is_now != fullscreen) {
        msgv_o(cocoa->window, S("toggleFullScreen:"), nil);
    }
}

static const gui_backend_cb_t cocoa_window_cb = {
    .free           = cocoa_window_free,
    .poll           = cocoa_window_poll,
    .draw           = cocoa_window_draw,
    .set_title      = cocoa_window_set_title,
    .grab_input     = cocoa_window_grab_input,
    .hide_cursor    = cocoa_window_hide_cursor,
    .set_win_size   = cocoa_window_set_win_size,
    .set_min_size   = cocoa_window_set_min_size,
    .get_position   = cocoa_window_get_position,
    .set_position   = cocoa_window_set_position,
    .get_scr_size   = cocoa_window_get_scr_size,
    .set_fullscreen = cocoa_window_set_fullscreen,
};

bool cocoa_window_init(gui_window_t* win)
{
    gui_backend_register(win, &cocoa_window_cb);

    if (!cocoa_global_init()) {
        rvvm_error("Failed to initialize NSApplication");
        return false;
    }

    void* pool = objc_autoreleasePoolPush();

    cocoa_window_t* cocoa = safe_new_obj(cocoa_window_t);
    gui_backend_set_data(win, cocoa);
    cocoa->win        = win;
    cocoa->colorspace = CGColorSpaceCreateDeviceRGB();

    // Allocate VRAM the framebuffer device scans out of
    size_t vram_size = gui_backend_get_vram_size(win);
    void*  vram      = vma_alloc(NULL, vram_size, VMA_RDWR);
    if (vram) {
        gui_backend_set_vram(win, vram, vram_size);
    } else {
        rvvm_error("vma_alloc() failed!");
        objc_autoreleasePoolPop(pool);
        return false;
    }

    CGRect     rect  = CGRectMake(0, 0, gui_window_width(win), gui_window_height(win));
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable //
                     | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

    id window = ((id (*)(id, SEL, CGRect, NSUInteger, NSUInteger, BOOL))objc_msgSend)( //
        msg((id)g_win_class, S("alloc")), S("initWithContentRect:styleMask:backing:defer:"), //
        rect, style, NSBackingStoreBuffered, NO);
    *(cocoa_window_t**)(void*)((char*)window + g_win_ctx_off) = cocoa;
    msgv_o(window, S("setDelegate:"), window);
    msgv_b(window, S("setAcceptsMouseMovedEvents:"), YES);
    msgv_b(window, S("setReleasedWhenClosed:"), NO);
    msgv_b(window, S("setOpaque:"), YES);
    msgv_o(window, S("setBackgroundColor:"), msg(C("NSColor"), S("blackColor")));

    // initWithFrame: takes a CGRect, so it needs its own exact prototype
    id view = ((id (*)(id, SEL, CGRect))objc_msgSend)(msg((id)g_view_class, S("alloc")), S("initWithFrame:"), rect);
    *(cocoa_window_t**)(void*)((char*)view + g_view_ctx_off) = cocoa;
    msgv_u(view, S("setAutoresizingMask:"), NSViewWidthSizable | NSViewHeightSizable);

    cocoa->window = window;
    cocoa->view   = view;

    msgv_o(window, S("setContentView:"), view);
    msgv_o(window, S("makeFirstResponder:"), view);
    msgv(view, S("release"));

    msgv(window, S("center"));
    msgv_o(window, S("makeKeyAndOrderFront:"), nil);

    objc_autoreleasePoolPop(pool);
    return true;
}

#else

bool cocoa_window_init(gui_window_t* win)
{
    UNUSED(win);
    return false;
}

#endif

POP_OPTIMIZATION_SIZE
