#ifndef _JIO_READFILE_H
#define _JIO_READFILE_H

#include "thunk.h"

typedef struct iou_t iou_t;

THUNK_DECLARE(readfile, iou_t *, iou, const char *, path, char *, buf, size_t *, size, thunk_t *, closure);

#endif
