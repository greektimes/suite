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

- Upstream origin: the Microsoft.Web.WebView2 NuGet package, unpacked
  into `third_party/webview2/` in the working tree. The Suite
  redistributes `WebView2Loader.dll`, which Microsoft's distribution
  documentation explicitly intends to ship with applications.
- What is in this REPOSITORY: only the two public SDK headers the build
  actually reads, `third_party/webview2/build/native/include/WebView2.h`
  and `WebView2EnvironmentOptions.h`. The rest of the unpacked package,
  including its `LICENSE.txt` and `NOTICE.txt`, is present in a working
  tree that has run the SDK unpack step but is deliberately NOT tracked;
  `.gitignore` excludes the package because the static libraries,
  runtimes, managed assemblies and tools are tens of megabytes the build
  never reads. Obtain them with the package from
  https://www.nuget.org/packages/Microsoft.Web.WebView2 .
- Copyright, as found in the package's `LICENSE.txt`:
  - `Copyright (C) Microsoft Corporation. All rights reserved.`
- License: the Microsoft WebView2 SDK license, distributed with the
  NuGet package as `LICENSE.txt`, together with its `NOTICE.txt`
  (third-party notices for material Microsoft incorporated).
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

## Hershey Fonts (RIPscrip stroked fonts 1 to 10)

- Upstream origin: the public-domain Hershey vector font distribution,
  in the `.jhf` format. The glyphs are the digitised vector letterforms
  developed by **Dr. Allen Vincent Hershey at the United States National
  Bureau of Standards** (now the National Institute of Standards and
  Technology) in the 1960s. As work produced at a US federal agency the
  vector data is in the **public domain**, and it has been distributed
  as such for decades, most widely through the Usenet `.jhf` collection.
- What is in this repository: not the upstream `.jhf` files themselves
  but generated C tables, `src/rip_hershey_data.c`, produced from them
  by `tools/gen_hershey.py`. The generator reads the `.jhf` coordinate
  pairs and emits them as a compact `signed char` array; it performs no
  redrawing and adds no glyph of its own, so the shipped outlines are
  the public-domain Hershey outlines and nothing else.
- License: **public domain**. The distribution asks that the following
  two acknowledgements accompany the font data, and they are reproduced
  here verbatim for that purpose. They are also carried in the header of
  `src/rip_hershey_data.c` and shown in the Suite's About box.

  - The Hershey Fonts were originally created by Dr. A. V. Hershey
    while working at the U. S. National Bureau of Standards.
  - The format of the Font data in this distribution was originally
    created by James Hurt, Cognition, Inc., 900 Technology Park Drive,
    Billerica, MA 01821.

  Note that the second acknowledgement credits the `.jhf` FILE FORMAT,
  not the letterforms. Neither acknowledgement is a copyright claim and
  neither imposes a condition beyond attribution.
- Used for: the RIPscrip stroked fonts, font numbers 1 to 10, in the
  RIPscrip tab. RIPscrip names the same font families the Hershey set
  provides, and the 1.54 specification's own font metric tables size
  them at exactly the Hershey natural capital height.
- **WHY PUBLIC-DOMAIN GLYPHS SPECIFICALLY.** A RIPscrip terminal of the
  period drew its stroked fonts from Borland's BGI `.CHR` stroke files,
  which are proprietary and are not redistributable. Sourcing the
  letterforms from the public-domain Hershey set is what lets this
  renderer produce the correct stroked output with **no proprietary font
  data of any kind in the tree or in the shipped binary**. It is the
  same discipline applied to the Zmodem CRC files described below:
  where a component would have brought in a licence the Suite cannot
  ship, it was replaced with one that carries no such condition rather
  than worked around.
- Not used: no Borland `.CHR` font file, no BGI font data, and no
  TeleGrafix font asset is read, converted, embedded or redistributed by
  this project.

## font8x8 (RIPscrip default font 0)

- Upstream origin: `font8x8` by Daniel Hepper, itself based on the
  public-domain 8x8 VGA font by Marcel Sondaas. Vendored as a generated
  C table in `src/rip_font8x8_data.c`, produced by
  `tools/gen_font8x8.py`.
- License: public domain.
- Used for: the RIPscrip default 8x8 bitmap font (font number 0) in the
  RIPscrip tab.

## RIPscrip protocol

- RIPscrip is a trademark of TeleGrafix Communications, Inc. This
  project implements the published RIPscrip 1.54 protocol
  specification, pinned at `docs/ripscrip-ref/RIPSCRIP-1.54.DOC`. No
  TeleGrafix code or asset is used or redistributed.

