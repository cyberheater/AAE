//==========================================================================
// AAE - Another Arcade Emulator
// Copyright (C) 2026 Tim Cottrill - GNU GPL v3 or later.
//==========================================================================
//==============================================================================
// sdl_input.cpp -- see sdl_input.h. Mirrors evdev_input.cpp's semantics:
// key[] is indexed by AaeKey, mickeys are read-and-reset deltas.
//==============================================================================
#include "sdl_input.h"

#include "sys_input.h"
#include "sys_window.h"
#include "sys_log.h"

#include <SDL.h>

#include <cstring>

unsigned char key[256] = {0};
int mouse_b = 0;

namespace {

unsigned int s_lastkey[256] = {0};
float s_mouseScale = 1.0f;
bool  s_paused      = false;
bool  s_initialized = false;
bool  s_kbdSeen     = false;
bool  s_mouseSeen   = false;

KeyCallback            s_keyCallback      = nullptr;
MouseButtonCallback    s_mouseBtnCallback = nullptr;
CursorPositionCallback s_cursorCallback   = nullptr;

struct MouseState {
	long x = 0, y = 0, wheel = 0;
	long dx = 0, dy = 0, dwheel = 0;
	bool left = false, middle = false, right = false;
};
MouseState s_mouse;

// SDL scancodes are positional (USB HID usage), so this is layout-independent
// in the same way the evdev KEY_* map is.
uint8_t SdlScancodeToAae(SDL_Scancode sc)
{
	if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
		return (uint8_t)(AAEKEY_A + (sc - SDL_SCANCODE_A));
	if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
		return (uint8_t)(AAEKEY_1 + (sc - SDL_SCANCODE_1));
	if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
		return (uint8_t)(AAEKEY_F1 + (sc - SDL_SCANCODE_F1));
	if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9)
		return (uint8_t)(AAEKEY_1_PAD + (sc - SDL_SCANCODE_KP_1));

