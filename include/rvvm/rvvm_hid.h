/*
<rvvm/rvvm_hid.h> - Human Interface Devices API
Copyright (C) 2020-2026 LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_HID_API_H
#define _RVVM_HID_API_H

#include <rvvm/rvvm_base.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_mouse_api HID Mouse API
 * @addtogroup rvvm_mouse_api
 * @{
 */

/*
 * Mouse buttons
 */
#define RVVM_MOUSE_LEFT   0x01 /**< Left mouse button                 */
#define RVVM_MOUSE_RIGHT  0x02 /**< Right mouse button                */
#define RVVM_MOUSE_MIDDLE 0x04 /**< Middle mouse button (Scrollwheel) */

/*
 * Mouse scroll constants
 */
#define RVVM_SCROLL_UP    (-1) /**< Scroll one unit upward   */
#define RVVM_SCROLL_DOWN  1    /**< Scroll one unit downward */

/*
 * Mouse implementation callbacks
 *
 * Must be valid during mouse lifetime, or static
 * Callbacks are optional (Nullable, no-op then)
 */
typedef struct {
    /**
     * Set mouse buttons state
     */
    void (*set_btns)(rvvm_mouse_t* mouse, uint32_t down, uint32_t up);

    /**
     * Scroll mouse horizontally / vertically
     */
    void (*scroll)(rvvm_mouse_t* mouse, int32_t x, int32_t y);

    /**
     * Set tablet resolution
     */
    void (*resolution)(rvvm_mouse_t* mouse, uint32_t x, uint32_t y);

    /**
     * Position the mouse
     */
    void (*position)(rvvm_mouse_t* mouse, int32_t x, int32_t y, bool abs);

} rvvm_mouse_cb_t;

/**
 * Create mouse handle
 *
 * Handles are dual-owned on implementation and user side,
 * and should be detached + freed on both sides
 *
 * \return Mouse handle
 */
RVVM_PUBLIC rvvm_mouse_t* rvvm_mouse_init(void);

/**
 * Register mouse implementation on a handle
 *
 * \param mouse Mouse handle
 * \param cb    Mouse callbacks
 * \param data  Mouse private data
 */
RVVM_PUBLIC void rvvm_mouse_register(rvvm_mouse_t* mouse, const rvvm_mouse_cb_t* cb, void* data);

/**
 * Get private mouse implementation data
 *
 * \param mouse Mouse handle
 * \return Mouse implementation private data
 */
RVVM_PUBLIC void* rvvm_mouse_data(rvvm_mouse_t* mouse);

/**
 * Detach mouse handle from mouse implementation
 *
 * Must be called when mouse implementation is freed on machine side
 * Mouse handle will exist till it's both disowned & freed
 *
 * \param mouse Mouse handle
 */
static inline void rvvm_mouse_detach(rvvm_mouse_t* mouse)
{
    rvvm_mouse_register(mouse, NULL, NULL);
}

/**
 * Free mouse handle from mouse user side
 *
 * Must be called when mouse is not intended to be used anymore
 * Mouse handle will exist till it's both disowned & freed
 *
 * \param mouse Mouse handle
 */
RVVM_PUBLIC void rvvm_mouse_free(rvvm_mouse_t* mouse);

/**
 * Set mouse buttons state
 *
 * \param mouse Mouse handle
 * \param down  Combination of RVVM_MOUSE_ constants to press
 * \param up    Combination of RVVM_MOUSE_ constants to release
 */
RVVM_PUBLIC void rvvm_mouse_set_btns(rvvm_mouse_t* mouse, uint32_t down, uint32_t up);

/**
 * Press mouse buttons
 *
 * \param mouse Mouse handle
 * \param btns  Combination of RVVM_MOUSE_ constants
 */
static inline void rvvm_mouse_press(rvvm_mouse_t* mouse, uint32_t btns)
{
    rvvm_mouse_set_btns(mouse, btns, 0);
}

/**
 * Release mouse buttons
 *
 * \param mouse Mouse handle
 * \param btns  Combination of RVVM_MOUSE_ constants
 */
static inline void rvvm_mouse_release(rvvm_mouse_t* mouse, uint32_t btns)
{
    rvvm_mouse_set_btns(mouse, 0, btns);
}

/**
 * Scroll mouse horizontally / vertically
 *
 * \param mouse Mouse handle
 * \param x     Horizontal scroll offset
 * \param y     Vertical scroll offset
 */
RVVM_PUBLIC void rvvm_mouse_scroll_xy(rvvm_mouse_t* mouse, int32_t x, int32_t y);

