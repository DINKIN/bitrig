/*	$OpenBSD: mount_vnd.c,v 1.16 2014/10/29 21:30:10 tedu Exp $	*/
/*
 * Copyright (c) 1993 University of Utah.
 * Copyright (c) 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
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
 * from: Utah $Hdr: vnconfig.c 1.1 93/12/15$
 *
 *	@(#)vnconfig.c	8.1 (Berkeley) 12/15/93
 */

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/disklabel.h>

#include <dev/vndioctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <util.h>

#define DEFAULT_VND	"vnd0"

#define VND_CONFIG	1
#define VND_UNCONFIG	2
#define VND_GET		3

int verbose = 0;
int run_mount_vnd = 0;

__dead void	 usage(void);
int		 config(char *, char *, int, struct disklabel *);
int		 getinfo(const char *);

int
main(int argc, char **argv)
{
	int	 ch, rv, action, opt_c, opt_l, opt_u;
	char	*mntopts;
	extern char *__progname;
	struct disklabel *dp = NULL;

	if (strcasecmp(__progname, "mount_vnd") == 0)
		run_mount_vnd = 1;

	opt_c = opt_l = opt_u = 0;
	mntopts = NULL;
	action = VND_CONFIG;

	while ((ch = getopt(argc, argv, "clo:t:uv")) != -1) {
		switch (ch) {
		case 'c':
			opt_c = 1;
			break;
		case 'l':
			opt_l = 1;
			break;
		case 'o':
			mntopts = optarg;
			break;
		case 't':
			dp = getdiskbyname(optarg);
			if (dp == NULL)
				errx(1, "unknown disk type: %s", optarg);
			break;
		case 'u':
			opt_u = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;

	if (opt_c + opt_l + opt_u > 1)
		errx(1, "-c, -l and -u are mutually exclusive options");

	if (opt_l)
		action = VND_GET;
	else if (opt_u)
		action = VND_UNCONFIG;
	else
		action = VND_CONFIG;	/* default behavior */

	if (action == VND_CONFIG && argc == 2) {
		int ind_raw, ind_reg;

		/* fix order of arguments. */
		if (run_mount_vnd) {
			ind_raw = 1;
			ind_reg = 0;
		} else {
			ind_raw = 0;
			ind_reg = 1;
		}
		rv = config(argv[ind_raw], argv[ind_reg], action, dp);
	} else if (action == VND_UNCONFIG && argc == 1)
		rv = config(argv[0], NULL, action, NULL);
	else if (action == VND_GET)
		rv = getinfo(argc ? argv[0] : NULL);
	else
		usage();

	exit(rv);
}

int
getinfo(const char *vname)
{
	int vd, print_all = 0;
	struct vnd_user vnu;

	if (vname == NULL) {
		vname = DEFAULT_VND;
		print_all = 1;
	}

	vd = opendev((char *)vname, O_RDONLY, OPENDEV_PART, NULL);
	if (vd < 0)
		err(1, "open: %s", vname);

	vnu.vnu_unit = -1;

query:
	if (ioctl(vd, VNDIOCGET, &vnu) == -1) {
		if (print_all && errno == ENXIO && vnu.vnu_unit > 0) {
			close(vd);
			return (0);
		} else {
			err(1, "ioctl: %s", vname);
		}
	}

	fprintf(stdout, "vnd%d: ", vnu.vnu_unit);

	if (!vnu.vnu_ino)
		fprintf(stdout, "not in use\n");
	else
		fprintf(stdout, "covering %s on %s, inode %llu\n",
		    vnu.vnu_file, devname(vnu.vnu_dev, S_IFBLK),
		    (unsigned long long)vnu.vnu_ino);

	if (print_all) {
		vnu.vnu_unit++;
		goto query;
	}

	close(vd);

	return (0);
}

int
config(char *dev, char *file, int action, struct disklabel *dp)
{
	struct vnd_ioctl vndio;
	char *rdev;
	int fd, rv = -1;

	if ((fd = opendev(dev, O_RDONLY, OPENDEV_PART, &rdev)) < 0)
		err(4, "%s", rdev);

	vndio.vnd_file = file;
	vndio.vnd_secsize = (dp && dp->d_secsize) ? dp->d_secsize : DEV_BSIZE;
	vndio.vnd_nsectors = (dp && dp->d_nsectors) ? dp->d_nsectors : 100;
	vndio.vnd_ntracks = (dp && dp->d_ntracks) ? dp->d_ntracks : 1;

	/*
	 * Clear (un-configure) the device
	 */
	if (action == VND_UNCONFIG) {
		rv = ioctl(fd, VNDIOCCLR, &vndio);
		if (rv)
			warn("VNDIOCCLR");
		else if (verbose)
			printf("%s: cleared\n", dev);
	}
	/*
	 * Configure the device
	 */
	if (action == VND_CONFIG) {
		rv = ioctl(fd, VNDIOCSET, &vndio);
		if (rv)
			warn("VNDIOCSET");
		else if (verbose)
			printf("%s: %llu bytes on %s\n", dev, vndio.vnd_size,
			    file);
	}

	close(fd);
	fflush(stdout);
	return (rv < 0);
}

__dead void
usage(void)
{
	extern char *__progname;

	if (run_mount_vnd)
		(void)fprintf(stderr,
		    "usage: %s [-o options] [-t disktype] image vnd_dev\n",
		    __progname);
	else
		(void)fprintf(stderr,
		    "usage: %s [-cluv] [-t disktype] vnd_dev image\n",
		    __progname);

	exit(1);
}
