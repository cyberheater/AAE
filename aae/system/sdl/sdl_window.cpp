//==========================================================================
// AAE - Another Arcade Emulator
// Copyright (C) 2026 Tim Cottrill - GNU GPL v3 or later.
//==========================================================================
//==============================================================================
// sdl_window.cpp -- see sdl_window.h.
//==============================================================================
#include "sdl_window.h"
#include "sdl_input.h"

#include "sys_gl.h"
#include "sys_input.h"   // RawInput_SetPaused, on focus change
#include "sys_log.h"

#include <SDL.h>
#include <SDL_vulkan.h>

void emulator_on_window_resize(int newW, int newH);

static SDL_Window* s_window = nullptr;
static SdlWindow*  s_owner  = nullptr;
static WindowSetup s_lastSetup;

SDL_Window* sdl_get_window() { return s_window; }

//------------------------------------------------------------------------------
// Presentation
//------------------------------------------------------------------------------
void SdlPresentSurface::SwapBuffers()
{
	if (s_window) SDL_GL_SwapWindow(s_window);
}

void SdlPresentSurface::GetDrawableSize(int* w, int* h) const
{
	int dw = 0, dh = 0;
	if (s_window) {
		if (SDL_GetWindowFlags(s_window) & SDL_WINDOW_VULKAN)
			SDL_Vulkan_GetDrawableSize(s_window, &dw, &dh);
		else
			SDL_GL_GetDrawableSize(s_window, &dw, &dh);
	}
	if (w) *w = dw;
	if (h) *h = dh;
}

