/*	$OpenBSD: mbr.c,v 1.44 2015/03/14 15:21:53 krw Exp $	*/

/*
 * Copyright (c) 1997 Tobias Weingartner
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>	/* DEV_BSIZE */
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/disklabel.h>
#include <sys/dkio.h>
#include <err.h>
#include <errno.h>
#include <util.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <memory.h>

#include "disk.h"
#include "part.h"
#include "misc.h"
#include "mbr.h"
#include "part.h"

void
MBR_init_GPT(struct disk *disk, struct mbr *mbr)
{
	/* Initialize a protective MBR for GPT. */
	bzero(&mbr->part, sizeof(mbr->part));

	/* Use whole disk, starting after MBR. */
	mbr->part[0].id = DOSPTYP_EFI;
	mbr->part[0].bs = 1;
	mbr->part[0].ns = disk->size - 1;

	/* Fix up start/length fields. */
	PRT_fix_CHS(disk, &mbr->part[0]);
}

void
MBR_init(struct disk *disk, struct mbr *mbr)
{
	extern int g_flag;

	if (g_flag) {
		MBR_init_GPT(disk, mbr);
		return;
	}

	/* Fix up given mbr for this disk */
	mbr->part[0].flag = 0;
	mbr->part[1].flag = 0;
	mbr->part[2].flag = 0;
	mbr->part[3].flag = DOSACTIVE;
	mbr->part[3].id = DOSPTYP_OPENBSD;
	mbr->signature = DOSMBR_SIGNATURE;

#if defined(__powerpc__) || defined(__mips__)
	/* Now fix up for the MS-DOS boot partition on PowerPC. */
	mbr->part[0].flag = DOSACTIVE;	/* Boot from dos part */
	mbr->part[3].flag = 0;
#endif

	MBR_fillremaining(mbr, disk, 3);
}

void
MBR_parse(struct disk *disk, struct dos_mbr *dos_mbr, off_t offset,
    off_t reloff, struct mbr *mbr)
{
	struct dos_partition dos_parts[NDOSPART];
	int i;

	memcpy(mbr->code, dos_mbr->dmbr_boot, sizeof(mbr->code));
	mbr->offset = offset;
	mbr->reloffset = reloff;
	mbr->signature = letoh16(dos_mbr->dmbr_sign);

	memcpy(dos_parts, dos_mbr->dmbr_parts, sizeof(dos_parts));

	for (i = 0; i < NDOSPART; i++)
		PRT_parse(disk, &dos_parts[i], offset, reloff, &mbr->part[i]);
}

void
MBR_make(struct mbr *mbr, struct dos_mbr *dos_mbr)
{
	struct dos_partition dos_partition;
	int i;

	memcpy(dos_mbr->dmbr_boot, mbr->code, sizeof(dos_mbr->dmbr_boot));
	dos_mbr->dmbr_sign = htole16(DOSMBR_SIGNATURE);

	for (i = 0; i < NDOSPART; i++) {
		PRT_make(&mbr->part[i], mbr->offset, mbr->reloffset,
		    &dos_partition);
		memcpy(&dos_mbr->dmbr_parts[i], &dos_partition,
		    sizeof(dos_mbr->dmbr_parts[i]));
	}
}

void
MBR_print(struct mbr *mbr, char *units)
{
	int i;

	/* Header */
	printf("Signature: 0x%X\n", (int)mbr->signature);
	PRT_print(0, NULL, units);

	/* Entries */
	for (i = 0; i < NDOSPART; i++)
		PRT_print(i, &mbr->part[i], units);
}

int
MBR_read(int fd, off_t where, struct dos_mbr *dos_mbr)
{
	char *secbuf;

	secbuf = MBR_readsector(fd, where);
	if (secbuf == NULL)
		return (-1);

	memcpy(dos_mbr, secbuf, sizeof(*dos_mbr));
	free(secbuf);

	return (0);
}

int
MBR_write(int fd, off_t where, struct dos_mbr *dos_mbr)
{
	char *secbuf;

	secbuf = MBR_readsector(fd, where);
	if (secbuf == NULL)
		return (-1);

	/*
	 * Place the new MBR at the start of the sector and
	 * write the sector back to "disk".
	 */
	memcpy(secbuf, dos_mbr, sizeof(*dos_mbr));
	MBR_writesector(fd, secbuf, where);
	ioctl(fd, DIOCRLDINFO, 0);

	free(secbuf);

	return (0);
}

/*
 * Parse the MBR partition table into 'mbr', leaving the rest of 'mbr'
 * untouched.
 */
void
MBR_pcopy(struct disk *disk, struct mbr *mbr)
{
	int i, fd, error;
	struct dos_mbr dos_mbr;
	struct dos_partition dos_parts[NDOSPART];

	fd = DISK_open(disk->name, O_RDONLY);
	error = MBR_read(fd, 0, &dos_mbr);
	close(fd);

	if (error == -1)
		return;

	memcpy(dos_parts, dos_mbr.dmbr_parts, sizeof(dos_parts));

	for (i = 0; i < NDOSPART; i++)
		PRT_parse(disk, &dos_parts[i], 0, 0, &mbr->part[i]);
}

/*
 * Read the sector at 'where' into a sector sized buf and return the latter.
 */
char *
MBR_readsector(int fd, off_t where)
{
	char *secbuf;
	const int secsize = unit_types[SECTORS].conversion;
	ssize_t len;
	off_t off;

	where *= secsize;
	off = lseek(fd, where, SEEK_SET);
	if (off != where)
		return (NULL);

	secbuf = calloc(1, secsize);
	if (secbuf == NULL)
		return (NULL);

	len = read(fd, secbuf, secsize);
	if (len == -1 || len != secsize) {
		free(secbuf);
		return (NULL);
	}

	return (secbuf);
}