	switch (sc) {
	case SDL_SCANCODE_0:            return AAEKEY_0;
	case SDL_SCANCODE_KP_0:         return AAEKEY_0_PAD;
	case SDL_SCANCODE_ESCAPE:       return AAEKEY_ESC;
	case SDL_SCANCODE_GRAVE:        return AAEKEY_TILDE;
	case SDL_SCANCODE_MINUS:        return AAEKEY_MINUS;
	case SDL_SCANCODE_EQUALS:       return AAEKEY_EQUALS;
	case SDL_SCANCODE_BACKSPACE:    return AAEKEY_BACKSPACE;
	case SDL_SCANCODE_TAB:          return AAEKEY_TAB;
	case SDL_SCANCODE_LEFTBRACKET:  return AAEKEY_OPENBRACE;
	case SDL_SCANCODE_RIGHTBRACKET: return AAEKEY_CLOSEBRACE;
	case SDL_SCANCODE_RETURN:       return AAEKEY_ENTER;
	case SDL_SCANCODE_SEMICOLON:    return AAEKEY_COLON;
	case SDL_SCANCODE_APOSTROPHE:   return AAEKEY_QUOTE;
	case SDL_SCANCODE_BACKSLASH:    return AAEKEY_BACKSLASH;
	case SDL_SCANCODE_NONUSBACKSLASH: return AAEKEY_BACKSLASH2;
	case SDL_SCANCODE_COMMA:        return AAEKEY_COMMA;
	case SDL_SCANCODE_PERIOD:       return AAEKEY_STOP;
	case SDL_SCANCODE_SLASH:        return AAEKEY_SLASH;
	case SDL_SCANCODE_SPACE:        return AAEKEY_SPACE;
	case SDL_SCANCODE_INSERT:       return AAEKEY_INSERT;
	case SDL_SCANCODE_DELETE:       return AAEKEY_DEL;
	case SDL_SCANCODE_HOME:         return AAEKEY_HOME;
	case SDL_SCANCODE_END:          return AAEKEY_END;
	case SDL_SCANCODE_PAGEUP:       return AAEKEY_PGUP;
	case SDL_SCANCODE_PAGEDOWN:     return AAEKEY_PGDN;
	case SDL_SCANCODE_LEFT:         return AAEKEY_LEFT;
	case SDL_SCANCODE_RIGHT:        return AAEKEY_RIGHT;
	case SDL_SCANCODE_UP:           return AAEKEY_UP;
	case SDL_SCANCODE_DOWN:         return AAEKEY_DOWN;
	case SDL_SCANCODE_KP_DIVIDE:    return AAEKEY_SLASH_PAD;
	case SDL_SCANCODE_KP_MULTIPLY:  return AAEKEY_ASTERISK;
	case SDL_SCANCODE_KP_MINUS:     return AAEKEY_MINUS_PAD;
	case SDL_SCANCODE_KP_PLUS:      return AAEKEY_PLUS_PAD;
	case SDL_SCANCODE_KP_PERIOD:    return AAEKEY_DEL_PAD;
	case SDL_SCANCODE_KP_ENTER:     return AAEKEY_ENTER_PAD;
	case SDL_SCANCODE_PRINTSCREEN:  return AAEKEY_PRTSCR;
	case SDL_SCANCODE_PAUSE:        return AAEKEY_PAUSE;
	case SDL_SCANCODE_LSHIFT:       return AAEKEY_LSHIFT;
	case SDL_SCANCODE_RSHIFT:       return AAEKEY_RSHIFT;
	case SDL_SCANCODE_LCTRL:        return AAEKEY_LCONTROL;
	case SDL_SCANCODE_RCTRL:        return AAEKEY_RCONTROL;
	case SDL_SCANCODE_LALT:         return AAEKEY_ALT;
	case SDL_SCANCODE_RALT:         return AAEKEY_ALTGR;
	case SDL_SCANCODE_LGUI:         return AAEKEY_LWIN;
	case SDL_SCANCODE_RGUI:         return AAEKEY_RWIN;
	case SDL_SCANCODE_APPLICATION:  return AAEKEY_MENU;
	case SDL_SCANCODE_SCROLLLOCK:   return AAEKEY_SCRLOCK;
	case SDL_SCANCODE_NUMLOCKCLEAR: return AAEKEY_NUMLOCK;
	case SDL_SCANCODE_CAPSLOCK:     return AAEKEY_CAPSLOCK;
	default:                        return 0;
	}
}

int ComputeModifierFlags()
{
	int mods = 0;
	if (key[AAEKEY_LSHIFT]   || key[AAEKEY_RSHIFT])   mods |= RI_MOD_SHIFT;
	if (key[AAEKEY_LCONTROL] || key[AAEKEY_RCONTROL]) mods |= RI_MOD_CONTROL;
	if (key[AAEKEY_ALT]      || key[AAEKEY_ALTGR])    mods |= RI_MOD_ALT;
	if (key[AAEKEY_LWIN]     || key[AAEKEY_RWIN])     mods |= RI_MOD_SUPER;
	return mods;
}

void ClearAllState()
{
	memset(key, 0, sizeof(key));
	memset(s_lastkey, 0, sizeof(s_lastkey));
	mouse_b = 0;
	s_mouse.left = s_mouse.right = s_mouse.middle = false;
	s_mouse.dx = s_mouse.dy = s_mouse.dwheel = 0;
}

void HandleKey(const SDL_KeyboardEvent& k)
{
	const uint8_t aae = SdlScancodeToAae(k.keysym.scancode);
	if (!aae) return;
	const bool released = (k.type == SDL_KEYUP);
	s_kbdSeen = true;

	if (released) {
		key[aae] = 0;
		s_lastkey[aae] = 0;
	} else {
		key[aae] = 1;
		s_lastkey[aae] = (s_lastkey[aae] + 1) % 0xFFFFFFFF;
		if (s_lastkey[aae] == 0) s_lastkey[aae] = 1;
	}

	// Auto-repeat is counted in s_lastkey (isKeyHeld) but not re-announced,
	// matching evdev's value==2 handling.
	if (s_keyCallback && !k.repeat)
		s_keyCallback((int)aae, (int)k.keysym.scancode, released ? 0 : 1, ComputeModifierFlags());
}

