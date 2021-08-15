/*
 *  Copyright (C) 2021 - Vito Caputo - <vcaputo@pengaru.com>
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

/* `jio report entry-arrays` attempts to characterize how wasteful the
 * EntryArrayObject objects are across all accessible journals.
 *
 * Currently it just gives some basic insights into how many of these
 * objects have identical payloads, which can both waste space and harm
 * performance by blowing out caches during journal searches involving
 * entry array chains of multiple data objects.  Especially if they tend to
 * occur in the larger and commonly searched entry arrays, it might make
 * sense to explore some sharing technique.
 *
 * It also gives rudimentary utilization numbers.  Entry arrays grow
 * quadratically as an optimization, which can result in very poor utilization
 * %ages when the latest entry array is first created, if it never fills up
 * before being archived, especially if it's in a long entry array chain where
 * the latest doubling produced a large allocation.
 *
 * When archiving journals, journald should likely punch holes in the unused
 * areas of large EntryArrayObjects to reclaim osme of that space.  This
 * subcommand helps give a sense of how much space would be reclaimed.
 */

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include <iou.h>
#include <openssl/sha.h>
#include <thunk.h>

#include "humane.h"
#include "journals.h"
#include "machid.h"
#include "op.h"
#include "report-entry-arrays.h"

#include "upstream/journal-def.h"

#define N_BUCKETS	(64 * 1024)

typedef struct entry_array_t {
	struct entry_array_t	*next;
	unsigned char		digest[SHA_DIGEST_LENGTH];
	uint64_t		count, size, utilized;
} entry_array_t;

typedef struct entry_array_stats_t {
	uint64_t	count, unique;
	entry_array_t	*buckets[N_BUCKETS];
} entry_array_stats_t;


THUNK_DEFINE_STATIC(per_entry_array_payload, iou_t *, iou, iou_op_t *, op, uint64_t, payload_size, char *, payload_buf, entry_array_stats_t *, stats, thunk_t *, closure)
{
	unsigned char	digest[SHA_DIGEST_LENGTH];
	int		bucket = 0;
	SHA_CTX		ctx;
	entry_array_t	*ea;

	assert(iou);
	assert(payload_size);
	assert(payload_buf);

	if (op->result < 0)
		return op->result;

	if (op->result != payload_size)
		return -EINVAL;

	SHA1_Init(&ctx);
	SHA1_Update(&ctx, payload_buf, payload_size);
	SHA1_Final(digest, &ctx);

	/* this is a cheesy way to turn the digest into a bucket id */
	for (int i = 0; i < sizeof(digest); i++) {
		bucket += digest[i];
		bucket %= N_BUCKETS;
	}

	for (ea = stats->buckets[bucket]; ea; ea = ea->next) {
		if (!memcmp(ea->digest, digest, sizeof(digest)))
			break;
	}

	if (!ea) {
		ea = calloc(1, sizeof(*ea));
		if (!ea)
			return -ENOMEM;

		{
			le64_t	*items = (le64_t *)payload_buf, utilized = 0;

			for (int i = 0; i < payload_size / sizeof(le64_t); i++) {
				if (items[i])
					utilized += sizeof(le64_t);
			}

			ea->utilized = utilized;
		}

		memcpy(ea->digest, digest, sizeof(digest));
		ea->size = payload_size;
		ea->next = stats->buckets[bucket];
		stats->buckets[bucket] = ea;
		stats->unique++;
	}

	ea->count++;

	free(payload_buf);

	return thunk_dispatch(closure);
}


/* this is derived from journal_iter_objects_dispatch(), and frankly the need for a separate dispatch
 * thunk is pretty much entirely because thunk.h doesn't have a more streamlined means of controlling
 * thunk instance life-cycles.  If the return value could control freeing in thunk_dispatch(), I don't
 * think this kruft would exist at all.  But in the interest of just making things work for now, leave
 * it be and do this junk TODO FIXME
 * XXX also, if this manual dispatch sticks around, journals.[ch] should prolly just export this variant
 * for the manual iter cases...
 */
THUNK_DEFINE_STATIC(per_object_dispatch, uint64_t *, iter_offset, thunk_t *, closure)
{
	if (!(*iter_offset))
		return thunk_dispatch(closure);

	return thunk_dispatch_keep(closure);
}


/* borrowed from systemd upstream basic/util.h */
static inline unsigned u64log2(uint64_t n) {
#if __SIZEOF_LONG_LONG__ == 8
	return (n > 1) ? (unsigned) __builtin_clzll(n) ^ 63U : 0;
#else
#error "Wut?"
#endif
}


