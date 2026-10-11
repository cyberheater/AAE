//==========================================================================
// AAE - Another Arcade Emulator
// Copyright (C) 2026 Tim Cottrill - GNU GPL v3 or later.
//==========================================================================
//==============================================================================
// sdl_joystick.cpp -- joystick.h on SDL2's GameController API.
//
// SDL already maps Xbox, PlayStation, Switch and MFi pads to one Xbox-style
// layout (including Sony's face buttons), so this publishes joy[] in exactly
// the slot order evdev_joystick.cpp and Joystick.cpp use.
//==============================================================================
#include "joystick.h"
#include "sdl_input.h"
#include "pad_map.h"
#include "sys_log.h"

#include <SDL.h>

#include <cstring>
#include <string>

int num_joysticks = 0;
int _joystick_installed = 0;
JOYSTICK_INFO joy[MAX_JOYSTICKS] = {};

namespace {

constexpr int DIGITAL_THRESHOLD = 32;     // out of 127, matching Joystick.cpp
constexpr int TRIGGER_THRESHOLD = 30;     // out of 255

const char* const kButtonNames[16] = {
	"A", "B", "X", "Y",
	"LB", "RB",
	"Back", "Start",
	"LStick", "RStick",
	"DPadUp", "DPadDown", "DPadLeft", "DPadRight",
	"LT", "RT"
};

struct PadSlot {
	SDL_GameController* pad = nullptr;
	SDL_JoystickID      instance = -1;
	bool        connected = false;
	std::string name;
	std::string identity;    // SDL joystick GUID string
	uint16_t    buttons = 0; // AAE_JOYBTN_* bitmask
	int         comboHoldFrames[JOY_MAX_COMBOS] = {0};
};

PadSlot s_pads[MAX_JOYSTICKS];
int s_numPads = 0;
JoystickHotplugCallback s_hotplugCallback = nullptr;

uint16_t s_comboMasks[JOY_MAX_COMBOS] = {0};
int      s_numCombos = 0;

int GetComboIndex(uint16_t mask)
{
	for (int i = 0; i < s_numCombos; i++)
		if (s_comboMasks[i] == mask) return i;
	if (s_numCombos >= JOY_MAX_COMBOS) return 0;
	s_comboMasks[s_numCombos] = mask;
	return s_numCombos++;
}

int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void SetAxis(JOYSTICK_AXIS_INFO& axis, int pos)
{
	axis.pos = ClampInt(pos, -128, 127);
	axis.d1  = (axis.pos < -DIGITAL_THRESHOLD) ? 1 : 0;
	axis.d2  = (axis.pos >  DIGITAL_THRESHOLD) ? 1 : 0;
}

void ResetJoyEntry(int index)
{
	JOYSTICK_INFO& j = joy[index];
	j.flags = 0;
	j.num_sticks = 0;
	j.num_buttons = 0;
	j.is_gamepad = 0;
	for (int s = 0; s < MAX_JOYSTICK_STICKS; s++) {
		j.stick[s].flags = 0;
		j.stick[s].num_axis = 0;
		j.stick[s].name = "n/a";
		for (int a = 0; a < MAX_JOYSTICK_AXIS; a++) {
			j.stick[s].axis[a].pos = 0;
			j.stick[s].axis[a].d1 = 0;
			j.stick[s].axis[a].d2 = 0;
			j.stick[s].axis[a].name = "n/a";
		}
	}
	for (int b = 0; b < MAX_JOYSTICK_BUTTONS; b++) {
		j.button[b].b = 0;
		j.button[b].name = "n/a";
	}
}

void SetupDescriptor(int index)
{
	JOYSTICK_INFO& j = joy[index];
	j.flags = JOYFLAG_DIGITAL | JOYFLAG_ANALOGUE | JOYFLAG_SIGNED;
	j.is_gamepad = 1;
	j.num_sticks = 2;
	j.stick[0].flags = j.flags;
	j.stick[0].num_axis = 2;
	j.stick[0].name = "Left Stick";
	j.stick[0].axis[0].name = "X";
	j.stick[0].axis[1].name = "Y";
	j.stick[1].flags = j.flags;
	j.stick[1].num_axis = 2;
	j.stick[1].name = "Right Stick";
	j.stick[1].axis[0].name = "X";
	j.stick[1].axis[1].name = "Y";
	j.num_buttons = 16;
	for (int b = 0; b < 16; b++)
		j.button[b].name = kButtonNames[b];
}

// SDL axes are -32768..32767; AAE's are -128..127.
int ScaleAxis(SDL_GameController* pad, SDL_GameControllerAxis a)
{
	return ClampInt(SDL_GameControllerGetAxis(pad, a) / 256, -128, 127);
}

int SlotForInstance(SDL_JoystickID id)
{
	for (int i = 0; i < s_numPads; i++)
		if (s_pads[i].connected && s_pads[i].instance == id) return i;
	return -1;
}

void AttachPad(int deviceIndex)
{
	if (!SDL_IsGameController(deviceIndex)) return;

	char guid[64] = {};
	SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(deviceIndex), guid, sizeof(guid));
	const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(deviceIndex);
	if (SlotForInstance(id) >= 0) return;   // already open

