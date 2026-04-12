// SPDX-License-Identifier: GPL-2.0
/*
 * dir.c - Directory operations for XV6 FS
 *
 * Roadmap stages 3 (directory listing) and 4 (lookup).
 *
 * What you need to implement here:
 *   xv6fs_readdir  — iterate a directory's entries for getdents(2) / ls(1).
 *   xv6fs_lookup   — resolve one filename component within a directory.
 *
 * XV6 directory format reminder:
 *   A directory's data is a flat, unsorted array of struct xv6fs_dirent
 *   records.  Each record is exactly sizeof(struct xv6fs_dirent) bytes.
 *   An entry with inum == 0 is free (deleted or never used) — skip it.
 *   The first two valid entries are always "." and ".." (inum != 0).
 *   Filenames are NOT null-terminated when they are exactly XV6FS_DIRSIZ
 *   bytes long; use strnlen() / strncmp() accordingly.
 */

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include "xv6fs.h"

/* ------------------------------------------------------------------
 * xv6fs_readdir
 *
 * Fill the caller's buffer with directory entries by calling dir_emit()
 * for each valid xv6fs_dirent found in the directory's data blocks.
 *
 * ctx->pos is the byte offset into the directory data (not an index).
 * Start from the entry at offset ctx->pos and advance ctx->pos by
 * sizeof(struct xv6fs_dirent) after each emitted entry.
 *
 * Steps to follow:
 *   1. Compute which xv6fs_dirent record corresponds to ctx->pos:
 *        offset = ctx->pos  (starts at 0)
 *   2. For each record at [offset, offset + sizeof(dirent)):
 *      a. Determine which disk block and offset-within-block:
 *           block  = offset / XV6FS_BSIZE
 *           blkoff = offset % XV6FS_BSIZE
 *      b. Use xv6fs_get_block() or look up ei->addrs[] directly to
 *         find the physical block number, then sb_bread() to read it.
 *      c. Cast bh->b_data + blkoff to struct xv6fs_dirent *.
 *      d. Skip entries with dirent->inum == 0.
 *      e. Call dir_emit(ctx, name, namelen, ino, DT_UNKNOWN).
 *         dir_emit() returns false when the caller's buffer is full;
 *         stop iterating and return 0 immediately in that case.
 *      f. Advance offset += sizeof(struct xv6fs_dirent); ctx->pos = offset.
 *      g. brelse(bh).
 *   3. Return 0 when all entries have been processed.
 *
 * API hints:
 *   dir_emit(ctx, name, namelen, ino, type) — <linux/fs.h>
 *     Returns false when the output buffer is full.
 *   dir_emit_dots(file, ctx)               — <linux/fs.h>
 *     Emits "." and ".." automatically if ctx->pos < 2; advances ctx->pos.
 *     You can use this instead of emitting "." and ".." manually.
 *   strnlen(name, XV6FS_DIRSIZ)            — safe name length
 *   sb_bread(sb, block) / brelse(bh)
 *   file_inode(file)                       — <linux/fs.h>; get inode from file
 *
 * VFS guarantees:
 *   - file is a valid, open directory (S_ISDIR tested at open time).
 *   - file_inode(file) is non-NULL and holds an active reference.
 *   - inode->i_rwsem is held shared (iterate_shared), so concurrent
 *     readdir calls are safe but writes to the directory are excluded.
 *   - ctx->pos is preserved across multiple getdents(2) calls so the
 *     caller can resume iteration where it left off.
 * ---------------------------------------------------------------- */
