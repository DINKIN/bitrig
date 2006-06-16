/*	$OpenBSD: power.c,v 1.1 2006/06/16 22:01:30 jason Exp $	*/

/*
 * Copyright (c) 2006 Jason L. Wright (jason@thought.net)
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
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Driver for power-button device on U5, U10, etc.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/device.h>
#include <sys/conf.h>
#include <sys/timeout.h>

#include <machine/bus.h>
#include <machine/autoconf.h>
#include <machine/openfirm.h>

#include <sparc64/dev/ebusreg.h>
#include <sparc64/dev/ebusvar.h>

struct power_softc {
	struct device		sc_dev;
	bus_space_tag_t		sc_tag;
	bus_space_handle_t	sc_handle;
	struct intrhand		*sc_ih;
};

int	power_match(struct device *, void *, void *);
void	power_attach(struct device *, struct device *, void *);
int	power_intr(void *);

struct cfattach power_ca = {
	sizeof(struct power_softc), power_match, power_attach
};

struct cfdriver power_cd = {
	NULL, "power", DV_DULL
};

int
power_match(parent, match, aux)
	struct device *parent;
	void *match;
	void *aux;
{
	struct ebus_attach_args *ea = aux;

	if (strcmp(ea->ea_name, "power") == 0)
		return (1);
	return (0);
}

void
power_attach(parent, self, aux)
	struct device *parent, *self;
	void *aux;
{
	struct power_softc *sc = (void *)self;
	struct ebus_attach_args *ea = aux;

	sc->sc_tag = ea->ea_memtag;

	/* Use prom address if available, otherwise map it. */
	if (ea->ea_nvaddrs) {
		if (bus_space_map(sc->sc_tag, ea->ea_vaddrs[0], 0,
		    BUS_SPACE_MAP_PROMADDRESS, &sc->sc_handle)) {
			printf(": can't map PROM register space\n");
			return;
		}
	} else if (ebus_bus_map(sc->sc_tag, 0,
	    EBUS_PADDR_FROM_REG(&ea->ea_regs[0]), ea->ea_regs[0].size, 0, 0,
	    &sc->sc_handle) != 0) {
		printf(": can't map register space\n");
                return;
	}

	if (ea->ea_nintrs > 0 && OF_getproplen(ea->ea_node, "button") >= 0) {
	        sc->sc_ih = bus_intr_establish(sc->sc_tag, ea->ea_intrs[0],
		    IPL_HIGH, 0, power_intr, sc, self->dv_xname);
		if (sc->sc_ih == NULL) {
			printf(": can't establish interrupt\n");
			return;
		}
	}

	printf("\n");
}

int
power_intr(void *vsc)
{
	return (0);
}
