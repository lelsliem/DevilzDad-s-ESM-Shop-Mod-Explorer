set_xmakever("3.0.0")

set_project("SFShopExplorer")
set_version("1.0.0")

-- The CI setup action exports XMAKE_ACTION_BUILD_CACHE. xmake otherwise
-- disables build.ccache for MSVC, so opt in only for that CI path; a local
-- build keeps the faster default because cl.exe preprocessing dominates a
-- cached compile.
if os.getenv("XMAKE_ACTION_BUILD_CACHE") then
    set_policy("build.ccache", true)
end
set_arch("x64")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- ---------------------------------------------------------------------------
-- Archive-format regression gate: see build.cmd.
--
-- It deliberately does NOT live in a before_build hook here. This xmake
-- build (v3.0.9+HEAD) sandboxes the build script: os.iorunv raises even for
-- `cmd /c echo`, and neither task.runv nor pcall is exposed, so a gate
-- implemented in Lua cannot spawn the harness at all. build.cmd runs the
-- same checks as a real pre-build step and propagates the exit status.
-- ---------------------------------------------------------------------------


-- force MSVC toolchain
set_toolchains("msvc")

-- include commonlibsf so its rules and the commonlibsf target are visible
includes("../Depends/commonlibsf-main/xmake.lua")

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
    add_includedirs("src", "Depends/imgui", "../Depends/commonlibsf-main/lib/commonlib-shared/include")

    add_files("src/ShopExplorer.cpp", "src/ImguiOverlay.cpp", "src/ItemCapture.cpp", "src/ShopAudio.cpp", "src/ShopPause.cpp", "src/BuildGate.cpp", "src/OfflineModel.cpp", "src/LiveModel.cpp", "src/NativeModel.cpp", "src/main.cpp")
    add_files("Depends/imgui/imgui.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_draw.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_tables.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_widgets.cpp", { pcxheader = false })
    add_files("Depends/imgui/backends/imgui_impl_dx12.cpp", { pcxheader = false })
    add_files("Depends/imgui/backends/imgui_impl_win32.cpp", { pcxheader = false })
    add_headerfiles("src/**.h")
    set_pcxxheader("src/PCH.h")

    -- xaudio2.lib is the SDK import library for XAudio2_9.dll (verified: the
    -- .lib records XAudio2_9.dll as its only DLL dependency); there is no
    -- xaudio2_9.lib. A hard import is acceptable here: XAudio2_9.dll ships in
    -- System32 from Windows 8 onward, Starfield requires Windows 10, and the
    -- plugin already hard-imports d3d12/dxgi/dwmapi. Delay loading was tried
    -- and dropped: xmake never emitted /DELAYLOAD (the commonlibsf.plugin rule
    -- resets add_ldflags), and MSVC rejects it in a linker pragma (LNK4229 with
    -- the leading slash, LNK1276 without).
    -- WASAPI: XAudio2 cannot create a voice on this machine at all (every
-- CreateSourceVoice returns XAUDIO2_E_INVALID_CALL, verified standalone), so the
-- audio backend is IAudioClient in shared mode instead. uuid supplies the MMDEV
-- and IAudioClient CLSIDs/IIDs, which are __uuidof references rather than a lib.
-- Media Foundation decodes the player's own tracks (mp3/flac/wma/m4a) so no
-- codec ships with the mod; shlwapi supplies SHCreateStreamOnFileEx, which is
-- how the source reader is handed a path with spaces or non-ASCII in it.
add_links("d3d12", "dxgi", "d3dcompiler", "user32", "ole32", "uuid", "mfplat", "mfreadwrite", "mfuuid", "shlwapi")
    add_packages("zlib", "lz4")
    add_defines("IMGUI_DISABLE_OBSOLETE_FUNCTIONS", "IMGUI_DISABLE_DEMO_WINDOWS")

    set_configvar("COMMONLIB_PLUGIN_NAME", "SFShopExplorer")
    set_configvar("COMMONLIB_PLUGIN_AUTHOR", "lelsliem")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MAJOR", "1")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MINOR", "0")
    set_configvar("COMMONLIB_PLUGIN_VERSION_PATCH", "0")
end)
-- ---------------------------------------------------------------------------
-- OSF front end.
--
-- Same engine, same sources, one extra translation unit: the shop is presented
-- by an OSF UI view (WebView2) instead of the DXGI Present overlay. The overlay
-- itself is still compiled in (the next pass renders previews through it), but
-- main.cpp does not install it under SFE_OSF_FRONTEND, so this build never
-- hooks Present and never opens its own window.
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
    add_includedirs("src", "Depends/imgui", "../Depends/commonlibsf-main/lib/commonlib-shared/include")

    add_files("src/ShopExplorer.cpp", "src/ImguiOverlay.cpp", "src/ItemCapture.cpp", "src/ShopAudio.cpp", "src/ShopPause.cpp", "src/BuildGate.cpp", "src/OfflineModel.cpp", "src/LiveModel.cpp", "src/NativeModel.cpp", "src/OsfFrontend.cpp", "src/PreviewHost.cpp", "src/BackdropArt.cpp", "src/main.cpp")
    add_files("Depends/imgui/imgui.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_draw.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_tables.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_widgets.cpp", { pcxheader = false })
    add_files("Depends/imgui/backends/imgui_impl_dx12.cpp", { pcxheader = false })
    add_files("Depends/imgui/backends/imgui_impl_win32.cpp", { pcxheader = false })
    add_headerfiles("src/**.h")
    set_pcxxheader("src/PCH.h")

    -- No windowscodecs: PreviewHost writes the PNG itself with zlib, because
    -- WIC's encoder negotiates its own pixel format and a refusal there was a
    -- silent "could not be encoded" with no way to see why from in game.
    add_links("d3d12", "dxgi", "d3dcompiler", "user32", "ole32", "uuid", "mfplat", "mfreadwrite", "mfuuid", "shlwapi")
    add_packages("zlib", "lz4")
    add_defines("IMGUI_DISABLE_OBSOLETE_FUNCTIONS", "IMGUI_DISABLE_DEMO_WINDOWS", "SFE_OSF_FRONTEND")

    set_configvar("COMMONLIB_PLUGIN_NAME", "SFShopExplorer")
    set_configvar("COMMONLIB_PLUGIN_AUTHOR", "lelsliem")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MAJOR", "1")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MINOR", "0")
    set_configvar("COMMONLIB_PLUGIN_VERSION_PATCH", "0")
end)
