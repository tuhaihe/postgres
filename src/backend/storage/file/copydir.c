/*-------------------------------------------------------------------------
 *
 * copydir.c
 *	  copies a directory
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	While "xcopy /e /i /q" works fine for copying directories, on Windows XP
 *	it requires a Window handle which prevents it from working when invoked
 *	as a service.
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/copydir.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef HAVE_COPYFILE_H
#include <copyfile.h>
#endif
#include <fcntl.h>
#include <unistd.h>

#include "common/file_utils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/copydir.h"
#include "storage/fd.h"

/* GUCs */
int			file_copy_method = FILE_COPY_METHOD_COPY;

static void clone_file(const char *fromfile, const char *tofile);

/*
 * copydir: copy a directory
 *
 * If recurse is false, subdirectories are ignored.  Anything that's not
 * a directory or a regular file is ignored.
 *
 * This function uses the file_copy_method GUC.  New uses of this function must
 * be documented in doc/src/sgml/config.sgml.
 */
void
copydir(const char *fromdir, const char *todir, bool recurse)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		fromfile[MAXPGPATH * 2];
	char		tofile[MAXPGPATH * 2];

	if (MakePGDirectory(todir) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", todir)));

	xldir = AllocateDir(fromdir);

	while ((xlde = ReadDir(xldir, fromdir)) != NULL)
	{
		PGFileType	xlde_type;

		/* If we got a cancel signal during the copy of the directory, quit */
		CHECK_FOR_INTERRUPTS();

		if (strcmp(xlde->d_name, ".") == 0 ||
			strcmp(xlde->d_name, "..") == 0)
			continue;

		snprintf(fromfile, sizeof(fromfile), "%s/%s", fromdir, xlde->d_name);
		snprintf(tofile, sizeof(tofile), "%s/%s", todir, xlde->d_name);

		xlde_type = get_dirent_type(fromfile, xlde, false, ERROR);

		if (xlde_type == PGFILETYPE_DIR)
		{
			/* recurse to handle subdirectories */
			if (recurse)
				copydir(fromfile, tofile, true);
		}
		else if (xlde_type == PGFILETYPE_REG)
		{
			if (file_copy_method == FILE_COPY_METHOD_CLONE)
				clone_file(fromfile, tofile);
			else
				copy_file(fromfile, tofile);
		}
	}
	FreeDir(xldir);

	/*
	 * Be paranoid here and fsync all files to ensure the copy is really done.
	 * But if fsync is disabled, we're done.
	 */
	if (!enableFsync)
		return;

	xldir = AllocateDir(todir);

	while ((xlde = ReadDir(xldir, todir)) != NULL)
	{
		if (strcmp(xlde->d_name, ".") == 0 ||
			strcmp(xlde->d_name, "..") == 0)
			continue;

		snprintf(tofile, sizeof(tofile), "%s/%s", todir, xlde->d_name);

		/*
		 * We don't need to sync subdirectories here since the recursive
		 * copydir will do it before it returns
		 */
		if (get_dirent_type(tofile, xlde, false, ERROR) == PGFILETYPE_REG)
			fsync_fname(tofile, false);
	}
	FreeDir(xldir);

	/*
	 * It's important to fsync the destination directory itself as individual
	 * file fsyncs don't guarantee that the directory entry for the file is
	 * synced. Recent versions of ext4 have made the window much wider but
	 * it's been true for ext3 and other filesystems in the past.
	 */
	fsync_fname(todir, true);
}

/*
 * copy one file
 */
void
copy_file(const char *fromfile, const char *tofile)
{
	char	   *buffer;
	int			srcfd;
	int			dstfd;
	int			nbytes;
	off_t		offset;
	off_t		flush_offset;

	/* Size of copy buffer (read and write requests) */
#define COPY_BUF_SIZE (8 * BLCKSZ)

	/*
	 * Size of data flush requests.  It seems beneficial on most platforms to
	 * do this every 1MB or so.  But macOS, at least with early releases of
	 * APFS, is really unfriendly to small mmap/msync requests, so there do it
	 * only every 32MB.
	 */
#if defined(__darwin__)
#define FLUSH_DISTANCE (32 * 1024 * 1024)
#else
#define FLUSH_DISTANCE (1024 * 1024)
#endif

	/* Use palloc to ensure we get a maxaligned buffer */
	buffer = palloc(COPY_BUF_SIZE);

	/*
	 * Open the files
	 */
	srcfd = OpenTransientFile(fromfile, O_RDONLY | PG_BINARY);
	if (srcfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", fromfile)));

	dstfd = OpenTransientFile(tofile, O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
	if (dstfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tofile)));

	/*
	 * Do the data copying.
	 */
	flush_offset = 0;
	for (offset = 0;; offset += nbytes)
	{
		/* If we got a cancel signal during the copy of the file, quit */
		CHECK_FOR_INTERRUPTS();

		/*
		 * We fsync the files later, but during the copy, flush them every so
		 * often to avoid spamming the cache and hopefully get the kernel to
		 * start writing them out before the fsync comes.
		 */
		if (offset - flush_offset >= FLUSH_DISTANCE)
		{
			pg_flush_data(dstfd, flush_offset, offset - flush_offset);
			flush_offset = offset;
		}

		pgstat_report_wait_start(WAIT_EVENT_COPY_FILE_READ);
		nbytes = read(srcfd, buffer, COPY_BUF_SIZE);
		pgstat_report_wait_end();
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", fromfile)));
		if (nbytes == 0)
			break;
		errno = 0;
		pgstat_report_wait_start(WAIT_EVENT_COPY_FILE_WRITE);
		if ((int) write(dstfd, buffer, nbytes) != nbytes)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m", tofile)));
		}
		pgstat_report_wait_end();
	}

	if (offset > flush_offset)
		pg_flush_data(dstfd, flush_offset, offset - flush_offset);

	if (CloseTransientFile(dstfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tofile)));

	if (CloseTransientFile(srcfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", fromfile)));

	pfree(buffer);
}

/*
 * clone one file
 */
static void
clone_file(const char *fromfile, const char *tofile)
{
#if defined(HAVE_COPYFILE) && defined(COPYFILE_CLONE_FORCE)
	if (copyfile(fromfile, tofile, NULL, COPYFILE_CLONE_FORCE) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not clone file \"%s\" to \"%s\": %m",
						fromfile, tofile)));
#elif defined(HAVE_COPY_FILE_RANGE)
	int			srcfd;
	int			dstfd;
	ssize_t		nbytes;

	srcfd = OpenTransientFile(fromfile, O_RDONLY | PG_BINARY);
	if (srcfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", fromfile)));

	dstfd = OpenTransientFile(tofile, O_WRONLY | O_CREAT | O_EXCL | PG_BINARY);
	if (dstfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tofile)));

	do
	{
		/*
		 * Don't copy too much at once, so we can check for interrupts from
		 * time to time if it falls back to a slow copy.
		 */
		CHECK_FOR_INTERRUPTS();
		pgstat_report_wait_start(WAIT_EVENT_COPY_FILE_COPY);
		nbytes = copy_file_range(srcfd, NULL, dstfd, NULL, 1024 * 1024, 0);
		if (nbytes < 0 && errno != EINTR)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not clone file \"%s\" to \"%s\": %m",
							fromfile, tofile)));
		pgstat_report_wait_end();
	}
	while (nbytes != 0);

	if (CloseTransientFile(dstfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tofile)));

	if (CloseTransientFile(srcfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", fromfile)));
#else
	/* If there is no CLONE support this function should not be called. */
	pg_unreachable();
#endif
}
