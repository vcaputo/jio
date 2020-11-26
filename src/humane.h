#ifndef _JIO_HUMANE_H
#define _JIO_HUMANE_H

#include <stdint.h>

typedef struct humane_t {
	char	buf[sizeof("1023.99 EiB")];
} humane_t;

char * humane_bytes(humane_t *humane, uint64_t bytes);

#endif
