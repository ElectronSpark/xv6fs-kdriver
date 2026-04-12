// SPDX-License-Identifier: GPL-2.0
/*
 * inode.c - Inode cache and block-mapping for XV6 FS
 *
 * Roadmap stages 2 (inode reads) and 4 (block mapping / file reads).
 *
 * What you need to implement here:
 *   xv6fs_iget       — look up (or allocate + read) an inode by number.
 *   xv6fs_get_block  — map a file's logical block number to a disk block.
 *
 * The inode slab cache boilerplate (xv6fs_init_inode_cache,
 * xv6fs_alloc_inode, xv6fs_destroy_inode) is already provided because
 * it is identical in every Linux filesystem driver.
 */

#include <linux/buffer_head.h>
#include <linux/slab.h>
#include "xv6fs.h"

/* ------------------------------------------------------------------
 * Inode slab cache
 *
 * We embed xv6fs_inode_info inside every VFS inode so the kernel
 * must allocate sizeof(xv6fs_inode_info) instead of sizeof(inode).
 * A private slab cache tells the allocator the right size.
 * ---------------------------------------------------------------- */

static struct kmem_cache *xv6fs_inode_cachep;

/*
 * xv6fs_inode_init_once - slab constructor called once per new object.
 *
 * Must call inode_init_once() to zero-initialise the embedded VFS inode.
 * API: inode_init_once()  — <linux/fs.h>
 */
static void xv6fs_inode_init_once(void *obj)
{
	struct xv6fs_inode_info *ei = obj;

	inode_init_once(&ei->vfs_inode);
}

/*
 * xv6fs_init_inode_cache - create the inode slab; called from xv6fs_init().
 *
 * API: kmem_cache_create(name, size, align, flags, ctor)
 *        — <linux/slab.h>
 *      SLAB_RECLAIM_ACCOUNT — tell the memory manager this slab can be
 *        reclaimed under memory pressure (important for correctness).
 */
int xv6fs_init_inode_cache(void)
{
	xv6fs_inode_cachep = kmem_cache_create(
		"xv6fs_inode_cache",
		sizeof(struct xv6fs_inode_info),
		0,
		SLAB_RECLAIM_ACCOUNT,
		xv6fs_inode_init_once);

	return xv6fs_inode_cachep ? 0 : -ENOMEM;
}

/*
 * xv6fs_destroy_inode_cache - destroy the slab; called from xv6fs_exit().
 *
 * rcu_barrier() waits for all pending call_rcu() callbacks (see
 * xv6fs_destroy_inode below) to finish before we tear down the cache.
 */
void xv6fs_destroy_inode_cache(void)
{
	rcu_barrier();
	kmem_cache_destroy(xv6fs_inode_cachep);
}

/*
 * xv6fs_alloc_inode - allocate a new xv6fs_inode_info + embedded VFS inode.
 *
 * Called by the VFS (via super_ops.alloc_inode) whenever it needs a
 * new inode object.  Return &ei->vfs_inode so the VFS sees a plain inode.
 *
 * API: kmem_cache_alloc(cachep, GFP_KERNEL)  — <linux/slab.h>
 */
struct inode *xv6fs_alloc_inode(struct super_block *sb)
{
	struct xv6fs_inode_info *ei;

	ei = kmem_cache_alloc(xv6fs_inode_cachep, GFP_KERNEL);
	if (!ei)
		return NULL;

	return &ei->vfs_inode;
}

/*
 * xv6fs_i_callback - RCU callback that frees the inode after a grace period.
 */
static void xv6fs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	kmem_cache_free(xv6fs_inode_cachep, xv6fs_i(inode));
}

/*
 * xv6fs_destroy_inode - schedule an inode for freeing via RCU.
 *
 * Called by the VFS (via super_ops.destroy_inode).
 * Must NOT free immediately — other CPUs might still hold RCU references.
 *
 * API: call_rcu(&inode->i_rcu, callback)  — <linux/rcupdate.h>
 */
void xv6fs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, xv6fs_i_callback);
}

/* ------------------------------------------------------------------
 * Inode operations table
 *
 * Populated with your implementations from the TODO sections below.
 * ---------------------------------------------------------------- */

const struct inode_operations xv6fs_file_inode_ops = {
	/* TODO (stage 5): add .setattr when you implement writes */
};

