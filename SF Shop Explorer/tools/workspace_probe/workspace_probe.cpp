// Headless probe for the shop workspace's four arrangements.
//
// The overlay's workspace is one table whose three panes (plugins, items,
// basket) draw in source order; an arrangement only changes which column each
// pane lands in, and the "catalog top, basket bottom" preset instead uses two
// rows with a nested table on top. This probe lays out that same table without
// a renderer and checks the geometry each pane actually receives, because the
// failure it exists for was invisible in the source: selecting a column with
// TableSetColumnIndex() does not start a row and does not lock the table
// layout, so the first cell was drawn at an unset row position and the shop
// died on its first frame.
//
// It runs every arrangement twice - once as the shop now does it, once with the
// row call removed - and prints the pane sizes it measured. The fixed run must
// give every pane a positive width and height; the broken run shows the zero
// geometry that used to reach the renderer.
//
// Build: xmake -P . ; run build/.../workspace_probe
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
	enum class Layout : int
	{
		kClassic = 0,
		kBasketCentre,
		kBasketLeft,
		kCatalogTop,
		kTotal
	};

	const char* LayoutName(Layout a_layout)
	{
		switch (a_layout)
		{
		case Layout::kClassic: return "Classic";
		case Layout::kBasketCentre: return "Basket centre";
		case Layout::kBasketLeft: return "Basket left";
		case Layout::kCatalogTop: return "Catalog top, basket bottom";
		default: return "?";
		}
	}

	struct Pane
	{
		float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
	};

	struct Measurement
	{
		Pane plugins{};
		Pane items{};
		Pane basket{};
		bool ran = false;
	};

	// Draws one pane as the shop does: a bordered child that fills its cell.
	Pane PaneBody(const char* a_label, bool a_broken)
	{
		Pane pane{};
		ImGui::BeginChild(a_label, ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
		const ImVec2 pos = ImGui::GetWindowPos();
		const ImVec2 size = ImGui::GetWindowSize();
		pane.x = pos.x;
		pane.y = pos.y;
		pane.w = size.x;
		pane.h = size.y;
		if (!a_broken)
		{
			ImGui::Text("%s", a_label);
		}
		ImGui::EndChild();
		return pane;
	}

	Measurement DrawWorkspace(Layout a_layout, bool a_skipRow)
	{
		Measurement out{};
		const int  layoutMode = static_cast<int>(a_layout);
		const bool stacked = a_layout == Layout::kCatalogTop;
		int        pluginColumn = 0;
		int        itemsColumn = 1;
		int        detailsColumn = 2;
		if (layoutMode == static_cast<int>(Layout::kBasketCentre))
		{
			detailsColumn = 1;
			itemsColumn = 2;
		}
		else if (layoutMode == static_cast<int>(Layout::kBasketLeft))
		{
			detailsColumn = 0;
			pluginColumn = 1;
			itemsColumn = 2;
		}
		if (stacked)
		{
			pluginColumn = 0;
			itemsColumn = 1;
			detailsColumn = 0;
		}
		const float pluginWidth = 280.0f;
		const float detailsWidth = 340.0f;
		const float workspaceOffset = -32.0f;
		const float workspaceBody = std::max(120.0f, ImGui::GetContentRegionAvail().y + workspaceOffset);

		// One table ID per arrangement. With a shared ID ImGui reuses the first
		// arrangement's column widths, so switching presets would show the old
		// widths until the layout was next rebuilt.
		ImGui::PushID(layoutMode);
		if (ImGui::BeginTable("reference_workspace", stacked ? 1 : 3,
				ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
				ImVec2(0.0f, workspaceOffset)))
		{
			if (stacked)
			{
				ImGui::TableSetupColumn("workspace", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				// Capped by a child: a table's height argument only bounds a
				// scrolling table, so the uncapped nested table took the whole
				// pane and left the basket 29 px.
				const float splitHeight = std::clamp(workspaceBody * 0.6f, 140.0f, std::max(140.0f, workspaceBody - 180.0f));
				ImGui::BeginChild("workspace_catalog_top", ImVec2(0.0f, splitHeight), 0);
				ImGui::BeginTable("workspace_catalog_split", 2,
					ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
					ImVec2(0.0f, 0.0f));
				ImGui::TableSetupColumn("plugins", ImGuiTableColumnFlags_WidthFixed, pluginWidth);
				ImGui::TableSetupColumn("items", ImGuiTableColumnFlags_WidthStretch);
				if (!a_skipRow)
				{
					ImGui::TableNextRow();
				}
			}
			else
			{
				for (int column = 0; column < 3; ++column)
				{
					if (column == pluginColumn)
					{
						ImGui::TableSetupColumn("plugins", ImGuiTableColumnFlags_WidthFixed, pluginWidth);
					}
					else if (column == detailsColumn)
					{
						ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthFixed, detailsWidth);
					}
					else
					{
						ImGui::TableSetupColumn("items", ImGuiTableColumnFlags_WidthStretch);
					}
				}
				if (!a_skipRow)
				{
					ImGui::TableNextRow();
				}
			}

			ImGui::TableSetColumnIndex(pluginColumn);
			out.plugins = PaneBody("plugins", a_skipRow);
			ImGui::TableSetColumnIndex(itemsColumn);
			out.items = PaneBody("items", a_skipRow);
			if (stacked)
			{
				ImGui::EndTable();
				ImGui::EndChild();
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
			}
			ImGui::TableSetColumnIndex(detailsColumn);
			out.basket = PaneBody("basket", a_skipRow);
			ImGui::EndTable();
			out.ran = true;
		}
		ImGui::PopID();
		return out;
	}

	bool Sane(const Measurement& a_m)
	{
		return a_m.ran && a_m.plugins.w > 20.0f && a_m.plugins.h > 20.0f &&
		       a_m.items.w > 20.0f && a_m.items.h > 20.0f &&
		       a_m.basket.w > 20.0f && a_m.basket.h > 20.0f;
	}

	void Print(const char* a_tag, Layout a_layout, const Measurement& a_m)
	{
		std::printf("%-8s %-24s plugins=(%.0f,%.0f %.0fx%.0f) items=(%.0f,%.0f %.0fx%.0f) basket=(%.0f,%.0f %.0fx%.0f)\n",
			a_tag, LayoutName(a_layout),
			a_m.plugins.x, a_m.plugins.y, a_m.plugins.w, a_m.plugins.h,
			a_m.items.x, a_m.items.y, a_m.items.w, a_m.items.h,
			a_m.basket.x, a_m.basket.y, a_m.basket.w, a_m.basket.h);
	}

	void Frame(Layout a_layout, bool a_skipRow, Measurement& a_out)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(1280, 720));
		ImGui::Begin("probe", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		a_out = DrawWorkspace(a_layout, a_skipRow);
		ImGui::End();
		ImGui::Render();
	}
}

