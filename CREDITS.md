# Credits

Dedicated to the memory of

Basile Papadopoulos
and
Despina Kavalou-Papadopoulos

---

## Protocol lineage

The Retro Mode of this app speaks protocols designed by other people, most of
them decades ago. Their work is the reason any of this runs.

### Archie (1989-1996)

**Alan Emtage, Bill Heelan and Peter Deutsch**
Creators of Archie, the first Internet search engine, McGill University, 1989

**Bunyip Information Systems, Inc.**
Commercial development of Archie, through the final 3.5 release, 1996

**ICM, University of Warsaw**
Preserved the last surviving copy of the Archie source

**[The Serial Port](https://serialport.org/)**
Found it and set it free, 2024

The Suite's Archie client is original code. It speaks the Prospero ARDP
protocol rather than reusing any part of the original implementation.

### Prospero and ARDP

- **B. Clifford Neuman**, USC Information Sciences Institute. Author of the
  Prospero file system and of ARDP, the reliable datagram protocol that
  carries an Archie query.

### Archie clients used as parser authorities

- **George Ferguson**, author of **xarchie**, the X11 Archie client.
- **Brendan Kehoe**, author of the **archie** C client.

Both are cited by file and line throughout this project's protocol notes. Their
parsers settled several questions about the wire format that the documentation
alone could not.

### WAIS

- **Brewster Kahle**, **Harry Morris**, **Franklin Davis** and
  **Jonny Goldman**. Authors of WAIS.
- **Jane Smith** and **Jim Fullton**, CNIDR, who maintained and released the
  free WAIS implementation.
- **Ulrich Pfeifer** and **Norbert Govert**, authors of **freeWAIS-sf**, the
  code this client's Z39.50-1988 layer is built from.

### CU-SeeMe

- **Tim Dorcey** and the CU-SeeMe team at **Cornell University**. Authors of
  CU-SeeMe, the first widely used internet video conferencing system.

### File Transfer Protocol and FTP-Mail

- **Jon Postel**, USC Information Sciences Institute. Author of RFC 765,
  June 1980, the File Transfer Protocol specification this app's ARPANET
  FTP-Mail client follows.

### Gopher

- The **Gopher team at the University of Minnesota**, who designed and released
  Gopher, specified in RFC 1436.

### Internet Relay Chat

- **Jarkko Oikarinen**, author of IRC.

### The World Wide Web

- **Tim Berners-Lee** and the **W3C**, authors of **libwww**, the reference HTTP
  library this app's retro browser is built on.

Where an individual author could not be verified from a primary source, the
project or institution is credited instead. An unverified name in a credits
file is worse than an omission.

---

## Third-party code

Full licence texts are in `THIRD-PARTY-NOTICES.md` and the `licenses/`
directory. The components are freeWAIS-sf (CNIDR, 1993), W3C libwww 5.4.1,
FFmpeg TrueSpeech (LGPL 2.1 or later), minimp3 (CC0), the Ubuntu Font, the
Microsoft WebView2 SDK, and the Internet Archive BookReader (AGPLv3).

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
