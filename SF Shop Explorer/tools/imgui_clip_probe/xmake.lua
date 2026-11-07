-- Headless harness for the catalog table's row geometry: the vendored ImGui,
-- no renderer, no game. See star_probe.cpp.
add_rules("mode.debug", "mode.release")

target("star_probe")
    set_kind("binary")
    set_languages("c++20")
    add_includedirs("../../Depends/imgui")
    add_files("star_probe.cpp")
    add_files("../../Depends/imgui/imgui.cpp")
    add_files("../../Depends/imgui/imgui_draw.cpp")
    add_files("../../Depends/imgui/imgui_tables.cpp")
    add_files("../../Depends/imgui/imgui_widgets.cpp")
    set_policy("build.c++.modules", false)
