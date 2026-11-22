CC = gcc
CFLAGS = -O2 -g -Wall -Wextra -std=gnu99
CORE = ../core/routing.c ../core/nsp.c ../core/mirror.c
HDRS = ../core/dn.h ../core/dn_priv.h dnenv.h ../dngw/dngw.h
DNGW = ../dngw/dngw.c

all: dnhost dntool nsptest routetest dngwtest

dnhost: dnhost.c dnenv.c $(DNGW) $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ dnhost.c dnenv.c $(DNGW) $(CORE)

dntool: dntool.c dnenv.c ../dap/dap.c ../dap/dap.h ../nml/nml.c ../nml/nml.h ../cterm/cterm.c ../cterm/cterm.h ../cterm/ctermh.c ../cterm/ctermh.h ../lat/lat.c ../lat/lat.h $(DNGW) $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ dntool.c dnenv.c ../dap/dap.c ../nml/nml.c ../cterm/cterm.c ../cterm/ctermh.c ../lat/lat.c $(DNGW) $(CORE)

nsptest: nsptest.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ nsptest.c $(CORE)

routetest: routetest.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ routetest.c $(CORE)

dngwtest: dngwtest.c $(DNGW) ../dngw/dngw.h
	$(CC) $(CFLAGS) -o $@ dngwtest.c $(DNGW)

test: nsptest routetest dngwtest
	./nsptest
	./routetest
	./dngwtest

clean:
	rm -f dnhost dntool nsptest routetest dngwtest
