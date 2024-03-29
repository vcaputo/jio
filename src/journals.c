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
#include <dirent.h>
#include <fcntl.h>
#include <liburing.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <iou.h>
#include <thunk.h>

#include "journals.h"
#include "op.h"

#include "upstream/journal-def.h"


#define PERSISTENT_PATH	"/var/log/journal"

#define JOURNAL_BUF_SIZE	8192
#define JOURNAL_BUF_CNT		8

#ifndef container_of
#define container_of(_ptr, _type, _member) \
	(_type *)((void *)(_ptr) - offsetof(_type, _member))
#endif


typedef struct journal_buf_t journal_buf_t;

struct journal_buf_t {
	journal_buf_t	*lru_prev, *lru_next;
	uint64_t	offset, length;
	unsigned	valid:1;
	int		idx;
	uint8_t		data[JOURNAL_BUF_SIZE];
};

typedef struct _journal_t {
	journal_t	public;
	journal_buf_t	*lru_head, *lru_tail;
	journal_buf_t	bufs[JOURNAL_BUF_CNT];
} _journal_t;

struct journals_t {
	int		dirfd;
	size_t		n_journals, n_allocated, n_opened;
	_journal_t	journals[];
};


/* helper for bumping buf down to the bottom of journal->lru_{head,tail},
 * this should probably just pull in some linked list header like the kernel's
 * list.h, open coded slop for now
 */
static void buf_used(_journal_t *journal, journal_buf_t *buf)
{
	assert(journal);
	assert(buf);

	if (journal->lru_tail == buf)
		return;

	if (buf->lru_prev)
		buf->lru_prev->lru_next = buf->lru_next;
	else
		journal->lru_head = buf->lru_next;

	buf->lru_next->lru_prev = buf->lru_prev;
	buf->lru_next = NULL;

	journal->lru_tail->lru_next = buf;
	buf->lru_prev = journal->lru_tail;
	journal->lru_tail = buf;
}


THUNK_DEFINE_STATIC(buf_got_read, iou_t *, iou, iou_op_t *, op, void *, dest, uint64_t, offset, uint64_t, length, journal_buf_t *, buf, thunk_t *, closure)
{
	assert(iou);
	assert(op);
	assert(closure);
	assert(dest || !buf);

	if (op->result < 0)
		return op->result;

	if (op->result < length)
		return -EINVAL;

	if (buf) {
		memcpy(dest, buf->data, length);
		buf->length = op->result;
		buf->offset = offset;
		buf->valid = 1;
	}

	return thunk_end(thunk_dispatch(closure));
}


/* read size bytes from offset offset in journal to dest, dispatch closure when done.
 * for reads <= JOURNAL_BUF_SIZE in size, a per-journal cache of
 * JOURNAL_BUF_CNT*JOURNAL_BUF_SIZE buffers is maintained and if the data is
 * present it's simply copied from there before dispatching closure.
 */
int journal_read(iou_t *iou, journal_t *journal, uint64_t offset, uint64_t length, void *dest, thunk_t *closure)
{
	_journal_t	*_journal = container_of(journal, _journal_t, public);
	journal_buf_t	*buf;
	iou_op_t	*op;

	assert(iou);
	assert(journal);
	assert(length);
	assert(dest);
	assert(closure);

	if (length <= JOURNAL_BUF_SIZE) {
		/* small enough to fit, look in the buffers */

		for (int i = 0; i < JOURNAL_BUF_CNT; i++) {
			journal_buf_t	*buf = &_journal->bufs[i];

			if (!buf->valid)
				continue;

			if (offset >= buf->offset && offset + length <= buf->offset + buf->length) {
				buf_used(_journal, buf);
				memcpy(dest, &buf->data[offset - buf->offset], length);

				return thunk_end(thunk_dispatch(closure));
			}
		}

		/* buffer fits, but wasn't found, read it into the "fixed" lru buf,
		 * buf_got_read() will then copy out of the buf into dest when loaded.
		 */
		buf = _journal->lru_head;
		buf_used(_journal, buf);
		buf->valid = 0;

		op = iou_op_new(iou);
		if (!op)
			return -ENOMEM;

		io_uring_prep_read_fixed(op->sqe, journal->idx, buf->data, JOURNAL_BUF_SIZE, offset, buf->idx);
		op->sqe->flags = IOSQE_FIXED_FILE;
		op_queue(iou, op, THUNK(buf_got_read(iou, op, dest, offset, length, buf, closure)));
	} else {
		/* buffer doesn't fit, plain unbuffered read it into provided dest (assumed to be non-fixed)  */

		op = iou_op_new(iou);
		if (!op)
			return -ENOMEM;

		io_uring_prep_read(op->sqe, journal->idx, dest, length, offset);
		op->sqe->flags = IOSQE_FIXED_FILE;
		op_queue(iou, op, THUNK(buf_got_read(iou, op, NULL, offset, length, NULL, closure)));
	}

	return 0;
}