/**
 * Scroll mouse wheel
 *
 * \param mouse Mouse handle
 * \param off   Scroll offset (Positive offset goes downward)
 */
static inline void rvvm_mouse_scroll(rvvm_mouse_t* mouse, int32_t off)
{
    rvvm_mouse_scroll_xy(mouse, 0, off);
}

/**
 * Set tablet resolution
 *
 * Must be called before rvvm_mouse_place() for proper screen dimensions
 *
 * \param mouse Mouse handle
 * \param x     Screen width
 * \param y     Screen height
 */
RVVM_PUBLIC void rvvm_mouse_resolution(rvvm_mouse_t* mouse, uint32_t x, uint32_t y);

/**
 * Relative / Absolute mouse movement
 *
 * \param mouse Mouse handle
 * \param x     X movement in pixels
 * \param y     Y movement in pixels, goes downward
 * \param abs   Coordinates are absolute
 */
RVVM_PUBLIC void rvvm_mouse_position(rvvm_mouse_t* mouse, int32_t x, int32_t y, bool abs);

/**
 * Relative mouse movement
 *
 * \param mouse Mouse handle
 * \param x     Relative X movement in pixels
 * \param y     Relative Y movement in pixels, goes downward
 */
static inline void rvvm_mouse_move(rvvm_mouse_t* mouse, int32_t x, int32_t y)
{
    rvvm_mouse_position(mouse, x, y, false);
}

/**
 * Absolute mouse movement (Tablet mode for cursor integration)
 *
 * Screen resolution must be set with hid_mouse_resolution()
 *
 * \param mouse Mouse handle
 * \param x     Cursor X position on screen
 * \param y     Cursor Y position on screen, goes downward
 */
static inline void rvvm_mouse_place(rvvm_mouse_t* mouse, int32_t x, int32_t y)
{
    rvvm_mouse_position(mouse, x, y, true);
}

/** @}*/

/**
 * @defgroup rvvm_keyboard_api HID Keyboard API
 * @addtogroup rvvm_keyboard_api
 * @{
 */

/*
 * Keyboard implementation callbacks
 *
 * Must be valid during keyboard lifetime, or static
 * Callbacks are optional (Nullable, no-op then)
 */
typedef struct {
    /**
     * Set keyboard key state
     */
    void (*set_key)(rvvm_keyboard_t* kb, rvvm_keycode_t key, bool set);

} rvvm_keyboard_cb_t;

/**
 * Create keyboard handle
 *
 * Handles are dual-owned on implementation and user side,
 * and should be detached + freed on both sides
 *
 * \return Keyboard handle
 */
RVVM_PUBLIC rvvm_keyboard_t* rvvm_keyboard_init(void);

/**
 * Register keyboard implementation on a handle
 *
 * \param kb   Keyboard handle
 * \param cb   Keyboard callbacks
 * \param data Keyboard private data
 */
RVVM_PUBLIC void rvvm_keyboard_register(rvvm_keyboard_t* kb, const rvvm_keyboard_cb_t* cb, void* data);

/**
 * Get private keyboard implementation data
 *
 * \param kb Keyboard handle
 * \return Keyboard implementation private data
 */
RVVM_PUBLIC void* rvvm_keyboard_data(rvvm_keyboard_t* kb);

/**
 * Detach keyboard handle from keyboard implementation
 *
 * Must be called when keyboard implementation is freed on machine side
 * Keyboard handle will exist till it's both disowned & freed
 *
 * \param kb Keyboard handle
 */
static inline void rvvm_keyboard_detach(rvvm_keyboard_t* kb)
{
    rvvm_keyboard_register(kb, NULL, NULL);
}

/**
 * Free keyboard handle from keyboard user side
 *
 * Must be called when keyboard is not intended to be used anymore
 * Keyboard handle will exist till it's both disowned & freed
 *
 * \param kb Keyboard handle
 */
RVVM_PUBLIC void rvvm_keyboard_free(rvvm_keyboard_t* kb);

/**
 * Set keyboard key state
 *
 * \param kb  Keyboard handle
 * \param key Keyboard keycode
 * \param set Key state
 */
RVVM_PUBLIC void rvvm_keyboard_set_key(rvvm_keyboard_t* kb, rvvm_keycode_t key, bool set);

/**
 * Press a keyboard key
 *
 * \param kb  Keyboard handle
 * \param key Keyboard keycode
 */
static inline void rvvm_keyboard_press(rvvm_keyboard_t* kb, rvvm_keycode_t key)
{
    rvvm_keyboard_set_key(kb, key, true);
}

