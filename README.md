# MGT Unicorn Suite

[![download](https://img.shields.io/badge/download-v0.3.0--beta-blue)](https://github.com/greektimes/suite/releases) [![license](https://img.shields.io/badge/license-AGPL--3.0-blue)](https://github.com/greektimes/suite/blob/main/COPYING)

The official Windows app of The Montreal Greek Times
(greektimes.ca).

- Watch **Montreal Greek TV** live
- Listen to **Montreal Greek Radio** live
- Read the print replica of **The Montreal Greek Times**
  newspaper in a page-flip reader
- Follow real-time news from the Greek community of
  Greater Montreal

## Retro Mode

For vintage internet enthusiasts, the app also includes a
Retro Mode: a text-first client for the live retro access
points of the MGT Unicorn server:
- Unicorn Desktop (Active Channel CDF over HTTP, retro.greektimes.ca)
- Retro Web Browser (HTTP and HTML via W3C libwww, home page retro.greektimes.ca)
- Gopher (RFC 1436, gopher.greektimes.ca)
- ARPANET FTP-Mail (RFC 765/691 mail over FTP, arpanet.greektimes.ca port 2121)
- WAIS Search (Z39.50-1988 via freeWAIS-sf, wais.greektimes.ca port 210)
- Archie (Prospero/ARDP file search, archie.greektimes.ca port 1525; interactive telnet on port 2323)
- IRC (IRCv3 read-only view of #retro, irc.greektimes.ca port 6667)
- CU-SeeMe Live TV (CU-SeeMe over UDP, cu-seeme.greektv.ca port 7648)
- Terminal (Telnet RFC 854 with VT220 emulation, plus Finger RFC 1288 and QOTD RFC 865 shortcuts, telnet/finger/qotd.greektimes.ca)

## Requirements

Windows 11 (also runs on Windows 10 22H2 or later). Modern Mode
requires the Microsoft WebView2 Runtime, which ships in-box on
Windows 11; on Windows 10 install the WebView2 Evergreen
Bootstrapper before first launch.

## Building

Native Win32 C, no framework. Built with MSYS2 UCRT64
GCC on x64. From the repository root:

    build_x64.bat

The build produces MGT_Unicorn_Suite_x64.exe.
WebView2Loader.dll must sit beside the executable at runtime.

## Downloads

Prebuilt binaries are published on the Releases page and on
ftp://ftp.greektimes.ca/pub/suite/MGTSUITE.ZIP

## License

Copyright (C) 2026 Dimitri Papadopoulos and
The Montreal Greek Times.

The MGT Unicorn Suite is free software, released under the
GNU Affero General Public License v3.0 or later. See COPYING.

The Suite embeds and links third-party open source components,
including freeWAIS-sf 2.2.14 (CNIDR 1993 license), W3C libwww
5.4.1 (W3C Software Notice), the FFmpeg TrueSpeech decoder
(LGPL 2.1 or later), minimp3 (CC0), the Ubuntu Font (Ubuntu
Font Licence 1.0) and the Microsoft WebView2 SDK. Full
attribution and license texts: THIRD-PARTY-NOTICES.md and
the licenses/ directory.

The Newspaper tab consumes an Internet Archive BookReader
(AGPLv3) instance hosted by The Montreal Greek Times; its
source is at github.com/internetarchive/bookreader

## See also

The standalone WAIS client: github.com/greektimes/wais
