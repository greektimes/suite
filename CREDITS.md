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

### NAPLPS videotex

**The joint ANSI X3L2.1 Task Group and CSA Working Group on Videotex**
Authors of ANSI X3.110-1983 / CSA T500-1983, "Videotex/Teletext Presentation
Level Protocol Syntax (North American PLPS)", published December 1983. The
Suite's NAPLPS decoder is a clean-room implementation written from that
standard. No NAPLPS decoder or renderer source was copied or adapted

**The Canadian Communications Research Centre** and **Norpak Corporation**
(Kanata, Ontario)
Developers of Telidon, the Canadian videotex system the North American PLPS
grew out of, and of the decoder hardware that made it a product

**Bell Canada**
Ran Alex, the Montreal videotex service, from 1988 to 1994. Its ALEXTEL
terminal, designed by Norpak and manufactured by Northern Telecom, is the
compatibility target this tab is built toward. Alex and Alextel are named
here as historical context and as that target, not as branding

**Nick Fox-Gieg**
Author of naplps.js (MIT), used as an independent black-box cross-check
oracle while this decoder was written, and of the annotated sample frame
that the segmentation gate is measured against. No code was ported

**Microstar Software Ltd.** (Nepean, Ontario, 1992)
Authors of Personality+III, the period NAPLPS terminal used as a black-box
rendering oracle, and of its README, which is the only source found for the
ALEXTEL emulation profile: a 7 data bit, even parity, 1 stop bit character
frame, the ESC % A and ESC % @ presentation mode switch, and the default
function key sequences

**John Durno**, University of Victoria Libraries
Author of the two technical notes on displaying and interacting with NAPLPS
graphics on a modern computer, which established the DOSBox and virtual
serial method the real-client gate uses

### Minitel videotex (the Videotex tab's Minitel mode)

**France Telecom**
Authors of the "Specifications Techniques d'Utilisation du Minitel 1B"
(STUM1B). The Suite's Minitel decoder is a clean-room implementation
written from that specification, its code tables read from the scanned
pages. No Minitel decoder, emulator or emitter source was read, copied or
adapted

**The volunteer transcribers of STUM1B** at jbellue.github.io/stum1b,
whose HTML transcription was used as a reading aid beside the scan

**Jean-Francois Del Nero**
Author of VDT2BMP, used as a black-box rendering oracle: the same pages in,
BMPs out, compared cell by cell. No code was read or used

**Dylan "Asphox" Soton**
Author of eMinitel (MIT), used as a black-box oracle by earlier gates and
assessed for reuse from its README and licence. It was not reused (the
reasons are in `docs/2026-09-27_SUITE_MINITEL_MODE.md`), and no code of it
is in the Suite

**Frederic Bisson** and **the MiniPavi project**
Authors of the Minitel web emulator MiniPavi hosts, run as a black-box
oracle in a browser; its key map is the model for the tab's PC keys, and
the tab's CRT scan lines reproduce its Scanlines effect as measured from
its rendered pixels (switch on and off, same page). No code was read or
used

### Web Minitel terminal (web/minitel/, phase W1)

**The Emscripten authors**
Emscripten 6.0.10 compiles the Suite's own Minitel decoder and renderer,
unchanged, to WebAssembly for the web terminal; the page it builds carries
small parts of Emscripten's runtime (MIT or University of Illinois/NCSA),
of musl libc (MIT) and of LLVM compiler-rt (Apache 2.0 with LLVM
Exceptions). Licence texts: `web/minitel/site/third-party-licenses.txt`,
served beside the page

**Jean-Arthur Silve** and **the MiniPavi project**
Author of the MiniPavi gateway (GPL), of which the estate runs its own copy
at wss://videotex.greektimes.ca/ws; the web terminal connects to it. It
runs on the server and no code of it is in the page

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

The web Minitel terminal (`web/minitel/`, not part of the installer) is the
Suite's own Minitel engine compiled to WebAssembly by Emscripten; the built
page carries small parts of the Emscripten runtime, musl libc and LLVM
compiler-rt, whose licences are served with it.

The NABU Native tab links in the emulation core of Marduk (MIT), together
with the Z80, TMS9918 and AY-3-8910 cores it carries, and boots the machine
on MGT IPL, the Suite's fork of OpenNabu IPL (MIT). Those are compiled into
the Suite itself rather than launched beside it.

The NAPLPS (videotex) tab contains NO third-party code at all. Its decoder
and raster are clean-room from ANSI X3.110-1983 / CSA T500-1983, its 5x7
glyphs are original work for this project, and naplps.js and
Personality+III were used only as black-box oracles, which ships none of
them and so owes no notice. The standard itself is an ANSI and CSA
copyrighted document held outside the repository as a reference copy, in
the same way RIPSCRIP-1.54.DOC is held, and is not redistributed.

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
