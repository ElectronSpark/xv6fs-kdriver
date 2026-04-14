// SPDX-License-Identifier: GPL-2.0
/*
 * file.c - File operations for XV6 FS
 *
 * Roadmap stage 4 (file reads) and stage 5 (file writes).
 *
 * The read path delegates almost entirely to the Linux page cache:
 *   read(2) → generic_file_read_iter() → page cache →
 *   xv6fs_read_folio() (inode.c) → xv6fs_get_block() (inode.c) → disk I/O
 *
 * Your job is to implement xv6fs_get_block() and xv6fs_read_folio() in
 * inode.c.  The file_operations wiring below can stay as-is.
 *
 * For writes (stage 5) you will need to replace the .write_iter stub
 * with generic_file_write_iter() and implement the write-side helpers
 * in inode.c (.write_begin / .write_end in address_space_operations).
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include "xv6fs.h"

/* ------------------------------------------------------------------
 * File operations table
 *
 * .read_iter  — generic_file_read_iter() drives the page-cache read path.
 *               It calls address_space_operations.read_folio (inode.c)
 *               when a page is not yet cached.
 *               Declared in <linux/fs.h>.
 *
 * .write_iter — TODO (stage 5): replace the stub with
 *               generic_file_write_iter() once you have implemented
 *               .write_begin / .write_end in xv6fs_aops (inode.c).
 *               Declared in <linux/fs.h>.
 *
 * .mmap       — generic_file_mmap() wires mmap(2) into the page cache.
 *               Declared in <linux/mm.h>.
 *
 * .llseek     — generic_file_llseek() handles lseek(2) correctly for
 *               regular files.  Declared in <linux/fs.h>.
 *
 * .fsync      — generic_file_fsync() flushes dirty pages and, if the
 *               underlying device supports it, issues a cache-flush
 *               command.  Declared in <linux/fs.h>.
 *               Add this once you have write support (stage 5).
 * ---------------------------------------------------------------- */

/*
 * xv6fs_file_write_iter - stub write handler.
 *
 * XV6 FS write support is not yet implemented.
 * TODO (stage 5): replace the body with:
 *   return generic_file_write_iter(iocb, from);
 * and implement .write_begin / .write_end in xv6fs_aops (inode.c).
 *
 * API hints:
 *   generic_file_write_iter(iocb, from)  — <linux/fs.h>
 *   iov_iter_count(from)                 — <linux/uio.h>; check for empty write
 *   generic_write_checks(iocb, from)     — <linux/fs.h>; validate write params
 *
 * VFS guarantees:
 *   - @iocb->ki_filp is a valid, open regular file with write access.
 *   - inode->i_rwsem is held (via fdget_pos / generic write path).
 *   - @from describes the user buffer; iov_iter_count(from) >= 0.
 */
static ssize_t xv6fs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	return generic_file_write_iter(iocb, from);
}

const struct file_operations xv6fs_file_fops = {
	.read_iter  = generic_file_read_iter,
	.write_iter = xv6fs_file_write_iter,
	.fsync      = generic_file_fsync,
	.mmap       = generic_file_mmap,
	.llseek     = generic_file_llseek,
};
