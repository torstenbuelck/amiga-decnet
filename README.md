# DECnet for the Amiga

*Deutsch: Jedes Paket enthält neben der englischen ReadMe eine deutsche
LiesMich ([DECnet](installer/LiesMich), [DECnet-Term](installer/term/LiesMich)).
Die Installationsprogramme sprechen auf einem deutschen AmigaOS 3.x Deutsch.*

A DECnet Phase IV endnode for AmigaOS 2.04 and newer, 68000 and up. The
Amiga becomes a node in a DECnet network with VAX/VMS, PDP-11 or other
DECnet systems:

- fetches and stores files on them (DAP/FAL): `dncopy`, `dndir`
- shows their files as a disk named `DN:` (AmigaDOS handler), so Workbench
  and every program can open them
- answers `NCP LOOP NODE`, reads node names from a node (`dnnodes`)
- logs in to VMS and other hosts from Amiga terminal programs (Term, NComm,
  VAXterm) through `dnterm.device`, a serial.device look-alike that dials
  CTERM nodes or LAT services
- optionally lets chosen DECnet nodes and LAT terminal servers log in to an
  Amiga Shell (maintenance login, password protected)

It runs on any Ethernet card with a SANA-II driver, side by side with
AmiTCP, Roadshow or Miami on the same card.

## Packages

Download from [Releases](../../releases):

| Archive | What |
|---|---|
| `DECnet.lha` | the stack: dnetd, decnet.library, DN: handler, dncopy, dndir, dnnodes, dncheck, installer |
| `DECnet-Term.lha` | add-on: dnterm.device (CTERM and LAT), latd, maintenance login, dntermpatch |
| `3c589auto.lha` | 3c589.device 1.5 with BNC/10BASE-T auto-select (unofficial patch) |
| `cnetfix.lha` | cnet.device 1.9 with two DECnet bugs fixed (unofficial patch) |

Unpack, read the ReadMe, double-click the installer. AmigaOS 2.04 needs
Installer and LhA from Aminet first (see the ReadMe). The ReadMe also lists
the network cards tested so far.

## Network card drivers

`drivers/` holds fixes for two third-party SANA-II drivers that did not work
with DECnet (both change the Ethernet address, which TCP/IP never does):

- **3c589** – Neil Cafferkey's 3c589.device 1.5 (Aminet `driver/net/3c589`,
  GPL): picks BNC when there is no 10BASE-T link.
- **cnet** – cnet.device 1.9 by Bruce Abbott, Harry Sintonen and Rolf Anders
  (Aminet `driver/net/cnetdevice`, public domain): two one-byte fixes.

These are **unofficial patches, not releases by the drivers' authors**. Each
directory has the patch, the source and a ReadMe with what was changed and
how it was tested.

## Building

Cross-compiled on Linux with [bebbo's amiga-gcc](https://codeberg.org/bebbo/amiga-gcc)
in `/opt/amiga`:

```
make -C src/amiga && make -C src/apps && make -C src/handler
make -C src/term && make -C src/maint && make -C src/lat latd
tools/mkdist.sh        # builds dist/DECnet.lha and dist/DECnet-Term.lha
```

`mkdist.sh` needs LHa for UNIX ([jca02266/lha](https://github.com/jca02266/lha))
to create the archives. The driver archives are built by `drivers/*/build.sh`.

`src/linux` has `dntool` and `dnhost`, a small DECnet node for Linux (raw
sockets) used to test the stack: file transfer, CTERM and LAT logins, loop
tests.

## Layout

```
src/core      NSP, routing layer (endnode), Ethernet framing
src/amiga     dnetd (the stack process), decnet.library, dncheck, dnstartup
src/dap       DAP client          src/handler   DN: handler
src/apps      dncopy, dndir, dnnodes
src/cterm     CTERM               src/lat       LAT (latd)
src/term      dnterm.device       src/maint     maintenance login (dnmaint)
src/nml       NML (loop, node names)
src/linux     Linux test node (dntool, dnhost)
installer     Installer scripts, ReadMe/LiesMich, start scripts
include       decnet.library headers
docs          design notes (German)
tools         packaging and emulator helpers
```

## Licence

GNU General Public License, version 2 or later (see [LICENSE](LICENSE)).
Parts are based on the Linux DECnet code and on dnprogs and latd by
Christine Caulfield. The cnet.device fix stays public domain like the
original; the 3c589.device patch is GPL like the original.

Copyright (C) 2026 Torsten Bülck (tuti). Discussion and reports:
[forum.classic-computing.de](https://forum.classic-computing.de), thread
"DECnet auf dem Amiga".