const char* const* SdlPresentSurface::RequiredVkInstanceExtensions(uint32_t* count) const
{
	static const char* s_exts[8] = {};
	static unsigned int s_count = 0;
	if (s_count == 0 && s_window) {
		unsigned int n = 0;
		if (SDL_Vulkan_GetInstanceExtensions(s_window, &n, nullptr) && n <= 8 &&
		    SDL_Vulkan_GetInstanceExtensions(s_window, &n, s_exts))
			s_count = n;
		else
			LOG_ERROR("SDL: SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
	}
	if (count) *count = s_count;
	return s_exts;
}

bool SdlPresentSurface::CreateVkSurface(void* instance, void* outSurface)
{
	if (!s_window || !instance || !outSurface) return false;
	VkSurfaceKHR surface = 0;
	if (!SDL_Vulkan_CreateSurface(s_window, (VkInstance)instance, &surface)) {
		LOG_ERROR("SDL: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
		return false;
	}
	*(VkSurfaceKHR*)outSurface = surface;
	return true;
}

//------------------------------------------------------------------------------
// Window
//------------------------------------------------------------------------------
SdlWindow::~SdlWindow()
{
	Destroy();
}

bool SdlWindow::Create(const WindowSetup& setup)
{
	s_owner     = this;
	s_lastSetup = setup;

	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
		LOG_ERROR("SDL: video init failed: %s", SDL_GetError());
		return false;
	}

	Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
	if (m_useVulkan) {
		flags |= SDL_WINDOW_VULKAN;
	} else {
		// Attributes must be set BEFORE the window exists. macOS only offers a
		// core profile at 3.2+, and only forward-compatible, so ask for 3.2
		// core FC: it actually returns the highest core version (4.1), which
		// runs the renderer's "#version 330 core" shaders.
		flags |= SDL_WINDOW_OPENGL;
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#else
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
		SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
	}
	if (setup.useFullscreen)
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

	const int w = setup.windowWidth  > 0 ? setup.windowWidth  : 1024;
	const int h = setup.windowHeight > 0 ? setup.windowHeight : 768;
	const int pos = setup.centerWindow ? SDL_WINDOWPOS_CENTERED : SDL_WINDOWPOS_UNDEFINED;

	s_window = SDL_CreateWindow("AAE", pos, pos, w, h, flags);
	if (!s_window) {
		LOG_ERROR("SDL: window creation failed: %s", SDL_GetError());
		return false;
	}
	m_fullscreen = setup.useFullscreen;
	GetWindowSetup().borderlessFullscreen = m_fullscreen;
	SDL_SetWindowMinimumSize(s_window, setup.minWindowWidth, setup.minWindowHeight);

	RefreshSize();
	LOG_INFO("SdlWindow: created %dx%d (%s), drawable %dx%d, dpi scale %.2f",
	         w, h, m_useVulkan ? "Vulkan" : "OpenGL", m_clientW, m_clientH, m_dpiScale);
	UpdateTitle();
	return true;
}

bool sdl_recreate_window_for_opengl()
{
	if (!s_owner) return false;
	if (s_window && (SDL_GetWindowFlags(s_window) & SDL_WINDOW_OPENGL)) return true;
	LOG_INFO("SdlWindow: recreating the window for OpenGL (Vulkan fallback)");
	s_owner->Destroy();
	s_owner->SetUseVulkan(false);
	WindowSetup setup = s_lastSetup;
	setup.useFullscreen = GetWindowSetup().borderlessFullscreen;
	if (!s_owner->Create(setup)) return false;
	s_owner->EnableCursorClip(GetWindowSetup().cursorClipEnabled);
	return true;
}

void SdlWindow::Destroy()
{
	if (!s_window) return;
	SDL_SetRelativeMouseMode(SDL_FALSE);
	SDL_DestroyWindow(s_window);
	s_window = nullptr;
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void SdlWindow::RefreshSize()
{
	if (!s_window) return;
	int lw = 0, lh = 0, pw = 0, ph = 0;
	SDL_GetWindowSize(s_window, &lw, &lh);
	m_present.GetDrawableSize(&pw, &ph);
	const bool changed = (pw != m_clientW) || (ph != m_clientH);
	m_clientW  = pw;
	m_clientH  = ph;
	m_dpiScale = (lw > 0) ? (float)pw / (float)lw : 1.0f;
	GetWindowSetup().clientWidth  = pw;
	GetWindowSetup().clientHeight = ph;
	GetWindowSetup().dpiScale     = m_dpiScale;
	if (changed && pw > 0 && ph > 0)
		emulator_on_window_resize(pw, ph);
}

bool SdlWindow::PumpEvents()
{
	if (!s_window) return false;
	bool keepRunning = true;

	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {
		switch (ev.type) {
		case SDL_QUIT:
			LOG_INFO("SdlWindow: quit requested");
			keepRunning = false;
			break;
		case SDL_WINDOWEVENT:
			switch (ev.window.event) {
			case SDL_WINDOWEVENT_SIZE_CHANGED:
			case SDL_WINDOWEVENT_RESIZED:
				RefreshSize();
				break;
			case SDL_WINDOWEVENT_FOCUS_GAINED:
				m_focused = true;
				GetWindowSetup().isFocused = true;
				RawInput_SetPaused(false);
				ForceCursorClipUpdate();
				break;
			case SDL_WINDOWEVENT_FOCUS_LOST:
				m_focused = false;
				GetWindowSetup().isFocused = false;
				RawInput_SetPaused(true);
				ForceCursorClipUpdate();
				break;
			case SDL_WINDOWEVENT_MINIMIZED:
				GetWindowSetup().isMinimized = true;
				break;
			case SDL_WINDOWEVENT_RESTORED:
				GetWindowSetup().isMinimized = false;
				break;
			default:
				break;
			}
			break;
		case SDL_MOUSEBUTTONDOWN:
			// Clicking back into the window re-arms capture after F9 released
			// it, the same window-management rule winmain.cpp follows.
			if (!m_clipEnabled &&
			    (ev.button.button == SDL_BUTTON_LEFT || ev.button.button == SDL_BUTTON_RIGHT)) {
				EnableCursorClip(true);
			}
			SdlInput_HandleEvent(ev);
			break;
		default:
			SdlInput_HandleEvent(ev);
			break;
		}
	}
	return keepRunning;
}

void SdlWindow::ToggleBorderlessFullscreen()
{
	if (!s_window) return;
	m_fullscreen = !m_fullscreen;
	GetWindowSetup().borderlessFullscreen = m_fullscreen;
	SDL_SetWindowFullscreen(s_window, m_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	RefreshSize();
	LOG_INFO("SdlWindow: fullscreen %s", m_fullscreen ? "on" : "off");
}

void SdlWindow::RestoreViewport()
{
	if (m_fullscreen)
		ToggleBorderlessFullscreen();
}

void SdlWindow::SetCursorVisible(bool visible)
{
	if (visible == m_cursorVisible) return;
	m_cursorVisible = visible;
	SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
}

void SdlWindow::UpdateTitle()
{
	if (!s_window) return;
	SDL_SetWindowTitle(s_window, m_clipEnabled
		? "AAE - Mouse Captured (F9 to release)"
		: "AAE");
}

void SdlWindow::EnableCursorClip(bool enable)
{
	m_clipEnabled = enable;
	GetWindowSetup().cursorClipEnabled = enable;
	UpdateTitle();
	ForceCursorClipUpdate();
}

void SdlWindow::ForceCursorClipUpdate()
{
	// Relative mode both hides and confines the pointer and delivers raw
	// motion deltas, which is what the trackball/yoke games read as mickeys.
	const bool want = m_clipEnabled && m_focused;
	SetCursorVisible(!want);
	SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
}

void SdlWindow::SetMousePos(int x, int y)
{
	if (!s_window) return;
	// Callers work in drawable pixels; SDL warps in window points.
	const float s = m_dpiScale > 0.0f ? m_dpiScale : 1.0f;
	SDL_WarpMouseInWindow(s_window, (int)(x / s), (int)(y / s));
}

void SdlWindow::GetMousePos(int* x, int* y) const
{
	int mx = 0, my = 0;
	SDL_GetMouseState(&mx, &my);
	if (x) *x = (int)(mx * m_dpiScale);
	if (y) *y = (int)(my * m_dpiScale);
}
