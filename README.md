# MGT Unicorn Suite

The official Windows app of The Montreal Greek Times. Watch Montreal Greek
Television live, listen to Montreal Greek Radio, read the newspaper, and follow
the news of the Greek community of Greater Montreal.

[![download](https://img.shields.io/badge/download-v0.3.1--beta-blue)](https://github.com/greektimes/suite/releases)
[![license](https://img.shields.io/badge/license-AGPL--3.0-blue)](https://github.com/greektimes/suite/blob/main/COPYING)

---

## Status

**v0.3.1-beta**, 25 July 2026. Beta: usable, and still changing.

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
| Web | https://greektimes.ca/wp-content/uploads/2026/07/MGTSUITE.zip |
| FTP | ftp://ftp.greektimes.ca/pub/suite/MGTSUITE.ZIP |
| Gopher | gopher://gopher.greektimes.ca/1/software |

Unzip anywhere. Keep `WebView2Loader.dll` next to `MGT_Unicorn_Suite_x64.exe`
and run the executable. There is no installer.

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

| tab | protocol | default server |
| --- | --- | --- |
| Unicorn Desktop | Active Channel style desktop | greektimes.ca |
| Retro Web Browser | HTTP, W3C libwww 5.4.1 | http://retro.greektimes.ca/ |
| Gopher | RFC 1436 | gopher.greektimes.ca:70 |
| ARPANET FTP-Mail | RFC 765 and RFC 691 | arpanet.greektimes.ca:2121, INFO@GREEKTIMES |
| WAIS | Z39.50-1988, freeWAIS-sf | wais.greektimes.ca:210, database `greektimes` |
| Archie | Prospero ARDP over UDP | archie.greektimes.ca:1525, telnet on 2323 |
| IRC | RFC 1459, read-only view | irc.greektimes.ca:6667, channel `#retro` |
| CU-SeeMe Live TV | Cornell CU-SeeMe over UDP | cu-seeme.greektv.ca:7648 |
| Terminal | Telnet VT220, with Finger and QOTD | telnet.greektimes.ca |

---

## Build

MSYS2 UCRT64, GCC. From the project root:

```
build_x64.bat
```

---

## License

Free software under the **GNU Affero General Public License, version 3 or
later**. See [COPYING](COPYING).

Third-party components keep their own licences, listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and the `licenses/`
directory.

---

## Credits

The protocol authors and the projects this is built on are named in
[CREDITS.md](CREDITS.md).

---

Dedicated to the memory of

Basile Papadopoulos
and
Despina Kavalou-Papadopoulos

---

## Contact

The Montreal Greek Times
https://www.greektimes.ca
