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
#include <inttypes.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdio_ext.h>

#include <zstd.h>
#include <zstd_errors.h>

#include <iou.h>
#include <thunk.h>

#include "journals.h"
#include "machid.h"
#include "verify-hashed-objects.h"

#include "upstream/journal-def.h"
#include "upstream/lookup3.h"
#include "upstream/siphash24.h"

/* This simply loads all hashed objects (field and data objects) and verifies their
 * hashes against their contents.  It doesn't examine entry item hashes and verify
 * they match the referenced objects, but maybe it should do that too.  If it adds
 * that ability, it probably makes sense to rename to verify-hashes.
 */

/* borrowed from systemd */
static uint64_t hash(Header *header, void *payload, uint64_t size)
{
	if (header->incompatible_flags & HEADER_INCOMPATIBLE_KEYED_HASH)
		return siphash24(payload, size, header->file_id.bytes);

	return jenkins_hash64(payload, size);
}

/* borrowed from systemd */
static int zstd_ret_to_errno(size_t ret) {
	switch (ZSTD_getErrorCode(ret)) {
	case ZSTD_error_dstSize_tooSmall:
		return -ENOBUFS;
	case ZSTD_error_memory_allocation:
		return -ENOMEM;
	default:
		return -EBADMSG;
	}
}


static int decompress(int compression, void *src, uint64_t src_size, void **dest, size_t *dest_size)
{
	uint64_t	size;
	ZSTD_DCtx	*dctx;

	assert(src);
	assert(src_size > 0);
	assert(dest);
	assert(dest_size);
	assert(compression & OBJECT_COMPRESSED_ZSTD);

/* vaguely borrowed from systemd */
	size = ZSTD_getFrameContentSize(src, src_size);
	if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN)
		return -EBADMSG;

	if (size > SIZE_MAX)
		return -E2BIG;

	if (malloc_usable_size(*dest) < size) {
		free(*dest);
		*dest = malloc(size);
		if (!*dest)
			return -ENOMEM;
	}

	dctx = ZSTD_createDCtx();
	if (!dctx) {
		free(*dest);
		return -ENOMEM;
	}

	ZSTD_inBuffer input = {
		.src = src,
		.size = src_size,
	};
	ZSTD_outBuffer output = {
		.dst = *dest,
		.size = size,
	};

	size_t k = ZSTD_decompressStream(dctx, &output, &input);
	if (ZSTD_isError(k)) {
		return zstd_ret_to_errno(k);
	}
	assert(output.pos >= size);

	*dest_size = size;

        return 0;
}


THUNK_DEFINE_STATIC(per_hashed_object, journal_t *, journal, Header *, header, Object **, iter_object, void **, decompressed, thunk_t *, closure)
{
	int		compression;
	uint64_t	payload_size, h;
	void		*payload;
	Object		*o;

	assert(iter_object && *iter_object);

	o = *iter_object;

	switch (o->object.type) {
	case OBJECT_FIELD:
		payload_size = o->object.size - offsetof(FieldObject, payload),
		payload = o->field.payload;
		break;
	case OBJECT_DATA:
		payload_size = o->object.size - offsetof(DataObject, payload),
		payload = o->data.payload;
		break;
	default:
		assert(0);
	}

	/* TODO: hash payload, compare to hash..
	 * this kind of cpu-bound work would benefit from a thread-pool, and it would be
	 * neat if iou abstracted such a thing as if it were just another iou_op, except
	 * for execution by worker threads it abstracted, which upon completion would get
	 * their associated closures dispatched as if it were any other iou_op being completed.
	 * as-is this work will delay iou_run() from getting called again until the hashing
	 * and decompression if needed will complete, which may have a serializing effect on
	 * the otherwise parallel-processed journals.
	 */

	compression = (o->object.flags & OBJECT_COMPRESSION_MASK);
	if (compression) {
		int	r;
		size_t	b_size;

		r = decompress(compression, payload, payload_size, decompressed, &b_size);
		if (r < 0)
			return r;

		h = hash(header, *decompressed, b_size);
	} else {
		h = hash(header, payload, payload_size);
	}

	if (h != o->data.hashed.hash) {
		printf("mismatch %"PRIx64" != %"PRIx64"\ncontents=\"%.*s\"\n",
			h, o->data.hashed.hash,
			(int)payload_size, payload);
		return -EBADMSG;
	}

	return thunk_end(thunk_dispatch(closure));
}


THUNK_DEFINE_STATIC(per_object, thunk_t *, self, iou_t *, iou, journal_t **, journal, Header *, header, uint64_t *, iter_offset, ObjectHeader *, iter_object_header, Object **, iter_object, void **, decompressed)
{
	assert(iter_offset);
	assert(iter_object_header);
	assert(iter_object);

	if (!*iter_offset) {
		free(*iter_object);
		free(*decompressed);
		*iter_object = *decompressed = NULL;
		return 0;
	}

	/* skip non-hashed objects */
	if (iter_object_header->type != OBJECT_FIELD && iter_object_header->type != OBJECT_DATA)
		return	thunk_mid(journal_iter_next_object(iou, journal, header, iter_offset, iter_object_header, self));

	if (malloc_usable_size(*iter_object) < iter_object_header->size) {
		free(*iter_object);

		*iter_object = malloc(iter_object_header->size);
		if (!*iter_object)
			return -ENOMEM;
	}

	return	thunk_mid(journal_get_object(iou, journal, iter_offset, &iter_object_header->size, iter_object, THUNK(
			per_hashed_object(*journal, header, iter_object, decompressed, THUNK(
				journal_iter_next_object(iou, journal, header, iter_offset, iter_object_header, self))))));
}


THUNK_DEFINE_STATIC(per_journal, iou_t *, iou, journal_t **, journal_iter)
{
	struct {
		journal_t	*journal;
		Header		header;
		uint64_t	iter_offset;
		ObjectHeader	iter_object_header;
		Object		*iter_object;
		void		*decompressed;
	} *foo;

	thunk_t		*closure;

	assert(iou);
	assert(journal_iter);

	closure = THUNK_ALLOC(per_object, (void **)&foo, sizeof(*foo));
	foo->journal = *journal_iter;
	foo->iter_object = foo->decompressed = NULL;

	return thunk_mid(journal_get_header(iou, &foo->journal, &foo->header, THUNK(
			journal_iter_next_object(iou, &foo->journal, &foo->header, &foo->iter_offset, &foo->iter_object_header, THUNK_INIT(
					per_object(closure, closure, iou, &foo->journal, &foo->header, &foo->iter_offset, &foo->iter_object_header, &foo->iter_object, &foo->decompressed))))));
}


/* verify the hashes of all "hashed objects" (field and data objects) */
int jio_verify_hashed_objects(iou_t *iou, int argc, char *argv[])
{
	char		*machid;
	journals_t	*journals;
	journal_t	*journal_iter;
	int		r;

	r = machid_get(iou, &machid, THUNK(
		journals_open(iou, &machid, O_RDONLY, &journals, THUNK(
			journals_for_each(&journals, &journal_iter, THUNK(
				per_journal(iou, &journal_iter)))))));
	if (r < 0)
		return r;

	r = iou_run(iou);
	if (r < 0)
		return r;

	return 0;
}
