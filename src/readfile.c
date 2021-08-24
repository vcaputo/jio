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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <iou.h>
#include <thunk.h>

#include "readfile.h"
#include "op.h"

/* Implements iou-based file read into a supplied buffer.
 */


/* call user-supplied closure now that buf is populated */
THUNK_DEFINE_STATIC(read_done, iou_t *, iou, iou_op_t *, op, int, fd, char *, buf, size_t *, size, thunk_t *, closure)
{
	assert(iou);
	assert(op);
	assert(closure);

	if (op->result < 0)
		return op->result;

	(void) close(fd);

	*size = op->result;

	return thunk_end(thunk_dispatch(closure));
}


/* ask iou to read from the opened fd into buf */
THUNK_DEFINE_STATIC(open_done, iou_t *, iou, iou_op_t *, op, char *, buf, size_t *, size, thunk_t *, closure)
{
	thunk_t		*read_closure;
	iou_op_t	*read_op;

	assert(iou);
	assert(op);

	if (op->result < 0)
		return op->result;

	read_op = iou_op_new(iou);
	if (!read_op)
		return -ENOMEM;

	read_closure = THUNK(read_done(iou, read_op, op->result, buf, size, closure));
	if (!read_closure)
		return -ENOMEM;

	io_uring_prep_read(read_op->sqe, op->result, buf, *size, 0);
	op_queue(iou, read_op, read_closure);

	return 0;
}


/* read a file into a buffer via iou, dispatching closure when complete */
/* size specifies the buf size, and will also get the length of what's read
 * written to it upon completion.
 */
/* returns < 0 on error, 0 on successful queueing of operation */
THUNK_DEFINE(readfile, iou_t *, iou, const char *, path, char *, buf, size_t *, size, thunk_t *, closure)
{
	iou_op_t	*op;

	op = iou_op_new(iou);
	if (!op)
		return -ENOMEM;

	/* req iou to open path, passing the req to read its contents */
	io_uring_prep_openat(op->sqe, 0, path, 0, O_RDONLY);
	op_queue(iou, op, THUNK(open_done(iou, op, buf, size, closure)));

	return 0;
}
