CC = /opt/amiga/bin/m68k-amigaos-gcc
CFLAGS = -m68000 -Os -Wall -Wno-pointer-sign -I../../include
CORE = ../core/routing.c ../core/nsp.c ../core/mirror.c
CORE_H = ../core/dn.h ../core/dn_priv.h

all: dnetd decnet.library dncheck dnstartup

DNGW = dngwamiga.c ../dngw/dngw.c
DNGW_H = dngwamiga.h ../dngw/dngw.h

dnetd: dnetd.c s2buff.h dnserver.c dnserver.h dnmsg.h $(CORE) $(CORE_H) $(DNGW) $(DNGW_H)
	$(CC) $(CFLAGS) -noixemul -o $@ dnetd.c dnserver.c $(DNGW) $(CORE)

dncheck: dncheck.c s2buff.h
	$(CC) $(CFLAGS) -noixemul -o $@ dncheck.c

dnstartup: dnstartup.c
	$(CC) $(CFLAGS) -noixemul -o $@ dnstartup.c

# no C library: -fno-toplevel-reorder keeps LibNoRun first
decnet.library: decnet_lib.c dnmsg.h ../../include/libraries/decnet.h
	$(CC) $(CFLAGS) -fno-toplevel-reorder -nostartfiles -nostdlib -o $@ decnet_lib.c -lgcc

clean:
	rm -f dnetd decnet.library dncheck dnstartup