static int xv6fs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct xv6fs_inode_info *ei = xv6fs_i(inode);
	unsigned int pos = ctx->pos;
	unsigned int size = inode->i_size;
	struct buffer_head *ind_bh = NULL;
	__le32 *ind_data = NULL;

	if (pos >= size)
		return 0;

	/* Pre-read the indirect block if needed */
	if (ei->addrs[XV6FS_NDIRECT] && size > XV6FS_NDIRECT * XV6FS_BSIZE) {
		ind_bh = sb_bread(sb, ei->addrs[XV6FS_NDIRECT]);
		if (!ind_bh)
			return -EIO;
		ind_data = (__le32 *)ind_bh->b_data;
	}

	for (; pos < size; pos += sizeof(struct xv6fs_dirent)) {
		unsigned int blk_idx = pos / XV6FS_BSIZE;
		unsigned int blk_off = pos % XV6FS_BSIZE;
		__u32 phys;

		/* Map logical block to physical */
		if (blk_idx < XV6FS_NDIRECT) {
			phys = ei->addrs[blk_idx];
		} else if (ind_data) {
			phys = le32_to_cpu(ind_data[blk_idx - XV6FS_NDIRECT]);
		} else {
			break;
		}

		if (phys == 0)
			continue;

		struct buffer_head *bh = sb_bread(sb, phys);
		if (!bh) {
			brelse(ind_bh);
			return -EIO;
		}

		struct xv6fs_dirent *de = (struct xv6fs_dirent *)(bh->b_data + blk_off);
		__u16 inum = le16_to_cpu(de->inum);

		if (inum != 0) {
			unsigned int namelen = strnlen(de->name, XV6FS_DIRSIZ);

			if (!dir_emit(ctx, de->name, namelen, inum, DT_UNKNOWN)) {
				brelse(bh);
				brelse(ind_bh);
				return 0;
			}
		}

		brelse(bh);
		ctx->pos = pos + sizeof(struct xv6fs_dirent);
	}

	brelse(ind_bh);
	return 0;
}

/* ------------------------------------------------------------------
 * xv6fs_lookup
 *
 * Search directory @dir for the filename in @dentry->d_name and
 * return the dentry for the child inode (or a negative dentry if the
 * file does not exist).
 *
 * Steps to follow:
 *   1. Walk through the directory's data blocks (same approach as
 *      xv6fs_readdir) looking for a dirent whose name matches
 *      @dentry->d_name.name (use strncmp with XV6FS_DIRSIZ).
 *
 *   2. If a match is found:
 *        inode = xv6fs_iget(dir->i_sb, le16_to_cpu(dirent->inum));
 *      If xv6fs_iget() fails, propagate the error with ERR_CAST().
 *
 *   3. If no match is found, set inode = NULL (negative dentry).
 *
 *   4. Return d_splice_alias(inode, dentry).
 *      d_splice_alias() handles both the found and not-found cases,
 *      as well as hardlink aliases.  Passing NULL creates a negative
 *      dentry so that the VFS caches the "not found" result.
 *
 * API hints:
 *   dentry->d_name.name  — const unsigned char *, the filename to find
 *   dentry->d_name.len   — length of the filename
 *   strncmp(a, b, n)     — compare at most n bytes
 *   xv6fs_iget(sb, ino)  — inode.c
 *   d_splice_alias(inode, dentry) — <linux/dcache.h>
 *   ERR_CAST(ptr)        — <linux/err.h>; re-cast an error pointer
 *
 * VFS guarantees:
 *   - @dir is a valid, referenced directory inode (S_ISDIR).
 *   - @dir->i_rwsem is held (exclusive for create/unlink, shared for
 *     lookup), so no concurrent modifications to this directory.
 *   - @dentry is a new, unhashed dentry with a valid d_name.
 *   - dentry->d_name.len <= NAME_MAX (255).
 * ---------------------------------------------------------------- */
static struct dentry *xv6fs_lookup(struct inode *dir, struct dentry *dentry,
				   unsigned int flags)
{
	/* TODO (stage 3) */
	return ERR_PTR(-ENOENT);
}

/* ------------------------------------------------------------------
 * Operations tables
 * ---------------------------------------------------------------- */

/*
 * inode_operations for directories.
 *
 * Add .create, .mkdir, .unlink, .rmdir, .link in stage 5 (write support).
 */
const struct inode_operations xv6fs_dir_inode_ops = {
	.lookup = xv6fs_lookup,
};

/*
 * file_operations for directories.
 *
 * .iterate_shared is called by getdents(2) / readdir(3).
 * .llseek uses generic_file_llseek() which honours inode->i_size.
 */
const struct file_operations xv6fs_dir_fops = {
	.iterate_shared = xv6fs_readdir,
	.llseek         = generic_file_llseek,
};