/* an open on journal->name was attempted, result in op->result.
 * bump *journals->n_opened, when it matches *journals->n_journals, dispatch closure
 */
THUNK_DEFINE_STATIC(opened_journal, iou_t *, iou, iou_op_t *, op, journals_t *, journals, _journal_t *, _journal, thunk_t *, closure)
{
	journal_t	*journal = &_journal->public;

	assert(iou);
	assert(op);
	assert(journals);
	assert(_journal);
	assert(closure);

	/* note n_opened is a count of open calls, not successes, the closure only gets called
	 * when all the opens have been performed hence the need to count them.
	 */
	journals->n_opened++;

	if (op->result < 0 ) {
		if (op->result != -EPERM && op->result != -EACCES)
			return op->result;

		fprintf(stderr, "Permission denied opening \"%s\", ignoring\n", journal->name);
		journal->fd = -1;
	} else {

		/* setup journal->lru_{head,tail} bufs list */
		_journal->lru_head = _journal->lru_tail = &_journal->bufs[0];
		for (int i = 1; i < JOURNAL_BUF_CNT; i++) {
			journal_buf_t	*buf = &_journal->bufs[i];

			_journal->lru_tail->lru_next = buf;
			buf->lru_prev = _journal->lru_tail;
			_journal->lru_tail = buf;
		}

		journal->fd = op->result;
	}

	if (journals->n_opened == journals->n_journals) {
		int	*fds, r;

		/* "register" all the opened journals, storing their respective
		 * index in journal->idx for convenience.
		 */
		fds = malloc(sizeof(*fds) * journals->n_journals);
		if (!fds)
			return -ENOMEM;

		for (int i = 0; i < journals->n_journals; i++) {
			fds[i] = journals->journals[i].public.fd;
			journals->journals[i].public.idx = i;
		}

		r = io_uring_register_files(iou_ring(iou), fds, journals->n_journals);
		if (r < 0)
			return r;

		{
			struct iovec	*bufs;
			int		n_bufs = 0;

			bufs = malloc(sizeof(*bufs) * JOURNAL_BUF_CNT * journals->n_journals);
			if (!bufs)
				return -ENOMEM;

			for (int i = 0; i < journals->n_journals; i++) {
				/* "register" buffers with io_uring for a perf boost */


				for (int j = 0; j < JOURNAL_BUF_CNT; j++) {
					journal_buf_t	*buf = &journals->journals[i].bufs[j];

					bufs[n_bufs].iov_base = buf->data;
					bufs[n_bufs].iov_len = JOURNAL_BUF_SIZE;
					buf->idx = n_bufs++;
				}
			}

			r = io_uring_register_buffers(iou_ring(iou), bufs, n_bufs);
			if (r < 0) {
				free(bufs);
				return r;
			}

			free(bufs);
		}

		free(fds);

		return thunk_end(thunk_dispatch(closure));
	}

	return 0;
}


