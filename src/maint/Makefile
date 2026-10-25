CC = /opt/amiga/bin/m68k-amigaos-gcc
CFLAGS = -m68000 -Os -Wall -Wno-pointer-sign -I../../include -noixemul
SRCS = dnmaint.c session.c login.c console.c edit.c latlink.c ../cterm/ctermh.c
HDRS = dnmaint.h ../cterm/ctermh.h ../lat/latmsg.h ../../include/libraries/decnet.h

dnmaint: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f dnmaint
