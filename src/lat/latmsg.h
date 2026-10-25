/*
 * latmsg.h - talking to latd.
 *
 * latd's public port "LAT" takes the DnMsg messages of dnetd's private
 * protocol (../amiga/dnmsg.h), so a program that talks to dnetd needs
 * little more for LAT:
 *
 *   DNC_CONNECT  dm_Buf struct LatConnect, dm_Timeout (ms, 0: 30 s)
 *                -> dm_Link session, or dm_Result -1 and dm_Reason (a LAT
 *                reason, see lat.h; LAT_R_NOSVC if the service was not
 *                announced within the timeout). A leading T or P (from
 *                ATDT/ATDP) is dropped if the name is unknown with it
 *   DNC_READ     dm_Buf/dm_Len/dm_Timeout -> dm_Result bytes of terminal
 *                output, DNR_TIMEOUT, or DNR_CLOSED with dm_Reason
 *   DNC_WRITE    dm_Buf/dm_Len keys -> 0, or -1 (session gone); waits
 *                while the session's buffer is full
 *   DNC_CLOSE    dm_Link
 *   DNC_REASON   dm_Link -> dm_Reason
 *   LTC_BREAK    dm_Link: send a BREAK
 *   LTC_LIST     dm_Buf/dm_Len -> dm_Result length of a text with one
 *                line per known service
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LATMSG_H
#define LATMSG_H

#define LAT_PORTNAME "LAT"

#define LTC_BREAK 100
#define LTC_LIST  101

struct LatConnect {
	char lc_Service[17];
	char lc_Port[17];           /* our port name, e.g. "LAT4" */
};

#endif
