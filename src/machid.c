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

#include "machid.h"
#include "readfile.h"

/* Implements iou-based machine-id retrieval.
 *
 * this is basically copy & paste from bootid.c,
 * which suggests this should just be readfile.c
 */


#define MACHID_PATH "/etc/machine-id"


/* call user-supplied closure now that buf is populated */
THUNK_DEFINE_STATIC(have_machid, iou_t *, iou, char *, buf, size_t *, size, char **, res_ptr, thunk_t *, closure)
{
	assert(iou);
	assert(closure);

	/* FIXME: I'm just assuming it's a proper nl-terminated machid for now, null terminate it */
	buf[*size - 1] = '\0';

	*res_ptr = strdup(buf);
	if (!*res_ptr)
		return -ENOMEM;

	return thunk_dispatch(closure);
}


/* get a machid via iou, scheduling closure once gotten */
/* returns < 0 on error, 0 on successful queueing of operation */
THUNK_DEFINE(machid_get, iou_t *, iou, char **, res_ptr, thunk_t *, closure)
{
	thunk_t	*machid_thunk;
	struct {
		char	buf[4096];
		size_t	len;
	}	*buf;

	machid_thunk = THUNK_ALLOC(have_machid, (void **)&buf, sizeof(*buf));
	buf->len = sizeof(buf->buf);
	THUNK_INIT(have_machid(machid_thunk, iou, buf->buf, &buf->len, res_ptr, closure));

	return readfile(iou, MACHID_PATH, buf->buf, &buf->len, machid_thunk);
}
