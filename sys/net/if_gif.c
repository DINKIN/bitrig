/*	$OpenBSD: if_gif.c,v 1.5 2000/01/12 06:40:45 angelos Exp $	*/

/*
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * gif.c
 */


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/syslog.h>
#include <machine/cpu.h>

#include <net/if.h>
#include <net/if_types.h>
#include <net/netisr.h>
#include <net/route.h>
#include <net/bpf.h>

#ifdef	INET
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>
#include <netinet/in_gif.h>
#endif	/* INET */

#ifdef INET6
#ifndef INET
#include <netinet/in.h>
#endif
#include <netinet6/in6_var.h>
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>
#include <netinet6/in6_gif.h>
#endif /* INET6 */

#include <net/if_gif.h>

#include "gif.h"
#include "bpfilter.h"

#include <net/net_osdep.h>

#if NGIF > 0

void gifattach __P((int));

/*
 * gif global variable definitions
 */
int ngif = NGIF;		/* number of interfaces */
struct gif_softc *gif = 0;

void
gifattach(dummy)
	int dummy;
{
	register struct gif_softc *sc;
	register int i;

	gif = sc = malloc (ngif * sizeof(struct gif_softc), M_DEVBUF, M_WAIT);
	bzero(sc, ngif * sizeof(struct gif_softc));
	for (i = 0; i < ngif; sc++, i++) {
		sprintf(sc->gif_if.if_xname, "gif%d", i);
		sc->gif_if.if_mtu    = GIF_MTU;
		sc->gif_if.if_flags  = IFF_POINTOPOINT | IFF_MULTICAST;
		sc->gif_if.if_ioctl  = gif_ioctl;
		sc->gif_if.if_output = gif_output;
		sc->gif_if.if_type   = IFT_GIF;
		if_attach(&sc->gif_if);

#if NBPFILTER > 0
		bpfattach(&sc->gif_if.if_bpf, &sc->gif_if, DLT_NULL,
			  sizeof(u_int));
#endif
	}
}

int
gif_output(ifp, m, dst, rt)
	struct ifnet *ifp;
	struct mbuf *m;
	struct sockaddr *dst;
	struct rtentry *rt;	/* added in net2 */
{
	register struct gif_softc *sc = (struct gif_softc*)ifp;
	int error = 0;
	static int called = 0;	/* XXX: MUTEX */
	int calllimit = 3;	/* XXX: adhoc */

	/*
	 * gif may cause infinite recursion calls when misconfigured.
	 * We'll prevent this by introducing upper limit.
	 * XXX: this mechanism may introduce another problem about
	 *      mutual exclusion of the variable CALLED, especially if we
	 *      use kernel thread.
	 */
	if (++called >= calllimit) {
		log(LOG_NOTICE,
		    "gif_output: recursively called too many times(%d)\n",
		    called);
		m_freem(m);
		error = EIO;	/* is there better errno? */
		goto end;
	}

	ifp->if_lastchange = time;	

	m->m_flags &= ~(M_BCAST|M_MCAST);
	if (!(ifp->if_flags & IFF_UP) ||
	    sc->gif_psrc == NULL || sc->gif_pdst == NULL) {
		m_freem(m);
		error = ENETDOWN;
		goto end;
	}

#if NBPFILTER > 0
	if (ifp->if_bpf) {
		/*
		 * We need to prepend the address family as
		 * a four byte field.  Cons up a dummy header
		 * to pacify bpf.  This is safe because bpf
		 * will only read from the mbuf (i.e., it won't
		 * try to free it or keep a pointer a to it).
		 */
		struct mbuf m0;
		u_int af = dst->sa_family;

		m0.m_next = m;
		m0.m_len = 4;
		m0.m_data = (char *)&af;
		
		bpf_mtap(ifp->if_bpf, &m0);
	}
#endif
	ifp->if_opackets++;	
	ifp->if_obytes += m->m_pkthdr.len;

	switch (sc->gif_psrc->sa_family) {
#ifdef INET
	case AF_INET:
		error = in_gif_output(ifp, dst->sa_family, m, rt);
		break;
#endif
#ifdef INET6
	case AF_INET6:
		error = in6_gif_output(ifp, dst->sa_family, m, rt);
		break;
#endif
	default:
		m_freem(m);		
		error = ENETDOWN;
	}

  end:
	called = 0;		/* reset recursion counter */
	if (error) ifp->if_oerrors++;
	return error;
}

