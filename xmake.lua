set_xmakever("3.0.0")

set_project("SFShopExplorer")
set_version("0.9.1")
set_arch("x64")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- CommonLibSF (https://github.com/libxse/commonlibsf) has to sit in
-- Depends/commonlibsf; Dear ImGui in Depends/imgui.
includes("Depends/commonlibsf/xmake.lua")

-- force MSVC toolchain
set_toolchains("msvc")

add_requires("zlib", "lz4")

target("SFShopExplorer", function()
    set_kind("shared")
    set_default(true)
    set_symbols("debug")

    add_rules("commonlibsf.plugin", {
        options = {
            address_library = true,
            sig_scanning = true,
            layout_dependent = true,
            no_struct_use = false,
        },
    })

    add_deps("commonlibsf", { public = true })
    add_includedirs("src", "Depends/imgui", "Depends/commonlibsf/lib/commonlib-shared/include")

    add_files("src/ShopExplorer.cpp", "src/ImguiOverlay.cpp", "src/ItemCapture.cpp", "src/ShopAudio.cpp", "src/ShopPause.cpp", "src/BuildGate.cpp", "src/OfflineModel.cpp", "src/LiveModel.cpp", "src/NativeModel.cpp", "src/main.cpp")
    add_files("Depends/imgui/imgui.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_draw.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_tables.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_widgets.cpp", { pcxheader = false })
    add_files("Depends/imgui/backends/imgui_impl_dx12.cpp", { pcxheader = false })
    add_files("Depends/imgui/backends/imgui_impl_win32.cpp", { pcxheader = false })
    add_headerfiles("src/**.h")
    set_pcxxheader("src/PCH.h")

    -- xaudio2.lib is the SDK import library for XAudio2_9.dll; WASAPI is the
    -- backend the sources actually use. Media Foundation decodes the player's
    -- own music files; shlwapi supplies SHCreateStreamOnFileEx.
    add_links("d3d12", "dxgi", "d3dcompiler", "user32", "ole32", "uuid", "mfplat", "mfreadwrite", "mfuuid", "shlwapi")
    add_packages("zlib", "lz4")
    add_defines("IMGUI_DISABLE_OBSOLETE_FUNCTIONS", "IMGUI_DISABLE_DEMO_WINDOWS")

    set_configvar("COMMONLIB_PLUGIN_NAME", "SFShopExplorer")
    set_configvar("COMMONLIB_PLUGIN_AUTHOR", "lelsliem")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MAJOR", "0")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MINOR", "9")
    set_configvar("COMMONLIB_PLUGIN_VERSION_PATCH", "1")
end)

-- ---------------------------------------------------------------------------
-- OSF front end.
--
-- Same engine, same sources, one extra translation unit: the shop is presented
-- by an OSF UI view (WebView2) instead of the DXGI Present overlay. The overlay
-- itself is still compiled in, but main.cpp does not install it under
-- SFE_OSF_FRONTEND, so this build never hooks Present and never opens its own
-- window.
-- ---------------------------------------------------------------------------
target("DevilzShopOSF", function()
    set_kind("shared")
    set_default(false)
    set_symbols("debug")

    add_rules("commonlibsf.plugin", {
        options = {
            address_library = true,
            sig_scanning = true,
            layout_dependent = true,
            no_struct_use = false,
        },
    })

    add_deps("commonlibsf", { public = true })
    add_includedirs("src", "Depends/imgui", "Depends/commonlibsf/lib/commonlib-shared/include")

    add_files("src/ShopExplorer.cpp", "src/ImguiOverlay.cpp", "src/ItemCapture.cpp", "src/ShopAudio.cpp", "src/ShopPause.cpp", "src/BuildGate.cpp", "src/OfflineModel.cpp", "src/LiveModel.cpp", "src/NativeModel.cpp", "src/OsfFrontend.cpp", "src/PreviewHost.cpp", "src/BackdropArt.cpp", "src/main.cpp")
    add_files("Depends/imgui/imgui.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_draw.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_tables.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_widgets.cpp", { pcxheader = false })
    add_files("Depends/imgui/backends/imgui_impl_dx12.cpp", { pcxheader = false })
    add_files("Depends/imgui/backends/imgui_impl_win32.cpp", { pcxheader = false })
    add_headerfiles("src/**.h")
    set_pcxxheader("src/PCH.h")

    add_links("d3d12", "dxgi", "d3dcompiler", "user32", "ole32", "uuid", "mfplat", "mfreadwrite", "mfuuid", "shlwapi")
    add_packages("zlib", "lz4")
    add_defines("IMGUI_DISABLE_OBSOLETE_FUNCTIONS", "IMGUI_DISABLE_DEMO_WINDOWS", "SFE_OSF_FRONTEND")

    set_configvar("COMMONLIB_PLUGIN_NAME", "SFShopExplorer")
    set_configvar("COMMONLIB_PLUGIN_AUTHOR", "lelsliem")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MAJOR", "0")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MINOR", "9")
    set_configvar("COMMONLIB_PLUGIN_VERSION_PATCH", "1")
end)