	// Reuse a disconnected slot with the same identity so player numbers
	// survive a replug.
	int slot = -1;
	for (int i = 0; i < s_numPads; i++)
		if (!s_pads[i].connected && s_pads[i].identity == guid) { slot = i; break; }
	if (slot < 0) {
		if (s_numPads >= MAX_JOYSTICKS) {
			LOG_WARN("SDL: more than %d gamepads; extra pad ignored", MAX_JOYSTICKS);
			return;
		}
		slot = s_numPads++;
	}

	SDL_GameController* pad = SDL_GameControllerOpen(deviceIndex);
	if (!pad) {
		LOG_WARN("SDL: could not open gamepad %d: %s", deviceIndex, SDL_GetError());
		return;
	}

	PadSlot& p = s_pads[slot];
	p.pad       = pad;
	p.instance  = id;
	p.connected = true;
	p.identity  = guid;
	const char* nm = SDL_GameControllerName(pad);
	p.name      = nm ? nm : "Gamepad";
	p.buttons   = 0;
	LOG_INFO("SDL: gamepad %d connected: %s (rumble %s)", slot, p.name.c_str(),
	         SDL_GameControllerHasRumble(pad) ? "yes" : "no");
	if (s_hotplugCallback) s_hotplugCallback(slot, true, nullptr);
}

void DetachPad(int slot)
{
	PadSlot& p = s_pads[slot];
	LOG_INFO("SDL: gamepad %d disconnected: %s", slot, p.name.c_str());
	if (p.pad) SDL_GameControllerClose(p.pad);
	p.pad = nullptr;
	p.instance = -1;
	p.connected = false;
	p.buttons = 0;
	ResetJoyEntry(slot);
	if (s_hotplugCallback) s_hotplugCallback(slot, false, "Controller disconnected");
}

