/*	$NetBSD: if_le_pci.c,v 1.6.4.1 1996/06/03 20:32:13 cgd Exp $	*/

/*-
 * Copyright (c) 1995 Charles M. Hannum.  All rights reserved.
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Ralph Campbell and Rick Macklem.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)if_le.c	8.2 (Berkeley) 11/16/93
 */

#include "bpfilter.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/syslog.h>
#include <sys/socket.h>
#include <sys/device.h>

#include <vm/vm.h>
#include <vm/vm_kern.h>
#include <vm/vm_param.h>

#include <net/if.h>

#ifdef INET
#include <netinet/in.h>
#include <netinet/if_ether.h>
#endif

#include <vm/vm.h>

#include <machine/cpu.h>
#include <machine/bus.h>
#include <machine/intr.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcidevs.h>

#include <dev/ic/am7990reg.h>
#include <dev/ic/am7990var.h>

#include <dev/pci/if_levar.h>

#ifdef __alpha__			/* XXX */
/* XXX XXX NEED REAL DMA MAPPING SUPPORT XXX XXX */ 
#define	vtophys(va)	__alpha_bus_XXX_dmamap(lesc->sc_bc, (void *)(va))
#endif

int le_pci_match __P((struct device *, void *, void *));
void le_pci_attach __P((struct device *, struct device *, void *));

struct cfattach le_pci_ca = {
	sizeof(struct le_softc), le_pci_match, le_pci_attach
};

hide void le_pci_wrcsr __P((struct am7990_softc *, u_int16_t, u_int16_t));
hide u_int16_t le_pci_rdcsr __P((struct am7990_softc *, u_int16_t));

/*
 * PCI constants.
 * XXX These should be in a common file!
 */
#define PCI_CBIO	0x10		/* Configuration Base IO Address */

hide void
le_pci_wrcsr(sc, port, val)
	struct am7990_softc *sc;
	u_int16_t port, val;
{
	struct le_softc *lesc = (struct le_softc *)sc;
	bus_chipset_tag_t bc = lesc->sc_bc;
	bus_io_handle_t ioh = lesc->sc_ioh;

	bus_io_write_2(bc, ioh, lesc->sc_rap, port);
	bus_io_write_2(bc, ioh, lesc->sc_rdp, val);
}

hide u_int16_t
le_pci_rdcsr(sc, port)
	struct am7990_softc *sc;
	u_int16_t port;
{
	struct le_softc *lesc = (struct le_softc *)sc;
	bus_chipset_tag_t bc = lesc->sc_bc;
	bus_io_handle_t ioh = lesc->sc_ioh;
	u_int16_t val;

	bus_io_write_2(bc, ioh, lesc->sc_rap, port);
	val = bus_io_read_2(bc, ioh, lesc->sc_rdp);
	return (val);
}

int
le_pci_match(parent, match, aux)
	struct device *parent;
	void *match, *aux;
{
	struct pci_attach_args *pa = aux;

	if (PCI_VENDOR(pa->pa_id) != PCI_VENDOR_AMD)
		return (0);

	switch (PCI_PRODUCT(pa->pa_id)) {
	case PCI_PRODUCT_AMD_PCNET_PCI:
		return (1);
	}

	return (0);
}

void
le_pci_attach(parent, self, aux)
	struct device *parent, *self;
	void *aux;
{
	struct le_softc *lesc = (void *)self;
	struct am7990_softc *sc = &lesc->sc_am7990;
	struct pci_attach_args *pa = aux;
	pci_intr_handle_t ih;
	bus_io_addr_t iobase;
	bus_io_size_t iosize;
	bus_io_handle_t ioh;
	bus_chipset_tag_t bc = pa->pa_bc;
	pci_chipset_tag_t pc = pa->pa_pc;
	pcireg_t csr;
	int i;
	const char *model, *intrstr;

	switch (PCI_PRODUCT(pa->pa_id)) {
	case PCI_PRODUCT_AMD_PCNET_PCI:
		model = "PCnet-PCI Ethernet";
		lesc->sc_rap = PCNET_PCI_RAP;
		lesc->sc_rdp = PCNET_PCI_RDP;
		break;

	default:
		model = "unknown model!";
	}

	printf(": %s\n", model);

	if (pci_io_find(pc, pa->pa_tag, PCI_CBIO, &iobase, &iosize)) {
		printf("%s: can't find I/O base\n", sc->sc_dev.dv_xname);
		return;
	}
	if (bus_io_map(bc, iobase, iosize, &ioh)) {
		printf("%s: can't map I/O space\n", sc->sc_dev.dv_xname);
		return;
	}

	/*
	 * Extract the physical MAC address from the ROM.
	 */
	for (i = 0; i < sizeof(sc->sc_arpcom.ac_enaddr); i++)
		sc->sc_arpcom.ac_enaddr[i] = bus_io_read_1(bc, ioh, i);

	sc->sc_mem = malloc(16384, M_DEVBUF, M_NOWAIT);
	if (sc->sc_mem == 0) {
		printf("%s: couldn't allocate memory for card\n",
		    sc->sc_dev.dv_xname);
		return;
	}

	lesc->sc_bc = bc;
	lesc->sc_ioh = ioh;

	sc->sc_conf3 = 0;
	sc->sc_addr = vtophys(sc->sc_mem);	/* XXX XXX XXX */
	sc->sc_memsize = 16384;

	sc->sc_copytodesc = am7990_copytobuf_contig;
	sc->sc_copyfromdesc = am7990_copyfrombuf_contig;
	sc->sc_copytobuf = am7990_copytobuf_contig;
	sc->sc_copyfrombuf = am7990_copyfrombuf_contig;
	sc->sc_zerobuf = am7990_zerobuf_contig;

	sc->sc_rdcsr = le_pci_rdcsr;
	sc->sc_wrcsr = le_pci_wrcsr;
	sc->sc_hwinit = NULL;

	printf("%s", sc->sc_dev.dv_xname);
	am7990_config(sc);

	/* Enable the card. */
	csr = pci_conf_read(pc, pa->pa_tag,
	    PCI_COMMAND_STATUS_REG);
	pci_conf_write(pc, pa->pa_tag, PCI_COMMAND_STATUS_REG,
	    csr | PCI_COMMAND_MASTER_ENABLE);

	/* Map and establish the interrupt. */
	if (pci_intr_map(pc, pa->pa_intrtag, pa->pa_intrpin,
	    pa->pa_intrline, &ih)) {
		printf("%s: couldn't map interrupt\n", sc->sc_dev.dv_xname);
		return;
	}
	intrstr = pci_intr_string(pc, ih);
	lesc->sc_ih = pci_intr_establish(pc, ih, IPL_NET, am7990_intr, sc,
	    sc->sc_dev.dv_xname);
	if (lesc->sc_ih == NULL) {
		printf("%s: couldn't establish interrupt",
		    sc->sc_dev.dv_xname);
		if (intrstr != NULL)
			printf(" at %s", intrstr);
		printf("\n");
		return;
	}
	printf("%s: interrupting at %s\n", sc->sc_dev.dv_xname, intrstr);
}
