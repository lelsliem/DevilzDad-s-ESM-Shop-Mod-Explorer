// Standalone ImGui harness for the "Backspace stays pressed" report.
//
// The overlay subclasses the game window and swallows the key-up of every key
// whose key-down it swallowed (see ImguiOverlay.cpp, ConsumeSwallowedKeyUp).
// Swallowing the up keeps the game from seeing an orphan ESC up edge, but the
// same up never reaches ImGui either, so io.KeysData[Backspace].Down stays true
// for the rest of the session. This harness feeds ImGui exactly that: one
// key-down, then N frames with no key-up, and counts how many characters the
// InputText loses. A control run adds the key-up on the next frame.
//
// The field is focused with a simulated mouse click, not SetKeyboardFocusHere:
// activation by keyboard/promise selects the whole buffer, so a single
// backspace would then delete everything and hide what is being measured.
//
// Build: build_backspace_latch_harness.bat
// Run:   backspace_latch_harness.exe

#include "imgui.h"

#include <cstdio>
#include <cstring>

namespace
{
	constexpr std::size_t kClickFrame = 2;
	constexpr std::size_t kPressFrame = 6;  // the field owns the keyboard by now

	// One simulated frame of the shop's search box.
	std::size_t RunFrames(const bool a_sendKeyUp, char* a_buffer, const std::size_t a_frames, const bool a_verbose)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(1280.0f, 720.0f);
		io.DeltaTime = 1.0f / 60.0f;
		io.IniFilename = nullptr;
		// No renderer backend here: tell ImGui 1.92 it owns the font atlas, so
		// NewFrame builds and manages it instead of asserting that a backend did.
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

		const std::size_t start = std::strlen(a_buffer);
		for (std::size_t frame = 0; frame < a_frames; ++frame) {
			if (frame == kClickFrame) {
				io.AddMousePosEvent(200.0f, 40.0f);
				io.AddMouseButtonEvent(0, true);
			}
			if (frame == kClickFrame + 1) {
				io.AddMouseButtonEvent(0, false);
			}
			if (frame == kPressFrame) {
				io.AddKeyEvent(ImGuiKey_Backspace, true);
			}
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(0, 0));
			ImGui::SetNextWindowSize(ImVec2(400, 80));
			ImGui::Begin("search", nullptr, ImGuiWindowFlags_NoSavedSettings);
			ImGui::SetCursorScreenPos(ImVec2(10.0f, 30.0f));
			ImGui::InputText("##shop_search", a_buffer, 128);
			ImGui::End();
			ImGui::Render();
			if (a_verbose && frame >= kPressFrame && frame <= kPressFrame + 4) {
				std::printf("  frame %zu: down=%d len=%zu\n", frame,
					io.KeysData[ImGuiKey_Backspace - ImGuiKey_NamedKey_BEGIN].Down, std::strlen(a_buffer));
			}
			if (a_sendKeyUp && frame == kPressFrame + 1) {
				io.AddKeyEvent(ImGuiKey_Backspace, false);
			}
		}
		return start - std::strlen(a_buffer);
	}
}

int main()
{
	char latched[128]{};
	std::strcpy(latched, "alienzabrakhorn");
	char released[128]{};
	std::strcpy(released, "alienzabrakhorn");

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	const std::size_t latchedDeleted = RunFrames(false, latched, 90, true);
	ImGui::DestroyContext();

	ImGui::CreateContext();
	const std::size_t releasedDeleted = RunFrames(true, released, 90, false);
	ImGui::DestroyContext();

	std::printf("no key-up   : deleted %zu char(s), left \"%s\"\n", latchedDeleted, latched);
	std::printf("with key-up : deleted %zu char(s), left \"%s\"\n", releasedDeleted, released);
	return 0;
}
