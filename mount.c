/*
 * Mini mount implementation for busybox
 *
 * Copyright (C) 1995, 1996 by Bruce Perens <bruce@pixar.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * 3/21/1999	Charles P. Wright <cpwright@cpwright.com>
 *		searches through fstab when -a is passed
 *		will try mounting stuff with all fses when passed -t auto
 *
 * 1999-04-17	Dave Cinege...Rewrote -t auto. Fixed ro mtab.
 *
 * 1999-10-07	Erik Andersen <andersen@lineo.com>, <andersee@debian.org>.
 *              Rewrote of a lot of code. Removed mtab usage (I plan on
 *              putting it back as a compile-time option some time), 
 *              major adjustments to option parsing, and some serious 
 *              dieting all around.
 *
 * 2000-01-12   Ben Collins <bcollins@debian.org>, Borrowed utils-linux's
 *              mount to add loop support.
 */

#include "internal.h"
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <mntent.h>
#include <sys/mount.h>
#include <ctype.h>
#include <fstab.h>

#if defined BB_FEATURE_MOUNT_LOOP
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/loop.h>

static int set_loop(const char *device, const char *file, int offset, int *loopro);
static char *find_unused_loop_device (void);

static int use_loop = 0;
#endif

extern const char mtab_file[]; /* Defined in utility.c */

static const char mount_usage[] = "\tmount [flags]\n"
    "\tmount [flags] device directory [-o options,more-options]\n"
    "\n"
    "Flags:\n"
    "\t-a:\tMount all file systems in fstab.\n"
#ifdef BB_MTAB
    "\t-f:\t\"Fake\" mount. Add entry to mount table but don't mount it.\n"
    "\t-n:\tDon't write a mount table entry.\n"
#endif
    "\t-o option:\tOne of many filesystem options, listed below.\n"
    "\t-r:\tMount the filesystem read-only.\n"
    "\t-t filesystem-type:\tSpecify the filesystem type.\n"
    "\t-w:\tMount for reading and writing (default).\n"
    "\n"
    "Options for use with the \"-o\" flag:\n"
    "\tasync / sync:\tWrites are asynchronous / synchronous.\n"
    "\tdev / nodev:\tAllow use of special device files / disallow them.\n"
    "\texec / noexec:\tAllow use of executable files / disallow them.\n"
#if defined BB_FEATURE_MOUNT_LOOP
    "\tloop: Mounts a file via loop device.\n"
#endif
    "\tsuid / nosuid:\tAllow set-user-id-root programs / disallow them.\n"
    "\tremount: Re-mount a currently-mounted filesystem, changing its flags.\n"
    "\tro / rw: Mount for read-only / read-write.\n"
    "\t"
    "There are EVEN MORE flags that are specific to each filesystem.\n"
    "You'll have to see the written documentation for those.\n";


struct mount_options {
    const char *name;
    unsigned long and;
    unsigned long or;
};

static const struct mount_options mount_options[] = {
    {"async", ~MS_SYNCHRONOUS, 0},
    {"defaults", ~0, 0},
    {"dev", ~MS_NODEV, 0},
    {"exec", ~MS_NOEXEC, 0},
    {"nodev", ~0, MS_NODEV},
    {"noexec", ~0, MS_NOEXEC},
    {"nosuid", ~0, MS_NOSUID},
    {"remount", ~0, MS_REMOUNT},
    {"ro", ~0, MS_RDONLY},
    {"rw", ~MS_RDONLY, 0},
    {"suid", ~MS_NOSUID, 0},
    {"sync", ~0, MS_SYNCHRONOUS},
    {0, 0, 0}
};

static int
do_mount(char* specialfile, char* dir, char* filesystemtype, 
	long flags, void* string_flags, int useMtab, int fakeIt, char* mtab_opts)
{
    int status=0;

#if defined BB_MTAB
    if (fakeIt==FALSE)
#endif
    {
#if defined BB_FEATURE_MOUNT_LOOP
	if (use_loop) {
	    int loro = flags & MS_RDONLY;
	    char *lofile = specialfile;
	    specialfile = find_unused_loop_device();
	    if (specialfile == NULL) {
		fprintf(stderr, "Could not find a spare loop device\n");
		exit(1);
	    }
	    if (set_loop (specialfile, lofile, 0, &loro)) {
		fprintf(stderr, "Could not setup loop device\n");
		exit(1);
	    }
	    if (!(flags & MS_RDONLY) && loro) { /* loop is ro, but wanted rw */
		fprintf(stderr, "WARNING: loop device is read-only\n");
		flags &= ~MS_RDONLY;
	    }
	}
#endif
	status=mount(specialfile, dir, filesystemtype, flags, string_flags);
    }
#if defined BB_MTAB
    if (status == 0) {
	if (useMtab==TRUE)
	    write_mtab(specialfile, dir, filesystemtype, flags, mtab_opts);
	return 0;
    }
    else
#endif
	return(status);
}



