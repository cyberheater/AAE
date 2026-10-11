//==========================================================================
// AAE - Another Arcade Emulator
// Copyright (C) 2026 Tim Cottrill - GNU GPL v3 or later.
//==========================================================================
//==============================================================================
// sdl_input.h -- keyboard and mouse (sys_input.h) on SDL2 events.
//
// SDL gives one merged keyboard and one merged mouse, so the per-device
// RawInput_* queries report exactly one of each. Gamepads live in
// sdl_joystick.cpp.
//==============================================================================
#pragma once

union SDL_Event;

bool SdlInput_Initialize();
void SdlInput_HandleEvent(const SDL_Event& ev);   // called by SdlWindow::PumpEvents
void RawInput_Shutdown();

// Gamepad hotplug, in sdl_joystick.cpp. The window pump owns the SDL event
// queue, so device events are routed here from SdlInput_HandleEvent.
void SdlJoystick_HandleDeviceEvent(const SDL_Event& ev);