void PublishPad(int slot)
{
	PadSlot& p = s_pads[slot];
	JOYSTICK_INFO& j = joy[slot];
	SDL_GameController* c = p.pad;

	struct Map { SDL_GameControllerButton sdl; uint16_t aae; };
	static const Map kMap[] = {
		{ SDL_CONTROLLER_BUTTON_A,             AAE_JOYBTN_A },
		{ SDL_CONTROLLER_BUTTON_B,             AAE_JOYBTN_B },
		{ SDL_CONTROLLER_BUTTON_X,             AAE_JOYBTN_X },
		{ SDL_CONTROLLER_BUTTON_Y,             AAE_JOYBTN_Y },
		{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  AAE_JOYBTN_LEFT_SHOULDER },
		{ SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, AAE_JOYBTN_RIGHT_SHOULDER },
		{ SDL_CONTROLLER_BUTTON_BACK,          AAE_JOYBTN_BACK },
		{ SDL_CONTROLLER_BUTTON_START,         AAE_JOYBTN_START },
		{ SDL_CONTROLLER_BUTTON_LEFTSTICK,     AAE_JOYBTN_LEFT_THUMB },
		{ SDL_CONTROLLER_BUTTON_RIGHTSTICK,    AAE_JOYBTN_RIGHT_THUMB },
		{ SDL_CONTROLLER_BUTTON_DPAD_UP,       AAE_JOYBTN_DPAD_UP },
		{ SDL_CONTROLLER_BUTTON_DPAD_DOWN,     AAE_JOYBTN_DPAD_DOWN },
		{ SDL_CONTROLLER_BUTTON_DPAD_LEFT,     AAE_JOYBTN_DPAD_LEFT },
		{ SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    AAE_JOYBTN_DPAD_RIGHT },
	};
	uint16_t b = 0;
	for (const Map& m : kMap)
		if (SDL_GameControllerGetButton(c, m.sdl)) b |= m.aae;
	p.buttons = b;

	const int trigL = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  / 128;
	const int trigR = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 128;

	SetupDescriptor(slot);
	SetAxis(j.stick[0].axis[0], ScaleAxis(c, SDL_CONTROLLER_AXIS_LEFTX));
	SetAxis(j.stick[0].axis[1], ScaleAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
	SetAxis(j.stick[1].axis[0], ScaleAxis(c, SDL_CONTROLLER_AXIS_RIGHTX));
	SetAxis(j.stick[1].axis[1], ScaleAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));

	// The D-pad also drives stick 0 at full deflection, as on the other
	// back-ends, so digital games work from either.
	if (b & AAE_JOYBTN_DPAD_LEFT) {
		j.stick[0].axis[0].pos = -128; j.stick[0].axis[0].d1 = 1; j.stick[0].axis[0].d2 = 0;
	} else if (b & AAE_JOYBTN_DPAD_RIGHT) {
		j.stick[0].axis[0].pos =  127; j.stick[0].axis[0].d1 = 0; j.stick[0].axis[0].d2 = 1;
	}
	if (b & AAE_JOYBTN_DPAD_UP) {
		j.stick[0].axis[1].pos = -128; j.stick[0].axis[1].d1 = 1; j.stick[0].axis[1].d2 = 0;
	} else if (b & AAE_JOYBTN_DPAD_DOWN) {
		j.stick[0].axis[1].pos =  127; j.stick[0].axis[1].d1 = 0; j.stick[0].axis[1].d2 = 1;
	}

	j.button[0].b  = (b & AAE_JOYBTN_A) ? 1 : 0;
	j.button[1].b  = (b & AAE_JOYBTN_B) ? 1 : 0;
	j.button[2].b  = (b & AAE_JOYBTN_X) ? 1 : 0;
	j.button[3].b  = (b & AAE_JOYBTN_Y) ? 1 : 0;
	j.button[4].b  = (b & AAE_JOYBTN_LEFT_SHOULDER)  ? 1 : 0;
	j.button[5].b  = (b & AAE_JOYBTN_RIGHT_SHOULDER) ? 1 : 0;
	j.button[6].b  = (b & AAE_JOYBTN_BACK)  ? 1 : 0;
	j.button[7].b  = (b & AAE_JOYBTN_START) ? 1 : 0;
	j.button[8].b  = (b & AAE_JOYBTN_LEFT_THUMB)  ? 1 : 0;
	j.button[9].b  = (b & AAE_JOYBTN_RIGHT_THUMB) ? 1 : 0;
	j.button[10].b = (b & AAE_JOYBTN_DPAD_UP)    ? 1 : 0;
	j.button[11].b = (b & AAE_JOYBTN_DPAD_DOWN)  ? 1 : 0;
	j.button[12].b = (b & AAE_JOYBTN_DPAD_LEFT)  ? 1 : 0;
	j.button[13].b = (b & AAE_JOYBTN_DPAD_RIGHT) ? 1 : 0;
	j.button[14].b = (trigL > TRIGGER_THRESHOLD) ? 1 : 0;
	j.button[15].b = (trigR > TRIGGER_THRESHOLD) ? 1 : 0;
}

void CountConnected()
{
	int n = 0;
	for (int i = 0; i < s_numPads; i++) if (s_pads[i].connected) n++;
	num_joysticks = n;
}

} // namespace