#if defined BB_MTAB
#define whine_if_fstab_is_missing() {} 
#else
extern void whine_if_fstab_is_missing()
{
    struct stat statBuf;
    if (stat("/etc/fstab", &statBuf) < 0) 
	fprintf(stderr, "/etc/fstab file missing -- install one to name /dev/root.\n\n");
}
#endif


/* Seperate standard mount options from the nonstandard string options */
static void
parse_mount_options ( char *options, unsigned long *flags, char *strflags)
{
    while (options) {
	int gotone=FALSE;
	char *comma = strchr (options, ',');
	const struct mount_options* f = mount_options;
	if (comma)
	    *comma = '\0';

	while (f->name != 0) {
	    if (strcasecmp (f->name, options) == 0) {

		*flags &= f->and;
		*flags |= f->or;
		gotone=TRUE;
		break;
	    }
	    f++;
	}
#if defined BB_FEATURE_MOUNT_LOOP
	if (gotone==FALSE && !strcasecmp ("loop", options)) { /* loop device support */
	    use_loop = 1;
	    gotone=TRUE;
	}
#endif
	if (*strflags && strflags!= '\0' && gotone==FALSE) {
	    char *temp=strflags;
	    temp += strlen (strflags);
	    *temp++ = ',';
	    *temp++ = '\0';
	}
	if (gotone==FALSE)
	    strcat (strflags, options);
	if (comma) {
	    *comma = ',';
	    options = ++comma;
	} else {
	    break;
	}
    }
}

int
mount_one(char *blockDevice, char *directory, char *filesystemType,
	   unsigned long flags, char *string_flags, int useMtab, int fakeIt, char *mtab_opts)
{
    int status = 0;

    char buf[255];

#if defined BB_FEATURE_USE_PROCFS
    if (strcmp(filesystemType, "auto") == 0) {
	FILE *f = fopen ("/proc/filesystems", "r");

	if (f == NULL)
	    return( FALSE);

	while (fgets (buf, sizeof (buf), f) != NULL) {
	    filesystemType = buf;
	    if (*filesystemType == '\t') {	// Not a nodev filesystem

		// Add NULL termination to each line
		while (*filesystemType && *filesystemType != '\n')
		    filesystemType++;
		*filesystemType = '\0';

		filesystemType = buf;
		filesystemType++;	// hop past tab

		status = do_mount (blockDevice, directory, filesystemType,
				flags | MS_MGC_VAL, string_flags, useMtab, 
				fakeIt, mtab_opts);
		if (status == 0)
		    break;
	    }
	}
	fclose (f);
    } else
#endif
    {
	status = do_mount (blockDevice, directory, filesystemType,
			flags | MS_MGC_VAL, string_flags, useMtab, 
			fakeIt, mtab_opts);
    }

    if (status) {
	fprintf (stderr, "Mounting %s on %s failed: %s\n",
		 blockDevice, directory, strerror(errno));
	return (FALSE);
    }
    return (TRUE);
}

