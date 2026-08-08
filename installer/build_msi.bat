@REM build_msi.bat - Build MGT_Unicorn_Suite_<version>.msi with WiX.
@REM
@REM Part of the Montreal Greek Times Unicorn Suite.
@REM Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
@REM Licensed under the GNU Affero General Public License v3.0 or later.
@REM
@REM PREREQUISITES, pinned. Install once:
@REM     dotnet tool install --global wix --version 5.0.2
@REM     wix extension add --global WixToolset.UI.wixext/5.0.2
@REM     wix extension add --global WixToolset.Util.wixext/5.0.2
@REM Needs a .NET SDK 6.0 or later; 8.0.423 is what this was built with.
@REM
@REM Run build_x64.bat FIRST. This script packages the exe that is there;
@REM it does not compile it.
@REM
@REM THE VERSION IS NOT TYPED ANYWHERE IN THIS FILE OR IN THE .wxs. It is
@REM read back out of src\suite_version.h through the same C preprocessor
@REM that compiles the app, so the MSI ProductVersion, the app's About
@REM box and the resource VERSIONINFO cannot disagree.

@echo off
setlocal EnableDelayedExpansion

set HERE=%~dp0
set ROOT=%HERE%..
set CC=C:\msys64\ucrt64\bin\gcc.exe
set WIX=wix
set PAYLOAD=%HERE%payload
set OUTDIR=%HERE%out

if not exist "%ROOT%\MGT_Unicorn_Suite_x64.exe" (
    echo ERROR: MGT_Unicorn_Suite_x64.exe not found. Run build_x64.bat first.
    exit /b 1
)

@REM ------------------------------------------------------------------
@REM Version, straight out of the header.
@REM
@REM Asking the preprocessor rather than parsing the header by hand means
@REM there is no second parser to get out of step. The echo below expands
@REM to a line like:  MGTVER 0 4 0 "-beta"
@REM ------------------------------------------------------------------
set ARTDIR=%HERE%art
set ICONDIR=%ROOT%\assets

set VERFILE=%TEMP%\mgt_ver_%RANDOM%.txt
echo MGTVER SUITE_VERSION_MAJOR SUITE_VERSION_MINOR SUITE_VERSION_PATCH SUITE_VERSION_SUFFIX ^
    | %CC% -E -P -include "%ROOT%\src\suite_version.h" -x c - > "%VERFILE%" 2>nul
if errorlevel 1 (
    echo ERROR: could not preprocess src\suite_version.h
    del "%VERFILE%" 2>nul
    exit /b 1
)

set VMAJ=
for /f "tokens=1-5" %%a in ('findstr /b MGTVER "%VERFILE%"') do (
    set VMAJ=%%b
    set VMIN=%%c
    set VPAT=%%d
    set VSUF=%%e
)
del "%VERFILE%" 2>nul

if "%VMAJ%"=="" (
    echo ERROR: could not read the version out of src\suite_version.h
    exit /b 1
)
@REM VSUF arrives quoted, as the string literal "-beta". Strip the quotes.
set VSUF=%VSUF:"=%

set MSIVERSION=%VMAJ%.%VMIN%.%VPAT%
set FULLVERSION=%MSIVERSION%%VSUF%

@REM The two display names, from the same header, for the same reason.
@REM Each expands to one quoted string literal, so tokens=1,* splits the
@REM marker off and leaves the whole quoted name in %%b.
set NAMEFILE=%TEMP%\mgt_name_%RANDOM%.txt
echo MGTLONG SUITE_APP_NAME_LONG ^
    | %CC% -E -P -include "%ROOT%\src\suite_version.h" -x c - > "%NAMEFILE%" 2>nul
echo MGTSHORT SUITE_APP_NAME_SHORT ^
    | %CC% -E -P -include "%ROOT%\src\suite_version.h" -x c - >> "%NAMEFILE%" 2>nul
for /f "tokens=1,* delims= " %%a in ('findstr /b MGTLONG "%NAMEFILE%"')  do set LONGNAME=%%b
for /f "tokens=1,* delims= " %%a in ('findstr /b MGTSHORT "%NAMEFILE%"') do set SHORTNAME=%%b
del "%NAMEFILE%" 2>nul
set LONGNAME=%LONGNAME:"=%
set SHORTNAME=%SHORTNAME:"=%
if "%LONGNAME%"=="" (
    echo ERROR: could not read the display names out of src\suite_version.h
    exit /b 1
)

