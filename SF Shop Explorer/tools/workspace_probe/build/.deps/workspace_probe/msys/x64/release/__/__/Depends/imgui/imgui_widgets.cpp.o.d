{
    files = {
        [[..\..\Depends\imgui\imgui_widgets.cpp]]
    },
    depfiles_format = "cl_json",
    values = {
        [[D:\Visual Studios 2022\Install\VC\Tools\MSVC\14.44.35207\bin\HostX64\x64\cl.exe]],
        {
            "-nologo",
            "-O2",
            "-std:c++20",
            [[-I..\..\Depends\imgui]],
            "-DNDEBUG"
        }
    },
    depfiles = "{\
    \"Version\": \"1.2\",\
    \"Data\": {\
        \"Source\": \"d:\\\\projects\\\\address library\\\\sf-shop\\\\sf shop explorer\\\\depends\\\\imgui\\\\imgui_widgets.cpp\",\
        \"ProvidedModule\": \"\",\
        \"Includes\": [\
            \"d:\\\\projects\\\\address library\\\\sf-shop\\\\sf shop explorer\\\\depends\\\\imgui\\\\imgui.h\",\
            \"d:\\\\projects\\\\address library\\\\sf-shop\\\\sf shop explorer\\\\depends\\\\imgui\\\\imconfig.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\float.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\corecrt.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\vcruntime.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\sal.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\concurrencysal.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\vadefs.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\stdarg.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\stddef.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\string.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\corecrt_memory.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\corecrt_memcpy_s.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\errno.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\vcruntime_string.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\corecrt_wstring.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\assert.h\",\
            \"d:\\\\projects\\\\address library\\\\sf-shop\\\\sf shop explorer\\\\depends\\\\imgui\\\\imgui_internal.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\stdio.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\corecrt_wstdio.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\corecrt_stdio_config.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\stdlib.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\corecrt_malloc.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\corecrt_search.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\corecrt_wstdlib.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\limits.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\math.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\corecrt_math.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\immintrin.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\wmmintrin.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\nmmintrin.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\smmintrin.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\tmmintrin.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\pmmintrin.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\emmintrin.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\xmmintrin.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\mmintrin.h\",\
            \"c:\\\\program files (x86)\\\\windows kits\\\\10\\\\include\\\\10.0.26100.0\\\\ucrt\\\\malloc.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\zmmintrin.h\",\
            \"d:\\\\visual studios 2022\\\\install\\\\vc\\\\tools\\\\msvc\\\\14.44.35207\\\\include\\\\stdint.h\",\
            \"d:\\\\projects\\\\address library\\\\sf-shop\\\\sf shop explorer\\\\depends\\\\imgui\\\\imstb_textedit.h\",\
            \"d:\\\\projects\\\\address library\\\\sf-shop\\\\sf shop explorer\\\\depends\\\\imgui\\\\imstb_textedit.h\"\
        ],\
        \"ImportedModules\": [],\
        \"ImportedHeaderUnits\": []\
    }\
}"
}