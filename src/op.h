#ifndef _JIO_OP_H
#define _JIO_OP_H

#include <iou.h>
#include <thunk.h>

/* ergonomic helper for submitting a thunk_t as a cb+cb_data pair to iou_op_queue() */
static inline void op_queue(iou_t *iou, iou_op_t *op, thunk_t *thunk)
{
	return iou_op_queue(iou, op, (int (*)(void *))thunk->dispatch, thunk);
}

#endif