echo Identity from src\suite_version.h:
echo    MSI ProductVersion : %MSIVERSION%
echo    canonical string   : %FULLVERSION%
echo    long name  (wizard): %LONGNAME%
echo    short name (ARP,   : %SHORTNAME%
echo                shortcuts^)
echo.

@REM ------------------------------------------------------------------
@REM Stage the payload.
@REM
@REM Everything here is DERIVED from a canonical file in the repository,
@REM never hand-maintained, so the installed documentation cannot drift
@REM from the repository's. payload\ is generated and gitignored.
@REM ------------------------------------------------------------------
echo Staging payload...
if exist "%PAYLOAD%" rmdir /s /q "%PAYLOAD%"
mkdir "%PAYLOAD%"
mkdir "%PAYLOAD%\LICENSES"

copy /y "%ROOT%\MGT_Unicorn_Suite_x64.exe" "%PAYLOAD%\" >nul || exit /b 1
copy /y "%ROOT%\WebView2Loader.dll"        "%PAYLOAD%\" >nul || exit /b 1
copy /y "%ROOT%\README.TXT"                "%PAYLOAD%\README.TXT"  >nul || exit /b 1
copy /y "%ROOT%\COPYING"                   "%PAYLOAD%\LICENSE.TXT" >nul || exit /b 1
copy /y "%ROOT%\THIRD-PARTY-NOTICES.md"    "%PAYLOAD%\NOTICES.TXT" >nul || exit /b 1
copy /y "%ROOT%\licenses\*.txt"            "%PAYLOAD%\LICENSES\"   >nul || exit /b 1

@REM ------------------------------------------------------------------
@REM The NABU emulator package.
@REM
@REM This is the NABU-only MAME 0.250 build that was proven against the
@REM live channel server, plus the preserved NABU boot ROMs it needs. It
@REM is a SEPARATE PROGRAM, spawned as its own process by the NABU tab
@REM and never linked into the Suite, which is what keeps its
@REM GPL-2.0-or-later terms an aggregation question rather than a
@REM licensing one. See THIRD-PARTY-NOTICES.md for the notice and the
@REM written offer for the corresponding source.
@REM
@REM SOURCE OF TRUTH is NABU_SRC below, the operator's tested package.
@REM It is NOT in the repository: it is 107 MB, most of it one binary.
@REM Getting it into the CI build is deferred to the go-live mission,
@REM which is where fetch-versus-commit gets decided.
@REM ------------------------------------------------------------------
if not defined NABU_SRC set "NABU_SRC=C:\NABU\nabu-mame\nabu-mame"
if not exist "%NABU_SRC%\mame.exe" (
    echo ERROR: the NABU emulator package was not found at %NABU_SRC%
    echo        Set NABU_SRC to the folder containing mame.exe, or install
    echo        the tested package there.
    exit /b 1
)
echo Staging the NABU emulator package from %NABU_SRC% ...
mkdir "%PAYLOAD%\NABU"
@REM /E subdirectories including empty, /I treat target as a directory,
@REM /Y overwrite without prompting, /Q quiet. The disks folder is
@REM excluded: the tab boots from the network channel, never a floppy
@REM image, and those two images are the only files in the package the
@REM Suite has no path to.
xcopy "%NABU_SRC%\*" "%PAYLOAD%\NABU\" /E /I /Y /Q /EXCLUDE:%HERE%nabu_exclude.txt >nul
if errorlevel 1 (
    echo ERROR: could not stage the NABU emulator package
    exit /b 1
)
if not exist "%PAYLOAD%\NABU\roms\nabupc" (
    echo ERROR: the NABU boot ROMs are missing from the staged package
    exit /b 1
)