/*
 * Write the sector sized 'secbuf' to the sector at 'where'.
 */
int
MBR_writesector(int fd, char *secbuf, off_t where)
{
	const int secsize = unit_types[SECTORS].conversion;
	ssize_t len;
	off_t off;

	len = -1;

	where *= secsize;
	off = lseek(fd, where, SEEK_SET);
	if (off == where)
		len = write(fd, secbuf, secsize);

	if (len == -1 || len != secsize) {
		/* short read or write */
		errno = EIO;
		return (-1);
	}

	return (0);
}

/*
 * If *dos_mbr has a 0xee or 0xef partition, nothing needs to happen. If no
 * such partition is present but the first or last sector on the disk has a
 * GPT, zero the GPT to ensure the MBR takes priority and fewer BIOSes get
 * confused.
 */
void
MBR_zapgpt(int fd, struct dos_mbr *dos_mbr, uint64_t lastsec)
{
	const int secsize = unit_types[SECTORS].conversion;
	struct dos_partition dos_parts[NDOSPART];
	char *secbuf;
	uint64_t sig;
	int i;

	memcpy(dos_parts, dos_mbr->dmbr_parts, sizeof(dos_parts));

	for (i = 0; i < NDOSPART; i++)
		if ((dos_parts[i].dp_typ == DOSPTYP_EFI) ||
		    (dos_parts[i].dp_typ == DOSPTYP_EFISYS))
			return;

	secbuf = MBR_readsector(fd, GPTSECTOR);
	if (secbuf == NULL)
		return;

	memcpy(&sig, secbuf, sizeof(sig));
	if (sig == GPTSIGNATURE) {
		memset(secbuf, 0, sizeof(sig));
		MBR_writesector(fd, secbuf, GPTSECTOR);
	}
	free(secbuf);

	secbuf = MBR_readsector(fd, lastsec);
	if (secbuf == NULL)
		return;

	memcpy(&sig, secbuf, sizeof(sig));
	if (sig == GPTSIGNATURE) {
		memset(secbuf, 0, sizeof(sig));
		MBR_writesector(fd, secbuf, lastsec);
	}
	free(secbuf);
}

int
MBR_verify(struct mbr *mbr)
{
	int i, j, n;
	struct prt *p1, *p2;

	for (i = 0, n = 0; i < NDOSPART; i++) {
		p1 = &mbr->part[i];
		if (p1->id == DOSPTYP_UNUSED)
			continue;

		if (p1->id == DOSPTYP_OPENBSD)
			n++;

		for (j = i + 1; j < NDOSPART; j++) {
			p2 = &mbr->part[j];
			if (p2->id != DOSPTYP_UNUSED && PRT_overlap(p1, p2)) {
				warnx("Partitions %d and %d are overlapping!", i ,j);
				if (!ask_yn("Write MBR anyway?"))
					return (-1);
			}
		}

		if (!p1->ns) {
			warnx("Partition %d has size zero!", i);
			if (!ask_yn("Write MBR anyway?"))
				return (-1);
		}
	}
	if (n >= 2) {
		warnx("MBR contains more than one OpenBSD partition!");
		if (!ask_yn("Write MBR anyway?"))
			return (-1);
	}

	return (0);
}

void
MBR_fillremaining(struct mbr *mbr, struct disk *disk, int pn)
{
	struct prt *part, *p;
	uint64_t adj;
	daddr_t i;

	part = &mbr->part[pn];

	/* Use whole disk. Reserve first track, or first cyl, if possible. */
	if (disk->heads > 1)
		part->shead = 1;
	else
		part->shead = 0;
	if (disk->heads < 2 && disk->cylinders > 1)
		part->scyl = 1;
	else
		part->scyl = 0;
	part->ssect = 1;

	/* Go right to the end */
	part->ecyl = disk->cylinders - 1;
	part->ehead = disk->heads - 1;
	part->esect = disk->sectors;

	/* Fix up start/length fields */
	PRT_fix_BN(disk, part, pn);

#if defined(__powerpc__) || defined(__mips__)
	if ((part->shead != 1) || (part->ssect != 1)) {
		/* align the partition on a cylinder boundary */
		part->shead = 0;
		part->ssect = 1;
		part->scyl += 1;
	}
	/* Fix up start/length fields */
	PRT_fix_BN(disk, part, pn);
#endif

	/* Start OpenBSD MBR partition on a power of 2 block number. */
	i = 1;
	while (i < DL_SECTOBLK(&dl, part->bs))
		i *= 2;
	adj = DL_BLKTOSEC(&dl, i) - part->bs;
	part->bs += adj;
	part->ns -= adj;
	PRT_fix_CHS(disk, part);

	/* Shrink to remaining free space */
	for (i = 0; i < NDOSPART; i++) {
		p = &mbr->part[i];
		if (i != pn && PRT_overlap(part, p)) {
			if (p->bs > part->bs) {
				part->ns = p->bs - part->bs;
			} else {
				part->ns += part->bs;
				part->bs = p->bs + p->ns;
				part->ns -= part->bs;
			}
		}
	}
	PRT_fix_CHS(disk, part);
}

void
MBR_grow_part(struct mbr *mbr, struct disk *disk, int pn)
{
	struct prt *part, *p;
	int i;

	part = &mbr->part[pn];
	part->ns = disk->size - part->bs;

	for (i = 0; i < NDOSPART; i++) {
		p = &mbr->part[i];
		if (i != pn && PRT_overlap(part, p)) {
			if (p->bs > part->bs)
				part->ns = p->bs - part->bs;
			else {
				warnx("No free space at sector %d!", part->bs);
				part->ns = 0;
			}
		}
	}
	PRT_fix_CHS(disk, part);
}
