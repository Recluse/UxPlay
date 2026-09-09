/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

#ifndef NETUTILS_H
#define NETUTILS_H

#ifdef __cplusplus
extern "C" {
#endif

int netutils_init();
void netutils_cleanup();

/* Dotted quad -> network-order IPv4, or -1 if ip is anything else.  Not
 * inet_pton(): that needs WSAStartup on Windows, which has not run yet when
 * -bind is resolved.  Shared so the -bind resolver and the setter below cannot
 * disagree about what counts as a valid address. */
int netutils_parse_ipv4(const char *ip, unsigned int *addr);

/* -bind: pin every listening socket to one local IPv4 address.  Returns -1 if
 * ip is not a dotted quad; NULL or "" restores INADDR_ANY (and clears the IPv6
 * half below).  Process state, not per-socket state: netutils_init_socket()
 * takes no context object to hang it off, and the engine is single-instance
 * (see airplay_core.h).  Must be called on EVERY engine start, including with
 * NULL, because these statics outlive a restart -- the engine image is never
 * unloaded. */
int netutils_set_bind_address(const char *ip);

/* The IPv6 half of the same pin: addr6 is 16 raw bytes of the chosen adapter's
 * link-local address and scope_id its interface scope (a link-local without one
 * is ambiguous).  NULL or scope_id 0 means "this adapter has no usable
 * link-local" and leaves the v6 listener on in6addr_any -- deliberately NOT a
 * failure: iOS was measured connecting over IPv6 link-local, so a receiver with
 * no v6 listener is a dead receiver, and a half-pinned one that works beats a
 * fully pinned one that does not.  Raw bytes, not struct in6_addr, to keep this
 * header free of socket headers: on Windows the caller runs before WSAStartup.
 * Call after netutils_set_bind_address(), which resets it. */
void netutils_set_bind_address6(const unsigned char *addr6, unsigned int scope_id);

/* "localhost", or the pinned address once -bind is in effect.  The HLS pipeline
 * connects back into our own httpd, which no longer listens on loopback. */
const char *netutils_get_bind_host(void);

int netutils_init_socket(unsigned short *port, int use_ipv6, int use_udp);
unsigned char *netutils_get_address(void *sockaddr, int *length, unsigned int *zone_id, unsigned short *port);
int netutils_parse_address(int family, const char *src, void *dst, int dstlen);

#ifdef __cplusplus
}
#endif

#endif
