/* What the stream implementations of core share. Not part of the public API. */
#ifndef TB_STREAM_INTERNAL_H
#define TB_STREAM_INTERNAL_H

#include "badger/stream.h"

/* The position a seek arrives at, from the current position and the size.
 * INVALID for an unknown origin or a position below zero, TOO_LARGE for one
 * past INT64_MAX. The output is unchanged on failure. */
tb_stream_result tb_stream_resolve_seek(uint64_t current, uint64_t size, int64_t offset,
                                        tb_stream_origin origin, uint64_t *position);

/* How many of `capacity` bytes lie in a stream of `size` bytes from `offset`:
 * zero at or past the end. */
size_t tb_stream_available(uint64_t size, uint64_t offset, size_t capacity);

#endif