extern int mount_main (int argc, char **argv)
{
    char string_flags_buf[1024]="";
    char *string_flags = string_flags_buf;
    char *extra_opts = string_flags_buf;
    unsigned long flags = 0;
    char *filesystemType = "auto";
    char *device = NULL;
    char *directory = NULL;
    int all = FALSE;
    int fakeIt = FALSE;
    int useMtab = TRUE;
    int i;

    /* Only compiled in if BB_MTAB is not defined */
    whine_if_fstab_is_missing();

    if (argc == 1) {
	FILE *mountTable = setmntent (mtab_file, "r");
	if (mountTable) {
	    struct mntent *m;
	    while ((m = getmntent (mountTable)) != 0) {
		struct fstab* fstabItem;
		char *blockDevice = m->mnt_fsname;
		/* Note that if /etc/fstab is missing, libc can't fix up /dev/root for us */
		if (strcmp (blockDevice, "/dev/root") == 0) {
		    fstabItem = getfsfile ("/");
		    if (fstabItem != NULL)
			blockDevice = fstabItem->fs_spec;
		}
		printf ("%s on %s type %s (%s)\n", blockDevice, m->mnt_dir,
			m->mnt_type, m->mnt_opts);
	    }
	    endmntent (mountTable);
	} else {
	    perror(mtab_file);
	}
	exit( TRUE);
    }


    /* Parse options */
    i = --argc;
    argv++;
    while (i > 0 && **argv) {
	if (**argv == '-') {
	    char *opt = *argv;
	    while (i>0 && *++opt) switch (*opt) {
	    case 'o':
		if (--i == 0) {
		    goto goodbye;
		}
		parse_mount_options (*(++argv), &flags, string_flags);
		break;
	    case 'r':
		flags |= MS_RDONLY;
		break;
	    case 't':
		if (--i == 0) {
		    goto goodbye;
		}
		filesystemType = *(++argv);
		break;
	    case 'w':
		flags &= ~MS_RDONLY;
		break;
	    case 'a':
		all = TRUE;
		break;
#ifdef BB_MTAB
	    case 'f':
		fakeIt = TRUE;
		break;
	    case 'n':
		useMtab = FALSE;
		break;
#endif
	    case 'v':
	    case 'h':
	    case '-':
		goto goodbye;
	    }
	} else {
	    if (device == NULL)
		device=*argv;
	    else if (directory == NULL)
		directory=*argv;
	    else {
		goto goodbye;
	    }
	}
	i--;
	argv++;
    }

    if (all == TRUE) {
	struct mntent *m;
	FILE *f = setmntent ("/etc/fstab", "r");

	if (f == NULL) {
	    perror("/etc/fstab");
	    exit( FALSE); 
	}
	while ((m = getmntent (f)) != NULL) {
	    // If the file system isn't noauto, and isn't mounted on /, 
	    // and isn't swap or nfs, then mount it
	    if ((!strstr (m->mnt_opts, "noauto")) &&
		    (m->mnt_dir[1] != '\0') && 
		    (!strstr (m->mnt_type, "swap")) && 
		    (!strstr (m->mnt_type, "nfs"))) 
	    {
		flags = 0;
		*string_flags = '\0';
		parse_mount_options(m->mnt_opts, &flags, string_flags);
		mount_one (m->mnt_fsname, m->mnt_dir, m->mnt_type, 
			flags, string_flags, useMtab, fakeIt, extra_opts);
	    }
	}
	endmntent (f);
    } else {
	if (device && directory) {
#ifdef BB_NFSMOUNT
	    if (strcmp(filesystemType, "nfs") == 0) {
		if (nfsmount(device, directory, &flags, &extra_opts, &string_flags, 1) != 0)
		exit(FALSE);
	    }
#endif
	    exit (mount_one (device, directory, filesystemType, 
			flags, string_flags, useMtab, fakeIt, extra_opts));
	} else {
	    goto goodbye;
	}
    }
    exit( TRUE);

goodbye:
    usage( mount_usage);
}

#if defined BB_FEATURE_MOUNT_LOOP
static int set_loop(const char *device, const char *file, int offset, int *loopro)
{
	struct loop_info loopinfo;
	int	fd, ffd, mode;
	
	mode = *loopro ? O_RDONLY : O_RDWR;
	if ((ffd = open (file, mode)) < 0 && !*loopro
	    && (errno != EROFS || (ffd = open (file, mode = O_RDONLY)) < 0)) {
	  perror (file);
	  return 1;
	}
	if ((fd = open (device, mode)) < 0) {
	  close(ffd);
	  perror (device);
	  return 1;
	}
	*loopro = (mode == O_RDONLY);

	memset(&loopinfo, 0, sizeof(loopinfo));
	strncpy(loopinfo.lo_name, file, LO_NAME_SIZE);
	loopinfo.lo_name[LO_NAME_SIZE-1] = 0;

	loopinfo.lo_offset = offset;

	loopinfo.lo_encrypt_key_size = 0;
	if (ioctl(fd, LOOP_SET_FD, ffd) < 0) {
		perror("ioctl: LOOP_SET_FD");
		exit(1);
	}
	if (ioctl(fd, LOOP_SET_STATUS, &loopinfo) < 0) {
		(void) ioctl(fd, LOOP_CLR_FD, 0);
		perror("ioctl: LOOP_SET_STATUS");
		exit(1);
	}
	close(fd);
	close(ffd);
	return 0;
}

static char *find_unused_loop_device (void)
{
    char dev[20];
    int i, fd, somedev = 0, someloop = 0;
    struct stat statbuf;
    struct loop_info loopinfo;

    for(i = 0; i < 256; i++) {
      sprintf(dev, "/dev/loop%d", i);
      if (stat (dev, &statbuf) == 0 && S_ISBLK(statbuf.st_mode)) {
	somedev++;
	fd = open (dev, O_RDONLY);
	if (fd >= 0) {
	  if(ioctl (fd, LOOP_GET_STATUS, &loopinfo) == 0)
	    someloop++; /* in use */
	  else if (errno == ENXIO) {
	    close (fd);
	    return strdup(dev); /* probably free */
	  }
	  close (fd);
        }
	continue;
      }
      if (i >= 7)
	break;
    }
    return NULL;
}
#endif /* BB_FEATURE_MOUNT_LOOP */
