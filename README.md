# MGT Unicorn Suite

The official Windows app of The Montreal Greek Times. Watch Montreal Greek
Television live, listen to Montreal Greek Radio, read the newspaper, and follow
the news of the Greek community of Greater Montreal.

[![download](https://img.shields.io/badge/download-v0.5.0--beta-blue)](https://github.com/greektimes/suite/releases)
[![license](https://img.shields.io/badge/license-AGPL--3.0-blue)](https://github.com/greektimes/suite/blob/main/COPYING)

---

## Status

**v0.5.0-beta**, 8 August 2026. Beta: usable, and still changing.

New in this release, all of it NABU:

- A **built-in NABU emulator**. The NABU tab is now an emulated NABU
  Personal Computer running inside the app's own window, booting the
  Montreal Greek Times NABU channel in seconds.
- **Fast-boot firmware.** The emulated machine boots on MGT IPL, this
  project's fork of OpenNabu IPL, which skips the floppy, hard disk and
  memory checks a machine with none of those does not need. It reaches
  the channel in under half a second where stock firmware takes ten.
- **Any NABU server.** Point the tab at a host and port of your choice.
- **Real NABU hardware.** If you own a NABU, the Suite can act as its
  Internet Adapter over a serial cable, so the actual machine boots its
  own ROM against a channel server.

---

## What it does

Four things, on four tabs:

- **Live TV** streams Montreal Greek Television.
- **Live Radio** streams Montreal Greek Radio.
- **Newspaper** opens the print edition of The Montreal Greek Times in a
  page-turning reader, with back issues.
- **Website** opens greektimes.ca inside the app.

---

## Download

| where | link |
| --- | --- |
| GitHub | https://github.com/greektimes/suite/releases |
| FTP | ftp://ftp.greektimes.ca/pub/suite/MGT_Unicorn_Suite_0.5.0.msi |
| Gopher | gopher://gopher.greektimes.ca/1/software |

Run `MGT_Unicorn_Suite_0.5.0.msi`. It installs to Program Files for all users
and adds a Start Menu entry; the desktop shortcut is optional and can be turned
off during setup. Uninstall from Settings, Apps and features.

The app checks for its own updates: **Help**, then **Update**. An installed
v0.4.0-beta will offer this release as an in-place upgrade.

---

## Requirements

- Windows 11. It also runs on Windows 10, version 22H2 or later.
- The Microsoft WebView2 Runtime, used by the Newspaper reader, the Website tab
  and the browser. Windows 11 already has it. On Windows 10, install the
  [Evergreen Bootstrapper](https://developer.microsoft.com/microsoft-edge/webview2/)
  before the first launch.

---

## Retro Mode

The switch at the top of the window turns the app into a 1990s internet client.
This part is for readers curious about how the internet worked before the web
took over. It is not needed to watch television or read the paper.

Each tab speaks to a live server run by the MGT Unicorn project, answering the
real protocol rather than imitating it.

The NABU tab is the one exception, and deliberately so. A NABU Personal
Computer is a Z80 machine, and the honest way to show one is to emulate one.
That emulation runs inside the Suite's own window: it is the Marduk emulation
core, with its Z80, TMS9918 and AY-3-8910 chip cores, compiled in and booting
on MGT IPL, this project's fork of OpenNabu IPL. All of it is MIT, and all of
it is vendored in this repository under `third_party/marduk`,
`third_party/opennabuipl` and `firmware/mgtipl`, with every local change
documented. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

The same tab can drive real hardware. Given a serial cable, the Suite acts as
a NABU Internet Adapter, and a genuine NABU boots its own ROM against a
channel server.

| tab | protocol | default server |
| --- | --- | --- |
| RIPscrip | RIPscrip 1.54 vector graphics over Telnet | ripscrip.greektimes.ca:23 |
| NABU | NABU adapter protocol over TCP, emulated in-window | nabu.greektimes.ca:5816 |
| Retro Web Browser | HTTP, W3C libwww 5.4.1 | http://retro.greektimes.ca/ |
| Gopher | RFC 1436 | gopher.greektimes.ca:70 |
| ARPANET FTP-Mail | RFC 765 and RFC 691 | arpanet.greektimes.ca:2121, INFO@GREEKTIMES |
| WAIS | Z39.50-1988, freeWAIS-sf | wais.greektimes.ca:210, database `greektimes` |
| Archie | Prospero ARDP over UDP | archie.greektimes.ca:1525, telnet on 2323 |
| IRC | RFC 1459, read-only view | irc.greektimes.ca:6667, channel `#retro` |
| CU-SeeMe Live TV | Cornell CU-SeeMe over UDP | cu-seeme.greektv.ca:7648 |
| Terminal | Telnet VT220, with Finger, QOTD and Zmodem | telnet.greektimes.ca |

---

## Build

MSYS2 UCRT64, GCC. From the project root:

```
build_x64.bat
```

To build the installer as well, with the WiX Toolset installed:

```
installer\build_msi.bat
```

The version is set in one place, `src/suite_version.h`. Everything else,
including the MSI ProductVersion, derives from it.

---

## License

Free software under the **GNU Affero General Public License, version 3 or
later**. See [COPYING](COPYING).

Third-party components keep their own licences, listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and the `licenses/`
directory.

---

## Code signing

**The Windows binaries are not code signed.** The installer and the
executable it installs carry no Authenticode signature. Windows SmartScreen
may therefore warn you on first run, as it did for previous releases. There
is no signing planned for this project at present.

Verify a download by its SHA-256 instead. The hash for each release is
published on this repository's Releases page, in the `README.TXT` in the
FTP directory, and in the update manifest at
`https://apps.greektimes.ca/unicorn-suite/manifest.json`. The Suite's own
update checker refuses any installer whose SHA-256 does not match the hash
the manifest gives.

### How binaries are built

The published v0.5.0-beta installer was built by the maintainer on a local
Windows machine, with `build_x64.bat` and then the MSI build script under
`installer/`, and uploaded from there.

A GitHub Actions workflow, `.github/workflows/build-msi.yml`, can also build
the MSI from this repository on a GitHub-hosted runner. It is not currently
the release path.

One identical file is published everywhere. The MSI on this repository's
Releases page is byte for byte the same file, with the same SHA-256, as the
one on the project's own distribution endpoint at `apps.greektimes.ca`, on
`ftp.greektimes.ca`, and on `gopher.greektimes.ca`.

The version is set in one place, `src/suite_version.h`, and everything else
including the MSI ProductVersion derives from it.

### Team roles

This is a single-maintainer project. Dimitri Papadopoulos (GitHub:
Dimitri-Papadopoulos), publisher of The Montreal Greek Times, is the sole
Author, Reviewer, and Approver, and is the only person with write or release
authority over this repository. Because the build scripts and CI
configuration determine the released output, changes to those files are
treated as security-relevant and reviewed as source.

### Privacy statement

The Suite is a client for The Montreal Greek Times' own services. It contains
no analytics, telemetry, crash reporting, or usage tracking of any kind, and it
transmits no personal or machine-identifying information to any server.

On startup it makes one automatic network request: an update check to
`https://apps.greektimes.ca/unicorn-suite/manifest.json` over HTTPS, throttled
to at most once every 24 hours and disableable in settings. The request carries
nothing beyond the plain GET; any update it offers is downloaded only over HTTPS
and installed only if its SHA-256 matches the hash the manifest gives.

Every other connection happens only when you choose it, by playing a stream,
opening a tab, or connecting a retro-protocol client. Through all of its own
default code paths the Suite contacts only servers operated by The Montreal
Greek Times and Montreal Greek TV, under `greektimes.ca`, `greektv.ca`, and
`greekradio.ca`: live TV and radio, the newspaper reader, and the historical
protocol services (Gopher, Telnet, WAIS, Archie, IRC, CU-SeeMe, ARPANET
FTP-Mail, RIPscrip, Finger, QOTD, and FTP).

Two honest exceptions. First, the Website and Newspaper tabs use Microsoft's
WebView2, a full browser control that loads real web pages and will fetch
whatever third-party resources those pages embed; WebView2 is a Microsoft
component with its own update and data behaviour, and stores its data locally
under your user profile. Second, the retro-protocol clients and the retro web
browser have editable address fields, so if you enter a third-party host, the
Suite will connect to it, at your direction.

No data leaves your machine without either your action or the update check
described above.

---

## Credits

The protocol authors and the projects this is built on are named in
[CREDITS.md](CREDITS.md).

---

Dedicated to the loving memory of

Basile Papadopoulos
and
Despina Kavalou-Papadopoulos

---

## Contact

The Montreal Greek Times
https://www.greektimes.ca
