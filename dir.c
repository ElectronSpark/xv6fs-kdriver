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

struct xv6fs_dir_cursor {
	struct buffer_head *bh; /* buffer head for the current block */
	struct xv6fs_dirent *de; /* pointer to the current directory entry */
	struct xv6fs_inode_info *ei; /* cached inode info for the directory */
	off_t pos;    /* current position within the directory */
	off_t last_empty; /* position of the last empty entry (for create) */
	off_t first_empty; /* position of the first empty entry (for create) */
};

#define INIT_DIR_CURSOR_OFFSET(cursor, inode, offset) do { \
	(cursor)->bh = NULL; \
	(cursor)->de = NULL; \
	(cursor)->ei = xv6fs_i(inode); \
	(cursor)->pos = (offset) - sizeof(struct xv6fs_dirent); \
	(cursor)->last_empty = -1; \
	(cursor)->first_empty = -1; \
} while (0)

#define INIT_DIR_CURSOR(cursor, inode) \
	INIT_DIR_CURSOR_OFFSET(cursor, inode, 0)

#define DIR_CURSOR_AT_END(cursor) \
	((cursor)->pos >= (off_t)(&(cursor)->ei->vfs_inode)->i_size)

static void release_dir_cursor(struct xv6fs_dir_cursor *cursor)
{
	if (cursor->bh) {
		brelse(cursor->bh);
		cursor->bh = NULL;
		cursor->de = NULL;
	}
}

static struct xv6fs_dirent *advance_dir_cursor(struct xv6fs_dir_cursor *cursor)
{
	struct super_block *sb = cursor->ei->vfs_inode.i_sb;
	struct inode *inode = &cursor->ei->vfs_inode;
	unsigned int size = inode->i_size;

	while (1) {
		/* Resolve block at cursor position if at a boundary.
		 * This fires when the cursor is placed at a block start
		 * (e.g. a hole) or when the advance below crosses into
		 * a new block.  Unmapped blocks are skipped entirely. */
		while (cursor->pos >= 0 && cursor->pos < size && !cursor->bh) {
			unsigned int blk_idx = cursor->pos / XV6FS_BSIZE;
			unsigned int blk_off = cursor->pos % XV6FS_BSIZE;
			off_t blk_remaining = XV6FS_BSIZE - blk_off;
			off_t remaining = size - cursor->pos;
			struct buffer_head map_bh = { .b_size = XV6FS_BSIZE };
			int err = xv6fs_get_block(inode, blk_idx, &map_bh, 0);
			if (err)
				return ERR_PTR(err);
			if (buffer_mapped(&map_bh)) {
				cursor->bh = sb_bread(sb, map_bh.b_blocknr);
				if (!cursor->bh) {
					cursor->de = NULL;
					return ERR_PTR(-EIO);
				}
				if (blk_off != 0) {
					/* Mid-block resume — bh loaded,
					 * let advance step position us. */
					break;
				}
				/* Process entry at offset 0 of this block */
				cursor->de = (struct xv6fs_dirent *)
					cursor->bh->b_data;
				if (cursor->de->inum == 0) {
					if (cursor->first_empty == -1)
						cursor->first_empty = cursor->pos;
					cursor->last_empty = cursor->pos;
					break;
				}
				return cursor->de;
			}
			/* Unmapped block — skip remaining entries */
			if (cursor->first_empty == -1)
				cursor->first_empty = cursor->pos;
			cursor->last_empty = cursor->pos
				+ round_down(MIN_T(off_t, blk_remaining, remaining),
					     sizeof(struct xv6fs_dirent))
				- sizeof(struct xv6fs_dirent);
			cursor->pos = cursor->last_empty + sizeof(struct xv6fs_dirent);
		}

		/* Advance to next entry */
		cursor->pos += sizeof(struct xv6fs_dirent);
		if (cursor->pos >= size)
			break;

		unsigned int blk_off = cursor->pos % XV6FS_BSIZE;
		if (!blk_off) {
			/* Crossed into a new block — release old bh and
			 * let the top-of-loop resolver handle it. */
			if (cursor->bh) {
				brelse(cursor->bh);
				cursor->bh = NULL;
				cursor->de = NULL;
			}
			continue;
		}

		cursor->de = (struct xv6fs_dirent *)(cursor->bh->b_data + blk_off);
		if (cursor->de->inum == 0) {
			if (cursor->first_empty == -1)
				cursor->first_empty = cursor->pos;
			cursor->last_empty = cursor->pos;
		} else {
			return cursor->de;
		}
	}