int
gif_ioctl(ifp, cmd, data)
	struct ifnet *ifp;
	u_long cmd;
	caddr_t data;
{
	struct gif_softc *sc  = (struct gif_softc*)ifp;
	struct ifreq     *ifr = (struct ifreq*)data;
	int error = 0, size;
	struct sockaddr *sa, *dst, *src;
		
	switch (cmd) {
	case SIOCSIFADDR:
		break;
		
	case SIOCSIFDSTADDR:
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		switch (ifr->ifr_addr.sa_family) {
#ifdef INET
		case AF_INET:	/* IP supports Multicast */
			break;
#endif /* INET */
#ifdef INET6
		case AF_INET6:	/* IP6 supports Multicast */
			break;
#endif /* INET6 */
		default:  /* Other protocols doesn't support Multicast */
			error = EAFNOSUPPORT;
			break;
		}
		break;

	case SIOCSIFPHYADDR:
#ifdef INET6
	case SIOCSIFPHYADDR_IN6:
#endif /* INET6 */
		switch (ifr->ifr_addr.sa_family) {
#ifdef INET
		case AF_INET:
			src = (struct sockaddr *)
				&(((struct in_aliasreq *)data)->ifra_addr);
			dst = (struct sockaddr *)
				&(((struct in_aliasreq *)data)->ifra_dstaddr);

			/* only one gif can have dst = INADDR_ANY */
#define satosaddr(sa) (((struct sockaddr_in *)(sa))->sin_addr.s_addr)

			if (satosaddr(dst) == INADDR_ANY) {
				int i;
				struct gif_softc *sc2;

			  	for (i = 0, sc2 = gif; i < ngif; i++, sc2++) {
					if (sc2 == sc) continue;
					if (sc2->gif_pdst &&
					    satosaddr(sc2->gif_pdst)
						== INADDR_ANY) {
					    error = EADDRNOTAVAIL;
					    goto bad;
					}
				}
			}
			size = sizeof(struct sockaddr_in);
			break;
#endif /* INET */
#ifdef INET6
		case AF_INET6:
			src = (struct sockaddr *)
				&(((struct in6_aliasreq *)data)->ifra_addr);
			dst = (struct sockaddr *)
				&(((struct in6_aliasreq *)data)->ifra_dstaddr);

			/* only one gif can have dst = in6addr_any */
#define satoin6(sa) (&((struct sockaddr_in6 *)(sa))->sin6_addr)

			if (IN6_IS_ADDR_UNSPECIFIED(satoin6(dst))) {
				int i;
				struct gif_softc *sc2;

			  	for (i = 0, sc2 = gif; i < ngif; i++, sc2++) {
					if (sc2 == sc) continue;
					if (sc2->gif_pdst &&
					    IN6_IS_ADDR_UNSPECIFIED(
						satoin6(sc2->gif_pdst)
								    )) {
					    error = EADDRNOTAVAIL;
					    goto bad;
					}
				}
			}
			size = sizeof(struct sockaddr_in6);
			break;
#endif /* INET6 */
		default:
			error = EPROTOTYPE;
			goto bad;
			break;
		}
		if (sc->gif_psrc != NULL)
			free((caddr_t)sc->gif_psrc, M_IFADDR);
		if (sc->gif_pdst != NULL)
			free((caddr_t)sc->gif_pdst, M_IFADDR);

		sa = (struct sockaddr *)malloc(size, M_IFADDR, M_WAITOK);
		bzero((caddr_t)sa, size);
		bcopy((caddr_t)src, (caddr_t)sa, size);
		sc->gif_psrc = sa;
		
		sa = (struct sockaddr *)malloc(size, M_IFADDR, M_WAITOK);
		bzero((caddr_t)sa, size);
		bcopy((caddr_t)dst, (caddr_t)sa, size);
		sc->gif_pdst = sa;
		
		ifp->if_flags |= (IFF_UP|IFF_RUNNING);
		if_up(ifp);		/* send up RTM_IFINFO */

		break;
			
	case SIOCGIFPSRCADDR:
#ifdef INET6
	case SIOCGIFPSRCADDR_IN6:
#endif /* INET6 */
		if (sc->gif_psrc == NULL) {
			error = EADDRNOTAVAIL;
			goto bad;
		}
		src = sc->gif_psrc;
		switch (sc->gif_psrc->sa_family) {
#ifdef INET
		case AF_INET:
			dst = &ifr->ifr_addr;
			size = sizeof(struct sockaddr_in);
			break;
#endif /* INET */
#ifdef INET6
		case AF_INET6:
			dst = (struct sockaddr *)
				&(((struct in6_ifreq *)data)->ifr_addr);
			size = sizeof(struct sockaddr_in6);
			break;
#endif /* INET6 */
		default:
			error = EADDRNOTAVAIL;
			goto bad;
		}
		bcopy((caddr_t)src, (caddr_t)dst, size);
		break;
			
	case SIOCGIFPDSTADDR:
#ifdef INET6
	case SIOCGIFPDSTADDR_IN6:
#endif /* INET6 */
		if (sc->gif_pdst == NULL) {
			error = EADDRNOTAVAIL;
			goto bad;
		}
		src = sc->gif_pdst;
		switch (sc->gif_pdst->sa_family) {
#ifdef INET
		case AF_INET:
			dst = &ifr->ifr_addr;
			size = sizeof(struct sockaddr_in);
			break;
#endif /* INET */
#ifdef INET6
		case AF_INET6:
			dst = (struct sockaddr *)
				&(((struct in6_ifreq *)data)->ifr_addr);
			size = sizeof(struct sockaddr_in6);
			break;
#endif /* INET6 */
		default:
			error = EADDRNOTAVAIL;
			goto bad;
		}
		bcopy((caddr_t)src, (caddr_t)dst, size);
		break;

	case SIOCSIFFLAGS:
		break;

	default:
		error = EINVAL;
		break;
	}
 bad:
	return error;
}
#endif /*NGIF > 0*/
