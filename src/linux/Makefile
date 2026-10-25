CC = gcc
CFLAGS = -O2 -g -Wall -Wextra -std=gnu99
CORE = ../core/routing.c ../core/nsp.c ../core/mirror.c
HDRS = ../core/dn.h ../core/dn_priv.h dnenv.h

all: dnhost dntool nsptest

dnhost: dnhost.c dnenv.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ dnhost.c dnenv.c $(CORE)

dntool: dntool.c dnenv.c ../dap/dap.c ../dap/dap.h ../nml/nml.c ../nml/nml.h ../cterm/cterm.c ../cterm/cterm.h ../cterm/ctermh.c ../cterm/ctermh.h ../lat/lat.c ../lat/lat.h $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ dntool.c dnenv.c ../dap/dap.c ../nml/nml.c ../cterm/cterm.c ../cterm/ctermh.c ../lat/lat.c $(CORE)

nsptest: nsptest.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ nsptest.c $(CORE)

test: nsptest
	./nsptest

clean:
	rm -f dnhost dntool nsptest
