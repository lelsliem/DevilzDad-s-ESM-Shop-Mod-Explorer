set_xmakever("3.0.0")
set_project("ParcelFix")
set_arch("x64")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")
set_toolchains("msvc")
add_rules("mode.releasedbg")
includes("../Depends/commonlibsf-main/xmake.lua")

-- Same shortcut the MCM project uses: link the CommonLibSF archive the shop
-- already built instead of compiling a second copy. The source submodule stays
-- authoritative; delete build/ in the shop project to force a rebuild of both.
add_linkdirs("../SF Shop Explorer/build/windows/x64/releasedbg")

target("ParcelFix", function()
    set_kind("shared")
    set_default(true)
    -- A version resource, so SFSE's own "<plugin> vX.Y.Z" banner agrees with
    -- the line this plugin writes instead of reading 0.0.0.0.
    set_version("1.0.0")
    set_symbols("hidden")
    -- No signature scanning and no struct layout use: this plugin reads the
    -- player's cell and the references in it through CommonLibSF's own
    -- declarations, so it needs the Address Library and nothing else. That is
    -- deliberate - a cleanup tool people run once should not be the thing that
    -- crashes on a game update.
    add_rules("commonlibsf.plugin", {
        options = {
            address_library = true,
            sig_scanning = false,
            layout_dependent = true,
            no_struct_use = false,
        },
    })
    add_deps("commonlibsf", { public = true })
    add_includedirs("src")
    add_files("src/main.cpp")
    set_pcxxheader("src/PCH.h")
    set_configvar("COMMONLIB_PLUGIN_NAME", "ParcelFix")
    set_configvar("COMMONLIB_PLUGIN_AUTHOR", "lelsliem")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MAJOR", "1")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MINOR", "0")
    set_configvar("COMMONLIB_PLUGIN_VERSION_PATCH", "0")
end)
