/*
 * errno.c - Per-process errno variable definition.
 *
 * A single global int suffices until pthreads are introduced, at which
 * point this should become __thread int errno for per-thread storage.
 */

#include "errno.h"

int errno = 0;