	if (cursor->bh) {
		brelse(cursor->bh);
		cursor->bh = NULL;
		cursor->de = NULL;
	}

	return NULL;
}

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
	struct xv6fs_dir_cursor cursor;
	struct xv6fs_dirent *de;

	INIT_DIR_CURSOR_OFFSET(&cursor, inode, ctx->pos);

	while ((de = advance_dir_cursor(&cursor)) != NULL) {
		if (IS_ERR(de)) {
			release_dir_cursor(&cursor);
			return PTR_ERR(de);
		}

		__u16 inum = le16_to_cpu(de->inum);
		unsigned int namelen = strnlen(de->name, XV6FS_DIRSIZ);

		if (!dir_emit(ctx, de->name, namelen, inum, DT_UNKNOWN)) {
			release_dir_cursor(&cursor);
			return 0;
		}

		ctx->pos = cursor.pos + sizeof(struct xv6fs_dirent);
	}

	ctx->pos = inode->i_size;
	release_dir_cursor(&cursor);
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
static struct xv6fs_dirent *xv6fs_find_entry(struct inode *dir,
					     const struct qstr *name,
					     struct xv6fs_dir_cursor *cursor)
{
	struct xv6fs_dirent *de;

	INIT_DIR_CURSOR(cursor, dir);

	while ((de = advance_dir_cursor(cursor)) != NULL) {
		unsigned int namelen;

		if (IS_ERR(de))
			return de;

		namelen = strnlen(de->name, XV6FS_DIRSIZ);

		if (namelen == name->len &&
		    strncmp(de->name, name->name, namelen) == 0)
			return de;
	}

	return NULL;
}

static struct dentry *xv6fs_lookup(struct inode *dir, struct dentry *dentry,
				   unsigned int flags)
{
	struct xv6fs_dir_cursor cursor;
	struct xv6fs_dirent *de;
	struct inode *inode = NULL;

	if (dentry->d_name.len > XV6FS_DIRSIZ)
		return ERR_PTR(-ENAMETOOLONG);

	de = xv6fs_find_entry(dir, &dentry->d_name, &cursor);
	if (IS_ERR(de)) {
		release_dir_cursor(&cursor);
		return ERR_CAST(de);
	}
	if (de)
		inode = xv6fs_iget(dir->i_sb, le16_to_cpu(de->inum));

	release_dir_cursor(&cursor);
	return d_splice_alias(inode, dentry);
}

/* ------------------------------------------------------------------
 * Operations tables
 * ---------------------------------------------------------------- */

static int xv6fs_dir_add_entry(struct inode *dir, const struct qstr *name,
			       unsigned int inum)
{
	struct xv6fs_dir_cursor cursor;
	struct xv6fs_dirent *de;
	struct buffer_head map_bh;
	struct buffer_head *bh;
	off_t write_pos;
	unsigned int blk_idx, blk_off;
	int err;

	if (name->len > XV6FS_DIRSIZ)
		return -ENAMETOOLONG;

	/* Scan entire directory to find first empty slot */
	INIT_DIR_CURSOR(&cursor, dir);
	while ((de = advance_dir_cursor(&cursor)) != NULL) {
		if (IS_ERR(de)) {
			release_dir_cursor(&cursor);
			return PTR_ERR(de);
		}
	}
	write_pos = cursor.first_empty;
	release_dir_cursor(&cursor);

	/* No empty slot: extend */
	if (write_pos < 0)
		write_pos = dir->i_size;

	blk_idx = write_pos / XV6FS_BSIZE;
	blk_off = write_pos % XV6FS_BSIZE;

	memset(&map_bh, 0, sizeof(map_bh));
	map_bh.b_size = XV6FS_BSIZE;
	err = xv6fs_get_block(dir, blk_idx, &map_bh, 1);
	if (err)
		return err;

	if (buffer_new(&map_bh)) {
		bh = sb_getblk(dir->i_sb, map_bh.b_blocknr);
		if (!bh)
			return -EIO;
		memset(bh->b_data, 0, XV6FS_BSIZE);
		set_buffer_uptodate(bh);
	} else {
		bh = sb_bread(dir->i_sb, map_bh.b_blocknr);
		if (!bh)
			return -EIO;
	}

	de = (struct xv6fs_dirent *)(bh->b_data + blk_off);
	de->inum = cpu_to_le16(inum);
	memset(de->name, 0, XV6FS_DIRSIZ);
	memcpy(de->name, name->name, min_t(unsigned int, name->len, XV6FS_DIRSIZ));

	mark_buffer_dirty(bh);
	brelse(bh);

	if (write_pos + (off_t)sizeof(struct xv6fs_dirent) > dir->i_size)
		dir->i_size = write_pos + sizeof(struct xv6fs_dirent);
	mark_inode_dirty(dir);

	return 0;
}

