CC = /opt/amiga/bin/m68k-amigaos-gcc
CFLAGS = -m68000 -Os -Wall -Wno-pointer-sign -I../../include -noixemul

# no startup code: DOS starts the handler process directly, so start.c
# (the entry point) must be the first object. miniprintf.c replaces
# libnix's vsnprintf, which needs its stdio startup.
SRCS = start.c dn-handler.c miniprintf.c ../dap/dap.c ../common/nodecfg.c

dn-handler: $(SRCS) ../dap/dap.h ../common/nodecfg.h
	$(CC) $(CFLAGS) -nostartfiles -o $@ $(SRCS)

clean:
	rm -f dn-handler
