CC = /opt/amiga/bin/m68k-amigaos-gcc
CFLAGS = -m68000 -Os -Wall -Wno-pointer-sign -I../../include -noixemul
COMMON = dapamiga.c ../dap/dap.c ../common/nodecfg.c
HDRS = dapamiga.h ../dap/dap.h ../common/nodecfg.h ../../include/libraries/decnet.h

all: dndir dncopy dnnodes

dndir: dndir.c $(COMMON) $(HDRS)
	$(CC) $(CFLAGS) -o $@ dndir.c $(COMMON)

dncopy: dncopy.c $(COMMON) $(HDRS)
	$(CC) $(CFLAGS) -o $@ dncopy.c $(COMMON)

# NML client, no DAP
dnnodes: dnnodes.c ../nml/nml.c ../nml/nml.h ../common/nodecfg.c ../common/nodecfg.h ../../include/libraries/decnet.h
	$(CC) $(CFLAGS) -o $@ dnnodes.c ../nml/nml.c ../common/nodecfg.c

# test tool for incoming links, not distributed
dnaccept: dnaccept.c ../../include/libraries/decnet.h
	$(CC) $(CFLAGS) -o $@ dnaccept.c

clean:
	rm -f dndir dncopy dnnodes dnaccept