static bool xv6fs_dir_is_empty(struct inode *dir)
{
	struct xv6fs_dir_cursor cursor;
	struct xv6fs_dirent *de;
	int count = 0;

	INIT_DIR_CURSOR(&cursor, dir);
	while ((de = advance_dir_cursor(&cursor)) != NULL) {
		if (IS_ERR(de)) {
			release_dir_cursor(&cursor);
			return false;
		}
		if (++count > 2) {
			release_dir_cursor(&cursor);
			return false;
		}
	}
	release_dir_cursor(&cursor);
	return true;
}

static int xv6fs_create(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode *inode;
	int err;

	inode = xv6fs_new_inode(dir, mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	err = xv6fs_dir_add_entry(dir, &dentry->d_name, inode->i_ino);
	if (err) {
		set_nlink(inode, 0);
		iput(inode);
		return err;
	}

	d_instantiate(dentry, inode);
	return 0;
}

static int xv6fs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	struct qstr dot = QSTR_INIT(".", 1);
	struct qstr dotdot = QSTR_INIT("..", 2);
	int err;

	inode = xv6fs_new_inode(dir, S_IFDIR | mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	err = xv6fs_dir_add_entry(inode, &dot, inode->i_ino);
	if (err)
		goto fail;

	err = xv6fs_dir_add_entry(inode, &dotdot, dir->i_ino);
	if (err)
		goto fail;

	err = xv6fs_dir_add_entry(dir, &dentry->d_name, inode->i_ino);
	if (err)
		goto fail;

	inode_inc_link_count(dir);
	d_instantiate(dentry, inode);
	return 0;

fail:
	set_nlink(inode, 0);
	iput(inode);
	return err;
}

static int xv6fs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct xv6fs_dir_cursor cursor;
	struct xv6fs_dirent *de;

	de = xv6fs_find_entry(dir, &dentry->d_name, &cursor);
	if (IS_ERR(de)) {
		release_dir_cursor(&cursor);
		return PTR_ERR(de);
	}
	if (!de) {
		release_dir_cursor(&cursor);
		return -ENOENT;
	}

	de->inum = 0;
	memset(de->name, 0, XV6FS_DIRSIZ);
	mark_buffer_dirty(cursor.bh);
	release_dir_cursor(&cursor);

	inode_dec_link_count(inode);
	mark_inode_dirty(dir);
	return 0;
}

static int xv6fs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int err;

	if (!xv6fs_dir_is_empty(inode))
		return -ENOTEMPTY;

	err = xv6fs_unlink(dir, dentry);
	if (err)
		return err;

	inode_dec_link_count(inode); /* drop "." */
	inode_dec_link_count(dir);   /* drop ".." */
	return 0;
}

static int xv6fs_link(struct dentry *old_dentry, struct inode *dir,
		      struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	int err;

	if (inode->i_nlink >= 65535)
		return -EMLINK;

	err = xv6fs_dir_add_entry(dir, &dentry->d_name, inode->i_ino);
	if (err)
		return err;

	inode_inc_link_count(inode);
	ihold(inode);
	d_instantiate(dentry, inode);
	return 0;
}

const struct inode_operations xv6fs_dir_inode_ops = {
	.lookup = xv6fs_lookup,
	.create = xv6fs_create,
	.mkdir  = xv6fs_mkdir,
	.unlink = xv6fs_unlink,
	.rmdir  = xv6fs_rmdir,
	.link   = xv6fs_link,
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