THUNK_DEFINE_STATIC(get_journals, iou_t *, iou, iou_op_t *, op, journals_t **, journals, int, flags, thunk_t *, closure)
{
	struct dirent	*dent;
	DIR		*jdir;
	size_t		n = 0;
	journals_t	*j;
	int		r;

	assert(iou);
	assert(op);
	assert(journals);
	assert(closure);

	if (op->result < 0)
		return op->result;

	/* I don't see any readdir/getdents ops for io_uring, so just do the opendir/readdir
	 * synchronously here before queueing the opening of all those paths.
	 */
	jdir = fdopendir(op->result);
	if (jdir == NULL) {
		int	r = errno;

		close(op->result); /* only on successful fdopendir is the fd owned and closed by jdir */

		return r;
	}

	while ((dent = readdir(jdir))) {
		if (dent->d_name[0] == '.')	/* just skip dot files and "." ".." */
			continue;

		n++;
	}
	rewinddir(jdir);

	if (!n)	/* no journals! */
		return 0;

	j = calloc(1, sizeof(journals_t) + sizeof(j->journals[0]) * n);
	if (!j) {
		closedir(jdir);
		return -ENOMEM;
	}

	j->n_allocated = n;

	while ((dent = readdir(jdir))) {
		if (dent->d_name[0] == '.')	/* just skip dot files and "." ".." */
			continue;

		j->journals[j->n_journals++].public.name = strdup(dent->d_name);

		assert(j->n_journals <= j->n_allocated);// FIXME this defends against the race, but the race
							// is a normal thing and should be handled gracefully,
							// or just stop doing this two pass dance.
	}

	/* duplicate the dirfd for openat use later on, since closedir() will close op->result */
	j->dirfd = dup(op->result);
	closedir(jdir);

	/* to keep things relatively simple, let's ensure there's enough queue space for at least one
	 * operation per journal.
	 */
	r = iou_resize(iou, j->n_journals);
	if (r < 0)
		return r;	/* TODO: cleanup j */

	/* we have a list of journal names, now queue requests to open them all */
	for (size_t i = 0; i < j->n_journals; i++) {
		iou_op_t	*oop;

		oop = iou_op_new(iou);
		if (!oop)
			return -ENOMEM;	// FIXME: cleanup

		io_uring_prep_openat(oop->sqe, j->dirfd, j->journals[i].public.name, flags, 0);
		op_queue(iou, oop, THUNK(opened_journal(iou, oop, j, &j->journals[i], closure)));
	}

	iou_flush(iou);

	/* stow the journals where they can be found, but note they aren't opened yet. */
	*journals = j;

	return 0;
}


THUNK_DEFINE_STATIC(var_opened, iou_t *, iou, iou_op_t *, op, char **, machid, journals_t **, journals, int, flags, thunk_t *, closure)
{
	iou_op_t	*mop;

	if (op->result < 0)
		return op->result;

	/* PERSISTENT_PATH is opened, req open ./$machid */
	mop = iou_op_new(iou);
	if (!mop)
		return -ENOMEM;

	/* req iou to open dir, passing the req to read and open its contents */
	io_uring_prep_openat(mop->sqe, op->result, *machid, O_DIRECTORY|O_RDONLY, 0);
	op_queue(iou, mop, THUNK(get_journals(iou, mop, journals, flags, closure)));

	return 0;
}


/* requesting opening the journals via iou, allocating the resulting journals_t @ *journals */
/* returns < 0 on error, 0 on successful queueing of operation */
THUNK_DEFINE(journals_open, iou_t *, iou, char **, machid, int, flags, journals_t **, journals, thunk_t *, closure)
{
	iou_op_t	*op;

	assert(iou);
	assert(machid);
	assert(journals);
	assert(closure);

	op = iou_op_new(iou);
	if (!op)
		return -ENOMEM;

	/* req iou to open PERSISTENT_PATH, passing on to var_opened */
	io_uring_prep_openat(op->sqe, 0, PERSISTENT_PATH, O_DIRECTORY|O_RDONLY, 0);
	op_queue(iou, op, THUNK(var_opened(iou, op, machid, journals, flags, closure)));

	return 0;
}



THUNK_DEFINE_STATIC(got_iter_object_header, iou_t *, iou, journal_t *, journal, uint64_t *, iter_offset, ObjectHeader *, iter_object_header, thunk_t *, closure)
{
	assert(iou);
	assert(journal);
	assert(iter_offset);
	assert(iter_object_header);
	assert(closure);

	iter_object_header->size = le64toh(iter_object_header->size);

	return thunk_end(thunk_dispatch(closure));
}


/* Queues IO on iou for getting the next object from (*journal)->fd
 * addressed by state in {iter_offset,iter_object_header} storing it @
 * *iter_object_header which must already have space allocated to accomodate
 * sizeof(ObjectHeader), registering closure for dispatch when completed.  This
 * only performs a single step, advancing the offset in *iter_offset by
 * *iter_object_header->size, ready for continuation in a subsequent call.
 *
 * When (*iter_offset == 0) it's considered the initial iteration, as this is
 * an invalid object offset (0=the journal header), the first object of the
 * journal will be loaded into *iter_object_header without advancing anything -
 * ignoring the initial contents of *iter_object_header.
 *
 * When closure gets dispatched with (*iter_offset == 0), there's no more
 * objects in journal, and *iter_object_header will have been unmodified.
 * Hence closure should always check if (*iter_offset == 0) before accessing
 * *iter_object_header, and do whatever is appropriate upon reaching the end of
 * the journal.  If journal_iter_next_object() recurs after reaching this
 * point, it will restart iterating from the first object of the journal.
 */
