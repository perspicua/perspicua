#ifndef _UACCESS_H_
#define _UACCESS_H_

#include "types.h"

// returns the number of bytes that could not be copied (0 on success)
int copy_from_user(void* dest, const void* src, size_t n);
int copy_to_user(void* dest, const void* src, size_t n);

// returns 1 if a fixup was applied, 0 otherwise
int fixup_exception(struct trap_frame* tf);

#endif // _UACCESS_H_
