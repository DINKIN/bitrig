/*	$OpenBSD: intr.h,v 1.1 2013/09/04 14:38:27 patrick Exp $	*/
/*	$NetBSD: intr.h,v 1.12 2003/06/16 20:00:59 thorpej Exp $	*/

/*
 * Copyright (c) 2001, 2003 Wasabi Systems, Inc.
 * All rights reserved.
 *
 * Written by Jason R. Thorpe for Wasabi Systems, Inc.
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
 *	This product includes software developed for the NetBSD Project by
 *	Wasabi Systems, Inc.
 * 4. The name of Wasabi Systems, Inc. may not be used to endorse
 *    or promote products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY WASABI SYSTEMS, INC. ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL WASABI SYSTEMS, INC
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef	_MACHINE_INTR_H_
#define	_MACHINE_INTR_H_

#include <machine/intrdefs.h>

#ifdef _KERNEL

#ifndef _LOCORE
#include <sys/device.h>
#include <sys/queue.h>
#include <sys/evcount.h>
#include <sys/proc.h>
#include <sys/softintr.h>

int	splraise(int);
int	spllower(int);
void	splx(int);

void	arm_do_pending_intr(int);
void	arm_set_intr_handler(
	void *(*intr_establish)(int irqno, int level, int (*func)(void *),
	    void *cookie, char *name),
	void (*intr_disestablish)(void *cookie),
	const char *(*intr_string)(void *cookie),
	void (*intr_handle)(void *));

struct arm_intr_func {
	void *(*intr_establish)(int irqno, int level, int (*func)(void *),
	    void *cookie, char *name);
	void (*intr_disestablish)(void *cookie);
	const char *(*intr_string)(void *cookie);
};

extern struct arm_intr_func arm_intr_func;

#define	splsoft()	splraise(IPL_SOFT)
#define	splsoftclock()	splraise(IPL_SOFTCLOCK)
#define	splsoftnet()	splraise(IPL_SOFTNET)
#define	splbio()	splraise(IPL_BIO)
#define	splnet()	splraise(IPL_NET)
#define	spltty()	splraise(IPL_TTY)
#define	splvm()		splraise(IPL_VM)
#define	splaudio()	splraise(IPL_AUDIO)
#define	splclock()	splraise(IPL_CLOCK)
#define	splstatclock()	splraise(IPL_STATCLOCK)

#define	spl0()		spllower(IPL_NONE)

void *arm_intr_establish(int irqno, int level, int (*func)(void *),
    void *cookie, char *name);
void arm_intr_disestablish(void *cookie);
const char *arm_intr_string(void *cookie);

/* XXX - this is probably the wrong location for this */
void arm_clock_register(void (*)(void), void (*)(u_int), void (*)(int),
    void (*)(void));

#define splassert(wantipl)	do { /* nothing */ } while (0)
#define splsoftassert(wantipl)	do { /* nothing */ } while (0)

struct intrsource {
	int is_maxlevel;		/* max. IPL for this source */
	int is_minlevel;		/* min. IPL for this source */
	int is_pin;			/* IRQ for legacy; pin for IO APIC */
	void (*is_run)(struct intrsource *);	/* Run callback to this source */
	struct intrhand *is_handlers;	/* handler chain */
	int is_flags;
	int is_type;			/* level, edge */
	int is_scheduled;		/* proc is runnable */
	struct proc *is_proc;		/* ithread proc */
	struct pic *is_pic;		/* XXX PIC for ithread */
	TAILQ_ENTRY(intrsource) entry;	/* entry in ithreads list */
	TAILQ_ENTRY(intrsource) entry_pending;	/* entry in pending list */
	int is_pending;			/* interrupt is pending */
};

struct intrhand {
	int (*ih_fun)(void *);		/* handler */
	void *ih_arg;			/* arg for handler */
	int ih_level;			/* IPL_* */
	int ih_flags;
	struct intrhand *ih_next;
	int ih_pin;			/* IRQ number */
	struct cpu_info *ih_cpu;
	int ih_irq;
	char *ih_name;
	struct evcount ih_count;
};

struct pic {
	struct device pic_dev;
	void (*pic_hwunmask)(struct pic *, int);
	void (*pic_hwmask)(struct pic *, int);
};

#define intrsource_mask(is)					\
	((is)->is_pic->pic_hwmask((is)->is_pic, (is)->is_pin))
#define intrsource_unmask(is)					\
	((is)->is_pic->pic_hwunmask((is)->is_pic, (is)->is_pin))

void intr_unpend(void);
extern struct pic softintr_pic;

#endif /* ! _LOCORE */

#define ARM_IRQ_HANDLER arm_intr

#endif /* _KERNEL */

#endif	/* _MACHINE_INTR_H_ */