THUNK_DEFINE(journal_iter_next_object, iou_t *, iou, journal_t **, journal, Header *, header, uint64_t *, iter_offset, ObjectHeader *, iter_object_header, thunk_t *, closure)
{
	assert(iou);
	assert(journal);
	assert(iter_offset);
	assert(iter_object_header);
	assert(closure);

	/* restart iterating when entered with (*iter_offset == 0) */
	if (!(*iter_offset))
		*iter_offset = header->header_size;
	else {
		if (iter_object_header->size)
			*iter_offset += ALIGN64(iter_object_header->size);
		else {
			fprintf(stderr, "Encountered zero-sized object, journal \"%s\" appears corrupt, ignoring remainder\n", (*journal)->name);
			*iter_offset = header->tail_object_offset + 1;
		}
	}

	/* final dispatch if past tail object */
	if (*iter_offset > header->tail_object_offset) {
		*iter_offset = 0;
		return thunk_dispatch(closure);
	}

	return	journal_read(iou, *journal, *iter_offset, sizeof(ObjectHeader), iter_object_header, THUNK(
			got_iter_object_header(iou, *journal, iter_offset, iter_object_header, closure)));
}


/* Helper for the journal_iter_objects() simple objects iteration. */
THUNK_DEFINE_STATIC(journal_iter_objects_dispatch, iou_t *, iou, journal_t **, journal, Header *, header, uint64_t *, iter_offset, ObjectHeader *, iter_object_header, thunk_t *, closure)
{
	int	r;

	r = thunk_dispatch(closure);
	if (r < 0)
		return r;

	if (!(*iter_offset))
		return 0;

	return	thunk_end(journal_iter_next_object(iou, journal, header, iter_offset, iter_object_header, THUNK(
			journal_iter_objects_dispatch(iou, journal, header, iter_offset, iter_object_header, closure))));
}


/* Convenience journal object iterator, dispatches thunk for every object in the journal.
 *
 * Note in this wrapper there's no way to control/defer the iterator, your
 * closure is simply dispatched in a loop, no continuation is passed to it for
 * resuming iteration, which tends to limit its use to simple situations.
 */
THUNK_DEFINE(journal_iter_objects, iou_t *, iou, journal_t **, journal, Header *, header, uint64_t *, iter_offset, ObjectHeader *, iter_object_header, thunk_t *, closure)
{
	assert(iter_offset);

	*iter_offset = 0;

	return	journal_iter_next_object(iou, journal, header, iter_offset, iter_object_header, THUNK(
			journal_iter_objects_dispatch(iou, journal, header, iter_offset, iter_object_header, closure)));
}


THUNK_DEFINE_STATIC(got_hash_table_iter_object_header, iou_t *, iou, journal_t *, journal, HashItem *, hash_table, uint64_t, nbuckets, uint64_t *, iter_bucket, uint64_t *, iter_offset, HashedObjectHeader *, iter_object_header, size_t, iter_object_size, thunk_t *, closure)
{
	assert(iou);
	assert(journal);
	assert(hash_table);
	assert(iter_bucket && *iter_bucket < nbuckets);
	assert(iter_offset);
	assert(iter_object_header);
	assert(closure);

	iter_object_header->object.size = le64toh(iter_object_header->object.size);
	iter_object_header->hash = le64toh(iter_object_header->hash);
	iter_object_header->next_hash_offset = le64toh(iter_object_header->next_hash_offset);

	if (iter_object_size > sizeof(HashedObjectHeader)) {
		/* The caller can iterate either the field or data hash tables,
		 * so just introspect and handle those two... using a size >
		 * than the minimum HashedObjectHeader as a heuristic.
		 * Scenarios that only need what's available in HashedObjectHeader
		 * can bypass all this and only read in the HashedObjectHeader
		 * while iterating.
		 */
		switch (iter_object_header->object.type) {
		case OBJECT_DATA: {
			DataObject	*data_object = (DataObject *)iter_object_header;

			assert(iter_object_size >= sizeof(DataObject));

			data_object->next_field_offset = le64toh(data_object->next_field_offset);
			data_object->entry_offset = le64toh(data_object->entry_offset);
			data_object->entry_array_offset = le64toh(data_object->entry_array_offset);
			data_object->n_entries = le64toh(data_object->n_entries);
			break;
		}
		case OBJECT_FIELD: {
			FieldObject	*field_object = (FieldObject *)iter_object_header;

			assert(iter_object_size >= sizeof(FieldObject));

			field_object->head_data_offset = le64toh(field_object->head_data_offset);
			break;
		}
		default:
			assert(0);
		}
	}

	return thunk_end(thunk_dispatch(closure));
}


