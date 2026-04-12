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
#include <linux/mpage.h>
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
 * __xv6fs_read_inode - read inode @ino from disk into VFS inode.
 *
 * Reads the block containing inode @ino, converts the on-disk dinode
 * into the in-memory xv6fs_inode_info and VFS inode fields using
 * xv6fs_dinode_to_cpu().  Does NOT set i_mode, i_op, i_fop, a_ops,
 * i_blocks, i_uid, i_gid, or timestamps — the caller must do that
 * based on ei->i_type.
 *
 * Returns 0 on success, -EIO if the block cannot be read.
 * ---------------------------------------------------------------- */
static int __xv6fs_read_inode(struct xv6fs_inode_info *ei)
{
	struct inode *inode = &ei->vfs_inode;
	struct super_block *sb = inode->i_sb;
	struct xv6fs_sb_info *sbi = xv6fs_sb(sb);
	unsigned long ino = inode->i_ino;

	// Read the block containing this inode
	sector_t block = XV6FS_IBLOCK(ino, sbi);
	struct buffer_head *bh = sb_bread(sb, block);
	if (!bh) {
		pr_err("xv6fs: failed to read inode block for inode %lu\n", ino);
		return -EIO;
	}

	// Index into the block and convert to host-endian
	struct xv6fs_dinode *raw = (struct xv6fs_dinode *)bh->b_data + XV6FS_IOFFSET(ino);
	xv6fs_dinode_to_cpu(raw, ei);

	brelse(bh);
	return 0;
}

/* ------------------------------------------------------------------
 * __xv6fs_write_inode - write VFS inode metadata back to disk.
 *
 * Reads the block containing inode @ino, converts the in-memory
 * xv6fs_inode_info and VFS inode fields back to on-disk format using
 * xv6fs_dinode_to_disk(), overwrites the slot, and marks the buffer
 * dirty.  Does NOT handle indirect block mapping or data blocks.
 *
 * Returns 0 on success, -EIO if the block cannot be read.
 * ---------------------------------------------------------------- */
static int __xv6fs_write_inode(struct xv6fs_inode_info *ei)
{
	struct inode *inode = &ei->vfs_inode;
	struct super_block *sb = inode->i_sb;
	struct xv6fs_sb_info *sbi = xv6fs_sb(sb);
	unsigned long ino = inode->i_ino;

	// Read the block containing this inode
	sector_t block = XV6FS_IBLOCK(ino, sbi);
	struct buffer_head *bh = sb_bread(sb, block);
	if (!bh) {
		pr_err("xv6fs: failed to read inode block for inode %lu\n", ino);
		return -EIO;
	}

	// Convert to on-disk format and write back
	struct xv6fs_dinode *raw = (struct xv6fs_dinode *)bh->b_data + XV6FS_IOFFSET(ino);
	xv6fs_dinode_to_disk(ei, raw);

	mark_buffer_dirty(bh);
	brelse(bh);
	return 0;
}

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
 *      a. Read the inode block: block = XV6FS_IBLOCK(ino, sbi)
 *         bh = sb_bread(sb, block)
 *      b. Index into the block:
 *         raw = (struct xv6fs_dinode *)bh->b_data + (ino % XV6FS_IPB)
 *      c. Copy fields into the VFS inode and xv6fs_inode_info:
 *           - set ei->i_type, ei->i_major, ei->i_minor  (le16_to_cpu)
 *           - copy ei->addrs[] (converting le32_to_cpu)
 *           - set inode->i_size (le32_to_cpu), set_nlink() (le16_to_cpu)
 *           - set inode->i_blocks = (inode->i_size + 511) / 512
 *           - set inode->i_uid/gid (fake root)
 *           - inode_set_atime/mtime/ctime(inode, 0, 0)
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
	// Reject invalid inode numbers (0 is not used in XV6)	
	if (ino == 0) {
		pr_err("xv6fs: invalid inode number: %lu\n", ino);
		return ERR_PTR(-EINVAL);
	}

	// Reject inode numbers that exceed the maximum defined in the superblock
	struct xv6fs_sb_info *sbi = xv6fs_sb(sb);
	if (ino >= sbi->raw_sb.ninodes) {
		pr_err("xv6fs: inode number %lu exceeds maximum %u\n",
		       ino, sbi->raw_sb.ninodes - 1);
		return ERR_PTR(-EINVAL);
	}

	// Look up the inode cache
	struct inode *inode = iget_locked(sb, ino);
	if (!inode) {
		return ERR_PTR(-ENOMEM);
	}

	if (!(inode->i_state & I_NEW)) {
		// Inode already exists in cache; return it
		return inode;
	}

	struct xv6fs_inode_info *ei = xv6fs_i(inode);
	int ret = __xv6fs_read_inode(ei);
	if (ret != 0) {
		iget_failed(inode);
		return ERR_PTR(ret);
	}

	// Set VFS inode fields based on xv6 inode type
	inode->i_blocks = (inode->i_size + 511) / 512;
	inode->i_uid = make_kuid(&init_user_ns, 1000);
	inode->i_gid = make_kgid(&init_user_ns, 1000);
	inode_set_atime(inode, 0, 0);
	inode_set_mtime(inode, 0, 0);
	inode_set_ctime(inode, 0, 0);

	switch (ei->i_type) {
	case XV6FS_T_DIR:
		inode->i_mode = S_IFDIR | 0755;
		inode->i_op = &xv6fs_dir_inode_ops;
		inode->i_fop = &xv6fs_dir_fops;
		break;
	case XV6FS_T_FILE:
		inode->i_mode = S_IFREG | 0644;
		inode->i_op = &xv6fs_file_inode_ops;
		inode->i_fop = &xv6fs_file_fops;
		inode->i_mapping->a_ops = &xv6fs_aops;
		break;
	case XV6FS_T_DEV:
		inode->i_mode = S_IFCHR | 0600;
		init_special_inode(inode, inode->i_mode,
				   MKDEV(ei->i_major, ei->i_minor));
		break;
	default:
		pr_err("xv6fs: unknown inode type %d for inode %lu\n",
		       ei->i_type, ino);
		iget_failed(inode);
		return ERR_PTR(-EINVAL);
	}

	unlock_new_inode(inode);
	return inode;
}

