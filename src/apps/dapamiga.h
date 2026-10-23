/*
 * dapamiga.h - DAP client over decnet.library (shared by dndir, dncopy).
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DAPAMIGA_H
#define DAPAMIGA_H

#include "../dap/dap.h"

/* Open decnet.library, connect to FAL on node and exchange CONFIG.
   Without user, USER and PASSWORD come from ENV:DECnet/nodes; NOBLOCK
   there clears blocking.
   Prints the reason, keeps it in dapa_error and returns NULL on failure. */
struct dap *dapa_open(const char *node, const char *user, const char *password,
                      int blocking, int verbose);
void dapa_close(struct dap *d);

extern char dapa_error[200];

#endif
