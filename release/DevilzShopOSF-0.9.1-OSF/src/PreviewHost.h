#pragma once

// OSF build only: renders an item preview offscreen and hands it to the page.
//
// The standalone shop draws its preview straight into an ImGui viewport: the
// geometry reaches OfflineModel's D3D12 renderer and the result is published as
// a shader-resource descriptor. A WebView cannot sample a GPU descriptor, so
// this host owns a small private D3D12 device, renders the same geometry with
// the same renderer, and copies the frame back as a PNG in the reply. The page
// shows a plain <img> with a data URL, which is why the preview looks exactly
// like the overlay's - it is the same code path, not a second renderer.
//
// The PNG deliberately never touches disk. OSF UI serves a view from a snapshot
// it takes at startup (and, in developer mode, from a mirror it watches), so a
// file written beside the page is not served - and writing there makes the
// watcher reload the interface, which aborts the menu it was opening.
//
// Everything here runs on the thread OSF UI dispatches requests on (the game
// main thread). One request = one frame: no continuous rendering, no present
// hook, nothing that runs when the page is idle.

#include <cstdint>
#include <string>

namespace SFE::OsfPreview
{
	struct Image
	{
		bool          ok{ false };
		std::string   dataUrl;  // "data:image/png;base64,..." for an <img src>
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::string   error;
	};

	// Renders a_formID from the requested camera and returns the image. a_yaw
	// and a_pitch are absolute degrees, a_zoom multiplies the INI framing.
	// Repeat calls with the same camera answer from the session's frame cache
	// instead of rendering again.
	Image Render(std::uint32_t a_formID, float a_yaw, float a_pitch, float a_zoom);

	// Drops the device, the staging buffer, the frame cache and the per-form
	// geometry cache.
	void Shutdown();
}