/* ------------------------------------------------------------------
 * xv6fs_get_block
 *
 * Map logical file block @lblk to a physical disk block and fill
 * @bh_result so the page-cache layer can issue the I/O.
 *
 * This function is the bridge between the VFS page cache and the
 * XV6 on-disk block layout.  It is called by mpage_read_folio()
 * (file.c) for every block in a page that needs to be read.
 *
 * Steps to follow:
 *   Direct blocks (lblk < XV6FS_NDIRECT):
 *     disk_block = ei->addrs[lblk]
 *     (addrs[] is already host-endian, converted in xv6fs_iget)
 *
 *   Indirect block (lblk >= XV6FS_NDIRECT):
 *     idx        = lblk - XV6FS_NDIRECT
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
 *
 * VFS guarantees:
 *   - @inode is a valid, referenced inode.
 *   - @lblk is non-negative.
 *   - @bh_result is a pre-allocated, unmapped buffer_head.
 *   - May be called concurrently for the same inode from different
 *     page cache read paths; ei->addrs[] is read-only (stage 4) so
 *     no locking is needed for reads.
 *   - @create == 0 for reads; @create == 1 for writes (stage 5).
 * ---------------------------------------------------------------- */
int xv6fs_get_block(struct inode *inode, sector_t lblk,
		    struct buffer_head *bh_result, int create)
{
	struct xv6fs_inode_info *ei = xv6fs_i(inode);

	if (lblk < XV6FS_NDIRECT) {
		// Direct block
		sector_t disk_block = ei->addrs[lblk];
		if (disk_block == 0) {
			// Hole
			return 0;
		}
		map_bh(bh_result, inode->i_sb, disk_block);
		return 0;
	} else if (lblk < XV6FS_NDIRECT + XV6FS_NINDIRECT) {
		sector_t ind_block = ei->addrs[XV6FS_NDIRECT];
		if (ind_block == 0) {
			// Hole (no indirect block)
			return 0;
		}
		struct buffer_head *ind_bh = sb_bread(inode->i_sb, ind_block);
		if (!ind_bh) {
			pr_err("xv6fs: failed to read indirect block for inode %lu\n",
			       inode->i_ino);
			return -EIO;
		}
		__le32 *ind_data = (__le32 *)ind_bh->b_data;
		sector_t disk_block = le32_to_cpu(ind_data[lblk - XV6FS_NDIRECT]);
		brelse(ind_bh);
		if (disk_block == 0) {
			// Hole
			return 0;
		}
		map_bh(bh_result, inode->i_sb, disk_block);
		return 0;
	}

	// Invalid block number
	pr_err("xv6fs: logical block %lu exceeds maximum for inode %lu\n",
		(unsigned long)lblk, inode->i_ino);
	return -EFBIG;
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
 * Delegates to mpage_read_folio(), which calls xv6fs_get_block for
 * each logical block in the folio to resolve physical disk blocks,
 * then issues the I/O.
 *
 * Steps to implement:
 *   1. Call mpage_read_folio(folio, xv6fs_get_block).
 *      mpage_read_folio handles folio locking/unlocking internally,
 *      so do NOT call folio_unlock() yourself.
 *
 * API:
 *   mpage_read_folio(folio, get_block)  — <linux/mpage.h>
 */
static int xv6fs_read_folio(struct file *file, struct folio *folio)
{
	return mpage_read_folio(folio, xv6fs_get_block);
}

/*
 * xv6fs_bmap - convert a file's logical block number to a disk sector.
 *
 * Used by tools such as lsof(8) and the FIBMAP ioctl.
 * Delegates to generic_block_bmap(), which calls xv6fs_get_block.
 *
 * Steps to implement:
 *   1. Call generic_block_bmap(mapping, block, xv6fs_get_block).
 *      It calls xv6fs_get_block internally to resolve the mapping.
 *
 * API:
 *   generic_block_bmap(mapping, block, get_block)  — <linux/buffer_head.h>
 */
static sector_t xv6fs_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, xv6fs_get_block);
}

const struct address_space_operations xv6fs_aops = {
	.read_folio = xv6fs_read_folio,
	/* TODO (stage 5): .writepage / .writepages for write support     */
	/* TODO (stage 5): .write_begin / .write_end for buffered writes  */
	.bmap       = xv6fs_bmap,
};
