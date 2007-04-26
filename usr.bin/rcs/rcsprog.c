/*	$OpenBSD: rcsprog.c,v 1.138 2007/04/26 21:51:04 sobrado Exp $	*/
/*
 * Copyright (c) 2005 Jean-Francois Brousseau <jfb@openbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL  DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/stat.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rcsprog.h"

#define RCS_CMD_MAXARG	128
#define RCSPROG_OPTSTRING	"A:a:b::c:e::ik:Ll::m:Mn:N:o:qt::TUu::Vx::z::"

const char rcs_version[] = "OpenRCS 4.1";

int	 rcsflags;
int	 rcs_optind;
char	*rcs_optarg;
char	*rcs_suffixes;
char	*rcs_tmpdir = RCS_TMPDIR_DEFAULT;

struct rcs_prog {
	char	*prog_name;
	int	(*prog_hdlr)(int, char **);
	void	(*prog_usage)(void);
} programs[] = {
	{ "rcs",	rcs_main,	rcs_usage	},
	{ "ci",		checkin_main,	checkin_usage   },
	{ "co",		checkout_main,	checkout_usage  },
	{ "rcsclean",	rcsclean_main,	rcsclean_usage	},
	{ "rcsdiff",	rcsdiff_main,	rcsdiff_usage	},
	{ "rcsmerge",	rcsmerge_main,	rcsmerge_usage	},
	{ "rlog",	rlog_main,	rlog_usage	},
	{ "ident",	ident_main,	ident_usage	},
	{ "merge",	merge_main,	merge_usage	},
};

struct rcs_wklhead rcs_temp_files;

void sighdlr(int);
static void  rcs_attach_symbol(RCSFILE *, const char *);

/* ARGSUSED */
void
sighdlr(int sig)
{
	rcs_worklist_clean(&rcs_temp_files, rcs_worklist_unlink);
	_exit(1);
}

int
rcs_init(char *envstr, char **argv, int argvlen)
{
	u_int i;
	int argc, error;
	char linebuf[256],  *lp, *cp;

	if (strlcpy(linebuf, envstr, sizeof(linebuf)) >= sizeof(linebuf))
		errx(1, "rcs_init: string truncation");
	(void)memset(argv, 0, argvlen * sizeof(char *));

	error = argc = 0;
	for (lp = linebuf; lp != NULL;) {
		cp = strsep(&lp, " \t\b\f\n\r\t\v");
		if (cp == NULL)
			break;
		else if (*cp == '\0')
			continue;

		if (argc == argvlen) {
			error++;
			break;
		}

		argv[argc] = xstrdup(cp);
		argc++;
	}

	if (error != 0) {
		for (i = 0; i < (u_int)argc; i++)
			xfree(argv[i]);
		argc = -1;
	}

	return (argc);
}

int
main(int argc, char **argv)
{
	u_int i;
	char *rcsinit, *cmd_argv[RCS_CMD_MAXARG];
	int ret, cmd_argc;

	ret = -1;
	rcs_optind = 1;
	SLIST_INIT(&rcs_temp_files);

	cmd_argc = 0;
	cmd_argv[cmd_argc++] = argv[0];
	if ((rcsinit = getenv("RCSINIT")) != NULL) {
		ret = rcs_init(rcsinit, cmd_argv + 1,
		    RCS_CMD_MAXARG - 1);
		if (ret < 0) {
			warnx("failed to prepend RCSINIT options");
			exit (1);
		}

		cmd_argc += ret;
	}

	if ((rcs_tmpdir = getenv("TMPDIR")) == NULL)
		rcs_tmpdir = RCS_TMPDIR_DEFAULT;

	for (ret = 1; ret < argc; ret++)
		cmd_argv[cmd_argc++] = argv[ret];

	signal(SIGHUP, sighdlr);
	signal(SIGINT, sighdlr);
	signal(SIGQUIT, sighdlr);
	signal(SIGABRT, sighdlr);
	signal(SIGALRM, sighdlr);
	signal(SIGTERM, sighdlr);

	for (i = 0; i < (sizeof(programs)/sizeof(programs[0])); i++)
		if (strcmp(__progname, programs[i].prog_name) == 0) {
			usage = programs[i].prog_usage;
			ret = programs[i].prog_hdlr(cmd_argc, cmd_argv);
			break;
		}

	/* clean up temporary files */
	rcs_worklist_run(&rcs_temp_files, rcs_worklist_unlink);

	exit(ret);
}


