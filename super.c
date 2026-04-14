// SPDX-License-Identifier: GPL-2.0
/*
 * super.c - Superblock operations for XV6 FS
 *
 * Roadmap stage 1: Read-Only Mount
 *
 * What you need to implement here:
 *   xv6fs_fill_super  — read the on-disk superblock, validate the magic
 *                       number, populate the VFS super_block, and hand
 *                       back the root dentry.
 *   xv6fs_put_super   — free xv6fs_sb_info when the FS is unmounted.
 *   xv6fs_statfs      — report filesystem statistics to df(1) / statfs(2).
 */

#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/writeback.h>
#include <linux/fs.h>
#include "xv6fs.h"

void xv6fs_release_sbi(struct xv6fs_sb_info *sbi)
{
	if (sbi) {
		if (sbi->bh) {
			brelse(sbi->bh);
			sbi->bh = NULL;
		}
		kfree(sbi);
	}
}

/* ------------------------------------------------------------------
 * xv6fs_put_super
 *
 * Called by the VFS just before the super_block is freed (unmount).
 * Release everything you allocated in xv6fs_fill_super.
 *
 * API hints:
 *   brelse(bh)  — release a buffer_head obtained from sb_bread().
 *                 Declared in <linux/buffer_head.h>.
 *   kfree(ptr)  — free a kmalloc/kzalloc allocation.
 *
 * VFS guarantees:
 *   - Called exactly once per mount, during unmount.
 *   - All dentries and inodes have been evicted before this is called
 *     (generic_shutdown_super handles that).
 *   - No other super_operations are running concurrently.
 *   - sb->s_fs_info is still the value set in fill_super.
 * ---------------------------------------------------------------- */
static void xv6fs_put_super(struct super_block *sb)
{
	xv6fs_release_dinodes(xv6fs_sb(sb));
	xv6fs_release_sbi(xv6fs_sb(sb));
	sb->s_fs_info = NULL;
	pr_info("xv6fs: superblock released for device %s\n", sb->s_id);
}

/* ------------------------------------------------------------------
 * xv6fs_statfs
 *
 * Fill @buf so that df(1) and statfs(2) work correctly.
 *
 * API hints:
 *   struct kstatfs (declared in <linux/statfs.h>) fields to set:
 *     f_type    = XV6FS_MAGIC
 *     f_bsize   = XV6FS_BSIZE
 *     f_blocks  = sbi->raw_sb.size
 *     f_bfree   = sbi->raw_sb.nblocks  (approximation; exact count
 *                 requires scanning the bitmap — implement later)
 *     f_bavail  = same as f_bfree
 *     f_files   = sbi->raw_sb.ninodes
 *     f_ffree   = 0  (exact count requires scanning the inode table)
 *     f_namelen = XV6FS_DIRSIZ
 *
 * VFS guarantees:
 *   - @dentry is a valid, referenced dentry within this filesystem.
 *   - sb->s_fs_info is valid (filesystem is mounted).
 *   - May be called concurrently with other operations; read-only
 *     access to sbi->raw_sb is safe without locking.
 * ---------------------------------------------------------------- */
static int xv6fs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct xv6fs_sb_info *sbi = xv6fs_sb(sb);
	buf->f_type = sbi->raw_sb.magic;
	buf->f_bsize = XV6FS_BSIZE;
	buf->f_blocks = sbi->raw_sb.nblocks;
	buf->f_bfree = xv6fs_b_count_free(sb);
	buf->f_bavail = xv6fs_b_count_free(sb);
	buf->f_files = sbi->raw_sb.ninodes;
	buf->f_ffree = sbi->free_inodes;
	buf->f_namelen = XV6FS_DIRSIZ;
	buf->f_flags = ST_NOATIME | ST_NODIRATIME | ST_NOEXEC | ST_NOSUID | ST_RDONLY;
	return 0;
}

/* ------------------------------------------------------------------
 * Superblock operations table
 *
 * Wire up your implementations below.  The VFS calls these through
 * sb->s_op.
 * ---------------------------------------------------------------- */

/*
 * xv6fs_write_inode - write VFS inode metadata back to disk.
 *
 * Called by the VFS writeback machinery when an inode has been marked
 * dirty (via mark_inode_dirty()) and the kernel decides it's time to
 * flush it — either during periodic writeback, sync(2), fsync(2), or
 * when evicting the inode from the cache.
 *
 * Steps to implement:
 *   1. Get the xv6fs_inode_info from the VFS inode:
 *        struct xv6fs_inode_info *ei = xv6fs_i(inode);
 *
 *   2. Call __xv6fs_write_inode(ei) (defined in inode.c) to:
 *      a. Convert in-memory fields back to on-disk format via
 *         xv6fs_dinode_to_disk() into sbi->dinodes[ino].
 *      b. Read the disk block containing this inode with sb_bread().
 *      c. Copy the dinode into the correct slot in the block.
 *      d. mark_buffer_dirty(bh) + brelse(bh).
 *
 *   3. If @wbc->sync_mode == WB_SYNC_ALL (e.g. fsync), the buffer
 *      must reach disk before returning.  __xv6fs_write_inode already
 *      calls mark_buffer_dirty(); for sync writes, additionally call
 *      sync_dirty_buffer(bh) instead — this submits I/O and waits.
 *      (For the initial implementation, mark_buffer_dirty is fine;
 *      the block layer will flush eventually.  sync_dirty_buffer can
 *      be added later for fsync correctness.)
 *
 *   4. Return 0 on success, or the error from __xv6fs_write_inode.
 *
 * API hints:
 *   xv6fs_i(inode)                       — xv6fs.h
 *   sync_dirty_buffer(bh)                — <linux/buffer_head.h>
 *
 * VFS guarantees:
 *   - @inode is a valid, referenced inode with I_DIRTY set.
 *   - @wbc describes the writeback context (sync vs async).
 *   - The VFS does NOT hold i_rwsem for this call.
 *   - May be called concurrently for different inodes.
 */
