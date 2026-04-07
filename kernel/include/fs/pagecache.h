#ifndef PERSPICUA_FS_PAGECACHE_H
#define PERSPICUA_FS_PAGECACHE_H

#include "types.h"
#include "fs/vfs.h"

/*
 * pagecache_init - Initializes the unified page cache layer.
 */
void pagecache_init(void);

/*
 * pagecache_get_page - Returns a pointer to the cached 4KB page data if present.
 */
void *pagecache_get_page(struct vfs_vnode *node, size_t page_index);

/*
 * pagecache_add_page - Registers a newly allocated and populated 4KB page in the cache.
 */
int pagecache_add_page(struct vfs_vnode *node, size_t page_index, void *data);

/*
 * pagecache_mark_dirty - Marks a cached page as modified.
 */
void pagecache_mark_dirty(struct vfs_vnode *node, size_t page_index);

/*
 * pagecache_writeback - Writes all dirty pages for a specific vnode to disk.
 *
 * Returns the number of pages written, or a negative error code.
 */
int pagecache_writeback(struct vfs_vnode *node);

/*
 * pagecache_sync - Writes all dirty pages in the cache to disk.
 *
 * Returns the total number of pages written.
 */
int pagecache_sync(void);

/*
 * pagecache_invalidate - Removes all cached pages for a vnode.
 *
 * Dirty pages are written back before removal.
 */
void pagecache_invalidate(struct vfs_vnode *node);

#endif /* PERSPICUA_FS_PAGECACHE_H */
