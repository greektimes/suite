# Credits

Dedicated to the loving memory of

Basile Papadopoulos
and
Despina Kavalou-Papadopoulos

---

## Protocol lineage

The Retro Mode of this app speaks protocols designed by other people, most of
them decades ago. Their work is the reason any of this runs.

Attributions below were taken from the specifications themselves wherever a
specification exists. Where an individual could not be confirmed from a primary
source, the project or institution is credited instead.

### Archie (1989-1996)

**Alan Emtage, Bill Heelan and Peter Deutsch**
Creators of Archie, the first Internet search engine, McGill University, 1989

**Bunyip Information Systems, Inc.**
Commercial development of Archie, through the final 3.5 release, 1996

**ICM, University of Warsaw**
Preserved the last surviving copy of the Archie source

**Mark Price and Ben Grubbs, [The Serial Port](https://serialport.org/)**
Tracked down the last surviving copy of Archie and published it, 2024

The Suite's Archie client is original code. It speaks the Prospero ARDP
protocol rather than reusing any part of the original implementation.

### Prospero and ARDP

**B. Clifford Neuman**, USC Information Sciences Institute
Author of the Prospero file system and of ARDP, the reliable datagram protocol
that carries an Archie query

### Archie clients used as parser authorities

**George Ferguson**, author of **xarchie**, the X11 Archie client
**Brendan Kehoe**, author of the **archie** C client

Both are cited by file and line throughout this project's protocol notes. Their
parsers settled several questions about the wire format that the documentation
alone could not.

### WAIS

**Brewster Kahle, Harry Morris, Franklin Davis and Jonny Goldman**
Authors of WAIS

**Jane Smith and Jim Fullton**, CNIDR
Maintained and released the free WAIS implementation

**Ulrich Pfeifer and Norbert Govert**
Authors of freeWAIS-sf, the code this client's Z39.50-1988 layer is built from

### Gopher

**Farhad Anklesaria, Mark McCahill, Paul Lindner, David Johnson, Daniel Torrey
and Bob Alberti**, University of Minnesota
Authors of the Internet Gopher Protocol, RFC 1436, March 1993. Gopher was
created at Minnesota in 1991 by the team McCahill led.

### Electronic mail and ARPANET FTP-Mail

**Ray Tomlinson**
Sent the first network email on the ARPANET in 1971, and chose the @ sign to
separate the user from the host

**Jon Postel**, USC Information Sciences Institute
Author of RFC 765, the File Transfer Protocol, June 1980, which this module
implements

**Brian Harvey**
Author of RFC 691, "One More Try on the FTP", which defines the mail extensions
this module uses

### Internet Relay Chat

**Jarkko Oikarinen**
Created IRC in 1988, and co-author with **Darren Reed** of RFC 1459, the IRC
protocol specification

### CU-SeeMe

**Tim Dorcey** and the CU-SeeMe team at **Cornell University**
Authors of CU-SeeMe, the first widely used Internet video conferencing system

### Telnet and the terminal

**Jon Postel and Joyce Reynolds**
Authors of RFC 854, the Telnet Protocol Specification

**Digital Equipment Corporation**
The VT220 terminal this module emulates

### Finger

**Les Earnest**
Wrote the original FINGER program at the Stanford Artificial Intelligence
Laboratory, as recorded in RFC 1288 itself

**David Zimmerman**
Author of RFC 1288, the Finger User Information Protocol

### Quote of the Day

**Jon Postel**, USC Information Sciences Institute
Author of RFC 865, the Quote of the Day Protocol

### The World Wide Web

**Tim Berners-Lee**
Invented the World Wide Web and HTTP, which this app's retro browser speaks

**Tim Berners-Lee and the W3C**
Authors of libwww, the reference HTTP library the retro browser is built on

### The NABU Network

**NABU Manufacturing Corporation** (Ottawa, 1982-1985)
Built the NABU Personal Computer and ran the first consumer network computer
service, delivering software and pages over cable television

**brijohn**
Author of the NABU PC emulation driver the NABU tab's bundled emulator is
built from

**S. V. Nickolas** (buricco)
Author of Marduk, the NABU emulator the in-window NABU Native tab is a port
of, and of OpenNabu IPL, the openly licensed replacement boot firmware that
tab starts the machine on. Both are MIT. The Suite's own MGT IPL is a fork
of the latter, cut down to load the channel and nothing else, and it is his
work that made a NABU tab possible without a line of preserved firmware

**Marcin Wołoszczuk** (zdebel)
Co-author of Marduk's NABU emulation

**Nicolas Allemand**, **Troy Schrapel** and **Mitsutaka Okazaki**
Authors of the Z80, TMS9918 and AY-3-8910 emulation cores that Marduk
carries and the NABU Native tab therefore runs on. All three are MIT

**Jason R. Thorpe**
Author of nabud, the adaptor server the Montreal Greek Times NABU channel
runs on. No nabud code is in the Suite, but its source is what settled the
channel-change exchange and the native serial rate. See
`THIRD-PARTY-NOTICES.md`

**The NABU preservation community**
The Vintagecomputer.ca archive, **Leo Binkowski**, and NabuNetwork.com, who
dumped and published the NABU boot ROMs. Without that work there would be
nothing to boot

### Newspaper reader

**The Internet Archive**
Authors of BookReader, which presents the print edition. BookReader is AGPLv3,
and is why the whole Suite is AGPLv3.

---

## Third-party code

Full licence texts are in `THIRD-PARTY-NOTICES.md` and the `licenses/`
directory. The components are freeWAIS-sf (CNIDR, 1993), W3C libwww 5.4.1,
FFmpeg TrueSpeech (LGPL 2.1 or later), minimp3 (CC0), the Ubuntu Font, the
Microsoft WebView2 SDK, and the Internet Archive BookReader (AGPLv3).

The NABU Native tab links in the emulation core of Marduk (MIT), together
with the Z80, TMS9918 and AY-3-8910 cores it carries, and boots the machine
on MGT IPL, the Suite's fork of OpenNabu IPL (MIT). Those are compiled into
the Suite itself rather than launched beside it.

The NABU tab additionally bundles a NABU-only build of MAME 0.250
(GPL-2.0-or-later, Copyright (c) 1997-2022 MAMEdev and contributors) as a
SEPARATE PROGRAM, launched as its own process and never linked into the Suite,
together with the preserved NABU boot ROMs. See `THIRD-PARTY-NOTICES.md` for
the licence, the written offer for the corresponding source, and the ROM
preservation credit.

---

## Acknowledgement

Written by **Dimitri Papadopoulos** for The Montreal Greek Times, with the
assistance of modern AI tools.

Thanks to the archives, mirrors and volunteers who kept this source code
readable thirty years after the servers it spoke to were switched off. Without
the people who preserved it, none of it could have been rebuilt.

---

The Montreal Greek Times
https://www.greektimes.ca