static int xv6fs_write_inode(struct inode *inode,
			    struct writeback_control *wbc)
{
	struct xv6fs_inode_info *ei = xv6fs_i(inode);
	struct super_block *sb = inode->i_sb;
	struct xv6fs_sb_info *sbi = xv6fs_sb(sb);
	unsigned long ino = inode->i_ino;
	int ret = 0;

	// Update in-memory cache
	xv6fs_dinode_to_disk(ei, &sbi->dinodes[ino]);

	// Write through to disk
	sector_t block = XV6FS_IBLOCK(ino, sbi);
	struct buffer_head *bh = sb_bread(sb, block);
	if (!bh) {
		pr_err("xv6fs: failed to read inode block for inode %lu\n", ino);
		return -EIO;
	}

	union xv6fs_dinode *raw = (union xv6fs_dinode *)bh->b_data + XV6FS_IOFFSET(ino);
	*raw = sbi->dinodes[ino];

	mark_buffer_dirty(bh);
	if (wbc->sync_mode == WB_SYNC_ALL) {
		ret = sync_dirty_buffer(bh);
		if (ret) {
			pr_err("xv6fs: failed to sync buffer for inode %lu: %d\n", ino, ret);
			brelse(bh);
			return ret;
		}
	}
	brelse(bh);
	return 0;
}

/*
 * xv6fs_evict_inode - clean up an inode being removed from the cache.
 *
 * Called by the VFS when an inode's last in-memory reference is dropped.
 * If nlink == 0 (file was deleted), this must free all data blocks
 * (direct + indirect) via xv6fs_bfree() and return the inode to the
 * free list in sbi->dinodes.
 *
 * Steps to implement:
 *   1. Call truncate_inode_pages_final(&inode->i_data) to discard
 *      all cached pages for this inode.
 *   2. If inode->i_nlink == 0:
 *      a. Free all direct blocks: walk ei->addrs[0..NDIRECT-1],
 *         xv6fs_bfree() each non-zero entry, zero it.
 *      b. Free the indirect block and its entries if present.
 *      c. Set inode->i_size = 0 and return the dinode to the
 *         free list in sbi->dinodes.
 *   3. Call clear_inode(inode) to mark the VFS inode as dead.
 *
 * VFS guarantees:
 *   - @inode is a valid inode with i_count == 0.
 *   - No other thread is accessing this inode.
 */
