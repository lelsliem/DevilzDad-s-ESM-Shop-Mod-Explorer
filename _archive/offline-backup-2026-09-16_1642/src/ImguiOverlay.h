#pragma once

#include <cstdint>

#include "imgui.h"
#include "ItemCapture.h"

namespace SFE::Overlay
{
	bool Install();
	void ProcessPendingActions();
	int ToggleKey();
	void SetDrawingEnabled(bool enabled);
	[[nodiscard]] bool IsDrawingEnabled();
	void DrawFrame();
	void Shutdown();

	[[nodiscard]] ItemCapture::Snapshot ItemCaptureSnapshot();

	[[nodiscard]] ImTextureID ShopLogoTexture();
	[[nodiscard]] ImTextureID ChestTexture();
	[[nodiscard]] ImTextureID CreditIconTexture();
	[[nodiscard]] ImTextureID SpaceTexture();
}