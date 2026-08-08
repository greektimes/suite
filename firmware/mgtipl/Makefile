# MGT IPL, the Montreal Greek Times fork of OpenNabu IPL.
#
# The assembler is the z80asm in the Debian repositories, version 1.8, which
# is what upstream OpenNabu IPL is written for. It is NOT z88dk's assembler
# of the same name; the dialects differ and this source will not assemble
# under z88dk unmodified.
#
# The Suite is built on Windows and this is assembled on the Unicorn server,
# which is Debian. See docs/2026-08-07_NABU_MGT_IPL_FAST_BOOT.md for the
# build and verify procedure, which checks the sha256 at both ends.

all:	mgtipl.bin

mgtipl.bin:	mgtipl.a80
	z80asm -o mgtipl.bin mgtipl.a80

clean:
	rm -f mgtipl.bin
