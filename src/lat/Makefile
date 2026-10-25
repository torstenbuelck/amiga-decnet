CC = /opt/amiga/bin/m68k-amigaos-gcc
CFLAGS = -m68000 -Os -Wall -Wno-pointer-sign -I../../include -noixemul
HDRS = lat.h latmsg.h ../amiga/dnmsg.h ../../include/libraries/decnet.h

all: latd lattest

latd: latd.c lat.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ latd.c lat.c

lattest: lattest.c latmsg.h ../amiga/dnmsg.h
	$(CC) $(CFLAGS) -o $@ lattest.c

clean:
	rm -f latd lattest
