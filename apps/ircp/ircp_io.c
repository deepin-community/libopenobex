#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef _WIN32
#define _GNU_SOURCE
#endif

#include <openobex/obex.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#define snprintf _snprintf
#define open _open
#define S_ISDIR(m) ((m & _S_IFDIR) != 0)
#endif

#if ! defined(_MSC_VER)
#include <sys/param.h>
#else
#define MAXPATHLEN 1024
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>

#include "debug.h"
#include "ircp_io.h"

#define TRUE  1
#define FALSE 0

//
// Get some file-info. (size and lastmod)
//
static int get_fileinfo(const char *name, char *lastmod)
{
	struct stat stats;
	struct tm *tm;

	if (stat(name, &stats) != -1) {
		tm = gmtime(&stats.st_mtime);
		snprintf(lastmod, 21, "%04d-%02d-%02dT%02d:%02d:%02dZ",
				tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
				tm->tm_hour, tm->tm_min, tm->tm_sec);
		return (int) stats.st_size;
	}
	else {
		DEBUG(1, "stat() error!\n");
		return 0;
	}
}


//
// Create an object from a file. Attach some info-headers to it
//
obex_object_t *build_object_from_file(obex_t *handle, const char *localname, const char *remotename)
{
	obex_object_t *object = NULL;
	obex_headerdata_t hdd;
	uint8_t *ucname;
	int ucname_len, size;
	char lastmod[21*2] = {"1970-01-01T00:00:00Z"};

	/* Get filesize and modification-time */
	size = get_fileinfo(localname, lastmod);

	object = OBEX_ObjectNew(handle, OBEX_CMD_PUT);
	if(object == NULL)
		return NULL;

	ucname_len = strlen(remotename)*2 + 2;
	ucname = malloc(ucname_len);
	if(ucname == NULL)
		goto err;

	ucname_len = OBEX_CharToUnicode(ucname, (uint8_t *) remotename, ucname_len);
	if (ucname_len < 0)
		goto err;

	hdd.bs = ucname;
	OBEX_ObjectAddHeader(handle, object, OBEX_HDR_NAME, hdd, ucname_len, 0);
	free(ucname);

	hdd.bq4 = size;
	OBEX_ObjectAddHeader(handle, object, OBEX_HDR_LENGTH, hdd, sizeof(uint32_t), 0);

#if 0
	/* Win2k excpects this header to be in unicode. I suspect this in
	   incorrect so this will have to wait until that's investigated */
	hdd.bs = lastmod;
	OBEX_ObjectAddHeader(handle, object, OBEX_HDR_TIME, hdd, strlen(lastmod)+1, 0);
#endif

	hdd.bs = NULL;
	OBEX_ObjectAddHeader(handle, object, OBEX_HDR_BODY,
				hdd, 0, OBEX_FL_STREAM_START);

	DEBUG(4, "Lastmod = %s\n", lastmod);
	return object;

err:
	if(object != NULL)
		OBEX_ObjectDelete(handle, object);
	return NULL;
}

//
// Check for dangerous filenames.
//
static int ircp_nameok(const char *name)
{
	DEBUG(4, "\n");

	/* No abs paths */
	if(name[0] == '/')
		return FALSE;

	if(strlen(name) >= 3) {
		/* "../../vmlinuz" */
		if(name[0] == '.' && name[1] == '.' && name[2] == '/')
			return FALSE;
		/* "dir/../../../vmlinuz" */
		if(strstr(name, "/../") != NULL)
			return FALSE;
	}
	return TRUE;
}

//
// Open a file, but do some sanity-checking first.
//
#ifndef DEFFILEMODE
#define DEFFILEMODE 0
#endif
int ircp_open_safe(const char *path, const char *name)
{
	char diskname[MAXPATHLEN];
	int fd;

	DEBUG(4, "\n");

	/* Check for dangerous filenames */
	if(ircp_nameok(name) == FALSE)
		return -1;

	if (path == NULL || path[0] == 0)
		path = ".";
	if (snprintf(diskname, sizeof(diskname), "%s/%s", path, name) >= (ssize_t) sizeof(diskname))
		return -1;

	/* never overwrite an existing file */
	fd = open(diskname, O_RDWR | O_CREAT | O_EXCL, DEFFILEMODE);
#ifndef _WIN32
	if (fd < 0 &&
	    snprintf(diskname, sizeof(diskname), "%s/%s_XXXXXX", path, name) < (ssize_t) sizeof(diskname))
		fd = mkstemp(diskname);
#endif

	if (fd >= 0) {
		DEBUG(4, "Creating file %s\n", diskname);
	}

	return fd;
}

//
// Go to a directory. Create if not exists and create is true.
//
int ircp_checkdir(const char *path, const char *dir, cd_flags flags)
{
	char newpath[MAXPATHLEN];
	struct stat statbuf;
	int ret = -1;

	if(!(flags & CD_ALLOWABS))	{
		if(ircp_nameok(dir) == FALSE)
			return -1;
	}

	if (path[0] != 0)
		snprintf(newpath, sizeof(newpath), "%s/%s", path, dir);
	else {
		memset(newpath, 0, sizeof(newpath));
		strncpy(newpath, dir, sizeof(newpath)-1);
	}

	DEBUG(4, "path = %s dir = %s, flags = %d\n", path, dir, flags);
	if(stat(newpath, &statbuf) == 0) {
		// If this directory aleady exist we are done
		if(S_ISDIR(statbuf.st_mode)) {
			DEBUG(4, "Using existing dir\n");
			ret = 1;
			goto out;
		}
		else  {
			// A non-directory with this name already exist.
			DEBUG(4, "A non-dir called %s already exist\n", newpath);
			ret = -1;
			goto out;
		}
	}
	if(flags & CD_CREATE) {
		DEBUG(4, "Will try to create %s\n", newpath);
#ifdef _WIN32
		ret = _mkdir(newpath);
#else
		ret = mkdir(newpath, DEFFILEMODE | S_IXGRP | S_IXUSR | S_IXOTH);
#endif
	}
	else {
		ret = -1;
	}

out:
	return ret;
}

int OBEX_UnicodeToChar(uint8_t *c, const uint8_t *uc, int size)
{
	int n;

	if (uc == NULL || c == NULL)
		return -1;

	/* Make sure buffer is big enough! */
	for (n = 0; uc[n*2+1] != 0; n++);

	if (n >= size)
		return -1;

	for (n = 0; uc[n*2+1] != 0; n++)
		c[n] = uc[n*2+1];
	c[n] = 0;

	return 0;
}

int OBEX_CharToUnicode(uint8_t *uc, const uint8_t *c, int size)
{
	int len, n;

	if (uc == NULL || c == NULL)
		return -1;

	len = n = strlen((char *) c);
	if (n*2+2 > size)
		return -1;

	uc[n*2+1] = 0;
	uc[n*2] = 0;

	while (n--) {
		uc[n*2+1] = c[n];
		uc[n*2] = 0;
	}

	return (len * 2) + 2;
}
