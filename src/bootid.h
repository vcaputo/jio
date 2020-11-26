#ifndef _JIO_BOOTID_H
#define _JIO_BOOTID_H

#include "thunk.h"

typedef struct iou_t iou_t;

THUNK_DECLARE(bootid_get, iou_t *, iou, char **, res_ptr, thunk_t *, closure);

#endif
