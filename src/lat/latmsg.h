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
 * Offering a service (host role, for dnmaint):
 *   LTC_OFFER    dm_Buf service name (NULL or "": our node name) -> dm_Id
 *                offer; latd announces the service while the offer stands
 *                (one at a time)
 *   LTC_ACCEPT   dm_Link offer, dm_Buf struct LatIncoming, dm_Timeout ->
 *                dm_Result 1 and dm_Id session (READ/WRITE/CLOSE as
 *                above), which waits for LTC_ANSWER; DNR_TIMEOUT (0) when
 *                none came, -1 when the offer is gone
 *   LTC_ANSWER   dm_Link session, dm_Reason 0 to accept, else a LAT
 *                reason (14 access denied); a rejected session is gone.
 *                Unanswered sessions are rejected after 30 s
 *   LTC_UNOFFER  dm_Link offer
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LATMSG_H
#define LATMSG_H

#define LAT_PORTNAME "LAT"

#define LTC_BREAK   100
#define LTC_LIST    101
#define LTC_OFFER   102
#define LTC_ACCEPT  103
#define LTC_ANSWER  104
#define LTC_UNOFFER 105

struct LatConnect {
	char lc_Service[17];
	char lc_Port[17];           /* our port name, e.g. "LAT4" */
};

/* LTC_ACCEPT: who comes in */
struct LatIncoming {
	char li_Node[17];           /* the terminal server's node name */
	char li_Port[17];           /* its port name */
};

#endif