/* Queues IO on iou for getting the next object from hash_table{_size}
 * positioned by state {iter_bucket,iter_offset,iter_object_header} storing it
 * @ *iter_object_header which must already have space allocated to accomodate
 * iter_object_size, registering closure for dispatch when completed.  This
 * only performs a single step, advancing the state in
 * {iter_bucket,iter_offset} appropriate for continuing via another call.
 *
 * When (*iter_offset == 0) it's considered the initial iteration, as this is
 * an invalid object offset (0=the journal header), and the first object of the
 * table will be loaded into *iter_object_header without advancing anything.
 *
 * When closure gets dispatched with (*iter_offset == 0), there's no more
 * objects in hash_table, and *iter_object_header will have been unmodified.
 * Hence closure should always check if (*iter_offset == 0) before using
 * *iter_object_header, and do whatever is appropriate upon reaching the end of
 * the hash table.  If journal_hash_table_iter_next_object() recurs after
 * reaching this point, it will restart iterating from the first object of the
 * table.
 */
THUNK_DEFINE(journal_hash_table_iter_next_object, iou_t *, iou, journal_t **, journal, HashItem **, hash_table, uint64_t *, hash_table_size, uint64_t *, iter_bucket, uint64_t *, iter_offset, HashedObjectHeader *, iter_object_header, size_t, iter_object_size, thunk_t *, closure)
{
	size_t		nbuckets;

	assert(iou);
	assert(journal);
	assert(hash_table);
	assert(hash_table_size && *hash_table_size >= sizeof(HashItem));
	assert(iter_bucket);
	assert(iter_offset);
	assert(iter_object_header);
	assert(iter_object_size >= sizeof(HashedObjectHeader));
	assert(closure);

	nbuckets = *hash_table_size / sizeof(HashItem);

	assert(*iter_bucket < nbuckets);

	/* restart iterating when entered with (*iter_offset == 0) */
	if (!*iter_offset)
		*iter_bucket = 0;

	if (*iter_offset && *iter_offset != (*hash_table)[*iter_bucket].tail_hash_offset) {
		*iter_offset = iter_object_header->next_hash_offset;
	} else {
		do {
			(*iter_bucket)++;
			if (*iter_bucket >= nbuckets) {
				*iter_offset = 0;
				return thunk_end(thunk_dispatch(closure));
			}
			(*iter_offset) = (*hash_table)[*iter_bucket].head_hash_offset;
		} while (!(*iter_offset));
	}

	return	journal_read(iou, *journal, *iter_offset, iter_object_size, iter_object_header, THUNK(
			got_hash_table_iter_object_header(iou, *journal, *hash_table, *hash_table_size, iter_bucket, iter_offset, iter_object_header, iter_object_size, closure)));
}


/* Helper for the journal_hash_table_for_each() simple hash table iteration. */
THUNK_DEFINE_STATIC(journal_hash_table_for_each_dispatch, iou_t *, iou, journal_t **, journal, HashItem **, hash_table, uint64_t *, hash_table_size, uint64_t *, iter_bucket, uint64_t *, iter_offset, HashedObjectHeader *, iter_object_header, size_t, iter_object_size, thunk_t *, closure)
{
	int	r;

	r = thunk_dispatch(closure);
	if (r < 0)
		return r;

	if (!*iter_offset)
		return 0;

	return	thunk_end(journal_hash_table_iter_next_object(iou, journal, hash_table, hash_table_size, iter_bucket, iter_offset, iter_object_header, iter_object_size, THUNK(
			journal_hash_table_for_each_dispatch(iou, journal, hash_table, hash_table_size, iter_bucket, iter_offset, iter_object_header, iter_object_size, closure))));
}


