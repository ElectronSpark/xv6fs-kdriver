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
 * ---------------------------------------------------------------- */
static void xv6fs_put_super(struct super_block *sb)
{
	/* TODO (stage 1):
	 *   struct xv6fs_sb_info *sbi = xv6fs_sb(sb);
	 *   brelse(sbi->bh);
	 *   kfree(sbi);
	 *   sb->s_fs_info = NULL;
	 */
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
 * ---------------------------------------------------------------- */
static int xv6fs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	/* TODO (stage 1) */
	return 0;
}

/* ------------------------------------------------------------------
 * Superblock operations table
 *
 * Wire up your implementations below.  The VFS calls these through
 * sb->s_op.
 * ---------------------------------------------------------------- */

const struct super_operations xv6fs_super_ops = {
	/* Memory management — implemented in inode.c */
	.alloc_inode   = xv6fs_alloc_inode,
	.destroy_inode = xv6fs_destroy_inode,

	/* TODO: add .write_inode  when you implement write support (stage 5) */

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
 *   4. Copy the raw superblock into sbi->raw_sb (fields remain __le32 —
 *      convert with le32_to_cpu() each time you read a field).  Store
 *      the buffer_head in sbi->bh, then set sb->s_fs_info = sbi.
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
	sbi->raw_sb = *(struct xv6fs_superblock *)bh->b_data; // copy raw superblock data for later use

	// step 5: Verify the magic number
	if (le32_to_cpu(sbi->raw_sb.magic) != XV6FS_MAGIC) {
		pr_err("xv6fs: invalid magic number: expected 0x%08x, got 0x%08x\n",
		       XV6FS_MAGIC, le32_to_cpu(sbi->raw_sb.magic));
		ret = -EINVAL;
		goto cleanup_sbi;
	}

	pr_info("xv6fs: magic number verified: 0x%08x\n", le32_to_cpu(sbi->raw_sb.magic));

	// step 6: Populate the VFS super_block
	sb->s_magic = sbi->raw_sb.magic;
	sb->s_op = &xv6fs_super_ops;
	sb->s_maxbytes = XV6FS_MAXFILE * XV6FS_BSIZE;
	sb->s_time_gran = 1; // XV6 has no timestamps; 1 ns granularity

	pr_info("xv6fs: mounted superblock with %u blocks, %u inodes\n",
		le32_to_cpu(sbi->raw_sb.size), le32_to_cpu(sbi->raw_sb.ninodes));

	ret = -EINVAL; // Always return an error for now until we implement the root dentry (stage 2)

cleanup_sbi:
	if (ret != 0) {
		sb->s_fs_info = NULL;
		xv6fs_release_sbi(sbi);
		sbi = NULL;
	}
out:
	return ret;
}

