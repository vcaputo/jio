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
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <iou.h>
#include <thunk.h>

#include "bootid.h"
#include "humane.h"
#include "journals.h"
#include "machid.h"
#include "reclaim-tail-waste.h"

#include "upstream/journal-def.h"


typedef struct tail_waste_t {
	unsigned	n_journals, n_reclaimed, n_ignored, n_errored, n_mules;
	uint64_t	reclaimed_bytes, ignored_bytes, errored_bytes;
} tail_waste_t;


THUNK_DEFINE_STATIC(reclaim_tail_waste, journal_t **, journal, Header *, journal_header, ObjectHeader *, tail_object_header, tail_waste_t *, tail_waste)
{
	uint64_t	sz, tail;
	struct stat	st;
	int		r;
	humane_t	h1;

	assert(journal);
	assert(journal_header);
	assert(tail_object_header);
	assert(tail_waste);

	r = fstat((*journal)->fd, &st);
	if (r < 0)
		return r;

	tail_waste->n_journals++;

	sz = st.st_size;
	tail = journal_header->tail_object_offset + ALIGN64(tail_object_header->size);

	if (sz == tail) {
		tail_waste->n_mules++;

		return 0;
	}

	if (journal_header->state != STATE_ARCHIVED) {
		printf("Ignoring %s of tail-waste on \"%s\" for not being archived (state=%s)\n",
			humane_bytes(&h1, sz - tail),
			(*journal)->name,
			journal_state_str(journal_header->state));

		tail_waste->n_ignored++;
		tail_waste->ignored_bytes += sz - tail;

		return 0;
	}

	if (ftruncate((*journal)->fd, tail) < 0) {
		fprintf(stderr, "Unable to truncate \"%s\" to %"PRIu64", ignoring: %s\n",
			(*journal)->name,
			tail,
			strerror(errno));

		tail_waste->n_errored++;
		tail_waste->errored_bytes += sz - tail;

		return 0;
	}

	tail_waste->n_reclaimed++;
	tail_waste->reclaimed_bytes += sz - tail;

	return 0;
}


THUNK_DEFINE_STATIC(per_journal_tail_waste, iou_t *, iou, journal_t **, journal_iter, tail_waste_t *, tail_waste)
{
	struct {
		journal_t	*journal;
		Header		header;
		ObjectHeader	tail_object_header;
	} *foo;

	thunk_t	*closure;

	assert(iou);
	assert(journal_iter);

	closure = THUNK_ALLOC(reclaim_tail_waste, (void **)&foo, sizeof(*foo));
	foo->journal = *journal_iter;

	return journal_get_header(iou, &foo->journal, &foo->header, THUNK(
			journal_get_object_header(iou, &foo->journal, &foo->header.tail_object_offset, &foo->tail_object_header, THUNK_INIT(
				reclaim_tail_waste(closure, &foo->journal, &foo->header, &foo->tail_object_header, tail_waste)))));
}


/* print the size of wasted space between each journal's tail object and EOF, and a sum total. */
int jio_reclaim_tail_waste(iou_t *iou, int argc, char *argv[])
{
	char		*machid;
	journals_t	*journals;
	journal_t	*journal_iter;
	tail_waste_t	tail_waste = {};
	int		r;
	humane_t	h1;

	r = machid_get(iou, &machid, THUNK(
		journals_open(iou, &machid, O_RDWR, &journals, THUNK(
			journals_for_each(&journals, &journal_iter, THUNK(
				per_journal_tail_waste(iou, &journal_iter, &tail_waste)))))));
	if (r < 0)
		return r;

	printf("\nReclaiming tail-waste...\n");
	r = iou_run(iou);
	if (r < 0)
		return r;

	printf("\nSummary:\n");
	if (!tail_waste.n_journals)
		printf("\tNo journal files opened!\n");

	if (tail_waste.n_mules)
		printf("\tSkipped %u journal files free of tail-waste\n",
			tail_waste.n_mules);

	if (tail_waste.n_ignored)
		printf("\tIgnored %u unarchived journal files totalling %s of tail-waste\n",
			tail_waste.n_ignored,
			humane_bytes(&h1, tail_waste.ignored_bytes));

	if (tail_waste.n_reclaimed)
		printf("\tReclaimed %s from %u journal files\n",
			humane_bytes(&h1, tail_waste.reclaimed_bytes),
			tail_waste.n_reclaimed);

	if (tail_waste.n_errored)
		printf("\tFailed to relcaim %s from %u journal files due to errors\n",
			humane_bytes(&h1, tail_waste.errored_bytes),
			tail_waste.n_errored);

	return 0;
}