static void xv6fs_evict_inode(struct inode *inode)
{
	/* TODO (stage 5) */
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

const struct super_operations xv6fs_super_ops = {
	/* Memory management — implemented in inode.c */
	.alloc_inode   = xv6fs_alloc_inode,
	.free_inode    = xv6fs_free_inode,

	.write_inode   = xv6fs_write_inode,
	.evict_inode   = xv6fs_evict_inode,

	.put_super     = xv6fs_put_super,
	.statfs        = xv6fs_statfs,
};

/* ------------------------------------------------------------------
 * xv6fs_fill_super
 *
 * Called by mount_bdev() after it has opened the block device.
 * This is the heart of the mount operation.
 *
 * Steps to follow:
 *   1. Allocate a struct xv6fs_sb_info with kzalloc().  This holds the
 *      raw on-disk superblock and the buffer_head.  Return -ENOMEM on
 *      failure.
 *
 *   2. Set the block size with sb_set_blocksize(sb, XV6FS_BSIZE).
 *      WARNING: returns the block size on success or 0 on failure
 *      (NOT a negative errno).  Check with: if (!sb_set_blocksize(...))
 *
 *   3. Read the on-disk superblock (block 1) with sb_bread(sb, 1).
 *      Block 0 is the boot block — skip it.
 *      Return -EIO if sb_bread returns NULL.
 *
 *   4. Convert the on-disk superblock to host-endian with
 *      xv6fs_sb_to_cpu() and store in sbi->raw_sb.  Store the
 *      buffer_head in sbi->bh, then set sb->s_fs_info = sbi.
 *
 *   5. Verify the magic number against XV6FS_MAGIC.
 *      Remember to convert with le32_to_cpu().
 *      Return -EINVAL if it does not match.
 *
 *   6. Populate the VFS super_block:
 *        sb->s_magic    = XV6FS_MAGIC
 *        sb->s_op       = &xv6fs_super_ops
 *        sb->s_maxbytes = XV6FS_MAXFILE * XV6FS_BSIZE
 *        sb->s_time_gran = 1   (XV6 has no timestamps; 1 ns granularity)
 *
 *   7. Get the root inode and create the root dentry:
 *        root_inode = xv6fs_iget(sb, XV6FS_ROOTINO);   (inode.c)
 *        — returns ERR_PTR() on failure; check with IS_ERR()
 *        sb->s_root = d_make_root(root_inode);
 *        — returns NULL on failure (and calls iput for you); return -ENOMEM
 *
 *   CLEANUP: any error after allocating sbi/bh must free those resources
 *   and clear sb->s_fs_info.  Use goto labels for cleanup paths.
 *
 * API hints:
 *   sb_set_blocksize(sb, size)  — <linux/buffer_head.h>; returns size or 0
 *   sb_bread(sb, block)         — <linux/buffer_head.h>; returns NULL on error
 *   brelse(bh)                  — <linux/buffer_head.h>
 *   le32_to_cpu(x)              — <linux/byteorder/generic.h> (via <linux/types.h>)
 *   kzalloc(size, GFP_KERNEL)   — <linux/slab.h>
 *   d_make_root(inode)          — <linux/dcache.h> (included via <linux/fs.h>)
 *   IS_ERR(ptr) / PTR_ERR(ptr)  — <linux/err.h> (included via <linux/fs.h>)
 * ---------------------------------------------------------------- */
int xv6fs_fill_super(struct super_block *sb, void *data, int silent)
{
	int ret = -EINVAL;  /* default return value for failure cases */

	// step 1: Allocate xv6fs_sb_info struct
	struct xv6fs_sb_info *sbi = kzalloc(sizeof(struct xv6fs_sb_info), GFP_KERNEL);
	if (!sbi) {
		pr_err("xv6fs: failed to allocate superblock info\n");
		ret = -ENOMEM;
		goto out;
	}

	pr_info("xv6fs: sbi structure created for device %s\n", sb->s_id);
	
	// step 2: Set the block size
	// The size of blocks is fixed in XV6FS(512B)
	if (!sb_set_blocksize(sb, XV6FS_BSIZE)) {
		pr_err("xv6fs: block size %d not supported by device\n", XV6FS_BSIZE);
		ret = -EINVAL;
		goto out;
	}

	pr_info("xv6fs: block size set to %lu bytes\n", sb->s_blocksize);

	// step 3: Read the on-disk superblock (block 1)
	// XV6 put superblock information in block 1 to make a room for bootloader in block 0
	struct buffer_head *bh = sb_bread(sb, 1);
	if (!bh) {
		pr_err("xv6fs: failed to read superblock\n");
		ret = -EIO;
		goto out;
	}

	pr_info("xv6fs: superblock read successfully\n");

	// step 4: Fill in the sbi structure
	sbi->bh = bh;
	sb->s_fs_info = sbi;
	xv6fs_sb_to_cpu((const struct xv6fs_dsuperblock *)bh->b_data, &sbi->raw_sb);

	// step 5: Verify the magic number
	if (sbi->raw_sb.magic != XV6FS_MAGIC) {
		pr_err("xv6fs: invalid magic number: expected 0x%08x, got 0x%08x\n",
		       XV6FS_MAGIC, sbi->raw_sb.magic);
		ret = -EINVAL;
		goto cleanup_sbi;
	}

	pr_info("xv6fs: magic number verified: 0x%08x\n", sbi->raw_sb.magic);

	// step 6: Populate the VFS super_block
	sb->s_magic = sbi->raw_sb.magic;
	sb->s_op = &xv6fs_super_ops;
	sb->s_maxbytes = XV6FS_MAXFILE * XV6FS_BSIZE;
	sb->s_time_gran = 1; // XV6 has no timestamps; 1 ns granularity

	pr_info("xv6fs: superblock populated: max file size = %lld bytes\n", sb->s_maxbytes);

	ret = xv6fs_load_dinodes(sb);
	if (ret)
		goto cleanup_sbi;

	struct inode *rooti = xv6fs_iget(sb, XV6FS_ROOTINO);
	if (IS_ERR(rooti)) {
		pr_err("xv6fs: failed to get root inode\n");
		ret = PTR_ERR(rooti);
		goto cleanup_sbi;
	}

	sb->s_root = d_make_root(rooti);
	if (!sb->s_root) {
		pr_err("xv6fs: failed to create root dentry\n");
		ret = -ENOMEM;
		goto cleanup_sbi;
	}

	pr_info("xv6fs: mounted superblock with %u blocks, %u inodes\n",
		sbi->raw_sb.size, sbi->raw_sb.ninodes);

	ret = 0;
	goto out;

cleanup_sbi:
	if (ret != 0) {
		xv6fs_release_dinodes(sbi);
		sb->s_fs_info = NULL;
		xv6fs_release_sbi(sbi);
		sbi = NULL;
	}
out:
	return ret;
}