/**
 * Release a keyboard key
 *
 * \param kb  Keyboard handle
 * \param key Keyboard keycode
 */
static inline void rvvm_keyboard_release(rvvm_keyboard_t* kb, rvvm_keycode_t key)
{
    rvvm_keyboard_set_key(kb, key, false);
}

/**
 * Type text string over guest keyboard
 *
 * \param kb  Keyboard handle
 * \param str Text string (ASCII only)
 */
RVVM_PUBLIC void rvvm_keyboard_type_text(rvvm_keyboard_t* kb, const char* str);

/*
 * Keyboard keycode definitions
 */
#define RVVM_KEY_NONE               0x00

/*
 * Keyboard errors (Non-physical keys)
 */
#define RVVM_KEY_ROLLOVER           0x01
#define RVVM_KEY_POSTFAIL           0x02
#define RVVM_KEY_UNDEFINED          0x03

/*
 * Typing keys
 */
#define RVVM_KEY_A                  0x04
#define RVVM_KEY_B                  0x05
#define RVVM_KEY_C                  0x06
#define RVVM_KEY_D                  0x07
#define RVVM_KEY_E                  0x08
#define RVVM_KEY_F                  0x09
#define RVVM_KEY_G                  0x0A
#define RVVM_KEY_H                  0x0B
#define RVVM_KEY_I                  0x0C
#define RVVM_KEY_J                  0x0D
#define RVVM_KEY_K                  0x0E
#define RVVM_KEY_L                  0x0F
#define RVVM_KEY_M                  0x10
#define RVVM_KEY_N                  0x11
#define RVVM_KEY_O                  0x12
#define RVVM_KEY_P                  0x13
#define RVVM_KEY_Q                  0x14
#define RVVM_KEY_R                  0x15
#define RVVM_KEY_S                  0x16
#define RVVM_KEY_T                  0x17
#define RVVM_KEY_U                  0x18
#define RVVM_KEY_V                  0x19
#define RVVM_KEY_W                  0x1A
#define RVVM_KEY_X                  0x1B
#define RVVM_KEY_Y                  0x1C
#define RVVM_KEY_Z                  0x1D

/*
 * Number keys
 */
#define RVVM_KEY_1                  0x1E
#define RVVM_KEY_2                  0x1F
#define RVVM_KEY_3                  0x20
#define RVVM_KEY_4                  0x21
#define RVVM_KEY_5                  0x22
#define RVVM_KEY_6                  0x23
#define RVVM_KEY_7                  0x24
#define RVVM_KEY_8                  0x25
#define RVVM_KEY_9                  0x26
#define RVVM_KEY_0                  0x27

/*
 * Control keys
 */
#define RVVM_KEY_ENTER              0x28 /**< Enter                             */
#define RVVM_KEY_ESC                0x29 /**< Esc                               */
#define RVVM_KEY_BACKSPACE          0x2A /**< Backspace                         */
#define RVVM_KEY_TAB                0x2B /**< Tab                               */
#define RVVM_KEY_SPACE              0x2C /**< Space                             */
#define RVVM_KEY_MINUS              0x2D /**< Minus                             */
#define RVVM_KEY_EQUAL              0x2E /**< Equal                             */
#define RVVM_KEY_LEFTBRACE          0x2F /**< Left brace or bracket             */
#define RVVM_KEY_RIGHTBRACE         0x30 /**< Right brace or bracket            */
#define RVVM_KEY_BACKSLASH          0x31 /**< Backslash                         */
#define RVVM_KEY_HASHTILDE          0x32 /**< Hash, tilde (Non-US ISO keyboard) */
#define RVVM_KEY_SEMICOLON          0x33 /**< Colon, semicolon                  */
#define RVVM_KEY_APOSTROPHE         0x34 /**< Quotation                         */
#define RVVM_KEY_GRAVE              0x35 /**< Tilde (Quake console)             */
#define RVVM_KEY_COMMA              0x36 /**< Comma, left angled bracket        */
#define RVVM_KEY_DOT                0x37 /**< Dot, right angled bracket         */
#define RVVM_KEY_SLASH              0x38 /**< Forward slash                     */
#define RVVM_KEY_CAPSLOCK           0x39 /**< Caps Lock                         */

/*
 * Function keys (F1 - F12)
 */
