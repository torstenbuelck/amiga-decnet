CC = /opt/amiga/bin/m68k-amigaos-gcc
CFLAGS = -Os -Wall -Wno-pointer-sign -noixemul -m68020
LDFLAGS = -noixemul

dnhello: dnhello.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f dnhello