/* Convenience hash table iterator, dispatches thunk for every object in the hash table.
 * iter_object_size would typically be sizeof(DataObject) or sizeof(FieldObject), as
 * these are the two hash table types currently handled.  The size is supplied rather
 * than a type, because one may iterate by only loading the HashedObjectHeader portion.
 * The size is introspected to determine if more than just the HashedObjectHeader is being
 * loaded, and its type asserted to fit in the supplied space.
 *
 * Note in this wrapper there's no way to control/defer the iterator, your
 * closure is simply dispatched in a loop, no continuation is passed to it for
 * resuming iteration, which tends to limit its use to simple situations.
 */
THUNK_DEFINE(journal_hash_table_for_each, iou_t *, iou, journal_t **, journal, HashItem **, hash_table, uint64_t *, hash_table_size, uint64_t *, iter_bucket, uint64_t *, iter_offset, HashedObjectHeader *, iter_object_header, size_t, iter_object_size, thunk_t *, closure)
{
	assert(iter_offset);

	*iter_offset = 0;

	return	journal_hash_table_iter_next_object(iou, journal, hash_table, hash_table_size, iter_bucket, iter_offset, iter_object_header, iter_object_size, THUNK(
			journal_hash_table_for_each_dispatch(iou, journal, hash_table, hash_table_size, iter_bucket, iter_offset, iter_object_header, iter_object_size, closure)));
}


THUNK_DEFINE_STATIC(got_hashtable, iou_t *, iou, HashItem *, table, uint64_t, size, HashItem **, res_hash_table, thunk_t *, closure)
{
	assert(iou);
	assert(table);
	assert(res_hash_table);
	assert(closure);

	for (uint64_t i = 0; i < size / sizeof(HashItem); i++) {
		table[i].head_hash_offset = le64toh(table[i].head_hash_offset);
		table[i].tail_hash_offset = le64toh(table[i].tail_hash_offset);
	}
	/* TODO: validation/sanity checks? */

	*res_hash_table = table;

	return thunk_end(thunk_dispatch(closure));
}


/* Queue IO on iou for loading a journal's hash table from *journal into memory allocated and stored @ *res_hash_table.
 * Registers closure for dispatch when completed.
 */
THUNK_DEFINE(journal_get_hash_table, iou_t *, iou, journal_t **, journal, uint64_t *, hash_table_offset, uint64_t *, hash_table_size, HashItem **, res_hash_table, thunk_t *, closure)
{
	HashItem	*table;

	assert(iou);
	assert(journal);
	assert(hash_table_offset);
	assert(hash_table_size);
	assert(res_hash_table);
	assert(closure);

	table = malloc(*hash_table_size);
	if (!table)
		return -ENOMEM;

	return	journal_read(iou, *journal, *hash_table_offset, *hash_table_size, table, THUNK(
			got_hashtable(iou, table, *hash_table_size, res_hash_table, closure)));
}


/* Validate and prepare journal header loaded via journal_get_header @ header, dispatch closure. */
THUNK_DEFINE_STATIC(got_header, iou_t *, iou, journal_t *, journal, Header *, header, thunk_t *, closure)
{
	assert(iou);
	assert(journal);
	assert(header);
	assert(closure);

	header->compatible_flags = le32toh(header->compatible_flags);
	header->incompatible_flags = le32toh(header->incompatible_flags);
	header->header_size = le64toh(header->header_size);
	header->arena_size = le64toh(header->arena_size);
	header->data_hash_table_offset = le64toh(header->data_hash_table_offset);
	header->data_hash_table_size = le64toh(header->data_hash_table_size);
	header->field_hash_table_offset = le64toh(header->field_hash_table_offset);
	header->field_hash_table_size = le64toh(header->field_hash_table_size);
	header->tail_object_offset = le64toh(header->tail_object_offset);
	header->n_objects = le64toh(header->n_objects);
	header->n_entries = le64toh(header->n_entries);
	header->tail_entry_seqnum = le64toh(header->tail_entry_seqnum);
	header->head_entry_seqnum = le64toh(header->head_entry_seqnum);
	header->entry_array_offset = le64toh(header->entry_array_offset);
	header->head_entry_realtime = le64toh(header->head_entry_realtime);
	header->tail_entry_realtime = le64toh(header->tail_entry_realtime);
	header->tail_entry_monotonic = le64toh(header->tail_entry_monotonic);
	header->n_data = le64toh(header->n_data);
	header->n_fields = le64toh(header->n_fields);
	header->n_tags = le64toh(header->n_tags);
	header->n_entry_arrays = le64toh(header->n_entry_arrays);
	header->data_hash_chain_depth = le64toh(header->data_hash_chain_depth);
	header->field_hash_chain_depth = le64toh(header->field_hash_chain_depth);
	/* TODO: validation/sanity checks? */

	return thunk_end(thunk_dispatch(closure));
}


