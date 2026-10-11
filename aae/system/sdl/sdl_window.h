//==========================================================================
// AAE - Another Arcade Emulator
// Copyright (C) 2026 Tim Cottrill - GNU GPL v3 or later.
//==========================================================================
//==============================================================================
// sdl_window.h -- ISystemWindow on SDL2, used for the macOS build (and usable
// on Linux for testing). One SDL window owns either a GL context (created by
// sys_gl.cpp's AAE_SDL path) or a Vulkan surface, chosen before Create().
//==============================================================================
#pragma once

#include "sys_window.h"

struct SDL_Window;
union SDL_Event;

class SdlPresentSurface : public IPresentSurface {
public:
	void SwapBuffers() override;
	void GetDrawableSize(int* w, int* h) const override;
	const char* const* RequiredVkInstanceExtensions(uint32_t* count) const override;
	bool CreateVkSurface(void* instance, void* outSurface) override;
};

class SdlWindow : public ISystemWindow {
public:
	~SdlWindow() override;

	// Must be called before Create(): an SDL window is created either for GL
	// or for Vulkan and cannot switch afterwards.
	void SetUseVulkan(bool vulkan) { m_useVulkan = vulkan; }

	bool Create(const WindowSetup& setup) override;
	void Destroy() override;
	bool PumpEvents() override;

	int   ClientWidth()  const override { return m_clientW; }
	int   ClientHeight() const override { return m_clientH; }
	float DpiScale()     const override { return m_dpiScale; }

	void ToggleBorderlessFullscreen() override;
	void RestoreViewport() override;

	void SetCursorVisible(bool visible) override;
	void EnableCursorClip(bool enable) override;
	void ForceCursorClipUpdate() override;
	void SetMousePos(int x, int y) override;
	void GetMousePos(int* x, int* y) const override;

	IPresentSurface* Presentation() override { return &m_present; }

private:
	void UpdateTitle();
	void RefreshSize();

	bool  m_useVulkan     = false;
	bool  m_fullscreen    = false;
	bool  m_focused       = true;
	bool  m_clipEnabled   = false;
	bool  m_cursorVisible = true;
	int   m_clientW       = 0;   // drawable size in PIXELS (Retina-aware)
	int   m_clientH       = 0;
	float m_dpiScale      = 1.0f;
	SdlPresentSurface m_present;
};

// The one SDL window, for sys_gl.cpp (context creation / swap) and the input
// layer. nullptr before Create().
SDL_Window* sdl_get_window();

// An SDL window is made either for Vulkan or for OpenGL. When a Vulkan
// session falls back to OpenGL (renderer_dispatch.cpp), sys_gl.cpp calls this
// to replace the Vulkan window with an OpenGL one of the same size.
bool sdl_recreate_window_for_opengl();
