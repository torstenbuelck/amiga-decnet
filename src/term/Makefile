CC = /opt/amiga/bin/m68k-amigaos-gcc
CFLAGS = -m68000 -Os -Wall -Wno-pointer-sign -I../../include -noixemul

# no startup code: norun.c must be the first source (-fno-toplevel-reorder
# keeps it in front). miniprintf.c replaces libnix's vsnprintf,
# which needs its stdio startup.
SRCS = norun.c dnterm.c ../cterm/cterm.c ../handler/miniprintf.c

all: dnterm.device dntermpatch

dnterm.device: $(SRCS) ../cterm/cterm.h ../amiga/dnmsg.h ../../include/libraries/decnet.h
	$(CC) $(CFLAGS) -fno-toplevel-reorder -nostartfiles -o $@ $(SRCS)

dntermpatch: dntermpatch.c
	$(CC) $(CFLAGS) -o $@ dntermpatch.c

clean:
	rm -f dnterm.device dntermpatch
