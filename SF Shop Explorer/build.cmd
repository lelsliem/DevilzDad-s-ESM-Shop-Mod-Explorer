@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ---------------------------------------------------------------------------
rem Archive-format regression gate.
rem
rem Two shipped bugs (notes 6r-5j, 6r-5k) were wrong assumptions about BA2
rem and CDB layout that only surfaced in game, costing a test cycle each. The
rem harness in tools\ba2_audit replicates the reader in src\LiveModel.cpp and
rem probes the real archives, so the assumption is checked before anything is
rem compiled instead of after someone launches Starfield.
rem
rem It lives here rather than in an xmake before_build hook because this xmake
rem build sandboxes the build script: os.iorunv raises even for `cmd /c echo`,
rem and neither task.runv nor pcall is exposed, so a Lua gate cannot spawn the
rem harness at all.
rem
rem Opt out with:     set SFSE_SKIP_BA2AUDIT=1
rem Point elsewhere:  set SFSE_STARFIELD_DATA=D:\path\to\Data
rem ---------------------------------------------------------------------------

set "SFSE_STARFIELD_DATA=B:\SteamLibrary\steamapps\common\Starfield\Data"
if defined SFSE_STARFIELD_DATA_OVERRIDE set "SFSE_STARFIELD_DATA=%SFSE_STARFIELD_DATA_OVERRIDE%"
set "AUDIT_PROBE=tools\ba2_audit\ba2_audit.py"
set "AUDIT_MATERIALS=%SFSE_STARFIELD_DATA%\Starfield - Materials.ba2"
set "AUDIT_TEXTURES=%SFSE_STARFIELD_DATA%\Starfield - Textures11.ba2"

rem ---------------------------------------------------------------------------
rem Outfit-assembly gate.
rem
rem The C++ fallback in LiveModel::BuildModels / AssembleSiblings can only be
rem checked against the assets that are actually installed. The Python audit
rem mirrors that rule and exits 1 on a record missing a part or a part NIF that
rem cannot produce a LOD0 mesh. It writes its report to %TEMP% so a routine
rem build does not modify the tracked reports directory.
rem
rem Opt out with:     set SFSE_SKIP_OUTFIT_AUDIT=1
rem Catalog override: set SFSE_CATALOG_OVERRIDE=D:\path\to\SFDevilzDump-catalog.csv
rem Mods override:    set SFSE_MODS_ROOT_OVERRIDE=D:\path\to\MO2\mods
rem ---------------------------------------------------------------------------

set "OUTFIT_AUDIT=tools\renderer_recon\outfit_assembly_audit.py"
set "OUTFIT_AUDIT_REPORT=%TEMP%\sfse_outfit_assembly_audit.md"
set "SFSE_CATALOG=%USERPROFILE%\Documents\My Games\Starfield\SFSE\Logs\SFDevilzDump-catalog.csv"
if defined SFSE_CATALOG_OVERRIDE set "SFSE_CATALOG=%SFSE_CATALOG_OVERRIDE%"
if not exist "!SFSE_CATALOG!" if exist "..\SFDevilzDump-all-forms.csv" set "SFSE_CATALOG=..\SFDevilzDump-all-forms.csv"
set "SFSE_MODS_ROOT=B:\MO2\Starfield\mods"
if defined SFSE_MODS_ROOT_OVERRIDE set "SFSE_MODS_ROOT=%SFSE_MODS_ROOT_OVERRIDE%"

rem Every variable is set above the blocks on purpose: inside a parenthesised
rem block cmd expands %VAR% when the block is parsed, so a `set` performed in
rem the same block reads back empty.
if defined SFSE_SKIP_BA2AUDIT goto :audit_skip
if not exist "!AUDIT_PROBE!" goto :audit_noprobe
if not exist "!AUDIT_MATERIALS!" goto :audit_nodata

echo [ba2audit] probing archive formats in %SFSE_STARFIELD_DATA%
rem One archive of each generation and kind. A wrong entry stride, field
rem offset or stream length shows up here as a decode failure.
python "!AUDIT_PROBE!" "!AUDIT_MATERIALS!" --sample 1 --quiet
if errorlevel 1 goto :audit_failed
python "!AUDIT_PROBE!" "!AUDIT_TEXTURES!" --sample 3 --quiet
if errorlevel 1 goto :audit_failed
echo [ba2audit] OK
goto :outfit_audit

:audit_skip
echo [ba2audit] SKIPPED ^(SFSE_SKIP_BA2AUDIT set^)
goto :outfit_audit

:audit_noprobe
echo [ba2audit] SKIPPED ^(harness not found: !AUDIT_PROBE!^)
goto :outfit_audit

:audit_nodata
rem Never block a machine without Starfield installed: this is a developer
rem gate, not a packaging dependency.
echo [ba2audit] SKIPPED ^(%SFSE_STARFIELD_DATA% not present^)
goto :outfit_audit

:audit_failed
echo.
echo [ba2audit] FAILED. The reader in src\LiveModel.cpp disagrees with the
echo            shipped archives, or the archives changed. Change src\LiveModel.cpp
echo            and tools\ba2_audit\ba2lib.py together, then rebuild.
echo            Override: set SFSE_SKIP_BA2AUDIT=1 to build without the gate.
endlocal
exit /b 1

:outfit_audit
if defined SFSE_SKIP_OUTFIT_AUDIT goto :outfit_skip
if not exist "!OUTFIT_AUDIT!" goto :outfit_noprobe
if not exist "%SFSE_STARFIELD_DATA%" goto :outfit_nodata
if not exist "!SFSE_CATALOG!" goto :outfit_nocatalog