/* ------------------------------------------------------------------
 * xv6fs_iget
 *
 * Return the VFS inode for inode number @ino, reading it from disk if
 * it is not already in the inode cache.
 *
 * Steps to follow:
 *   1. Call iget_locked(sb, ino) to look up the inode cache.
 *      If the inode is already cached the I_NEW flag will NOT be set;
 *      return it immediately.
 *
 *   2. For a newly allocated inode (I_NEW is set):
 *      a. Read the inode block: block = XV6FS_IBLOCK(ino, &sbi->raw_sb)
 *         bh = sb_bread(sb, block)
 *      b. Index into the block:
 *         raw = (struct xv6fs_dinode *)bh->b_data + (ino % XV6FS_IPB)
 *      c. Copy fields into the VFS inode and xv6fs_inode_info:
 *           - set ei->i_type, ei->i_major, ei->i_minor  (le16_to_cpu)
 *           - copy ei->addrs[] (converting le32_to_cpu)
 *           - set inode->i_size (le32_to_cpu), set_nlink() (le16_to_cpu)
 *           - set inode->i_blocks = (inode->i_size + 511) / 512
 *           - set inode->i_uid/gid (fake root)
 *           - XV6FS_ZERO_CTIME(inode); inode->i_atime = inode->i_mtime = …
 *           - switch on ei->i_type to set inode->i_mode, ->i_op, ->i_fop
 *             and (for files) inode->i_mapping->a_ops = &xv6fs_aops
 *      d. brelse(bh)
 *      e. unlock_new_inode(inode)
 *
 * API hints:
 *   iget_locked(sb, ino)     — <linux/fs.h>; acquires a lock on new inodes
 *   iget_failed(inode)       — <linux/fs.h>; call instead of unlock_new_inode
 *                              when reading the inode fails
 *   unlock_new_inode(inode)  — <linux/fs.h>
 *   set_nlink(inode, n)      — <linux/fs.h>
 *   make_kuid(&init_user_ns, 0)  — <linux/uidgid.h>
 *   make_kgid(&init_user_ns, 0)  — same
 *   init_special_inode(inode, mode, dev)  — <linux/fs.h>; for T_DEV
 *   MKDEV(major, minor)      — <linux/kdev_t.h>
 *   le16_to_cpu / le32_to_cpu — endianness helpers
 * ---------------------------------------------------------------- */
struct inode *xv6fs_iget(struct super_block *sb, unsigned long ino)
{
	/* TODO (stage 2) */
	return ERR_PTR(-EINVAL);
}

/* ------------------------------------------------------------------
 * xv6fs_get_block
 *
 * Map logical file block @iblock to a physical disk block and fill
 * @bh_result so the page-cache layer can issue the I/O.
 *
 * This function is the bridge between the VFS page cache and the
 * XV6 on-disk block layout.  It is called by mpage_read_folio()
 * (file.c) for every block in a page that needs to be read.
 *
 * Steps to follow:
 *   Direct blocks (iblock < XV6FS_NDIRECT):
 *     disk_block = ei->addrs[iblock]
 *     (addrs[] is already host-endian, converted in xv6fs_iget)
 *
 *   Indirect block (iblock >= XV6FS_NDIRECT):
 *     idx        = iblock - XV6FS_NDIRECT
 *     Return -EFBIG if idx >= XV6FS_NINDIRECT.
 *     indirect_bh = sb_bread(sb, ei->addrs[XV6FS_NDIRECT])
 *     disk_block  = le32_to_cpu(((__le32 *)indirect_bh->b_data)[idx])
 *     NOTE: indirect block data is raw on-disk (__le32), unlike
 *     ei->addrs[] which is already host-endian.
 *     brelse(indirect_bh)
 *
 *   If disk_block == 0 and @create == 0 → the block is a hole; return 0
 *   without calling map_bh() (the caller will zero-fill the page).
 *
 *   If disk_block != 0:
 *     map_bh(bh_result, sb, disk_block)
 *
 * Write support (create == 1) — TODO stage 5:
 *   Allocate a new data block from the free-block bitmap, write the
 *   address back into ei->addrs[] (or the indirect block), and mark
 *   the inode dirty with mark_inode_dirty().
 *
 * API hints:
 *   map_bh(bh, sb, block)    — <linux/buffer_head.h>; fills bh_result
 *   sb_bread(sb, block)      — <linux/buffer_head.h>
 *   brelse(bh)               — <linux/buffer_head.h>
 *   mark_inode_dirty(inode)  — <linux/fs.h>; schedule inode for write-back
 *   le32_to_cpu(x)           — endianness helper
 * ---------------------------------------------------------------- */
int xv6fs_get_block(struct inode *inode, sector_t iblock,
		    struct buffer_head *bh_result, int create)
{
	/* TODO (stage 4: read path; stage 5: write/allocate path) */
	return -EINVAL;
}

/* ------------------------------------------------------------------
 * Address-space operations
 *
 * The VFS page cache uses these to read/write file data.
 * xv6fs_get_block (above) does the actual block mapping.
 * ---------------------------------------------------------------- */

/*
 * xv6fs_read_folio - read one page of file data into the page cache.
 *
 * Delegate to mpage_read_folio(), passing xv6fs_get_block as the mapper.
 *
 * API hints:
 *   mpage_read_folio(folio, get_block)  — <linux/mpage.h>
 *     Kernel >= 5.19 only.  On older kernels use:
 *       block_read_full_page(page, get_block)  — <linux/buffer_head.h>
 *     and change the address_space_operations field from .read_folio
 *     to .readpage.
 */
static int xv6fs_read_folio(struct file *file, struct folio *folio)
{
	/* TODO (stage 4) */
	folio_unlock(folio);
	return -EINVAL;
}

/*
 * xv6fs_bmap - convert a file's logical block number to a disk sector.
 *
 * Used by tools such as lsof(8).  Delegate to generic_block_bmap().
 *
 * API: generic_block_bmap(mapping, block, get_block)  — <linux/buffer_head.h>
 */
static sector_t xv6fs_bmap(struct address_space *mapping, sector_t block)
{
	/* TODO (stage 4) */
	return 0;
}

const struct address_space_operations xv6fs_aops = {
	.read_folio = xv6fs_read_folio,
	/* TODO (stage 5): .writepage / .writepages for write support     */
	/* TODO (stage 5): .write_begin / .write_end for buffered writes  */
	.bmap       = xv6fs_bmap,
};