## mbzm (Zmodem receive core)

- Upstream origin: mbzm, https://github.com/roscopeco/mbzm, a small
  receive-only Zmodem implementation written against the published
  ZMODEM protocol specification. Vendored in `third_party/mbzm/`.
- Copyright, as found in the source headers and `LICENSE`:
  - `Copyright (c)2020 Ross Bamford`
- License: MIT.
- License text: `licenses/LICENSE.mbzm.txt` (a copy of the upstream
  `LICENSE`, which is also kept in-tree at `third_party/mbzm/LICENSE`).
- Used for: the Zmodem protocol primitives (header encode and decode,
  ZDLE escaping, data subpacket reads) behind the RIPscrip tab's file
  download. The session driver, the Win32 threading, the file naming
  policy and every security decision are Suite code in
  `src/zmodem_recv.[ch]` and are not part of the vendored core.
- MODIFICATIONS, and this one matters for licensing: upstream mbzm
  vendors its two CRC files (`crc16.c`, `crc32.c` and their headers)
  from Synchronet's smblib, which is **GNU GPL v2 or later**, not MIT.
  The Suite ships no copyleft code, so all four files were REMOVED from
  the vendored tree and replaced with implementations written from the
  polynomial definitions alone (CRC-16/XMODEM, poly 0x1021, init 0; and
  CRC-32, reflected poly 0xEDB88320, init 0xFFFFFFFF, final complement).
  The replacements build their tables at run time, are placed in the
  public domain by their author, and are checked against the standard
  "123456789" vectors. Nothing from Synchronet, lrzsz or Omen
  Technology is present in the binary or in this repository.
  See `licenses/LICENSE.mbzm-smblib-REMOVED.txt` for the full account
  and `third_party/mbzm/MGT-CHANGES.md` for the vendoring notes.
  Upstream's `rz.c` example driver was also dropped.

## WiX Toolset (installer build tooling, and installer UI resources)

- Upstream origin: the WiX Toolset, https://wixtoolset.org/, source at
  https://github.com/wixtoolset/wix. Installed as a .NET global tool and
  pinned; it is not vendored in this repository.
- Versions pinned and used to build the MSI:
  - `wix` **5.0.2**
  - `WixToolset.UI.wixext` **5.0.2**
  - `WixToolset.Util.wixext` **5.0.2**
- Copyright, as found in the project's `LICENSE.TXT` at tag `v5.0.2` and
  in the NuGet metadata of all three packages:
  - `Copyright (c) .NET Foundation and contributors. All rights reserved.`
- License: the **Microsoft Reciprocal License (MS-RL)**. Stated in
  `LICENSE.TXT` at tag `v5.0.2` ("This software is released under the
  Microsoft Reciprocal License (MS-RL)") and declared as the SPDX
  expression `MS-RL` in the `wix`, `WixToolset.UI.wixext` and
  `WixToolset.Util.wixext` 5.0.2 packages. License text:
  http://opensource.org/licenses/ms-rl
- Used for: building `MGT_Unicorn_Suite_<version>.msi` from
  `installer/MGTUnicornSuite.wxs`. See `installer/build_msi.bat`.
- **IT IS NOT ONLY BUILD TOOLING.** WiX also contributes content that is
  redistributed inside the shipped MSI, so it is credited here rather
  than treated as a build-time-only dependency the way ImageMagick and
  the MSYS2 toolchain are:
  - two compiled custom-action libraries embedded as MSI binary
    streams, `Wix4UtilCA_X64` (from the Util extension, used for the
    close-the-running-app prompt) and `WixUiCa_X64` (from the UI
    extension);
  - the `WixUI_FeatureTree` dialog set, which is WiX-authored content
    compiled into the package's Dialog and Control tables;
  - four small stock UI images kept as WiX shipped them:
    `WixUI_Ico_Exclam`, `WixUI_Ico_Info`, `WixUI_Bmp_New` and
    `WixUI_Bmp_Up`.
- Not from WiX: the two large wizard images, `WixUI_Bmp_Dialog` and
  `WixUI_Bmp_Banner`, are Montreal Greek Times artwork generated by
  `tools/gen_installer_art.py` and they replace WiX's stock bitmaps in
  the built package. The wizard text is likewise overridden from
  `installer/MGTUnicornSuite.wxl`.
- Scope note: MS-RL is a reciprocal license that applies per file. It
  covers the WiX files listed above, which are redistributed unmodified
  inside the installer. It does not reach the application itself:
  `MGT_Unicorn_Suite_x64.exe` contains no WiX code.

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
