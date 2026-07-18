# Third-Party Notices

The MGT Unicorn Suite incorporates the third-party components listed
below. Each section gives the upstream origin, the copyright notice as
found in the actual source files, the governing license, where the full
license text lives in this repository, and what the Suite uses the
component for.

Copyright signs that appear as the ISO-8859-1 byte 0xA9 in the original
files are rendered here as "(C)".

## freeWAIS-sf 2.2.14

- Upstream origin: freeWAIS-sf 2.2.14 (released 2000-06-07), the final
  release of the Pfeifer/Govert lineage at Universitaet Dortmund,
  descended from CNIDR freeWAIS and the Thinking Machines WAIS
  implementation. Tarball hosted at
  https://www.nic.funet.fi/index/files/index/networking/services/freeWAIS/freeWAIS-sf/
- Copyright, as found in the source headers (for example
  `src/wais/zutil.c`):
  - `(C) Copyright 1997, Universitaet Dortmund, all rights reserved.`
  - `(C) Copyright CNIDR (see ../doc/CNIDR/COPYRIGHT)`
  and in the license text itself:
  - `Copyright (c) MCNC, Clearinghouse for Networked Information
    Discovery and Retrieval, 1993.`
- License: CNIDR 1993 permissive license.
- License text: `licenses/CNIDR-1993.txt` (reproduced from
  `doc/CNIDR/COPYRIGHT` in the upstream 2.2.14 tarball).
- Used for: the WAIS protocol engine (Z39.50-1988 encode/decode,
  transport, document IDs) behind the F2 WAIS Search module. The
  preserved protocol sources live in `src/wais/`.

## W3C libwww 5.4.1

- Upstream origin: W3C's libwww reference library, version 5.4.1, from
  the W3C source repository (GitHub mirror tag `5.4.1`). Preserved in
  `src/libwww/`.
- Copyright, as found in `src/libwww/COPYRIGH` and the per-file
  headers (`(c) COPYRIGHT MIT 1995.`):
  - `Copyright (C) 1995-2003 World Wide Web Consortium, (Massachusetts
    Institute of Technology, European Research Consortium for
    Informatics and Mathematics, Keio University). All Rights
    Reserved.`
  - `Copyright (C) 1995 CERN. "This product includes computer software
    created and made available by CERN. This acknowledgment shall be
    mentioned in full in any product which includes the CERN computer
    software included herein or parts thereof."`
- License: W3C IPR Software Notice (the W3C Software Notice and
  License; permissive and GPL-compatible).
- License text: `licenses/W3C-Software-Notice.txt` (plain-text
  extraction of `src/libwww/LICENSE.html`; the HTML original and
  `src/libwww/COPYRIGH` are kept in-tree).
- Historical note: the libwww tree also ships `src/libwww/COPYING.LIB`
  (LGPL v2). That file is historical; the governing upstream license
  for libwww is the W3C Software Notice above. Both files are preserved
  in-tree and both licensors are credited.
- Used for: the HTTP/HTML retrieval and parsing engine behind the F4
  Web module (and the Active Channel / Active Desktop scenes built on
  it).

## FFmpeg TrueSpeech decoder

- Upstream origin: the DSP Group TrueSpeech compatible decoder from the
  FFmpeg project (libavcodec), adapted into
  `src/truespeech_decoder.[ch]` and `src/truespeech_data.h`.
- Copyright, as found in `src/truespeech_decoder.c`:
  - `Copyright (c) 2005 Konstantin Shishkov`
- License: GNU Lesser General Public License v2.1 or later.
- License text: `licenses/LGPL-2.1.txt`.
- Compliance note: LGPL v2.1 section 3 permits relicensing a copy under
  the GPL; the "or later" grant reaches GPLv3, and AGPLv3 links with
  GPLv3 code. The Suite publishes its complete corresponding source, so
  all source-availability obligations are satisfied.
