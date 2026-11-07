-- Headless harness for the workspace table's arrangements: the vendored ImGui,
-- no renderer, no game. See workspace_probe.cpp.
add_rules("mode.debug", "mode.release")
set_toolchains("msvc")

target("workspace_probe")
    set_kind("binary")
    set_languages("c++20")
    add_includedirs("../../Depends/imgui")
    add_files("workspace_probe.cpp")
    add_files("../../Depends/imgui/imgui.cpp")
    add_files("../../Depends/imgui/imgui_draw.cpp")
    add_files("../../Depends/imgui/imgui_tables.cpp")
    add_files("../../Depends/imgui/imgui_widgets.cpp")
    set_policy("build.c++.modules", false)
