// SPDX-License-Identifier: GPL-2.0
/*
 * xv6fs.c - XV6 File System Driver: module entry/exit
 *
 * Responsibilities of this file:
 *   - Define the struct file_system_type for "xv6fs"
 *   - Register / unregister the filesystem with the VFS
 *   - Initialise and tear down the inode slab cache
 *
 * You should not need to change this file.  All FS logic lives in
 * super.c, inode.c, dir.c, and file.c.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include "xv6fs.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("XV6 File System Driver");
MODULE_VERSION("0.1");

/* ------------------------------------------------------------------
 * File system type
 * ---------------------------------------------------------------- */

/*
 * xv6fs_mount - VFS calls this when the user runs:
 *   mount -t xv6fs /dev/sdXN /mnt/point
 *
 * API: mount_bdev(fs_type, flags, dev_name, data, fill_super)
 *   Opens the block device and calls xv6fs_fill_super() for you.
 *   For network / pseudo filesystems use mount_nodev() instead.
 *   Declared in <linux/fs.h>.
 */
static struct dentry *xv6fs_mount(struct file_system_type *fs_type, int flags,
				  const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, xv6fs_fill_super);
}

/*
 * xv6fs_kill_sb - VFS calls this when the filesystem is unmounted.
 *
 * API: kill_block_super(sb)
 *   Flushes dirty buffers and releases the block device.
 *   Declared in <linux/fs.h>.
 *
 * Any memory you allocated in xv6fs_fill_super (e.g. xv6fs_sb_info)
 * must be freed in xv6fs_put_super (super.c) *before* kill_block_super
 * is called.
 */
static void xv6fs_kill_sb(struct super_block *sb)
{
	kill_block_super(sb);
}

static struct file_system_type xv6fs_fs_type = {
	.owner    = THIS_MODULE,
	.name     = "xv6fs",
	.mount    = xv6fs_mount,
	.kill_sb  = xv6fs_kill_sb,
	.fs_flags = FS_REQUIRES_DEV,   /* backed by a block device */
};

/* ------------------------------------------------------------------
 * Module init / exit
 * ---------------------------------------------------------------- */

static int __init xv6fs_init(void)
{
	int err;

	/*
	 * Step 1: Create the inode slab cache.
	 *
	 * Every filesystem that embeds private data in struct inode
	 * must allocate inodes from its own slab cache so the VFS
	 * knows the right allocation size.
	 *
	 * See xv6fs_init_inode_cache() in inode.c.
	 */
	err = xv6fs_init_inode_cache();
	if (err) {
		pr_err("xv6fs: failed to create inode cache: %d\n", err);
		return err;
	}

	/*
	 * Step 2: Register the filesystem type.
	 *
	 * API: register_filesystem(fs_type)
	 *   Makes "xv6fs" visible to mount(2) and /proc/filesystems.
	 *   Declared in <linux/fs.h>.
	 */
	err = register_filesystem(&xv6fs_fs_type);
	if (err) {
		pr_err("xv6fs: failed to register filesystem: %d\n", err);
		xv6fs_destroy_inode_cache();
		return err;
	}

	pr_info("xv6fs: filesystem registered\n");
	return 0;
}

static void __exit xv6fs_exit(void)
{
	unregister_filesystem(&xv6fs_fs_type);
	xv6fs_destroy_inode_cache();
	pr_info("xv6fs: filesystem unregistered\n");
}

module_init(xv6fs_init);
module_exit(xv6fs_exit);