/* Queue IO on iou for loading a journal's header from *journal into *header.
 * Registers closure for dispatch when completed.
 */
THUNK_DEFINE(journal_get_header, iou_t *, iou, journal_t **, journal, Header *, header, thunk_t *, closure)
{
	assert(iou);
	assert(journal);
	assert(closure);

	return	journal_read(iou, *journal, 0, sizeof(*header), header, THUNK(
			got_header(iou, *journal, header, closure)));
}


/* Validate and prepare object header loaded via journal_get_object_header @ object_header, dispatch closure. */
THUNK_DEFINE_STATIC(got_object_header, iou_t *, iou, ObjectHeader *, object_header, thunk_t *, closure)
{
	assert(iou);
	assert(object_header);
	assert(closure);

	object_header->size = le64toh(object_header->size);
	/* TODO: validation/sanity checks? */

	return thunk_end(thunk_dispatch(closure));
}


/* Queue IO on iou for loading an object header from *journal @ offset *offset, into *object_header.
 * Registers closure for dispatch on the io when completed.
 */
THUNK_DEFINE(journal_get_object_header, iou_t *, iou, journal_t **, journal, uint64_t *, offset, ObjectHeader *, object_header, thunk_t *, closure)
{
	assert(iou);
	assert(journal);
	assert(offset);
	assert(object_header);
	assert(closure);

	return	journal_read(iou, *journal, *offset, sizeof(*object_header), object_header, THUNK(
			got_object_header(iou, object_header, closure)));
}

#define OBJECT_N_ITEMS(_o)	\
	((_o.object.size - offsetof(typeof(_o), items)) / sizeof(*_o.items))

/* Validate and prepare object loaded via journal_get_object @ object, dispatch closure. */
THUNK_DEFINE_STATIC(got_object, iou_t *, iou, uint64_t, size, Object *, object, thunk_t *, closure)
{
	assert(iou);
	assert(object);
	assert(closure);

	object->object.size = le64toh(object->object.size);

	/* TODO: validation/sanity checks? */
	switch (object->object.type) {
	case OBJECT_DATA:
		object->data.hashed.hash = le64toh(object->data.hashed.hash);
		object->data.hashed.next_hash_offset = le64toh(object->data.hashed.next_hash_offset);
		object->data.next_field_offset = le64toh(object->data.next_field_offset);
		object->data.entry_offset = le64toh(object->data.entry_offset);
		object->data.entry_array_offset = le64toh(object->data.entry_array_offset);
		object->data.n_entries = le64toh(object->data.n_entries);
		break;

	case OBJECT_FIELD:
		object->field.hashed.hash = le64toh(object->field.hashed.hash);
		object->field.hashed.next_hash_offset = le64toh(object->field.hashed.next_hash_offset);
		object->field.head_data_offset = le64toh(object->field.head_data_offset);
		break;

	case OBJECT_ENTRY:
		object->entry.seqnum = le64toh(object->entry.seqnum);
		object->entry.realtime = le64toh(object->entry.realtime);
		object->entry.monotonic = le64toh(object->entry.monotonic);
		//object->entry.boot_id
		object->entry.xor_hash = le64toh(object->entry.xor_hash);
		for (uint64_t i = 0, n_items = OBJECT_N_ITEMS(object->entry); i < n_items; i++) {
			object->entry.items[i].object_offset = le64toh(object->entry.items[i].object_offset);
			object->entry.items[i].hash = le64toh(object->entry.items[i].hash);
		}
		break;

	case OBJECT_DATA_HASH_TABLE:
	case OBJECT_FIELD_HASH_TABLE:
		for (uint64_t i = 0, n_items = OBJECT_N_ITEMS(object->hash_table); i < n_items; i++) {
			object->hash_table.items[i].head_hash_offset = le64toh(object->hash_table.items[i].head_hash_offset);
			object->hash_table.items[i].tail_hash_offset = le64toh(object->hash_table.items[i].tail_hash_offset);
		}
		break;

	case OBJECT_ENTRY_ARRAY:
		object->entry_array.next_entry_array_offset = le64toh(object->entry_array.next_entry_array_offset);
		for (uint64_t i = 0, n_items = OBJECT_N_ITEMS(object->entry_array); i < n_items; i++)
			object->entry_array.items[i] = le64toh(object->entry_array.items[i]);
		break;

	case OBJECT_TAG:
		object->tag.seqnum = le64toh(object->tag.seqnum);
		object->tag.epoch = le64toh(object->tag.epoch);
		break;

	default:
		/* XXX: should probably just ignore unknown types instead,
		 * but the idea here is to let callers safely assume loaded objects
		 * have been fully validated and byteswapped as needed.
		 */
		assert(0);
	}

	return thunk_end(thunk_dispatch(closure));
}


