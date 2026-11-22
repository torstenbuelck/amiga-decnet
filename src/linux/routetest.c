/*
 * routetest.c - test of the designated-router logic in the routing core:
 * a router set by hand (dn_set_router) stands in only while no router
 * hello is heard. No network needed: hellos are fed to dn_input.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/dn.h"
#include "../core/dn_priv.h"
static dn_u8 lastdst[6];
static void sf(void *c, const dn_u8 *d, const dn_u8 *p, unsigned n){(void)c;(void)p;(void)n;memcpy(lastdst,d,6);}
static void *al(void *c, unsigned n){(void)c;return malloc(n);}
static void fr(void *c, void *p){(void)c;free(p);}
static void lg(void *c,int l,const char *m){(void)c;(void)l;printf("   log: %s\n",m);}
static void hello(dn_u16 id, dn_u8 prio){
	dn_u8 f[40]; memset(f,0,sizeof f); unsigned n=2;
	f[n++]=0x0b; f[n++]=2;f[n++]=0;f[n++]=0;
	dn_node_mac(f+n,id); n+=6; f[n++]=2; f[n++]=0x00;f[n++]=0x05; f[n++]=prio; f[n++]=0; f[n++]=15; f[n++]=0;
	n+=10; f[0]=(dn_u8)(n-2); f[1]=0;
	dn_u8 src[6],dst[6]={0xab,0,0,4,0,0}; dn_node_mac(src,id);
	dn_input(src,dst,f,n);
}
static int fails;
static void expect(const char *what, dn_u16 r, int st){
	int ok = dn_designated_router()==r && dn_router_is_static()==st;
	printf("%s %-40s router %s static %d\n", ok?"PASS":"FAIL", what, dn_ntoa(dn_designated_router()), dn_router_is_static());
	if(!ok) fails++;
}
#define A(a,n) ((dn_u16)(((a)<<10)|(n)))
int main(void){
	struct dn_env e; memset(&e,0,sizeof e); e.send_frame=sf; e.alloc=al; e.free=fr; e.log=lg; e.loglevel=DN_LOG_INFO;
	dn_init(&e,A(1,10)); dn_tick(1);
	expect("no router, nothing heard", 0, 0);
	dn_set_router(A(1,1)); expect("static set", A(1,1), 1);
	hello(A(1,2),64); expect("hello from other router wins", A(1,2), 0);
	hello(A(1,3),32); expect("lower prio hello ignored", A(1,2), 0);
	for(int i=0;i<44;i++) dn_tick(1000);
	expect("44 s later still heard router", A(1,2), 0);
	for(int i=0;i<3;i++) dn_tick(1000);
	expect("after 3x15 s: back to static", A(1,1), 1);
	hello(A(1,1),64); expect("hello from the static router itself", A(1,1), 0);
	for(int i=0;i<50;i++) dn_tick(1000);
	expect("lost again: static again", A(1,1), 1);
	dn_set_router(0); expect("static cleared", 0, 0);
	hello(A(1,2),64); dn_set_router(A(1,5)); expect("set while router heard: heard stays", A(1,2), 0);
	/* data to unknown node goes via the router MAC */
	{ dn_u8 m[1]={0}; dn_u8 want[6]; dn_route_send(A(1,99),m,1,0); dn_node_mac(want,A(1,2));
	  int ok=!memcmp(lastdst,want,6); printf("%s data to unknown node goes to router\n",ok?"PASS":"FAIL"); fails+=!ok; }
	printf(fails?"%d FAILED\n":"all passed\n",fails); return fails!=0;
}
