set_xmakever("3.0.0")

set_project("SFShopExplorer")
set_arch("x64")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- force MSVC toolchain
set_toolchains("msvc")

-- include commonlibsf so its rules and the commonlibsf target are visible
includes("../Depends/commonlibsf-main/xmake.lua")

add_requires("zlib", "lz4")

target("SFShopExplorer", function()
    set_kind("shared")
    set_default(true)
    set_symbols("hidden")

    add_rules("commonlibsf.plugin", {
        options = {
            address_library = true,
            sig_scanning = false,
            layout_dependent = true,
            no_struct_use = false,
        },
    })

    add_deps("commonlibsf", { public = true })
    add_includedirs("src", "Depends/imgui", "../Depends/commonlibsf-main/lib/commonlib-shared/include")

    add_files("src/ShopExplorer.cpp", "src/ShopMenu.cpp", "src/ImguiOverlay.cpp", "src/ItemCapture.cpp", "src/OfflineModel.cpp", "src/LiveModel.cpp", "src/main.cpp")
    add_files("Depends/imgui/imgui.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_draw.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_tables.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_widgets.cpp", { pcxheader = false })
    add_files("Depends/imgui/backends/imgui_impl_dx12.cpp", { pcxheader = false })
    add_files("Depends/imgui/backends/imgui_impl_win32.cpp", { pcxheader = false })
    add_headerfiles("src/**.h")
    set_pcxxheader("src/PCH.h")

    add_links("d3d12", "dxgi", "d3dcompiler", "user32")
    add_packages("zlib", "lz4")
    add_defines("IMGUI_DISABLE_OBSOLETE_FUNCTIONS", "IMGUI_DISABLE_DEMO_WINDOWS")

    set_configvar("COMMONLIB_PLUGIN_NAME", "SFShopExplorer")
    set_configvar("COMMONLIB_PLUGIN_AUTHOR", "lelsliem")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MAJOR", "1")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MINOR", "0")
    set_configvar("COMMONLIB_PLUGIN_VERSION_PATCH", "0")
end)