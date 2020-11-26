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

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "humane.h"

/* Print bytes as a "human-readable" string in Si storage units into humane->buf and return it. */
char * humane_bytes(humane_t *humane, uint64_t bytes)
{
	double	z = bytes;

	static const char * units[] = {
		"B",
		"KiB",
		"MiB",
		"GiB",
		"TiB",
		"PiB",
		"EiB",
	};
	int		order = 0;

	while (z >= 1024) {
		order++;
		z /= 1024;
	}

	/* FIXME: isn't there a format specifier for adaptive precision? where %.2 means
	 * use a maximum of two digits but only extend a non-zero fraction up to that limit,
	 * i.e. don't produce outputs like 1.00. but produce 1.1 or 1.01, but 1.00 should be 1.
	 * I can't remember if there's a double format to do that, and can't waste more time
	 * reading the printf(3) man page.
	 */
	snprintf(humane->buf, sizeof(humane->buf), "%.2f %s", z, units[order]);

	return humane->buf;
}


#if 0
/* TODO: when/if unit tests become a thing in this tree, turn this into one of them and assert the
 * stringified "humane" outputs match expectations.
 */

#define U64(x)	UINT64_C(x)
int main(int argc, char *argv[])
{
	uint64_t	nums[] = {
		0,
		U64(1),
		U64(512),
		U64(1024),
		U64(1024) + U64(512),
		U64(1024) * U64(1024),
		U64(1024) * U64(1024) * U64(1024),
		U64(1024) * U64(1024) * U64(1024) * U64(1024),
		U64(1024) * U64(1024) * U64(1024) * U64(1024) * U64(1024),
		U64(1024) * U64(1024) * U64(1024) * U64(1024) * U64(1024) * U64(1024),
		UINT64_MAX,
	};
	humane_t		humane = {};

	for (int i = 0; i < sizeof(nums) / sizeof(*nums); i++)
		printf("%"PRIu64" humane: %s\n", nums[i], humane_bytes(&humane, nums[i]));
}
#endif
