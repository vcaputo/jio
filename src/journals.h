#ifndef _JIO_JOURNALS_H
#define _JIO_JOURNALS_H

#include <stdint.h>

/* open() includes since journals_open() reuses open() flags */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


#include "thunk.h"

#include "upstream/journal-def.h"

typedef struct iou_t iou_t;

typedef struct journal_t {
	char	*name;
	int	fd;
} journal_t;

typedef struct journals_t journals_t;

THUNK_DECLARE(journals_open, iou_t *, iou, char **, machid, int, flags, journals_t **, journals, thunk_t *, closure);
THUNK_DECLARE(journal_get_header, iou_t *, iou, journal_t **, journal, Header *, header, thunk_t *, closure);

THUNK_DECLARE(journal_iter_next_object, iou_t *, iou, journal_t **, journal, Header *, header, uint64_t *, iter_offset, ObjectHeader *, iter_object_header, thunk_t *, closure);
THUNK_DECLARE(journal_iter_objects, iou_t *, iou, journal_t **, journal, Header *, header, uint64_t *, iter_offset, ObjectHeader *, iter_object_header, thunk_t *, closure);

THUNK_DECLARE(journal_get_hash_table, iou_t *, iou, journal_t **, journal, uint64_t *, hash_table_offset, uint64_t *, hash_table_size, HashItem **, res_hash_table, thunk_t *, closure);
THUNK_DECLARE(journal_hash_table_iter_next_object, iou_t *, iou, journal_t **, journal, HashItem **, hash_table, uint64_t *, hash_table_size, uint64_t *, iter_bucket, uint64_t *, iter_offset, HashedObjectHeader *, iter_object_header, size_t, iter_object_size, thunk_t *, closure);
THUNK_DECLARE(journal_hash_table_for_each, iou_t *, iou, journal_t **, journal, HashItem **, hash_table, uint64_t *, hash_table_size, uint64_t *, iter_bucket, uint64_t *, iter_offset, HashedObjectHeader *, iter_object_header, size_t, iter_object_size, thunk_t *, closure);

THUNK_DECLARE(journal_get_object_header, iou_t *, iou, journal_t **, journal, uint64_t *, offset, ObjectHeader *, object_header, thunk_t *, closure);
THUNK_DECLARE(journals_for_each, journals_t **, journals, journal_t **, journal_iter, thunk_t *, closure);

const char * journal_object_type_str(ObjectType type);
const char * journal_state_str(JournalState state);

#endif
