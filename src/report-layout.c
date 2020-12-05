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
#include "report-layout.h"

#include "upstream/journal-def.h"


static const char type_map[_OBJECT_TYPE_MAX] = {
	[OBJECT_UNUSED] = '?',
	[OBJECT_DATA] = 'd',
	[OBJECT_FIELD] = 'f',
	[OBJECT_ENTRY] = 'e',
	[OBJECT_DATA_HASH_TABLE] = 'D',
	[OBJECT_FIELD_HASH_TABLE] = 'F',
	[OBJECT_ENTRY_ARRAY] = 'A',
	[OBJECT_TAG] = 'T',
};

/* TODO: this should be either argv settable or just determined at runtime */
#define PAGE_SIZE 4096

THUNK_DEFINE_STATIC(per_data_object, uint64_t *, iter_offset, ObjectHeader *, iter_object_header, FILE *, out)
{
	char		boundary_marker[22] = "";
	char		alignment_marker[3] = "";
	uint64_t	off, this_page, next_page, page_delta, aligned_delta;

	assert(iter_offset);
	assert(iter_object_header);
	assert(out);

	off = *iter_offset;

	if (!off) {
		fprintf(out, "\n");
		fflush(out);
		fclose(out);
		return 0;
	}


	this_page = off & ~(PAGE_SIZE-1);
	next_page = (off + iter_object_header->size + PAGE_SIZE-1) & ~(PAGE_SIZE-1);
	page_delta = next_page - this_page;

	if (page_delta > PAGE_SIZE * 2)
		snprintf(boundary_marker, sizeof(boundary_marker), "|%"PRIu64"|", page_delta / PAGE_SIZE - 1);
	else if (page_delta > PAGE_SIZE)
		snprintf(boundary_marker, sizeof(boundary_marker), "|");

	aligned_delta = ALIGN64(iter_object_header->size) - iter_object_header->size;

	if (aligned_delta > 1)
		snprintf(alignment_marker, sizeof(alignment_marker), "+%"PRIu64, aligned_delta);
	else if (aligned_delta)
		snprintf(alignment_marker, sizeof(alignment_marker), "+");

	fprintf(out, "%s%c%s%"PRIu64"%s ",
		this_page == off ? "| " : "",
		type_map[iter_object_header->type],
		boundary_marker,
		iter_object_header->size,
		alignment_marker);

	return 0;
}


THUNK_DEFINE_STATIC(per_journal, iou_t *, iou, journal_t **, journal_iter)
{
	struct {
		journal_t	*journal;
		Header		header;
		uint64_t	iter_offset;
		ObjectHeader	iter_object_header;
		FILE		*out;
	} *foo;

	thunk_t		*closure;
	char		*fname;
	FILE		*f;

	assert(iou);
	assert(journal_iter);

	fname = malloc(strlen((*journal_iter)->name) + sizeof(".layout"));
	if (!fname)
		return -ENOMEM;

	sprintf(fname, "%s.layout", (*journal_iter)->name);
	f = fopen(fname, "w+");
	free(fname);
	if (!f)
		return -errno;

	fprintf(f, "Layout for \"%s\"\n", (*journal_iter)->name);
	fprintf(f,
		"Legend:\n"
		"%c     OBJECT_UNUSED\n"
		"%c     OBJECT_DATA\n"
		"%c     OBJECT_FIELD\n"
		"%c     OBJECT_ENTRY\n"
		"%c     OBJECT_DATA_HASH_TABLE\n"
		"%c     OBJECT_FIELD_HASH_TABLE\n"
		"%c     OBJECT_ENTRY_ARRAY\n"
		"%c     OBJECT_TAG\n\n"
		"|N|    object spans N page boundaries (page size used=%u)\n"
		"|      single page boundary\n"
		"+N     N bytes of alignment padding\n"
		"+      single byte alignment padding\n\n",

		type_map[OBJECT_UNUSED],
		type_map[OBJECT_DATA],
		type_map[OBJECT_FIELD],
		type_map[OBJECT_ENTRY],
		type_map[OBJECT_DATA_HASH_TABLE],
		type_map[OBJECT_FIELD_HASH_TABLE],
		type_map[OBJECT_ENTRY_ARRAY],
		type_map[OBJECT_TAG],
		PAGE_SIZE);

	closure = THUNK_ALLOC(per_data_object, (void **)&foo, sizeof(*foo));
	foo->journal = *journal_iter;
	foo->out = f;

	return journal_get_header(iou, &foo->journal, &foo->header, THUNK(
			journal_iter_objects(iou, &foo->journal, &foo->header, &foo->iter_offset, &foo->iter_object_header, THUNK_INIT(
				per_data_object(closure, &foo->iter_offset, &foo->iter_object_header, foo->out)))));
}


/* print the layout of contents per journal */
int jio_report_layout(iou_t *iou, int argc, char *argv[])
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