void
rcs_usage(void)
{
	fprintf(stderr,
	    "usage: rcs [-IiLqTUV] [-Aoldfile] [-ausers] [-b[rev]]\n"
	    "           [-cstring] [-e[users]] [-kmode] [-l[rev]] [-mrev:msg]\n"
	    "           [-orev] [-tstr] [-u[rev]] [-xsuffixes] file ...\n");
}

/*
 * rcs_main()
 *
 * Handler for the `rcs' program.
 * Returns 0 on success, or >0 on error.
 */
int
rcs_main(int argc, char **argv)
{
	int fd;
	int i, j, ch, flags, kflag, lkmode;
	const char *nflag, *oldfilename, *orange;
	char fpath[MAXPATHLEN];
	char *logstr, *logmsg, *descfile;
	char *alist, *comment, *elist, *lrev, *urev;
	mode_t fmode;
	RCSFILE *file;
	RCSNUM *logrev;
	struct rcs_access *acp;
	time_t rcs_mtime = -1;

	kflag = RCS_KWEXP_ERR;
	lkmode = RCS_LOCK_INVAL;
	fmode =  S_IRUSR|S_IRGRP|S_IROTH;
	flags = RCS_RDWR|RCS_PARSE_FULLY;
	lrev = urev = descfile = NULL;
	logstr = alist = comment = elist = NULL;
	nflag = oldfilename = orange = NULL;

	/* match GNU */
	if (1 < argc && argv[1][0] != '-')
		warnx("warning: No options were given; "
		    "this usage is obsolescent.");

	while ((ch = rcs_getopt(argc, argv, RCSPROG_OPTSTRING)) != -1) {
		switch (ch) {
		case 'A':
			oldfilename = rcs_optarg;
			rcsflags |= CO_ACLAPPEND;
			break;
		case 'a':
			alist = rcs_optarg;
			break;
		case 'c':
			comment = rcs_optarg;
			break;
		case 'e':
			elist = rcs_optarg;
			rcsflags |= RCSPROG_EFLAG;
			break;
		case 'i':
			flags |= RCS_CREATE;
			break;
		case 'k':
			kflag = rcs_kflag_get(rcs_optarg);
			if (RCS_KWEXP_INVAL(kflag)) {
				warnx("invalid RCS keyword substitution mode");
				(usage)();
				exit(1);
			}
			break;
		case 'L':
			if (lkmode == RCS_LOCK_LOOSE)
				warnx("-U overriden by -L");
			lkmode = RCS_LOCK_STRICT;
			break;
		case 'l':
			/* XXX - Check with -u flag. */
			lrev = rcs_optarg;
			rcsflags |= RCSPROG_LFLAG;
			break;
		case 'm':
			if (logstr != NULL)
				xfree(logstr);
			logstr = xstrdup(rcs_optarg);
			break;
		case 'M':
			/* ignore for the moment */
			break;
		case 'n':
			nflag = rcs_optarg;
			break;
		case 'N':
			nflag = rcs_optarg;
			rcsflags |= RCSPROG_NFLAG;
			break;
		case 'o':
			orange = rcs_optarg;
			break;
		case 'q':
			rcsflags |= QUIET;
			break;
		case 't':
			descfile = rcs_optarg;
			rcsflags |= DESCRIPTION;
			break;
		case 'T':
			rcsflags |= PRESERVETIME;
			break;
		case 'U':
			if (lkmode == RCS_LOCK_STRICT)
				warnx("-L overriden by -U");
			lkmode = RCS_LOCK_LOOSE;
			break;
		case 'u':
			/* XXX - Check with -l flag. */
			urev = rcs_optarg;
			rcsflags |= RCSPROG_UFLAG;
			break;
		case 'V':
			printf("%s\n", rcs_version);
			exit(0);
		case 'x':
			/* Use blank extension if none given. */
			rcs_suffixes = rcs_optarg ? rcs_optarg : "";
			break;
		case 'z':
			/*
			 * kept for compatibility
			 */
			break;
		default:
			(usage)();
			exit(1);
		}
	}

	argc -= rcs_optind;
	argv += rcs_optind;

	if (argc == 0) {
		warnx("no input file");
		(usage)();
		exit(1);
	}

	for (i = 0; i < argc; i++) {
		fd = rcs_choosefile(argv[i], fpath, sizeof(fpath));
		if (fd < 0 && !(flags & RCS_CREATE)) {
			warn("%s", fpath);
			continue;
		}

		if (!(rcsflags & QUIET))
			(void)fprintf(stderr, "RCS file: %s\n", fpath);

		if ((file = rcs_open(fpath, fd, flags, fmode)) == NULL) {
			close(fd);
			continue;
		}

		if (rcsflags & DESCRIPTION) {
			if (rcs_set_description(file, descfile) == -1) {
				warn("%s", descfile);
				rcs_close(file);
				continue;
			}
		}
		else if (flags & RCS_CREATE) {
			if (rcs_set_description(file, NULL) == -1) {
				warn("stdin");
				rcs_close(file);
				continue;
			}
		}

		if (rcsflags & PRESERVETIME)
			rcs_mtime = rcs_get_mtime(file);

		if (nflag != NULL)
			rcs_attach_symbol(file, nflag);

		if (logstr != NULL) {
			if ((logmsg = strchr(logstr, ':')) == NULL) {
				warnx("missing log message");
				rcs_close(file);
				continue;
			}

			*logmsg++ = '\0';
			if ((logrev = rcsnum_parse(logstr)) == NULL) {
				warnx("`%s' bad revision number", logstr);
				rcs_close(file);
				continue;
			}

			if (rcs_rev_setlog(file, logrev, logmsg) < 0) {
				warnx("failed to set logmsg for `%s' to `%s'",
				    logstr, logmsg);
				rcs_close(file);
				rcsnum_free(logrev);
				continue;
			}

			rcsnum_free(logrev);
		}

		/* entries to add from <oldfile> */
		if (rcsflags & CO_ACLAPPEND) {
			RCSFILE *oldfile;
			int ofd;
			char ofpath[MAXPATHLEN];

			ofd = rcs_choosefile(oldfilename, ofpath, sizeof(ofpath));
			if (ofd < 0) {
				if (!(flags & RCS_CREATE))
					warn("%s", ofpath);
				exit(1);
			}
			if ((oldfile = rcs_open(ofpath, ofd, RCS_READ)) == NULL)
				exit(1);

			TAILQ_FOREACH(acp, &(oldfile->rf_access), ra_list)
				rcs_access_add(file, acp->ra_name);

			rcs_close(oldfile);
			(void)close(ofd);
		}

		/* entries to add to the access list */
		if (alist != NULL) {
			struct rcs_argvector *aargv;

			aargv = rcs_strsplit(alist, ",");
			for (j = 0; aargv->argv[j] != NULL; j++)
				rcs_access_add(file, aargv->argv[j]);

			rcs_argv_destroy(aargv);
		}

		if (comment != NULL)
			rcs_comment_set(file, comment);

		if (elist != NULL) {
			struct rcs_argvector *eargv;

			eargv = rcs_strsplit(elist, ",");
			for (j = 0; eargv->argv[j] != NULL; j++)
				rcs_access_remove(file, eargv->argv[j]);

			rcs_argv_destroy(eargv);
		} else if (rcsflags & RCSPROG_EFLAG) {
			struct rcs_access *rap;

			/* XXX rcs_access_remove(file, NULL); ?? */
			while (!TAILQ_EMPTY(&(file->rf_access))) {
				rap = TAILQ_FIRST(&(file->rf_access));
				TAILQ_REMOVE(&(file->rf_access), rap, ra_list);
				xfree(rap->ra_name);
				xfree(rap);
			}
			/* not synced anymore */
			file->rf_flags &= ~RCS_SYNCED;
		}

		rcs_kwexp_set(file, kflag);

		if (lkmode != RCS_LOCK_INVAL)
			(void)rcs_lock_setmode(file, lkmode);

		if (rcsflags & RCSPROG_LFLAG) {
			RCSNUM *rev;
			const char *username;
			char rev_str[16];

			if ((username = getlogin()) == NULL)
				err(1, "getlogin");
			if (lrev == NULL) {
				rev = rcsnum_alloc();
				rcsnum_cpy(file->rf_head, rev, 0);
			} else if ((rev = rcsnum_parse(lrev)) == NULL) {
				warnx("unable to unlock file");
				rcs_close(file);
				continue;
			}
			rcsnum_tostr(rev, rev_str, sizeof(rev_str));
			/* Make sure revision exists. */
			if (rcs_findrev(file, rev) == NULL)
				errx(1, "%s: cannot lock nonexisting "
				    "revision %s", fpath, rev_str);
			if (rcs_lock_add(file, username, rev) != -1 &&
			    !(rcsflags & QUIET))
				(void)fprintf(stderr, "%s locked\n", rev_str);
			rcsnum_free(rev);
		}

		if (rcsflags & RCSPROG_UFLAG) {
			RCSNUM *rev;
			const char *username;
			char rev_str[16];

			if ((username = getlogin()) == NULL)
				err(1, "getlogin");
			if (urev == NULL) {
				rev = rcsnum_alloc();
				rcsnum_cpy(file->rf_head, rev, 0);
			} else if ((rev = rcsnum_parse(urev)) == NULL) {
				warnx("unable to unlock file");
				rcs_close(file);
				continue;
			}
			rcsnum_tostr(rev, rev_str, sizeof(rev_str));
			/* Make sure revision exists. */
			if (rcs_findrev(file, rev) == NULL)
				errx(1, "%s: cannot unlock nonexisting "
				    "revision %s", fpath, rev_str);
			if (rcs_lock_remove(file, username, rev) == -1 &&
			    !(rcsflags & QUIET))
				warnx("%s: warning: No locks are set.", fpath);
			else {
				if (!(rcsflags & QUIET))
					(void)fprintf(stderr,
					    "%s unlocked\n", rev_str);
			}
			rcsnum_free(rev);
		}

		if (orange != NULL) {
			struct rcs_delta *rdp, *nrdp;
			char b[16];

			rcs_rev_select(file, orange);
			for (rdp = TAILQ_FIRST(&(file->rf_delta));
			    rdp != NULL; rdp = nrdp) {
				nrdp = TAILQ_NEXT(rdp, rd_list);

				/*
				 * Delete selected revisions.
				 */
				if (rdp->rd_flags & RCS_RD_SELECT) {
					rcsnum_tostr(rdp->rd_num, b, sizeof(b));
					if (!(rcsflags & QUIET)) {
						(void)fprintf(stderr, "deleting"
						    " revision %s\n", b);
					}
					(void)rcs_rev_remove(file, rdp->rd_num);
				}
			}
		}

		rcs_write(file);

		if (rcsflags & PRESERVETIME)
			rcs_set_mtime(file, rcs_mtime);

		rcs_close(file);

		if (!(rcsflags & QUIET))
			(void)fprintf(stderr, "done\n");
	}

	return (0);
}