/* Queue IO on iou for loading an entire object of size *size from *journal @ offset *offset, into *object
 * which must already be allocated.
 * Registers closure for dispatch on the io when completed.
 *
 * Note this doesn't allocate space for the object and requires the size be already known, it is the bare
 * minimum object loading into pre-allocated space when the size is known, which performs the necessary
 * le64toh() swapping of object-specific members before calling the supplied closure.
 *
 * It's expected that the caller must already retrieve the object's header in a separate step before
 * calling this to load the entirety of the object, since the header is needed first to know the size
 * for allocating the full object and then loading its contents.  Another heavier helper will be provided
 * for doing both the header load followed by the entire object load in one convenient step.
 */
THUNK_DEFINE(journal_get_object, iou_t *, iou, journal_t **, journal, uint64_t *, offset, uint64_t *, size, Object **, object, thunk_t *, closure)
{
	assert(iou);
	assert(journal);
	assert(offset);
	assert(size);
	assert(object && *object);
	assert(closure);

	return	journal_read(iou, *journal, *offset, *size, *object, THUNK(
			got_object(iou, *size, *object, closure)));
}


THUNK_DEFINE_STATIC(get_object_full_got_header, iou_t *, iou, journal_t **, journal, uint64_t *, offset, ObjectHeader *, object_header, Object **, object, thunk_t *, closure)
{
	Object	*o;

	o = malloc(object_header->size);
	if (!o)
		return -ENOMEM;

	*object = o;

	return journal_get_object(iou, journal, offset, &object_header->size, object, closure);
}


/* Queue IO on iou for loading an object header into *object_header, which must already be allocated,
 * registering a closure to then allocate space for the full object @ *object and queueing IO for loading
 * the full object into that space, with closure registered for dispatch once the full object is loaded.
 *
 * This will leave a newly allocated and populated object @ *object, ready for use.
 */
THUNK_DEFINE(journal_get_object_full, iou_t *, iou, journal_t **, journal, uint64_t *, offset, ObjectHeader *, object_header, Object **, object, thunk_t *, closure)
{
	return	journal_get_object_header(iou, journal, offset, object_header, THUNK(
			get_object_full_got_header(iou, journal, offset, object_header, object, closure)));
}


/* for every open journal in *journals, store the journal in *journal_iter and dispatch closure */
/* closure must expect to be dispatched multiple times; once per journal, and will be freed once at end */
THUNK_DEFINE(journals_for_each, journals_t **, journals, journal_t **, journal_iter, thunk_t *, closure)
{
	journals_t	*j;

	assert(journals && *journals);
	assert(journal_iter);
	assert(closure);

	j = *journals;

	for (size_t i = 0; i < j->n_journals; i++) {
		int	r;

		if (j->journals[i].public.fd < 0)
			continue;

		(*journal_iter) = &j->journals[i].public;

		r = thunk_dispatch(closure);
		if (r < 0)
			return r;
	}

	thunk_free(closure);

	return 0;
}


const char * journal_object_type_str(ObjectType type)
{
	static const char *names[] = {
		"UNUSED",
		"Data",
		"Field",
		"Entry",
		"DataHashTable",
		"FieldHashTable",
		"EntryArray",
		"Tag",
	};

	if (type < 0 || type >= sizeof(names) / sizeof(*names))
		return "UNKNOWN";

	return names[type];
}


const char * journal_state_str(JournalState state)
{
	static const char *names[] = {
		"Offline",
		"Online",
		"Archived",
	};

	if (state < 0 || state >= sizeof(names) / sizeof(*names))
		return "UNKNOWN";

	return names[state];
}
