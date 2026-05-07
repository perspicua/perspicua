#ifndef PERSPICUA_MM_ASID_H
#define PERSPICUA_MM_ASID_H

#include "core/lock.h"
#include "types.h"

#define MAX_ASID    255
#define BITMAP_SIZE 4

struct asid_pool_t {
    uint64_t bitmap[BITMAP_SIZE];
    uint64_t generation;
};

void asid_init(void);
void asid_get_active(unsigned long *asid_out, unsigned long *gen_out);
void asid_free(unsigned long *asid, unsigned long *gen);

#endif