static void
rcs_attach_symbol(RCSFILE *file, const char *symname)
{
	char *rnum;
	RCSNUM *rev;
	char rbuf[16];
	int rm;

	rm = 0;
	rev = NULL;
	if ((rnum = strrchr(symname, ':')) != NULL) {
		if (rnum[1] == '\0')
			rev = file->rf_head;
		*(rnum++) = '\0';
	} else {
		rm = 1;
	}

	if (rev == NULL && rm != 1) {
		if ((rev = rcsnum_parse(rnum)) == NULL)
			errx(1, "bad revision %s", rnum);
	}

	if (rcsflags & RCSPROG_NFLAG)
		rm = 1;

	if (rm == 1) {
		if (rcs_sym_remove(file, symname) < 0) {
			if (rcs_errno == RCS_ERR_NOENT &&
			    !(rcsflags & RCSPROG_NFLAG))
				warnx("cannot delete nonexisting symbol %s",
				    symname);
		} else {
			if (rcsflags & RCSPROG_NFLAG)
				rm = 0;
		}
	}

	if (rm == 0) {
		if (rcs_sym_add(file, symname, rev) < 0 &&
		    rcs_errno == RCS_ERR_DUPENT) {
			rcsnum_tostr(rcs_sym_getrev(file, symname),
			    rbuf, sizeof(rbuf));
			errx(1, "symbolic name %s already bound to %s",
			    symname, rbuf);
		}
	}
}