- Used for: decoding DSP Group TrueSpeech audio in the retro Audio
  Player.

## minimp3

- Upstream origin: https://github.com/lieff/minimp3, vendored as
  `src/minimp3.h`.
- Copyright, as found in `src/minimp3.h`:
  - `To the extent possible under law, the author(s) have dedicated all
    copyright and related and neighboring rights to this software to
    the public domain worldwide.`
- License: CC0 1.0 Universal (public domain dedication), per
  http://creativecommons.org/publicdomain/zero/1.0/ as referenced in
  the header.
- License text: the dedication in the `src/minimp3.h` header itself.
- Used for: MP3 decoding in the shared audio service (Gopher and Web
  module audio playback, retro Audio Player).

## Ubuntu Font (Ubuntu Mono)

- Upstream origin: the Ubuntu font family by Canonical Ltd (with
  Dalton Maag). Bundled as `src/fonts/UbuntuMono-Regular.ttf` and
  `src/fonts/UbuntuMono-Bold.ttf`, embedded in the binary as RCDATA
  resources.
- Copyright: Canonical Ltd, licensed under the terms in the in-tree
  license file.
- License: Ubuntu Font Licence, Version 1.0.
- License text: `src/fonts/COPYING.UbuntuFontLicense.txt` (in-tree
  copy).
- Used for: the F3 Gopher client's rendering font.

## Microsoft WebView2 SDK

- Upstream origin: the Microsoft.Web.WebView2 NuGet package, preserved
  in `third_party/webview2/`. The Suite redistributes
  `WebView2Loader.dll`, which Microsoft's distribution documentation
  explicitly intends to ship with applications.
- Copyright, as found in `third_party/webview2/LICENSE.txt`:
  - `Copyright (C) Microsoft Corporation. All rights reserved.`
- License: the Microsoft WebView2 SDK license, included verbatim as
  `third_party/webview2/LICENSE.txt`, with the accompanying
  `third_party/webview2/NOTICE.txt` (third-party notices for material
  incorporated by Microsoft) also included verbatim.
- Used for: hosting the WebView2 browser control in the Newspaper and
  Website modules. The WebView2 Runtime itself is a system component
  installed on the user's machine and is not part of this repository.

## Internet Archive BookReader

- Upstream origin: https://github.com/internetarchive/bookreader
- License: GNU Affero General Public License v3.0.
- License text: upstream repository; see also `/COPYING` in this
  repository for the AGPLv3 text.
- Relationship: BookReader is a remote hosted service consumed by the
  Newspaper tab. It is not compiled into the Suite binary and its code
  is not vendored in this repository. Its AGPLv3 licensing is the
  reason the combined work is published under AGPLv3; its source is
  available at the upstream repository above.

## GCC runtime (libgcc, libssp)

- Upstream origin: the GNU Compiler Collection (MinGW-w64 / MSYS2
  UCRT64 toolchain). The Suite links these runtime libraries
  statically.
- Copyright: Free Software Foundation, Inc.
- License: GPLv3 with the GCC Runtime Library Exception, which permits
  distributing the runtime as part of programs under any license.
- License text: https://www.gnu.org/licenses/gcc-exception-3.1.html
- Used for: compiler runtime support and stack-smashing protection
  (`-fstack-protector-strong`, `-lssp`) in the shipped binary.

## Windows in-box system libraries

The Suite links against Windows system libraries that ship with the
operating system (ws2_32, comctl32, comdlg32, gdi32, gdiplus, msimg32,
ole32, oleaut32, uuid, mfuuid, winmm, mfplat, mf, d3d11, dxgi, winhttp,
wininet, shell32, winspool, and the UCRT). These are linked at runtime
and are not redistributed with the Suite.

## Combined work license

The MGT Unicorn Suite as a combined work is licensed under the GNU
Affero General Public License v3.0 or later. The full license text is
in `/COPYING` at the repository root. The complete corresponding source
is published at https://github.com/greektimes/suite
