/*
 * mman.h - POSIX flags for memory
 */

#ifndef PERSPICUA_UAPI_MMAN_H
#define PERSPICUA_UAPI_MMAN_H

/* Memory protection flags */
#define PROT_READ  0x1
#define PROT_WRITE 0x2

/* Memory mapping flags */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED    ((void*)-1)

#endif /* PERSPICUA_UAPI_MMAN_H */
