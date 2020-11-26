/*
 *  Copyright (C) 2020 - Vito Caputo - <vcaputo@pengaru.com>
 *
 *  This program is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 3 as published
 *  by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <fcntl.h>
#include <liburing.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <iou.h>
#include <thunk.h>

#include "bootid.h"
#include "readfile.h"

/* Implements iou-based bootid retrieval.
 *
 * Note there's not really any reason for doing this async vio iou, it just
 * served as the initial test/use case while iou was fleshed out, so here it
 * is, no harm in using it.  I guess it's kind of convenient to have a little
 * kicking of the tires iou/io_uring user at the start of the program anyways,
 * rather than having something more complex to debug io_uring issues out of
 * the hole.  At least if the bootid is retrieved there's some evidence of the
 * basic machinery working.
 */


#define BOOTID_PATH "/proc/sys/kernel/random/boot_id"


/* perform in-place removal of hyphens from str */
static void dehyphen(char *str)
{
	char	*dest;

	for (dest = str; *str; str++) {
		if (*str == '-')
			continue;

		*dest++ = *str;
	}

	*dest = '\0';
}

/* call user-supplied closure now that buf is populated, after some baking of the data */
THUNK_DEFINE_STATIC(have_bootid, iou_t *, iou, char *, buf, size_t *, size, char **, res_ptr, thunk_t *, closure)
{
	assert(iou);
	assert(closure);

	/* FIXME: I'm just assuming it's a proper nl-terminated bootid for now, null terminate it */
	buf[*size - 1] = '\0';

	dehyphen(buf);	/* replicating what systemd does with the bootid */

	*res_ptr = strdup(buf);
	if (!*res_ptr)
		return -ENOMEM;

	return thunk_dispatch(closure);
}


/* get a bootid via iou, scheduling closure once gotten */
/* returns < 0 on error, 0 on successful queueing of operation */
THUNK_DEFINE(bootid_get, iou_t *, iou, char **, res_ptr, thunk_t *, closure)
{
	thunk_t	*bootid_thunk;
	struct {
		char	buf[4096];
		size_t	len;
	}	*buf;

	/* we need a buffer for readfiles, it only needs to last until have_bootid,
	 * where it gets null terminated and strdup()d, so allocae it as part of
	 * the have_bootid thunk.  The THUNK_ALLOC/THUNK_INIT interfaces are still
	 * evolving.
	 */
	bootid_thunk = THUNK_ALLOC(have_bootid, (void **)&buf, sizeof(*buf));
	buf->len = sizeof(buf->buf);
	THUNK_INIT(have_bootid(bootid_thunk, iou, buf->buf, &buf->len, res_ptr, closure));

	return readfile(iou, BOOTID_PATH, buf->buf, &buf->len, bootid_thunk);
}