#define RVVM_KEY_F1                 0x3A
#define RVVM_KEY_F2                 0x3B
#define RVVM_KEY_F3                 0x3C
#define RVVM_KEY_F4                 0x3D
#define RVVM_KEY_F5                 0x3E
#define RVVM_KEY_F6                 0x3F
#define RVVM_KEY_F7                 0x40
#define RVVM_KEY_F8                 0x41
#define RVVM_KEY_F9                 0x42
#define RVVM_KEY_F10                0x43
#define RVVM_KEY_F11                0x44
#define RVVM_KEY_F12                0x45

/*
 * Editing keys
 */
#define RVVM_KEY_SYSRQ              0x46 /**< Print Screen (REISUB, anyone?) */
#define RVVM_KEY_SCROLLLOCK         0x47 /**< Scroll Lock                    */
#define RVVM_KEY_PAUSE              0x48 /**< Pause                          */
#define RVVM_KEY_INSERT             0x49 /**< Insert                         */
#define RVVM_KEY_HOME               0x4A /**< Home                           */
#define RVVM_KEY_PAGEUP             0x4B /**< Page Up                        */
#define RVVM_KEY_DELETE             0x4C /**< Delete                         */
#define RVVM_KEY_END                0x4D /**< End                            */
#define RVVM_KEY_PAGEDOWN           0x4E /**< Page Down                      */
#define RVVM_KEY_RIGHT              0x4F /**< Right Arrow                    */
#define RVVM_KEY_LEFT               0x50 /**< Left Arrow                     */
#define RVVM_KEY_DOWN               0x51 /**< Down Arrow                     */
#define RVVM_KEY_UP                 0x52 /**< Up Arrow                       */

/*
 * Numpad keys
 */
#define RVVM_KEY_NUMLOCK            0x53 /**< Num Lock        */
#define RVVM_KEY_KPSLASH            0x54 /**< Numpad divide   */
#define RVVM_KEY_KPASTERISK         0x55 /**< Numpad multiply */
#define RVVM_KEY_KPMINUS            0x56 /**< Numpad subtract */
#define RVVM_KEY_KPPLUS             0x57 /**< Numpad add      */
#define RVVM_KEY_KPENTER            0x58 /**< Numpad enter    */
#define RVVM_KEY_KP1                0x59
#define RVVM_KEY_KP2                0x5A
#define RVVM_KEY_KP3                0x5B
#define RVVM_KEY_KP4                0x5C
#define RVVM_KEY_KP5                0x5D
#define RVVM_KEY_KP6                0x5E
#define RVVM_KEY_KP7                0x5F
#define RVVM_KEY_KP8                0x60
#define RVVM_KEY_KP9                0x61
#define RVVM_KEY_KP0                0x62
#define RVVM_KEY_KPDOT              0x63

/*
 * Non-US keyboard keys
 */
#define RVVM_KEY_102ND              0x64 /**< Non-US \ and |, also <> on nordic keyboards */
#define RVVM_KEY_COMPOSE            0x65 /**< Compose key                                 */
#define RVVM_KEY_POWER              0x66 /**< Poweroff key                                */
#define RVVM_KEY_KPEQUAL            0x67 /**< Keypad =                                    */

/*
 * Function keys (F13 - F24)
 */
#define RVVM_KEY_F13                0x68
#define RVVM_KEY_F14                0x69
#define RVVM_KEY_F15                0x6A
#define RVVM_KEY_F16                0x6B
#define RVVM_KEY_F17                0x6C
#define RVVM_KEY_F18                0x6D
#define RVVM_KEY_F19                0x6E
#define RVVM_KEY_F20                0x6F
#define RVVM_KEY_F21                0x70
#define RVVM_KEY_F22                0x71
#define RVVM_KEY_F23                0x72
#define RVVM_KEY_F24                0x73

/*
 * Non-US Media/special keys
 */
#define RVVM_KEY_OPEN               0x74 /**< Execute                                                */
#define RVVM_KEY_HELP               0x75 /**< Help                                                   */
#define RVVM_KEY_PROPS              0x76 /**< Context menu key (Near right Alt) - Linux evdev naming */
#define RVVM_KEY_MENU               0x76 /**< Context menu key, alias to RVVM_KEY_PROPS              */
#define RVVM_KEY_FRONT              0x77 /**< Select                                                 */
#define RVVM_KEY_STOP               0x78 /**< Stop                                                   */
#define RVVM_KEY_AGAIN              0x79 /**< Again                                                  */
#define RVVM_KEY_UNDO               0x7A /**< Undo                                                   */
#define RVVM_KEY_CUT                0x7B /**< Cut                                                    */
#define RVVM_KEY_COPY               0x7C /**< Copy                                                   */
#define RVVM_KEY_PASTE              0x7D /**< Paste                                                  */
#define RVVM_KEY_FIND               0x7E /**< Find                                                   */
#define RVVM_KEY_MUTE               0x7F /**< Mute                                                   */
#define RVVM_KEY_VOLUMEUP           0x80 /**< Volume Up                                              */
#define RVVM_KEY_VOLUMEDOWN         0x81 /**< Volume Down                                            */
#define RVVM_KEY_KPCOMMA            0x85 /**< Keypad Comma (Brazilian keypad period key)             */