echo [outfitaudit] checking outfit assembly in %SFSE_STARFIELD_DATA%
python "!OUTFIT_AUDIT!" --catalog "!SFSE_CATALOG!" --data "%SFSE_STARFIELD_DATA%" --mods-root "%SFSE_MODS_ROOT%" --out "!OUTFIT_AUDIT_REPORT!"
if errorlevel 1 goto :outfit_failed
echo [outfitaudit] OK ^(report: !OUTFIT_AUDIT_REPORT!^)
goto :do_build

:outfit_skip
echo [outfitaudit] SKIPPED ^(SFSE_SKIP_OUTFIT_AUDIT set^)
goto :do_build

:outfit_noprobe
echo [outfitaudit] SKIPPED ^(audit not found: !OUTFIT_AUDIT!^)
goto :do_build

:outfit_nodata
echo [outfitaudit] SKIPPED ^(%SFSE_STARFIELD_DATA% not present^)
goto :do_build

:outfit_nocatalog
rem The catalog is written by the in-game dump. Do not block a checkout that
rem has never run the game; SFSE_CATALOG_OVERRIDE points at another dump.
echo [outfitaudit] SKIPPED ^(catalog dump not found: !SFSE_CATALOG!^)
goto :do_build

:outfit_failed
echo.
echo [outfitaudit] FAILED. An installed outfit is missing a part, a part NIF
echo              cannot produce a LOD0 mesh, or no complete outfit groups were
echo              found. The C++ fallback and the installed assets disagree;
echo              inspect the report and tools\renderer_recon\
echo              outfit_assembly_audit.py before changing either.
echo              Report: !OUTFIT_AUDIT_REPORT!
echo              Override: set SFSE_SKIP_OUTFIT_AUDIT=1 to build without the gate.
endlocal
exit /b 1

:do_build
rem ---------------------------------------------------------------------------
rem Build, then report the artefact that was actually produced.
rem
rem The existence check and the SHA-256 used to run BEFORE xmake. Two
rem consequences: a fresh checkout, or any tree after `xmake clean -a`, failed
rem with "no build output" and never compiled at all, and every successful
rem build printed the hash of the PREVIOUS DLL under the label "built DLL
rem sha256" - the same stale-binary trap documented in README.md.
rem
rem xmake writes to build\windows\x64\<config> and this build runs releasedbg,
rem so that is the directory that must be deployed. The sibling "release"
rem directory is left over from an earlier config and ships a stale binary that
rem behaves in game exactly like a no-op change.
rem ---------------------------------------------------------------------------
set "XMAKE=xmake"
if exist "C:\Program Files\xmake\xmake.exe" set "XMAKE=C:\Program Files\xmake\xmake.exe"
set "OUTDIR=%CD%\build\windows\x64\releasedbg"
rem Reconfigure when there is no recorded config OR no build tree: `xmake clean
rem -a` removes the tree, and a later build then silently falls back to the
rem mingw toolchain, where zlib fails to compile against the MSVC environment.
set "NEEDCONFIG="
if not exist ".xmake\windows\x64\xmake.conf" set "NEEDCONFIG=1"
if not exist "!OUTDIR!" set "NEEDCONFIG=1"
if defined NEEDCONFIG (
    "%XMAKE%" f -p windows --toolchain=msvc -m releasedbg -y
    if errorlevel 1 (
        endlocal
        exit /b 1
    )
)
rem xmake returns 0 even when a translation unit fails to compile. Measured:
rem with a hard `#error` at the top of src\ShopExplorer.cpp, `xmake build -y`
rem exited 0 and the checks below then printed the SHA-256 of the PREVIOUS DLL
rem under "built DLL sha256" - a failed compile reported as a successful
rem deploy, i.e. exactly the stale-binary trap this script exists to prevent.
rem So the build is gated on its own log, not on xmake's exit code alone.
set "BUILDLOG=%TEMP%\sfse_xmake_build.log"
rem Both redirect targets are the same file, so the log is truncated per run.
"%XMAKE%" build -y > "!BUILDLOG!" 2>&1
set "XMAKERC=!errorlevel!"
type "!BUILDLOG!"
if not "!XMAKERC!"=="0" (
    echo [deploy] FAILED: xmake exited !XMAKERC! ^(log: !BUILDLOG!^)
    endlocal
    exit /b 1
)
findstr /C:"error C" /C:"error LNK" /C:"fatal error" "!BUILDLOG!" >nul
if not errorlevel 1 (
    echo [deploy] FAILED: compiler diagnostics in the build log; the artefact
    echo            below was NOT rebuilt. Full log: !BUILDLOG!
    endlocal
    exit /b 1
)
if not exist "!OUTDIR!\SFShopExplorer.dll" (
    echo [deploy] FAILED: no build output in "!OUTDIR!"
    endlocal
    exit /b 1
)
for /f "usebackq delims=" %%H in (`powershell -NoProfile -Command "Get-FileHash -LiteralPath '!OUTDIR!\SFShopExplorer.dll' -Algorithm SHA256 | Select-Object -ExpandProperty Hash"`) do set "OUTHASH=%%H"
echo [deploy] built DLL sha256=!OUTHASH!
set "STALEDLL=%CD%\build\windows\x64\release\SFShopExplorer.dll"
if exist "!STALEDLL!" (
    for /f "usebackq delims=" %%S in (`powershell -NoProfile -Command "Get-FileHash -LiteralPath '!STALEDLL!' -Algorithm SHA256 | Select-Object -ExpandProperty Hash"`) do set "STALEHASH=%%S"
    if not "!STALEHASH!"=="!OUTHASH!" (
        echo [deploy] WARNING: stale DLL in the sibling "release" directory ^(sha256=!STALEHASH!^).
        echo            Deploy ONLY from "!OUTDIR!". Do not copy from "release".
    )
)
endlocal
exit /b 0
