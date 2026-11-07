-- Headless harness for the backdrop art encode path: WIC only, no game, no
-- renderer. See backdrop_art_probe.cpp.
add_rules("mode.debug", "mode.release")

target("backdrop_art_probe")
    set_kind("binary")
    set_languages("c++20")
    add_files("backdrop_art_probe.cpp")
    add_syslinks("windowscodecs", "ole32", "uuid")
    set_policy("build.c++.modules", false)
