/*	$NetBSD: isadmavar.h,v 1.2 1994/10/27 04:17:09 cgd Exp $	*/

#define ISADMA_START_READ	0x0001	/* read from device */
#define ISADMA_START_WRITE	0x0002	/* write to device */

#define	ISADMA_MAP_WAITOK	0x0001	/* OK for isadma_map to sleep */
#define	ISADMA_MAP_BOUNCE	0x0002	/* use bounce buffer if necessary */
#define	ISADMA_MAP_CONTIG	0x0004	/* must be physically contiguous */
#define	ISADMA_MAP_8BIT		0x0008	/* must not cross 64k boundary */
#define	ISADMA_MAP_16BIT	0x0010	/* must not cross 128k boundary */

struct isadma_seg {		/* a physical contiguous segment */
	vm_offset_t addr;	/* address of this segment */
	vm_size_t length;	/* length of this segment (bytes) */
};

int isadma_map __P((caddr_t, vm_size_t, struct isadma_seg *, int));
void isadma_unmap __P((caddr_t, vm_size_t, int, struct isadma_seg *));
void isadma_copytobuf __P((caddr_t, vm_size_t, int, struct isadma_seg *));
void isadma_copyfrombuf __P((caddr_t, vm_size_t, int, struct isadma_seg *));

void isadma_cascade __P((int));
void isadma_start __P((caddr_t, vm_size_t, int, int));
void isadma_abort __P((int));
void isadma_done __P((int));

/*
 * XXX these are needed until all drivers have been cleaned up
 */
#define isa_dmacascade(c)	isadma_cascade((c))
#define isa_dmastart(f, a, s, c) \
    isadma_start((a), (s), (c), (f)&B_READ?ISADMA_START_READ:ISADMA_START_WRITE)
#define isa_dmaabort(c)		isadma_abort((c))
#define isa_dmadone(a, s, c, f)	isadma_abort((c))
