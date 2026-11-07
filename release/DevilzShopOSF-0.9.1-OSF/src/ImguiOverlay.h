// ============================================================================
// ImguiOverlay.h - the overlay's public surface
//
// The only header ShopExplorer.cpp needs for drawing. Every call is safe from
// the task thread: the drawing calls take the overlay's mutex and hand the
// frame to the render thread, which owns the device.
// ============================================================================

#pragma once

#include <cstdint>

#include "imgui.h"
#include "ItemCapture.h"

namespace SFE::Overlay
{
	bool Install();
	void ProcessPendingActions();
	int ToggleKey();

	// Edge-triggered key press taken straight from the game window's WM_KEYDOWN
	// stream, independent of ImGui's key state. The shop uses it for the keys
	// that must always work - Escape and the catalog arrows - because ImGui can
	// silently drop key events (a stale focus loss clears every key and every
	// character on each frame) while the mouse keeps working through this
	// overlay's raw-input cursor. Reads clear the bit, so one press is one edge.
	[[nodiscard]] bool ConsumeKeyPress(int a_vk);

	void SetDrawingEnabled(bool enabled);
	[[nodiscard]] bool IsDrawingEnabled();

	// Whether the shop may be opened at all: the DXGI factory hook is installed
	// and [overlay] render is 4. Opening freezes the world and starts the score,
	// so the browser refuses to open on anything less - a shop that cannot draw
	// left the player in a frozen world with music and no interface, which is
	// the "PgUp starts the music and the screen freezes" report.
	[[nodiscard]] bool CanDraw();

	// False when the game's swap chain predates the factory hook, i.e. the
	// overlay has nothing to present through for this session. Only used to name
	// the cause in that refusal.
	[[nodiscard]] bool PresentHooked();
	[[nodiscard]] int RenderMode();

	// Count of frames the shop has actually been drawn on. Read once per tick
	// while the shop is open as a liveness probe: a count that never moves means
	// the player is looking at a frozen world instead of a shop.
	[[nodiscard]] std::uint64_t FramesDrawn();
	void DrawFrame();
	void Shutdown();

	[[nodiscard]] ItemCapture::Snapshot ItemCaptureSnapshot();

	[[nodiscard]] ImTextureID ShopLogoTexture();
	[[nodiscard]] ImTextureID ChestTexture();
	[[nodiscard]] ImTextureID CreditIconTexture();
	[[nodiscard]] ImTextureID SpaceTexture();

	// Per-theme art. Themes whose suffix asset is missing fall back to the
	// shared ShopLogo/Chest/space textures, so an absent or not-yet-generated
	// theme image degrades to the current look instead of disappearing.
	// a_suffix is the lowercase theme id, e.g. "heaven".
	[[nodiscard]] ImTextureID ThemeChestTexture(const char* a_suffix);
	[[nodiscard]] ImTextureID ThemeSpaceTexture(const char* a_suffix);

	[[nodiscard]] float UiScale();

	// Strength of the themed space_*.png backdrop, 0-255, from
	// [overlay] backdropAlpha. A setting because the per-theme art varies a
	// lot in brightness and one fixed value cannot serve them all.
	[[nodiscard]] float BackdropAlpha();

	// Extra font sizes baked into the atlas at init, for the header banner.
	// May be null if no system TTF was found; callers fall back to the default
	// font rather than skipping the text.
	[[nodiscard]] ImFont* TitleFont();
	[[nodiscard]] ImFont* CaptionFont();
	[[nodiscard]] ImFont* BodyFont();
	void SetBodyFontSize(int a_size);
}