@REM ------------------------------------------------------------------
@REM The NABU Native tab's boot firmware.
@REM
@REM These two images go in the ROOT of NABU\, not under roms\, because
@REM that is where src\nabu_native_module.c looks for them. Its candidate
@REM list is relative to the folder holding the exe and reads, in order:
@REM
@REM     NABU\mgtipl.bin                    <- installed, preferred
@REM     firmware\mgtipl\mgtipl.bin         <- source tree
@REM     NABU\opennabu.bin                  <- installed, fallback
@REM     third_party\opennabu\opennabu.bin  <- source tree
@REM
@REM Up to and including 0.4.0 the installer staged NEITHER, and only the
@REM two source-tree paths existed, so a built exe run from the working
@REM copy found firmware and an INSTALLED one found none at all. The
@REM native tab could not boot after an install. Fixed for 0.5.0; the
@REM checks below are what keep it fixed, because both files are produced
@REM elsewhere (mgtipl.bin is assembled on the Unicorn server) and a
@REM missing one would otherwise be silent until someone pressed Connect.
@REM
@REM No wxs change is needed: the NabuFiles group harvests
@REM $(PayloadDir)\NABU\** wholesale.
@REM
@REM Both are MIT, by S. V. Nickolas. See THIRD-PARTY-NOTICES.md and
@REM LICENSES\LICENSE.opennabu.txt, which ships beside them.
@REM ------------------------------------------------------------------
echo Staging the NABU Native boot firmware...
if not exist "%ROOT%\firmware\mgtipl\mgtipl.bin" (
    echo ERROR: MGT IPL is missing at firmware\mgtipl\mgtipl.bin
    echo        Assemble it per docs\2026-08-07_NABU_MGT_IPL_FAST_BOOT.md
    exit /b 1
)
if not exist "%ROOT%\third_party\opennabu\opennabu.bin" (
    echo ERROR: the stock OpenNabu fallback is missing at
    echo        third_party\opennabu\opennabu.bin
    exit /b 1
)
copy /y "%ROOT%\firmware\mgtipl\mgtipl.bin"        "%PAYLOAD%\NABU\mgtipl.bin"   >nul || exit /b 1
copy /y "%ROOT%\third_party\opennabu\opennabu.bin" "%PAYLOAD%\NABU\opennabu.bin" >nul || exit /b 1

@REM CREDITS.TXT is generated from CREDITS.md so the two cannot drift.
python "%ROOT%\make_credits_txt.py" "%ROOT%\CREDITS.md" "%PAYLOAD%\CREDITS.TXT"
if errorlevel 1 (
    echo ERROR: make_credits_txt.py failed
    exit /b 1
)

@REM The wizard artwork is generated, not stored: installer\art is
@REM rebuilt from the logo and the icon sources every time so the
@REM installer's face cannot fall behind the brand assets.
python "%HERE%..\tools\gen_icons.py"
if errorlevel 1 ( echo ERROR: gen_icons.py failed & exit /b 1 )
python "%HERE%..\tools\gen_installer_art.py"
if errorlevel 1 ( echo ERROR: gen_installer_art.py failed & exit /b 1 )

@REM The WiX licence dialog wants RTF. Wrap COPYING rather than keeping a
@REM second copy of the AGPL that could fall behind it.
python "%HERE%make_license_rtf.py" "%ROOT%\COPYING" "%PAYLOAD%\LICENSE.rtf"
if errorlevel 1 (
    echo ERROR: make_license_rtf.py failed
    exit /b 1
)

@REM ------------------------------------------------------------------
@REM Build.
@REM ------------------------------------------------------------------
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
set MSINAME=MGT_Unicorn_Suite_%MSIVERSION%.msi

echo Building %MSINAME% ...
%WIX% build "%HERE%MGTUnicornSuite.wxs" ^
    -arch x64 ^
    -ext WixToolset.UI.wixext ^
    -ext WixToolset.Util.wixext ^
    -loc "%HERE%MGTUnicornSuite.wxl" ^
    -d Version=%MSIVERSION% ^
    -d FullVersion=%FULLVERSION% ^
    -d LongName="%LONGNAME%" ^
    -d ShortName="%SHORTNAME%" ^
    -d PayloadDir=%PAYLOAD% ^
    -d ArtDir=%ARTDIR% ^
    -d IconDir=%ICONDIR% ^
    -o "%OUTDIR%\%MSINAME%"
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

echo.
echo BUILD SUCCESSFUL: %OUTDIR%\%MSINAME%
powershell -NoProfile -Command "$h=(Get-FileHash '%OUTDIR%\%MSINAME%' -Algorithm SHA256).Hash.ToLower(); $s=(Get-Item '%OUTDIR%\%MSINAME%').Length; Write-Host ''; Write-Host ('  bytes  : ' + $s); Write-Host ('  sha256 : ' + $h); $h | Set-Content -NoNewline '%OUTDIR%\%MSINAME%.sha256'"
echo.
echo   sha256 also written to %OUTDIR%\%MSINAME%.sha256
endlocal
