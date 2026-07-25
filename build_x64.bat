@REM build_x64.bat - Build script for x64 Windows (MSYS2 UCRT64 / GCC 15)
@REM Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
@REM
@REM Three compile groups per BLUEPRINT 7.3:
@REM   1. WAIS protocol files (src/wais/*.c + sockets_win.c, win_stubs.c):
@REM      preserved CNIDR code, needs -std=gnu89 -fcommon -fpermissive.
@REM   2. libwww 5.4.1 (src/libwww/*.c): preserved W3C source, needs
@REM      -std=gnu89 -fcommon -fpermissive + HAVE_DIRENT_H + NAME_MAX.
@REM      HT_EXPAT / HT_ZLIB / HT_MD5 / HT_POSIX_REGEX disabled in
@REM      src/libwww/windows/config.h.
@REM   3. Suite shell and per-module files (-O2 -fstack-protector-strong).

@echo off
setlocal EnableDelayedExpansion

set CC=C:\msys64\ucrt64\bin\gcc.exe
set WINDRES=C:\msys64\ucrt64\bin\windres.exe

set CFLAGS_WAIS=-std=gnu89 -fcommon -fpermissive -O2 -fstack-protector-strong -D_WIN32 -DTELL_USER -DTCPIP -Isrc -Isrc\wais
set CFLAGS_LIBWWW=-std=gnu89 -fcommon -fpermissive -O2 ^
    -D_WINDOWS -DWIN32 -D_WIN32 ^
    -DHAVE_DIRENT_H -DNAME_MAX=255 ^
    -Isrc\libwww ^
    -Wno-implicit-function-declaration -Wno-implicit-int ^
    -Wno-int-conversion -Wno-incompatible-pointer-types ^
    -Wno-discarded-qualifiers -Wno-pointer-sign ^
    -Wno-format -Wno-unused-result
set CFLAGS_SUITE=-O2 -fstack-protector-strong -D_WIN32 -Isrc -Isrc\wais
@REM web_module.c drives libwww directly; needs the libwww header path
@REM and _WINDOWS so wwwsys.h's WWW_MSWINDOWS / WWW_WIN_WINDOW branches
@REM activate (NO_STDIO gets defined for free, suppressing libwww's
@REM stdio output in the GUI Suite binary).
set CFLAGS_WEB=-O2 -fstack-protector-strong -D_WIN32 -D_WINDOWS ^
    -Isrc -Isrc\wais -Isrc\libwww
@REM 2026-06-15 Newspaper dispatch: WebView2 SDK headers are MSVC-style
@REM EXTERN_C __declspec(selectany) const IID declarations and GCC
@REM warns ~295 times per TU that includes WebView2.h. The host
@REM service is the only TU that does so; CFLAGS_WV2 carries the
@REM include path. WinHTTP for the newspaper_service index fetch.
set CFLAGS_WV2=-O2 -fstack-protector-strong -D_WIN32 -D_WINDOWS ^
    -Isrc -Ithird_party\webview2\build\native\include
set CFLAGS_NEWS=-O2 -fstack-protector-strong -D_WIN32 -Isrc
@REM 2026-06-16 Radio engine dispatch: radio_engine.c uses the Microsoft
@REM AAC Decoder MFT (IMFTransform IIDs live in mfuuid) plus WASAPI
@REM (CLSID_MMDeviceEnumerator / IAudioClient, defined locally via
@REM INITGUID). AvSetMmThreadCharacteristics is resolved at runtime via
@REM LoadLibrary("avrt.dll"), so no -lavrt link entry is needed.
@REM 2026-06-16 Website module dispatch: website_module.c uses
@REM InternetCrackUrlW (wininet) for the domain-lock URL parse and
@REM ShellExecuteW (shell32) to open outbound links in the system browser.
set LDFLAGS=-lws2_32 -lcomctl32 -lcomdlg32 -lgdi32 -lgdiplus -lmsimg32 -lole32 -loleaut32 -luuid -lmfuuid -lwinmm -lmfplat -lmf -ld3d11 -ldxgi -lwinhttp -lwininet -lshell32 -lwinspool -static -mwindows -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va -lssp

set TARGET=MGT_Unicorn_Suite_x64.exe

echo Compiling WAIS protocol files (CNIDR preserved, x64 / GCC 15 / UCRT64)...
for %%f in (zutil zprot wprot wutil wmessage transprt docid list ztype1 irfileio cutil panic ustubs ui_win) do (
    echo   wais\%%f.c
    %CC% %CFLAGS_WAIS% -c -o src\wais\%%f.o src\wais\%%f.c
    if errorlevel 1 exit /b 1
)

echo Compiling libwww 5.4.1 (W3C source preserved, Win32 + GCC 15 / UCRT64)...
set FILES_LWWW_UTILS=HTArray HTAssoc HTAtom HTChunk HTHash HTList HTMemory HTString HTTrace HTUU md5
set FILES_LWWW_CORE=HTAlert HTAnchor HTChannl HTDNS HTError HTEscape HTEvent HTFormat HTHost HTInet HTLib HTLink HTMemLog HTMethod HTNet HTNoFree HTParse HTProt HTReqMan HTResponse HTStream HTTCP HTTimer HTTrans HTUTree HTUser HTWWWStr
set FILES_LWWW_HTTP=HTAABrow HTAAUtil HTCookie HTDigest HTTChunk HTTP HTTPGen HTTPReq HTTPRes HTTPServ HTPEP
set FILES_LWWW_HTML=HTMLPDTD SGML HTMLGen HTTeXGen HTPlain HTML HText HTHInit HTStyle
set FILES_LWWW_INIT=HTInit HTProfil
set FILES_LWWW_MIME=HTBound HTHeader HTMIME HTMIMPrs HTMIMERq HTMIMImp
set FILES_LWWW_STREAM=HTConLen HTEPtoCl HTFSave HTFWrite HTGuess HTMerge HTNetTxt HTSChunk HTTee HTXParse
set FILES_LWWW_TRANS=HTANSI HTBufWrt HTLocal HTReader HTSocket HTWriter
set FILES_LWWW_APP=HTAccess HTDialog HTEvtLst HTFilter HTHist HTHome HTLog HTProxy HTRules
set FILES_LWWW_FILE=HTBInit HTBind HTFile HTMulti
set FILES_LWWW_FTP=HTFTP HTFTPDir
set FILES_LWWW_GOPHER=HTGopher
set FILES_LWWW_NEWS=HTNDir HTNews HTNewsLs HTNewsRq
set FILES_LWWW_TELNET=HTTelnet
set FILES_LWWW_CACHE=HTCache
set FILES_LWWW_DIR=HTIcons HTDescpt HTDir

@REM Build a space-separated list of all libwww .o paths for the link
@REM step below. Compile each .c -> .o, append the .o path to LWWW_OBJS.
set LWWW_OBJS=
for %%g in (UTILS CORE HTTP HTML INIT MIME STREAM TRANS APP FILE FTP GOPHER NEWS TELNET CACHE DIR) do (
    for %%f in (!FILES_LWWW_%%g!) do (
        echo   libwww\%%f.c
        %CC% %CFLAGS_LIBWWW% -c -o src\libwww\%%f.o src\libwww\%%f.c
        if errorlevel 1 exit /b 1
        set LWWW_OBJS=!LWWW_OBJS! src\libwww\%%f.o
    )
)

echo Compiling Suite Win32 glue (sockets_win, win_stubs)...
for %%f in (sockets_win win_stubs) do (
    echo   %%f.c
    %CC% %CFLAGS_WAIS% -c -o src\%%f.o src\%%f.c
    if errorlevel 1 exit /b 1
)

echo Compiling Suite shell and modules...
for %%f in (suite_shell suite_fonts suite_branding suite_clipboard suite_logo audio_format_detect wav_parser tsp_metafile truespeech_decoder wais_module arpamail_module archie_ardp archie_module ftp_fetch vt_term telnet_proto finger_proto qotd_proto telnet_module irc_client irc_module cuseeme_proto cuseeme_video cuseeme_deltamod cuseeme_mulaw cuseeme_idvi cuseeme_module player_service mode_toggle placeholder_module modern_mode livetv_module audio_meter_service radio_engine liveradio_module) do (
    echo   %%f.c
    %CC% %CFLAGS_SUITE% -c -o src\%%f.o src\%%f.c
    if errorlevel 1 exit /b 1
)
echo   web_module.c (libwww-driven, uses CFLAGS_WEB)
%CC% %CFLAGS_WEB% -c -o src\web_module.o src\web_module.c
if errorlevel 1 exit /b 1

echo   web_module_cdf.c (Active Channel parser + dialog, libwww-driven, uses CFLAGS_WEB)
%CC% %CFLAGS_WEB% -c -o src\web_module_cdf.o src\web_module_cdf.c
if errorlevel 1 exit /b 1

echo   activedesktop_module.c (Phase 6b scene, includes render_engine.h, uses CFLAGS_WEB)
%CC% %CFLAGS_WEB% -c -o src\activedesktop_module.o src\activedesktop_module.c
if errorlevel 1 exit /b 1

echo   audio_service.c (shared Sun .au + MP3 player, libwww + winmm + minimp3, uses CFLAGS_WEB)
%CC% %CFLAGS_WEB% -c -o src\audio_service.o src\audio_service.c
if errorlevel 1 exit /b 1

echo   gopher_protocol.c (plain RFC 1436 over TCP, uses CFLAGS_SUITE)
%CC% %CFLAGS_SUITE% -c -o src\gopher_protocol.o src\gopher_protocol.c
if errorlevel 1 exit /b 1

echo   gophermap.c (gophermap line parser, uses CFLAGS_SUITE)
%CC% %CFLAGS_SUITE% -c -o src\gophermap.o src\gophermap.c
if errorlevel 1 exit /b 1

echo   gopher_module.c (F3 Gopher client UI, uses CFLAGS_SUITE)
%CC% %CFLAGS_SUITE% -c -o src\gopher_module.o src\gopher_module.c
if errorlevel 1 exit /b 1

echo   gopher_search_dialog.c (type 7 modal, in-memory DLGTEMPLATE)
%CC% %CFLAGS_SUITE% -c -o src\gopher_search_dialog.o src\gopher_search_dialog.c
if errorlevel 1 exit /b 1

echo   webview2_host_service.c (Newspaper dispatch, uses CFLAGS_WV2)
%CC% %CFLAGS_WV2% -c -o src\webview2_host_service.o src\webview2_host_service.c
if errorlevel 1 exit /b 1

echo   newspaper_service.c (WinHTTP index fetch, uses CFLAGS_NEWS)
%CC% %CFLAGS_NEWS% -c -o src\newspaper_service.o src\newspaper_service.c
if errorlevel 1 exit /b 1

echo   newspaper_module.c (BookReader host, uses CFLAGS_NEWS)
%CC% %CFLAGS_NEWS% -c -o src\newspaper_module.o src\newspaper_module.c
if errorlevel 1 exit /b 1

echo   newspaper_picker.c (back-issues overlay, uses CFLAGS_NEWS)
%CC% %CFLAGS_NEWS% -c -o src\newspaper_picker.o src\newspaper_picker.c
if errorlevel 1 exit /b 1

echo   newspaper_print.c (print pipeline, uses CFLAGS_NEWS)
%CC% %CFLAGS_NEWS% -c -o src\newspaper_print.o src\newspaper_print.c
if errorlevel 1 exit /b 1

echo   website_module.c (greektimes.ca reader, WV2 host + domain lock, uses CFLAGS_NEWS)
%CC% %CFLAGS_NEWS% -c -o src\website_module.o src\website_module.c
if errorlevel 1 exit /b 1

echo   suite.rc (icon + embedded fonts + version resource)
%WINDRES% -i src\suite.rc -O coff -o src\suite_res.o
if errorlevel 1 exit /b 1

echo Linking %TARGET%...
%CC% -o %TARGET% ^
    src\wais\zutil.o src\wais\zprot.o src\wais\wprot.o src\wais\wutil.o ^
    src\wais\wmessage.o src\wais\transprt.o src\wais\docid.o src\wais\list.o ^
    src\wais\ztype1.o src\wais\irfileio.o src\wais\cutil.o src\wais\panic.o ^
    src\wais\ustubs.o src\wais\ui_win.o ^
    %LWWW_OBJS% ^
    src\sockets_win.o src\win_stubs.o ^
    src\suite_shell.o src\suite_fonts.o src\suite_branding.o ^
    src\suite_clipboard.o src\suite_logo.o ^
    src\wais_module.o src\arpamail_module.o src\web_module.o ^
    src\archie_ardp.o src\archie_module.o src\ftp_fetch.o ^
    src\web_module_cdf.o ^
    src\activedesktop_module.o ^
    src\audio_service.o src\audio_format_detect.o ^
    src\wav_parser.o src\tsp_metafile.o src\truespeech_decoder.o ^
    src\gopher_protocol.o src\gophermap.o src\gopher_module.o ^
    src\gopher_search_dialog.o ^
    src\vt_term.o src\telnet_proto.o src\finger_proto.o src\qotd_proto.o ^
    src\telnet_module.o ^
    src\irc_client.o src\irc_module.o ^
    src\cuseeme_proto.o src\cuseeme_video.o src\cuseeme_deltamod.o ^
    src\cuseeme_mulaw.o src\cuseeme_idvi.o src\cuseeme_module.o ^
    src\player_service.o src\mode_toggle.o ^
    src\placeholder_module.o src\modern_mode.o src\livetv_module.o ^
    src\audio_meter_service.o src\radio_engine.o src\liveradio_module.o ^
    src\webview2_host_service.o ^
    src\newspaper_service.o src\newspaper_module.o src\website_module.o ^
    src\newspaper_picker.o src\newspaper_print.o ^
    src\suite_res.o ^
    %LDFLAGS%
if errorlevel 1 exit /b 1

echo BUILD SUCCESSFUL: %TARGET%
endlocal