/*
 * International keys
 */
#define RVVM_KEY_RO                 0x87 /**< International1 (Japanese Ro, \\ key)                                  */
#define RVVM_KEY_KATAKANAHIRAGANA   0x88 /**< International2 (Japanese Katakana/Hiragana, second right to spacebar) */
#define RVVM_KEY_YEN                0x89 /**< International3 (Japanese Yen)                                         */
#define RVVM_KEY_HENKAN             0x8A /**< International4 (Japanese Henkan, right to spacebar)                   */
#define RVVM_KEY_MUHENKAN           0x8B /**< International5 (Japanese Muhenkan, left to spacebar)                  */
#define RVVM_KEY_KPJPCOMMA          0x8C /**< International6 (Japanese Comma? See HID spec...)                      */

/*
 * LANG keys
 */
#define RVVM_KEY_HANGEUL            0x90 /**< LANG1 (Korean Hangul/English toggle key) */
#define RVVM_KEY_HANJA              0x91 /**< LANG2 (Korean Hanja control key)         */
#define RVVM_KEY_KATAKANA           0x92 /**< LANG3 (Japanese Katakana key)            */
#define RVVM_KEY_HIRAGANA           0x93 /**< LANG4 (Japanese Hiragana key)            */
#define RVVM_KEY_ZENKAKUHANKAKU     0x94 /**< LANG5 (Japanese Zenkaku/Hankaku key)     */

/*
 * Additional keypad keys
 */
#define RVVM_KEY_KPLEFTPAREN        0xB6 /**< Keypad ( */
#define RVVM_KEY_KPRIGHTPAREN       0xB7 /**< Keypad ) */

/*
 * Modifier keys
 */
#define RVVM_KEY_LEFTCTRL           0xE0 /**< Left Ctrl           */
#define RVVM_KEY_LEFTSHIFT          0xE1 /**< Left Shift          */
#define RVVM_KEY_LEFTALT            0xE2 /**< Left Alt            */
#define RVVM_KEY_LEFTMETA           0xE3 /**< Left "Windows" key  */
#define RVVM_KEY_RIGHTCTRL          0xE4 /**< Right Ctrl          */
#define RVVM_KEY_RIGHTSHIFT         0xE5 /**< Right Shift         */
#define RVVM_KEY_RIGHTALT           0xE6 /**< Right Alt           */
#define RVVM_KEY_RIGHTMETA          0xE7 /**< Right "Windows" key */

/*
 * Media keys
 */
#define RVVM_KEY_MEDIA_PLAYPAUSE    0xE8
#define RVVM_KEY_MEDIA_STOPCD       0xE9
#define RVVM_KEY_MEDIA_PREVIOUSSONG 0xEA
#define RVVM_KEY_MEDIA_NEXTSONG     0xEB
#define RVVM_KEY_MEDIA_EJECTCD      0xEC
#define RVVM_KEY_MEDIA_VOLUMEUP     0xED
#define RVVM_KEY_MEDIA_VOLUMEDOWN   0xEE
#define RVVM_KEY_MEDIA_MUTE         0xEF
#define RVVM_KEY_MEDIA_WWW          0xF0
#define RVVM_KEY_MEDIA_BACK         0xF1
#define RVVM_KEY_MEDIA_FORWARD      0xF2
#define RVVM_KEY_MEDIA_STOP         0xF3
#define RVVM_KEY_MEDIA_FIND         0xF4
#define RVVM_KEY_MEDIA_SCROLLUP     0xF5
#define RVVM_KEY_MEDIA_SCROLLDOWN   0xF6
#define RVVM_KEY_MEDIA_EDIT         0xF7
#define RVVM_KEY_MEDIA_SLEEP        0xF8
#define RVVM_KEY_MEDIA_COFFEE       0xF9
#define RVVM_KEY_MEDIA_REFRESH      0xFA
#define RVVM_KEY_MEDIA_CALC         0xFB

/** @}*/

RVVM_EXTERN_C_END

#endif
