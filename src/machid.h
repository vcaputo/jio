#ifndef _JIO_MACHID_H
#define _JIO_MACHID_H

#include "thunk.h"

typedef struct iou_t iou_t;

THUNK_DECLARE(machid_get, iou_t *, iou, char **, res_ptr, thunk_t *, closure);

#endif
