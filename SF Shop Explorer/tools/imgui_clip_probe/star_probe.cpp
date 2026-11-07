// Headless probe for the catalog row layout.
//
// It lays out the item table the way the shop's overlay does: five columns,
// the favourite star in the NAME cell on the same line as the label, the label
// submitted as a SpanAllColumns Selectable, under a ScrollY table with one
// frozen header row. For the first drawn rows it prints the row band
// (RowPosY1..RowPosY2) and the Y of the star item and of the name item, so a
// cell that starts at a different Y from its row is visible in the output
// instead of only on screen.
//
// The last row carries a newline in its label on purpose: that is the shape of
// the reported bug (a blank, double-height row at the top of a NAME sort), and
// it shows why FormDisplayName folds control bytes to spaces before the label
// reaches ImGui.
//
// Build: xmake -P . ; run build/.../star_probe
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
	constexpr float kStarSlot = 22.0f;
	constexpr float kStarGap = 6.0f;

	std::vector<std::string> g_names;

	void BuildNames()
	{
		g_names.clear();
		const char* seeds[] = { " Golden Glow", "\"Priceless\" Gem", "\"The Great Serpent\" Lie",
			"(Cheat Terminal Portable)", "(unnamed) [000127BE]", "Bo-Katan - Armor" };
		for (int i = 0; i < 40; ++i) {
			g_names.push_back(seeds[i % 6]);
		}
		// The label as a mod name could reach ImGui, and the same name after
		// FormDisplayName's fold. The first measures two lines and draws none.
		g_names[4] = "Golden\nGlow";
	}

	void DrawStar(ImDrawList* a_draw, const ImVec2& a_center, float a_radius, ImU32 a_color)
	{
		constexpr float kPi = 3.14159265358979323846f;
		constexpr int kPoints = 5;
		const float inner = a_radius * 0.44f;
		ImVec2 points[kPoints * 2];
		for (int i = 0; i < kPoints * 2; ++i) {
			const float angle = -kPi * 0.5f + static_cast<float>(i) * kPi / static_cast<float>(kPoints);
			const float radius = (i % 2 == 0) ? a_radius : inner;
			points[i] = ImVec2(a_center.x + std::cos(angle) * radius, a_center.y + std::sin(angle) * radius);
		}
		for (int i = 0; i < kPoints * 2; ++i) {
			a_draw->AddLine(points[i], points[(i + 1) % (kPoints * 2)], a_color, 1.5f);
		}
	}

	void DrawFrame(int a_frame)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(900, 640));
		ImGui::Begin("probe", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		if (ImGui::BeginTable("reference_form_table", 5,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
				ImVec2(0.0f, 0.0f))) {
			ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("TYPE", ImGuiTableColumnFlags_WidthFixed, 62.0f);
			ImGui::TableSetupColumn("VALUE", ImGuiTableColumnFlags_WidthFixed, 70.0f);
			ImGui::TableSetupColumn("WT", ImGuiTableColumnFlags_WidthFixed, 56.0f);
			ImGui::TableSetupColumn("FORMID", ImGuiTableColumnFlags_WidthFixed, 96.0f);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			ImGui::TableSetColumnIndex(0);
			{
				const ImVec2 start = ImGui::GetCursorScreenPos();
				const float lineHeight = ImGui::GetTextLineHeight();
				ImGui::Dummy(ImVec2(kStarSlot, lineHeight));
				DrawStar(ImGui::GetWindowDrawList(), ImVec2(start.x + kStarSlot * 0.5f, start.y + lineHeight * 0.5f),
					lineHeight * 0.36f, ImGui::GetColorU32(ImGuiCol_TextDisabled));
			}
			ImGui::SameLine(0.0f, kStarGap);
			const char* titles[5]{ "NAME", "TYPE", "VALUE", "WT", "FORMID" };
			for (int c = 0; c < 5; ++c) {
				if (c > 0) {
					ImGui::TableSetColumnIndex(c);
				}
				ImGui::SmallButton(titles[c]);
			}

			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(g_names.size()));
			std::printf("frame %d\n", a_frame);
			int printed = 0;
			float previousRowY1 = -1.0f;
			while (clipper.Step()) {
				for (int d = clipper.DisplayStart; d < clipper.DisplayEnd && printed < 6; ++d) {
					ImGui::TableNextRow();
					const float rowY1 = ImGui::GetCursorScreenPos().y;
					ImGui::TableSetColumnIndex(0);
					const ImVec2 cellPos = ImGui::GetCursorScreenPos();
					const float lineHeight = ImGui::GetTextLineHeight();
					ImGui::InvisibleButton("##favourite", ImVec2(kStarSlot, lineHeight));
					const float starY = ImGui::GetItemRectMin().y;
					DrawStar(ImGui::GetWindowDrawList(), ImVec2(cellPos.x + kStarSlot * 0.5f, cellPos.y + lineHeight * 0.5f),
						lineHeight * 0.36f, ImGui::GetColorU32(ImGuiCol_TextDisabled));
					ImGui::SameLine(0.0f, kStarGap);
					const std::string& name = g_names[static_cast<std::size_t>(d)];
					const float nameCellY = ImGui::GetCursorScreenPos().y;
					ImGui::Selectable(name.c_str(), d == 0, ImGuiSelectableFlags_SpanAllColumns);
					const float nameY = ImGui::GetItemRectMin().y;
					std::printf("  d=%d rowTop=%.2f lead=%.2f star=(y=%.2f) nameCell=(y=%.2f) name=(y=%.2f) '%s'%s\n",
						d, rowY1, previousRowY1 < 0.0f ? 0.0f : rowY1 - previousRowY1, starY, nameCellY, nameY, name.c_str(),
						std::fabs(starY - nameCellY) < 0.01f ? "" : "  <-- cells not on the same line");
					previousRowY1 = rowY1;
					for (int c = 1; c < 5; ++c) {
						ImGui::TableSetColumnIndex(c);
						ImGui::Text("c%d", c);
					}
					++printed;
				}
			}
			ImGui::EndTable();
		}
		ImGui::End();
		ImGui::Render();
	}
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(1280, 720);
	io.DeltaTime = 1.0f / 60.0f;
	io.IniFilename = nullptr;
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	BuildNames();
	for (int frame = 0; frame < 3; ++frame) {
		DrawFrame(frame);
	}
	ImGui::DestroyContext();
	return 0;
}
