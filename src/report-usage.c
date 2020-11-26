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
#include <stdint.h>
#include <stdio.h>

#include <iou.h>
#include <thunk.h>

#include "bootid.h"
#include "humane.h"
#include "journals.h"
#include "machid.h"
#include "report-usage.h"

#include "upstream/journal-def.h"


typedef struct usage_t {
	uint64_t	count_per_type[_OBJECT_TYPE_MAX];
	uint64_t	use_per_type[_OBJECT_TYPE_MAX];
	uint64_t	use_total;
	uint64_t	file_size, file_count;
} usage_t;


THUNK_DEFINE_STATIC(per_data_object, ObjectHeader *, iter_object_header, usage_t *, usage, usage_t *, total_usage)
{
	assert(iter_object_header);
	assert(usage);
	assert(total_usage);

	usage->count_per_type[iter_object_header->type]++;
	usage->use_per_type[iter_object_header->type] += iter_object_header->size;
	usage->use_total += iter_object_header->size;

	total_usage->count_per_type[iter_object_header->type]++;
	total_usage->use_per_type[iter_object_header->type] += iter_object_header->size;
	total_usage->use_total += iter_object_header->size;

	return 0;
}


THUNK_DEFINE_STATIC(per_journal, iou_t *, iou, journal_t **, journal_iter, usage_t *, total_usage, unsigned *, n_journals)
{
	struct {
		journal_t		*journal;
		Header			header;
		usage_t			usage;
		uint64_t		iter_offset;
		ObjectHeader		iter_object_header;
	} *foo;

	thunk_t		*closure;
	struct stat	st;
	int		r;

	assert(iou);
	assert(journal_iter);
	assert(total_usage);

	/* XXX: io_uring has a STATX opcode, so this too could be async */
	r = fstat((*journal_iter)->fd, &st);
	if (r < 0)
		return r;

	closure = THUNK_ALLOC(per_data_object, (void **)&foo, sizeof(*foo));
	foo->journal = *journal_iter;
	foo->usage.file_size = st.st_size;

	total_usage->file_size += st.st_size;
	(*n_journals)++;

	return journal_get_header(iou, &foo->journal, &foo->header, THUNK(
			journal_for_each(iou, &foo->journal, &foo->header, &foo->iter_offset, &foo->iter_object_header, THUNK_INIT(
				per_data_object(closure, &foo->iter_object_header, &foo->usage, total_usage)))));
}


/* print the amount of space used by various objects per journal, and sum totals */
int jio_report_usage(iou_t *iou, int argc, char *argv[])
{
	usage_t		aggregate_usage = {};
	char		*machid;
	journals_t	*journals;
	journal_t	*journal_iter;
	unsigned	n_journals = 0;
	humane_t	h1, h2;
	int		r;

	r = machid_get(iou, &machid, THUNK(
		journals_open(iou, &machid, O_RDONLY, &journals, THUNK(
			journals_for_each(&journals, &journal_iter, THUNK(
				per_journal(iou, &journal_iter, &aggregate_usage, &n_journals)))))));
	if (r < 0)
		return r;

	r = iou_run(iou);
	if (r < 0)
		return r;

	printf("Per-object-type usage:\n");
	for (int i = 0; i < _OBJECT_TYPE_MAX; i++)
		printf("%16s: [%"PRIu64"] %s\n",
			journal_object_type_str(i),
			aggregate_usage.count_per_type[i],
			humane_bytes(&h1,
			aggregate_usage.use_per_type[i]));

	printf("Aggregate object usage: %s of %s spanning %u journal files\n",
		humane_bytes(&h1, aggregate_usage.use_total),
		humane_bytes(&h2, aggregate_usage.file_size),
		n_journals);

	return 0;
}