int main(int argc, char** argv)
{
	// --broken runs the pre-fix form (no row call). It crashes, which is the
	// point: that is the CTD the shop shipped for one build.
	const bool broken = argc > 1 && std::string(argv[1]) == "--broken";
	setvbuf(stdout, nullptr, _IONBF, 0);
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(1280, 720);
	io.DeltaTime = 1.0f / 60.0f;
	io.IniFilename = nullptr;
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

	int failures = 0;
	if (broken)
	{
		for (int layout = 0; layout < static_cast<int>(Layout::kTotal); ++layout)
		{
			Measurement brokenRun{};
			Frame(static_cast<Layout>(layout), true, brokenRun);
			Print("no-row", static_cast<Layout>(layout), brokenRun);
		}
		ImGui::DestroyContext();
		return 0;
	}
	// Two frames per arrangement: the first submits the table and locks the
	// layout, the second is what the player would actually be looking at.
	for (int layout = 0; layout < static_cast<int>(Layout::kTotal); ++layout)
	{
		const auto mode = static_cast<Layout>(layout);
		Measurement fixed{};
		for (int frame = 0; frame < 2; ++frame)
		{
			Frame(mode, false, fixed);
		}
		Print("fixed", mode, fixed);
		if (!Sane(fixed))
		{
			std::printf("  FAIL: a pane was given no room\n");
			++failures;
		}
		if (mode == Layout::kCatalogTop)
		{
			const float catalogBottom = std::max(fixed.plugins.y + fixed.plugins.h, fixed.items.y + fixed.items.h);
			if (fixed.basket.y + 1.0f < catalogBottom)
			{
				std::printf("  FAIL: basket (y=%.0f) is not below the catalog (bottom=%.0f)\n", fixed.basket.y, catalogBottom);
				++failures;
			}
			// 29 px passed the positive-size check while being unusable, so the
			// stacked basket gets a real minimum of its own.
			if (fixed.basket.h < 120.0f)
			{
				std::printf("  FAIL: basket row is only %.0f px tall\n", fixed.basket.h);
				++failures;
			}
		}
		else
		{
			const float widest = std::max(fixed.plugins.w, fixed.items.w);
			(void)widest;
			if (std::fabs(fixed.plugins.y - fixed.items.y) > 1.0f || std::fabs(fixed.items.y - fixed.basket.y) > 1.0f)
			{
				std::printf("  FAIL: panes are not all on the same row\n");
				++failures;
			}
		}
	}

	ImGui::DestroyContext();
	std::printf("%s (%d failing arrangement(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
