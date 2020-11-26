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
#include <stdio.h>
#include <stdint.h>

#include <iou.h>
#include <thunk.h>

#include "bootid.h"
#include "humane.h"
#include "journals.h"
#include "machid.h"
#include "report-tail-waste.h"

#include "upstream/journal-def.h"


typedef struct tail_waste_t {
	unsigned	per_state_counts[_STATE_MAX];
	uint64_t	per_state_bytes[_STATE_MAX];
	uint64_t	total, total_file_size;
	unsigned	n_journals;
} tail_waste_t;


THUNK_DEFINE_STATIC(print_tail_waste, journal_t **, journal, Header *, journal_header, ObjectHeader *, tail_object_header, tail_waste_t *, tail_waste)
{
	uint64_t	sz, tail;
	struct stat	st;
	int		r;
	humane_t	h1, h2;

	assert(journal);
	assert(journal_header);
	assert(tail_object_header);
	assert(tail_waste);

	r = fstat((*journal)->fd, &st);
	if (r < 0)
		return r;

	sz = st.st_size;
	tail = journal_header->tail_object_offset + ALIGN64(tail_object_header->size);

	printf("\t%s: %s, size: %s, tail-waste: %s\n",
		journal_state_str(journal_header->state),
		(*journal)->name,
		humane_bytes(&h1, sz),
		humane_bytes(&h2, sz - tail));

	tail_waste->per_state_bytes[journal_header->state] += sz - tail;
	tail_waste->total += sz - tail;
	tail_waste->total_file_size += sz;
	tail_waste->per_state_counts[journal_header->state]++;
	tail_waste->n_journals++;

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

	closure = THUNK_ALLOC(print_tail_waste, (void **)&foo, sizeof(*foo));
	foo->journal = *journal_iter;

	return journal_get_header(iou, &foo->journal, &foo->header, THUNK(
			journal_get_object_header(iou, &foo->journal, &foo->header.tail_object_offset, &foo->tail_object_header, THUNK_INIT(
				print_tail_waste(closure, &foo->journal, &foo->header, &foo->tail_object_header, tail_waste)))));
}


/* print the size of wasted space between each journal's tail object and EOF, and a sum total. */
int jio_report_tail_waste(iou_t *iou, int argc, char *argv[])
{
	tail_waste_t	tail_waste = {};
	char		*machid;
	journals_t	*journals;
	journal_t	*journal_iter;
	humane_t	h1, h2;
	int		r;

	r = machid_get(iou, &machid, THUNK(
		journals_open(iou, &machid, O_RDONLY, &journals, THUNK(
			journals_for_each(&journals, &journal_iter, THUNK(
				per_journal_tail_waste(iou, &journal_iter, &tail_waste)))))));
	if (r < 0)
		return r;

	printf("\nPer-journal:\n");
	r = iou_run(iou);
	if (r < 0)
		return r;

	printf("\nTotals:\n");
	printf("\tTail-waste by state:\n");
	for (int i = 0; i < _STATE_MAX; i++) {
		printf("\t\t%10s [%u]: %s, %"PRIu64"%% of all tail-waste\n",
			journal_state_str(i),
			tail_waste.per_state_counts[i],
			humane_bytes(&h1, tail_waste.per_state_bytes[i]),
			tail_waste.total >= 100 ? tail_waste.per_state_bytes[i] / (tail_waste.total / 100) : 0);
	}

	printf("\n\tAggregate tail-waste: %s, %"PRIu64"%% of %s spanning %u journal files\n",
		humane_bytes(&h1, tail_waste.total),
		tail_waste.total_file_size >= 100 ? tail_waste.total / (tail_waste.total_file_size / 100) : 0,
		humane_bytes(&h2, tail_waste.total_file_size),
		tail_waste.n_journals);

	return 0;
}
