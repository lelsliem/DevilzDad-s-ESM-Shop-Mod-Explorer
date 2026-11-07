// ============================================================================
// BackdropArt.h - the player's own backdrop images, for the OSF page
//
// The overlay uploads these files as D3D12 textures, so the custom theme's
// Background picker can offer them by name. A WebView cannot: OSF UI serves a
// view from a snapshot it takes at startup, so a file dropped beside the page
// is never served, and the plugin's own assets folder is outside the view
// anyway. The bytes therefore ride back over the bridge as a data URL, and this
// is the code that produces one. The overlay side is unchanged and still reads
// the same files straight into a texture.
// ============================================================================

#pragma once

#include <string>
#include <vector>

namespace SFE::BackdropArt
{
	// Names of the backdrop images this install has: the <name> half of
	// space_<name>.<png|jpg|jpeg|bmp> in this build's assets folder, lowercased,
	// sorted, deduplicated. The overlay build has its own folder and its own
	// scan; both only offer art that is actually there.
	[[nodiscard]] std::vector<std::string> Names();

	// One image as "data:image/jpeg;base64,...", scaled so its long edge is at
	// most a_maxEdge (0 keeps the source size). The bridge refuses a message
	// over 8 MiB, and a full-size PNG backdrop is well past that once encoded;
	// JPEG at this size is a few hundred kilobytes and is what a full-window
	// background can actually show. False with a_error set when the file is
	// gone or cannot be decoded or encoded.
	bool DataUrl(const std::string& a_name, unsigned a_maxEdge, std::string& a_dataUrl,
		int& a_width, int& a_height, std::string& a_error);
}