THUNK_DEFINE_STATIC(per_object, thunk_t *, self, uint64_t *, iter_offset, ObjectHeader *, iter_object_header, iou_t *, iou, journal_t **, journal, Header *, header, entry_array_stats_t *, stats)
{
	assert(self);
	assert(iter_offset);
	assert(iter_object_header);

	if (!(*iter_offset)) { /* end of journal, print stats */
		struct {
			uint64_t	total;
			union {
				uint64_t unique;
				uint64_t utilized;
			};
		} log2_size_counts[64] = {}, log2_size_bytes[64] = {}, log2_size_utilized[64] = {};

		for (int i = 0; i < N_BUCKETS; i++) {
			for (entry_array_t *ea = stats->buckets[i]; ea; ea = ea->next) {
				unsigned l2sz = u64log2(ea->size);

				log2_size_counts[l2sz].unique++;
				log2_size_counts[l2sz].total += ea->count;

				log2_size_bytes[l2sz].unique = ea->size;
				log2_size_bytes[l2sz].total = ea->size * ea->count;

				log2_size_utilized[l2sz].total += ea->size * ea->count;
				log2_size_utilized[l2sz].utilized += ea->utilized * ea->count;
			}
		}

		printf("\n\nEntry-array stats for \"%s\":\n", (*journal)->name);
		printf("  Total EAs: %"PRIu64"\n", stats->count);
		printf("  Unique EAs: %"PRIu64" (%%%.1f)\n", stats->unique, stats->count ? (float)stats->unique / (float)stats->count * 100.f : 0.f);
		printf("  log2(size) counts (%%unique[total,unique] ...): ");

		for (int i = 0; i < 64; i++) {
			if (!log2_size_counts[i].total)
				printf("[] ");
			else
				printf("%.1f%%[%"PRIu64",%"PRIu64"] ",
					log2_size_counts[i].total ? (float)log2_size_counts[i].unique / (float)log2_size_counts[i].total * 100.f : 0.f,
					log2_size_counts[i].total,
					log2_size_counts[i].unique);
		}
		printf("\n");

		printf("  log2(size) sizes (%%unique[total,unique] ...): ");
		for (int i = 0; i < 64; i++) {
			humane_t	h1, h2;

			if (!log2_size_bytes[i].total)
				printf("[] ");
			else
				printf("%.1f%%[%s,%s] ",
					log2_size_bytes[i].total ? (float)log2_size_bytes[i].unique / (float)log2_size_bytes[i].total * 100.f : 0.f,
					humane_bytes(&h1, log2_size_bytes[i].total),
					humane_bytes(&h2, log2_size_bytes[i].unique));
		}
		printf("\n");

		printf("  log2(size) utilization (%%used[total,used] ...): ");
		for (int i = 0; i < 64; i++) {
			humane_t	h1, h2;

			if (!log2_size_utilized[i].total)
				printf("[] ");
			else
				printf("%.1f%%[%s,%s] ",
					log2_size_utilized[i].total ? (float)log2_size_utilized[i].utilized / (float)log2_size_utilized[i].total * 100.f : 0.f,
					humane_bytes(&h1, log2_size_utilized[i].total),
					humane_bytes(&h2, log2_size_utilized[i].utilized));
		}
		printf("\n");

		return 0;
	}

	/* skip non-entry-array objects */
	if (iter_object_header->type != OBJECT_ENTRY_ARRAY)
		return journal_iter_next_object(iou, journal, header, iter_offset, iter_object_header, THUNK(
				per_object_dispatch(iter_offset, self)));

	stats->count++;

	/* We need to load the actual entry array payload so we can hash it for
	 * counting duplicates, so allocate space for that and queue the op.
	 */
	{
		iou_op_t	*op;
		char		*buf;
		size_t		payload_size = iter_object_header->size - offsetof(EntryArrayObject, items);

		buf = malloc(payload_size);
		if (!buf)
			return -ENOMEM;

		op = iou_op_new(iou);
		if (!op)
			return -ENOMEM;

		io_uring_prep_read(op->sqe, (*journal)->fd, buf, payload_size, (*iter_offset) + offsetof(EntryArrayObject, items));
		op_queue(iou, op, THUNK(
			per_entry_array_payload(iou, op, payload_size, buf, stats, THUNK(
				journal_iter_next_object(iou, journal, header, iter_offset, iter_object_header, THUNK(
					per_object_dispatch(iter_offset, self)))))));
	}


	return 0;
}


THUNK_DEFINE_STATIC(per_journal, iou_t *, iou, journal_t **, journal_iter)
{
	struct {
		journal_t		*journal;
		Header			header;
		uint64_t		iter_offset;
		ObjectHeader		iter_object_header;
		entry_array_stats_t	stats;
	} *foo;

	thunk_t	*closure;

	assert(iou);
	assert(journal_iter);

	closure = THUNK_ALLOC(per_object, (void **)&foo, sizeof(*foo));
	foo->journal = *journal_iter;

	return journal_get_header(iou, &foo->journal, &foo->header, THUNK(
			journal_iter_next_object(iou, &foo->journal, &foo->header, &foo->iter_offset, &foo->iter_object_header, THUNK(
				per_object_dispatch(&foo->iter_offset, THUNK_INIT(
					per_object(closure, closure, &foo->iter_offset, &foo->iter_object_header, iou, &foo->journal, &foo->header, &foo->stats)))))));
}


/* print stats about entry arrays per journal */
int jio_report_entry_arrays(iou_t *iou, int argc, char *argv[])
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