int install_joystick()
{
	if (_joystick_installed) return 0;
	if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
		LOG_ERROR("SDL: game controller init failed: %s", SDL_GetError());
		return -1;
	}
	for (int i = 0; i < MAX_JOYSTICKS; i++) ResetJoyEntry(i);
	s_numPads = 0;
	for (int i = 0; i < SDL_NumJoysticks(); i++)
		AttachPad(i);
	CountConnected();
	_joystick_installed = 1;
	LOG_INFO("SDL joystick: installed, %d pad(s) connected", num_joysticks);
	return 0;
}

void remove_joystick()
{
	if (!_joystick_installed) return;
	for (int i = 0; i < s_numPads; i++) {
		if (s_pads[i].pad) {
			SDL_GameControllerRumble(s_pads[i].pad, 0, 0, 0);
			SDL_GameControllerClose(s_pads[i].pad);
		}
		s_pads[i] = PadSlot{};
	}
	s_numPads = 0;
	num_joysticks = 0;
	for (int i = 0; i < MAX_JOYSTICKS; i++) ResetJoyEntry(i);
	SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
	_joystick_installed = 0;
	LOG_INFO("SDL joystick: removed");
}

int poll_joystick()
{
	if (!_joystick_installed) return -1;

	SDL_GameControllerUpdate();
	for (int slot = 0; slot < s_numPads; slot++)
		if (s_pads[slot].connected) PublishPad(slot);
	CountConnected();
	return 0;
}

void SdlJoystick_HandleDeviceEvent(const SDL_Event& ev)
{
	if (!_joystick_installed) return;
	if (ev.type == SDL_CONTROLLERDEVICEADDED) {
		AttachPad(ev.cdevice.which);
	} else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
		const int slot = SlotForInstance(ev.cdevice.which);
		if (slot >= 0) DetachPad(slot);
	}
	CountConnected();
}

bool joystick_set_rumble(int player, float left_motor_speed, float right_motor_speed)
{
	if (player < 0 || player >= s_numPads || !s_pads[player].pad) return false;
	const Uint16 lo = (Uint16)(ClampInt((int)(left_motor_speed  * 65535.0f), 0, 65535));
	const Uint16 hi = (Uint16)(ClampInt((int)(right_motor_speed * 65535.0f), 0, 65535));
	// Long duration; joystick_stop_rumble ends it, matching the XInput model.
	return SDL_GameControllerRumble(s_pads[player].pad, lo, hi, 10000) == 0;
}

void joystick_stop_rumble(int player)
{
	if (player < 0 || player >= s_numPads || !s_pads[player].pad) return;
	SDL_GameControllerRumble(s_pads[player].pad, 0, 0, 0);
}

void set_joystick_hotplug_callback(JoystickHotplugCallback callback)
{
	s_hotplugCallback = callback;
}

bool joystick_check_combo(int /*player*/, uint16_t buttonMask)
{
	const int idx = GetComboIndex(buttonMask);
	bool triggered = false;
	for (int p = 0; p < s_numPads; ++p) {
		const bool held = s_pads[p].connected &&
			(s_pads[p].buttons & buttonMask) == buttonMask;
		if (pad_map_combo_step(held, &s_pads[p].comboHoldFrames[idx]))
			triggered = true;
	}
	return triggered;
}

bool joystick_using_xinput() { return false; }
const char* joystick_driver_name() { return "SDL GameController"; }
void joystick_device_change() {}
int joystick_device_count() { return s_numPads; }

const char* joystick_get_display_name(int index)
{
	if (index < 0 || index >= s_numPads) return "NONE";
	return s_pads[index].name.c_str();
}

int joystick_is_connected(int index)
{
	if (index < 0 || index >= s_numPads) return 0;
	return s_pads[index].connected ? 1 : 0;
}

const char* joystick_get_id(int index)
{
	static std::string s_id;
	if (index < 0 || index >= s_numPads) return "";
	s_id = "SDL:" + s_pads[index].identity;
	return s_id.c_str();
}

int joystick_find_by_id(const char* id)
{
	if (!id || strncmp(id, "SDL:", 4) != 0) return -1;
	const char* want = id + 4;
	for (int i = 0; i < s_numPads; i++)
		if (s_pads[i].identity == want) return i;
	return -1;
}

bool joystick_any_connected() { return num_joysticks > 0; }
