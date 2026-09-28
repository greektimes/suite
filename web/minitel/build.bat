@echo off
rem ---------------------------------------------------------------------
rem  build.bat - build the web Minitel terminal into web\minitel\dist\
rem
rem  Part of the MGT Unicorn Suite. AGPLv3 or later, see /COPYING.
rem
rem  The engine is the Suite's own Minitel decoder and renderer, compiled
rem  UNCHANGED to WebAssembly (web\minitel\engine\mt_web.c is only the
rem  glue). dist\ is generated and ignored by git; it is the whole site:
rem  index.html, minitel.css, minitel.js, engine.js, engine.wasm and
rem  third-party-licenses.txt (the Emscripten runtime's licences).
rem
rem  Toolchain: Emscripten 6.0.10, pinned. emsdk lives OUTSIDE the repo;
rem  set EMSDK_ROOT to override the default location. The build refuses
rem  any other emcc version, so a stray PATH cannot change the output.
rem ---------------------------------------------------------------------
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\.."
set "PIN=6.0.10"
if not defined EMSDK_ROOT set "EMSDK_ROOT=%USERPROFILE%\emsdk-%PIN%"

if not exist "%EMSDK_ROOT%\emsdk_env.bat" (
    echo build.bat: emsdk not found at %EMSDK_ROOT%
    echo            install it with: emsdk install %PIN% ^&^& emsdk activate %PIN%
    exit /b 1
)
rem  A shell started from Git Bash or MSYS2 inherits MSYSTEM, and emsdk then
rem  writes MSYS-style paths into its cmd environment script, which fails.
rem  This is a cmd script: say so.
set "MSYSTEM="
set "EMSDK_CMD=1"
call "%EMSDK_ROOT%\emsdk_env.bat" >nul 2>&1

rem  The pin is enforced: any other emcc stops the build. (The version
rem  line holds parentheses, so it is never expanded inside a block.)
emcc --version 2>nul | findstr /c:") %PIN% (" >nul
if errorlevel 1 goto wrong_emcc

if not exist "%HERE%dist" mkdir "%HERE%dist"

rem  -sENVIRONMENT=web,node: the page loads it, and so does the W-G1
rem  parity gate under Node, so the gate tests the very file that ships.
rem  -sFILESYSTEM=0: the engine does no I/O. emmalloc: Emscripten's own
rem  small allocator. No -g: the output is for serving.
emcc -O2 -Wall -Wextra ^
  -I"%ROOT%\src" ^
  "%HERE%engine\mt_web.c" ^
  "%ROOT%\src\minitel_decode.c" "%ROOT%\src\minitel_screen.c" ^
  "%ROOT%\src\minitel_font_data.c" "%ROOT%\src\naplps_font_data.c" ^
  -sMODULARIZE=1 -sEXPORT_NAME=MinitelEngine ^
  -sENVIRONMENT=web,node -sFILESYSTEM=0 -sMALLOC=emmalloc ^
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=16MB ^
  -sEXPORTED_RUNTIME_METHODS=HEAPU8 ^
  -o "%HERE%dist\engine.js"
if errorlevel 1 (
    echo build.bat: emcc failed
    exit /b 1
)

copy /y "%HERE%site\index.html"  "%HERE%dist\" >nul || exit /b 1
copy /y "%HERE%site\minitel.css" "%HERE%dist\" >nul || exit /b 1
copy /y "%HERE%site\minitel.js"  "%HERE%dist\" >nul || exit /b 1
copy /y "%HERE%site\third-party-licenses.txt" "%HERE%dist\" >nul || exit /b 1

echo build.bat: OK, emcc %PIN%
dir /b "%HERE%dist"
endlocal
exit /b 0

:wrong_emcc
echo build.bat: need emcc %PIN%; found:
emcc --version 2>nul | findstr /b "emcc"
exit /b 1
