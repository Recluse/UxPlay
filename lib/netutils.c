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
 *
 *==================================================================
 * modified by fduncanh 2022
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "compat.h"

/* -bind: one local address per family for every listening socket.  INADDR_ANY /
 * all-zero (== in6addr_any) = the historical behaviour, and is what every path
 * sees unless -bind was given and validated.
 *
 * The v6 half is not optional decoration: an iPhone 14 was measured connecting
 * to this receiver over IPv6 LINK-LOCAL ("Accepted IPv6 client ... Remote:
 * fe80::0c9c:1882:ab4b:403c%15"), so a pin that leaves httpd without a v6
 * listener locks that phone out entirely.  bind_addr6 holds the chosen
 * adapter's link-local and bind_scope_id its scope -- a link-local is
 * meaningless without one, since the same address may exist on several
 * adapters.  Both stay zero when the adapter has no usable link-local, which
 * degrades the v6 listener to in6addr_any rather than losing it. */
static unsigned int bind_addr = INADDR_ANY;
static char bind_host[INET_ADDRSTRLEN + 1] = "localhost";
static struct in6_addr bind_addr6;       /* all-zero == in6addr_any */
static unsigned int bind_scope_id = 0;

int
netutils_parse_ipv4(const char *ip, unsigned int *addr)
{
    unsigned int b[4] = { 0, 0, 0, 0 };
    int octet = 0, digits = 0;
    const char *p;

    /* Deliberately not inet_pton(): on Windows that is a Winsock call and
     * documents WSANOTINITIALISED, but WSAStartup does not run until
     * netutils_init() from raop_init() -- long after -bind has to be resolved,
     * so inet_pton() there would fail every valid address and silently kill the
     * feature on the one platform it was requested for.  A dotted quad needs no
     * socket library.  Strict on purpose: this is the trust boundary for a
     * hand-edited config.json, so leading space, a sign, "1.2.3", "1.2.3.4.5"
     * and octets above 255 are all rejected rather than coerced. */
    if (!ip || !addr) {
        return -1;
    }
    for (p = ip; *p; p++) {
        if (*p == '.') {
            if (!digits || octet == 3) return -1;
            octet++;
            digits = 0;
        } else if (*p >= '0' && *p <= '9') {
            if (++digits > 3) return -1;
            b[octet] = b[octet] * 10 + (unsigned int) (*p - '0');
            if (b[octet] > 255) return -1;
        } else {
            return -1;
        }
    }
    if (octet != 3 || !digits) {
        return -1;
    }
    *addr = htonl((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
    return 0;
}

int
netutils_set_bind_address(const char *ip)
{
    unsigned int a;
    if (!ip || !*ip) {
        bind_addr = INADDR_ANY;
        strcpy(bind_host, "localhost");
        /* clearing the pin clears both halves: the caller's unconditional
         * "no -bind" reset must not leave the previous run's v6 pin behind */
        memset(&bind_addr6, 0, sizeof(bind_addr6));
        bind_scope_id = 0;
        return 0;
    }
    if (netutils_parse_ipv4(ip, &a) < 0) {
        return -1;
    }
    bind_addr = a;
    /* the parse just proved ip is at most 15 characters */
    strncpy(bind_host, ip, sizeof(bind_host) - 1);
    bind_host[sizeof(bind_host) - 1] = '\0';
    /* A new IPv4 pin invalidates whatever v6 address went with the old one, so
     * drop it here rather than trusting every caller to pass a matching pair. */
    memset(&bind_addr6, 0, sizeof(bind_addr6));
    bind_scope_id = 0;
    return 0;
}

void
netutils_set_bind_address6(const unsigned char *addr6, unsigned int scope_id)
{
    /* Raw 16 bytes rather than a struct in6_addr so netutils.h stays free of
     * socket headers -- uxplay.cpp reaches this before WSAStartup has run. */
    if (!addr6 || !scope_id) {
        memset(&bind_addr6, 0, sizeof(bind_addr6));
        bind_scope_id = 0;
        return;
    }
    memcpy(&bind_addr6, addr6, sizeof(bind_addr6));
    bind_scope_id = scope_id;
}

const char *
netutils_get_bind_host(void)
{
    return bind_host;
}

int
netutils_init()
{
#ifdef WIN32
    WORD wVersionRequested;
	WSADATA wsaData;
	int ret;

	wVersionRequested = MAKEWORD(2, 2);
	ret = WSAStartup(wVersionRequested, &wsaData);
	if (ret) {
		return -1;
	}

	if (LOBYTE(wsaData.wVersion) != 2 ||
	    HIBYTE(wsaData.wVersion) != 2) {
		/* Version mismatch, requested version not found */
		return -1;
	}
#endif
    return 0;
}

void
netutils_cleanup()
{
#ifdef WIN32
    WSACleanup();
#endif
}

unsigned char *
netutils_get_address(void *sockaddr, int *length, unsigned int *zone_id, unsigned short *port)
{
    unsigned char ipv4_prefix[] = { 0,0,0,0,0,0,0,0,0,0,255,255 };
    struct sockaddr *address = sockaddr;

    assert(address);
    assert(length);
    assert(zone_id);
    if (address->sa_family == AF_INET) {
        struct sockaddr_in *sin;
        *zone_id = 0;
        sin = (struct sockaddr_in *)address;
        *length = sizeof(sin->sin_addr.s_addr);
        if (port) {
            *port = ntohs(sin->sin_port);
        }
        return (unsigned char *)&sin->sin_addr.s_addr;
    } else if (address->sa_family == AF_INET6) {
        struct sockaddr_in6 *sin6;

        sin6 = (struct sockaddr_in6 *)address;
        if (port) {
            *port = ntohs(sin6->sin6_port);
        }
        if (!memcmp(sin6->sin6_addr.s6_addr, ipv4_prefix, 12)) {
            /* Actually an embedded IPv4 address */
            *zone_id = 0;
            *length = sizeof(sin6->sin6_addr.s6_addr)-12;
            return (sin6->sin6_addr.s6_addr+12);
        }
        *zone_id = (unsigned int) sin6->sin6_scope_id;
        *length = sizeof(sin6->sin6_addr.s6_addr);
        return sin6->sin6_addr.s6_addr;
    }

    *length = 0;
    return NULL;
}

int
netutils_init_socket(unsigned short *port, int use_ipv6, int use_udp)
{
    int family = use_ipv6 ? AF_INET6 : AF_INET;
    int type = use_udp ? SOCK_DGRAM : SOCK_STREAM;
    int proto = use_udp ? IPPROTO_UDP : IPPROTO_TCP;

    struct sockaddr_storage saddr;
    socklen_t socklen = 0;
#ifndef _WIN32
    int reuseaddr = 1;
#else
    const char reuseaddr = 1;
#endif
    
    assert(port);

    int server_fd = socket(family, type, proto);
    if (server_fd == -1) {
        goto cleanup;
    }

    int ret = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof (reuseaddr));
    if (ret == -1) {
        goto cleanup;
    }

    memset(&saddr, 0, sizeof(saddr));
    if (use_ipv6) {
        struct sockaddr_in6 *sin6ptr = (struct sockaddr_in6 *)&saddr;

        /* Initialize sockaddr for bind */
        sin6ptr->sin6_family = family;
        /* -bind covers v6 too, because iOS actually arrives over IPv6
         * link-local (measured: an iPhone 14 connecting as
         * fe80::0c9c:1882:ab4b:403c%15).  Refusing this socket while pinned --
         * which is what this used to do -- left httpd with no v6 listener and
         * that phone unable to connect at all.  Unpinned, or pinned to an
         * adapter with no link-local, both fields are zero and this is byte for
         * byte the old in6addr_any bind. */
        sin6ptr->sin6_addr = bind_addr6;
        sin6ptr->sin6_scope_id = bind_scope_id;
        sin6ptr->sin6_port = htons(*port);

#ifndef _WIN32
        int v6only = 1;
        /* Make sure we only listen to IPv6 addresses */
        setsockopt(server_fd, IPPROTO_IPV6, IPV6_V6ONLY,
                   (char *) &v6only, sizeof(v6only));
#endif

        socklen = sizeof(*sin6ptr);
        ret = bind(server_fd, (struct sockaddr *)sin6ptr, socklen);
        if (ret == -1) {
            goto cleanup;
        }

        ret = getsockname(server_fd, (struct sockaddr *)sin6ptr, &socklen);
        if (ret == -1) {
            goto cleanup;
        }
        *port = ntohs(sin6ptr->sin6_port);
    } else {
        struct sockaddr_in *sinptr = (struct sockaddr_in *)&saddr;

        /* Initialize sockaddr for bind */
        sinptr->sin_family = family;
        sinptr->sin_addr.s_addr = bind_addr;
        sinptr->sin_port = htons(*port);

        socklen = sizeof(*sinptr);
        ret = bind(server_fd, (struct sockaddr *)sinptr, socklen);
        if (ret == -1) {
            goto cleanup;
        }

        ret = getsockname(server_fd, (struct sockaddr *)sinptr, &socklen);
        if (ret == -1) {
            goto cleanup;
        }
        *port = ntohs(sinptr->sin_port);
    }
    return server_fd;

    cleanup:
    ret = SOCKET_GET_ERROR();
    if (server_fd != -1) {
        CLOSESOCKET(server_fd);
    }
    SOCKET_SET_ERROR(ret);
    return -1;
}

// Src is the ip address
int
netutils_parse_address(int family, const char *src, void *dst, int dstlen)
{
    struct addrinfo *result;
    struct addrinfo *ptr;
    struct addrinfo hints;

    if (family != AF_INET && family != AF_INET6) {
        return -1;
    }
    if (!src || !dst) {
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;

    int ret = getaddrinfo(src, NULL, &hints, &result);
    if (ret != 0) {
        return -1;
    }

    int length = -1;
    for (ptr = result; ptr != NULL; ptr = ptr->ai_next) {
        if (family == ptr->ai_family && (unsigned int)dstlen >= ptr->ai_addrlen) {
            memcpy(dst, ptr->ai_addr, ptr->ai_addrlen);
            length = ptr->ai_addrlen;
            break;
        }
    }
    freeaddrinfo(result);
    return length;
}