void HandleMouseButton(const SDL_MouseButtonEvent& b)
{
	int bit = 0, button = 0;
	switch (b.button) {
	case SDL_BUTTON_LEFT:   bit = 0x01; button = 0; break;
	case SDL_BUTTON_RIGHT:  bit = 0x02; button = 1; break;
	case SDL_BUTTON_MIDDLE: bit = 0x04; button = 2; break;
	default: return;
	}
	const bool down = (b.type == SDL_MOUSEBUTTONDOWN);
	s_mouseSeen = true;
	if (down) mouse_b |= bit; else mouse_b &= ~bit;
	switch (bit) {
	case 0x01: s_mouse.left   = down; break;
	case 0x02: s_mouse.right  = down; break;
	case 0x04: s_mouse.middle = down; break;
	}
	if (s_mouseBtnCallback)
		s_mouseBtnCallback(button, down ? 1 : 0, ComputeModifierFlags());
}

} // namespace

bool SdlInput_Initialize()
{
	ClearAllState();
	s_paused = false;
	s_initialized = true;
	LOG_INFO("SDL input: keyboard and mouse via SDL events");
	return true;
}

void RawInput_Shutdown()
{
	if (!s_initialized) return;
	ClearAllState();
	s_initialized = false;
	LOG_INFO("SDL input: shutdown complete");
}

void SdlInput_HandleEvent(const SDL_Event& ev)
{
	if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) {
		SdlJoystick_HandleDeviceEvent(ev);
		return;
	}
	if (!s_initialized || s_paused) return;

	switch (ev.type) {
	case SDL_KEYDOWN:
	case SDL_KEYUP:
		HandleKey(ev.key);
		break;
	case SDL_MOUSEMOTION:
		s_mouseSeen = true;
		s_mouse.dx += ev.motion.xrel;
		s_mouse.dy += ev.motion.yrel;
		s_mouse.x  += ev.motion.xrel;
		s_mouse.y  += ev.motion.yrel;
		if (s_cursorCallback)
			s_cursorCallback((double)s_mouse.x, (double)s_mouse.y);
		break;
	case SDL_MOUSEWHEEL:
		s_mouseSeen = true;
		s_mouse.dwheel += ev.wheel.y * 120;
		s_mouse.wheel  += ev.wheel.y * 120;
		break;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
		HandleMouseButton(ev.button);
		break;
	default:
		break;
	}
}

//------------------------------------------------------------------------------
// sys_input.h
//------------------------------------------------------------------------------
int  isKeyHeld(int vkCode) { return (int)s_lastkey[vkCode & 0xff]; }
bool IsKeyDown(int vkCode) { return key[vkCode & 0xff] != 0; }
bool IsKeyUp(int vkCode)   { return key[vkCode & 0xff] == 0; }

int GetModifierFlags() { return ComputeModifierFlags(); }

void test_clr()
{
	memset(key, 0, sizeof(key));
	memset(s_lastkey, 0, sizeof(s_lastkey));
}

void RawInput_SetPaused(bool paused)
{
	const bool wasPaused = s_paused;
	s_paused = paused;
	if (paused && !wasPaused)
		ClearAllState();
}

int RawInput_GetKeyboardCount() { return 1; }
const char* RawInput_GetKeyboardName(int index) { return index == 0 ? "SDL Keyboard" : "NONE"; }
const char* RawInput_GetKeyboardPath(int index) { return index == 0 ? "sdl:keyboard" : ""; }
int RawInput_FindKeyboardByPath(const char* path)
{
	return (path && strcmp(path, "sdl:keyboard") == 0) ? 0 : -1;
}
int RawInput_KeyboardSeenInput(int index) { return (index == 0 && s_kbdSeen) ? 1 : 0; }
int RawInput_IsKeyDownEx(int index, int vk)
{
	if (index > 0) return 0;          // -1 = merged, 0 = the one keyboard
	return key[vk & 0xff];
}

