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

#include <stdio.h>
#include <string.h>

#include <iou.h>

#include "reclaim-tail-waste.h"
#include "report-entry-arrays.h"
#include "report-layout.h"
#include "report-tail-waste.h"
#include "report-usage.h"

#include "upstream/journal-def.h"

/* jio - journal-file input/output tool */

/* Copyright (c) 2020 Vito Caputo <vcaputo@pengaru.com> */

/* XXX: This is a WIP experiment, use at your own risk! XXX */

int main(int argc, char *argv[])
{
	iou_t	*iou;
	int	r;

	if (argc < 2) {
		printf("Usage: %s {help,reclaim,report} [subcommand-args]\n", argv[0]);
		return 0;
	}

	iou = iou_new(8);
	if (!iou)
		return 1;

	/* FIXME TODO This is ad-hoc open-coded jank for now */
	if (!strcmp(argv[1], "help")) {
		printf(
			"\n"
			" help                 show this help\n"
			" license              print license header\n"
			" reclaim [subcmd]     reclaim space from journal files\n"
			"         tail-waste   reclaim wasted space from tails of archives\n"
			"\n"
			" report  [subcmd]     report statistics about journal files\n"
			"         entry-arrays report statistics about entry array objects per journal\n"
			"         layout       report layout of objects, writes a .layout file per journal\n"
			"         usage        report space used by various object types\n"
			"         tail-waste   report extra space allocated onto tails\n"
			" version              print jio version\n"
			"\n"
		);
		return 0;
	} else if (!strcmp(argv[1], "license")) {
		printf(
			"\n"
			" Copyright (C) 2020 - Vito Caputo - <vcaputo@pengaru.com>\n"
			"\n"
			" This program is free software: you can redistribute it and/or modify it\n"
			" under the terms of the GNU General Public License version 3 as published\n"
			" by the Free Software Foundation.\n"
			"\n"
			" This program is distributed in the hope that it will be useful,\n"
			" but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
			" MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
			" GNU General Public License for more details.\n"
			"\n"
			" You should have received a copy of the GNU General Public License\n"
			" along with this program.  If not, see <http://www.gnu.org/licenses/>.\n"
			"\n"
		);
		return 0;
	} else if (!strcmp(argv[1], "reclaim")) {
		if (argc < 3) {
			printf("Usage: %s reclaim {tail-waste}\n", argv[0]);
			return 0;
		}

		if (!strcmp(argv[2], "tail-waste")) {
			r = jio_reclaim_tail_waste(iou, argc, argv);
			if (r < 0) {
				fprintf(stderr, "failed to reclaim tail waste: %s\n", strerror(-r));
				return 1;
			}
		} else {
			fprintf(stderr, "Unsupported reclaim subcommand: \"%s\"\n", argv[2]);
			return 1;
		}
	} else if (!strcmp(argv[1], "report")) {
		if (argc < 3) {
			printf("Usage: %s report {layout,usage,tail-waste}\n", argv[0]);
			return 0;
		}

		if (!strcmp(argv[2], "entry-arrays")) {
			r = jio_report_entry_arrays(iou, argc, argv);
			if (r < 0) {
				fprintf(stderr, "failed to report entry arrays: %s\n", strerror(-r));
				return 1;
			}
		} else if (!strcmp(argv[2], "layout")) {
			r = jio_report_layout(iou, argc, argv);
			if (r < 0) {
				fprintf(stderr, "failed to report layout: %s\n", strerror(-r));
				return 1;
			}
		} else if (!strcmp(argv[2], "tail-waste")) {
			r = jio_report_tail_waste(iou, argc, argv);
			if (r < 0) {
				fprintf(stderr, "failed to report tail waste: %s\n", strerror(-r));
				return 1;
			}
		} else if (!strcmp(argv[2], "usage")) {
			r = jio_report_usage(iou, argc, argv);
			if (r < 0) {
				fprintf(stderr, "failed to report usage: %s\n", strerror(-r));
				return 1;
			}
		} else {
			fprintf(stderr, "Unsupported report subcommand: \"%s\"\n", argv[2]);
			return 1;
		}
	} else if (!strcmp(argv[1], "version")) {
		puts("jio version " VERSION);
		return 0;
	} else {
		fprintf(stderr, "Unsupported subcommand: \"%s\"\n", argv[1]);
		return 1;
	}

	if (!r) {
		/* XXX: note this is a successful noop if there's no outstanding io */
		r = iou_run(iou);
		if (r < 0)
			fprintf(stderr, "iou error: %s\n", strerror(-r));
	}

	iou_free(iou);

	return r != 0;
}