void SetKeyCallback(KeyCallback cb)                       { s_keyCallback = cb; }
void SetMouseButtonCallback(MouseButtonCallback cb)        { s_mouseBtnCallback = cb; }
void SetCursorPositionCallback(CursorPositionCallback cb)  { s_cursorCallback = cb; }

void set_mouse_mickey_scale(float scale) { s_mouseScale = scale; }

void get_mouse_mickeys(int* mickeyx, int* mickeyy)
{
	const long tx = s_mouse.dx;
	const long ty = s_mouse.dy;
	s_mouse.dx = 0;
	s_mouse.dy = 0;
	if (mickeyx) *mickeyx = (int)(tx * s_mouseScale);
	if (mickeyy) *mickeyy = (int)(ty * s_mouseScale);
}

void get_mouse_mickeys_ex(int index, int* mickeyx, int* mickeyy)
{
	if (index <= 0) { get_mouse_mickeys(mickeyx, mickeyy); return; }
	if (mickeyx) *mickeyx = 0;
	if (mickeyy) *mickeyy = 0;
}

void get_mouse_win(int* mickeyx, int* mickeyy)
{
	int x = 0, y = 0;
	GetSystemWindow().GetMousePos(&x, &y);
	if (mickeyx) *mickeyx = x;
	if (mickeyy) *mickeyy = y;
}

int32_t GetMouseX()     { return (int32_t)s_mouse.x; }
int32_t GetMouseY()     { return (int32_t)s_mouse.y; }
int32_t GetMouseWheel() { return (int32_t)s_mouse.wheel; }
void SetMouseX(int32_t x)         { s_mouse.x = x; }
void SetMouseY(int32_t y)         { s_mouse.y = y; }
void SetMouseWheel(int32_t wheel) { s_mouse.wheel = wheel; }
int32_t GetMouseXChange()     { return (int32_t)s_mouse.dx; }
int32_t GetMouseYChange()     { return (int32_t)s_mouse.dy; }
int32_t GetMouseWheelChange() { return (int32_t)s_mouse.dwheel; }

bool IsMouseLButtonDown() { return s_mouse.left; }
bool IsMouseLButtonUp()   { return !s_mouse.left; }
bool IsMouseRButtonDown() { return s_mouse.right; }
bool IsMouseRButtonUp()   { return !s_mouse.right; }
bool IsMouseMButtonDown() { return s_mouse.middle; }
bool IsMouseMButtonUp()   { return !s_mouse.middle; }

int RawInput_GetMouseCount() { return 1; }
const char* RawInput_GetMouseName(int index) { return index == 0 ? "SDL Mouse" : "NONE"; }
const char* RawInput_GetMousePath(int index) { return index == 0 ? "sdl:mouse" : ""; }
int RawInput_FindMouseByPath(const char* path)
{
	return (path && strcmp(path, "sdl:mouse") == 0) ? 0 : -1;
}
int RawInput_GetMouseButtons(int index) { return index <= 0 ? mouse_b : 0; }
int RawInput_MouseSeenInput(int index)  { return (index == 0 && s_mouseSeen) ? 1 : 0; }

//------------------------------------------------------------------------------
// Keyboard LEDs (cabinet start/player lamps). macOS gives applications no way
// to drive keyboard LEDs, so the state is kept for osd_get_leds() and nothing
// is lit.
//------------------------------------------------------------------------------
static int s_ledMask = 0;
void osd_led_service_start() {}
void osd_led_service_stop()  { s_ledMask = 0; }
void osd_set_leds(int state) { s_ledMask = state & 0x07; }
int  osd_get_leds()          { return s_ledMask; }
