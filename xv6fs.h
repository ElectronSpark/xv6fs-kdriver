/* SPDX-License-Identifier: GPL-2.0 */
/*
 * xv6fs.h - XV6 File System Driver for Linux
 *
 * Defines the XV6 on-disk layout, in-memory types, constants, and all
 * cross-file declarations used by the driver.
 *
 * XV6 FS disk layout (each cell is one 512-byte block):
 *
 *  [ 0: boot | 1: superblock | log blocks | inode blocks | bitmap | data blocks ]
 *
 * XV6 FS known limitations (compared to modern file systems):
 *  - No timestamps (atime / mtime / ctime) — fake them as zero
 *  - Filenames are at most 14 bytes (XV6FS_DIRSIZ), NOT null-terminated when full
 *  - No permission bits beyond file type — fake mode as 0755 (dir) / 0644 (file)
 *  - No user/group ownership — fake as root
 *  - No symbolic links
 *  - Single indirect block only → max file size is (12 + 128) × 512 = 71 680 bytes
 *  - No sparse file support
 */

#ifndef _XV6FS_H
#define _XV6FS_H

#include <linux/fs.h>
#include <linux/types.h>
#include <linux/version.h>

/* -----------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------- */

/** Magic number stored in the on-disk superblock. */
#define XV6FS_MAGIC      0x10203040

/** Size of one XV6 disk block in bytes. */
#define XV6FS_BSIZE      512

/** Maximum length of a filename (bytes). NOT null-terminated when full. */
#define XV6FS_DIRSIZ     14

/** Number of direct block pointers per inode. */
#define XV6FS_NDIRECT    12

/** Number of block pointers that fit in one indirect block. */
#define XV6FS_NINDIRECT  (XV6FS_BSIZE / sizeof(__u32))

/** Maximum file size in blocks (direct + one level of indirect). */
#define XV6FS_MAXFILE    (XV6FS_NDIRECT + XV6FS_NINDIRECT)

/** Inode number of the root directory. */
#define XV6FS_ROOTINO    1

/* XV6 inode types stored in dinode.type */
#define XV6FS_T_DIR      1   /* directory */
#define XV6FS_T_FILE     2   /* regular file */
#define XV6FS_T_DEV      3   /* device file */

/** Number of on-disk inodes that fit in one block. */
#define XV6FS_IPB        (XV6FS_BSIZE / sizeof(struct xv6fs_dinode))

/** Number of data-block bits tracked by one bitmap block. */
#define XV6FS_BPB        (XV6FS_BSIZE * 8)

/* -----------------------------------------------------------------------
 * Derived block-number macros
 *
 * These accept a pointer to the in-memory superblock copy (xv6fs_superblock).
 * --------------------------------------------------------------------- */

/** Block number of the inode block that holds inode @ino. */
#define XV6FS_IBLOCK(ino, sb)  ((ino) / XV6FS_IPB + (sb)->inodestart)

/** Block number of the bitmap block that tracks data block @b. */
#define XV6FS_BBLOCK(b, sb)    ((b)   / XV6FS_BPB + (sb)->bmapstart)

/* -----------------------------------------------------------------------
 * On-disk structures  (use __leNN types — XV6 is little-endian)
 * --------------------------------------------------------------------- */

/**
 * struct xv6fs_superblock - on-disk superblock (block 1).
 *
 * Block 0 is the boot block and is never read by the driver.
 */
struct xv6fs_superblock {
	__le32 magic;       /* Must equal XV6FS_MAGIC                    */
	__le32 size;        /* Total blocks in the file system           */
	__le32 nblocks;     /* Number of data blocks                     */
	__le32 ninodes;     /* Number of inodes                          */
	__le32 nlog;        /* Number of log blocks                      */
	__le32 logstart;    /* Block number of the first log block       */
	__le32 inodestart;  /* Block number of the first inode block     */
	__le32 bmapstart;   /* Block number of the first free-map block  */
};

/**
 * struct xv6fs_dinode - on-disk inode.
 *
 * Packed tightly; XV6FS_IPB of these fit in one XV6FS_BSIZE block.
 */
struct xv6fs_dinode {
	__le16 type;                      /* File type (XV6FS_T_*)              */
	__le16 major;                     /* Device major (T_DEV only)          */
	__le16 minor;                     /* Device minor (T_DEV only)          */
	__le16 nlink;                     /* Number of hard links               */
	__le32 size;                      /* File size in bytes                 */
	__le32 addrs[XV6FS_NDIRECT + 1];  /* Direct block addrs + 1 indirect    */
};

/**
 * struct xv6fs_dirent - on-disk directory entry.
 *
 * Directories are a flat, unsorted array of these fixed-size records.
 * An entry with inum == 0 is free (deleted or never used).
 */
struct xv6fs_dirent {
	__le16 inum;              /* Inode number; 0 means the slot is free */
	char   name[XV6FS_DIRSIZ]; /* Filename — not null-terminated if full */
};

/* -----------------------------------------------------------------------
 * In-memory structures
 * --------------------------------------------------------------------- */

/**
 * struct xv6fs_sb_info - driver-private data attached to struct super_block.
 *
 * Access via:  xv6fs_sb(sb)  →  struct xv6fs_sb_info *
 * Stored in:   sb->s_fs_info
 */
struct xv6fs_sb_info {
	struct xv6fs_superblock raw_sb; /* Host-endian copy of the on-disk superblock */
	struct buffer_head     *bh;     /* Buffer head that holds the superblock block */
};

/**
 * struct xv6fs_inode_info - driver-private data embedded in each VFS inode.
 *
 * Access via:  xv6fs_i(inode)  →  struct xv6fs_inode_info *
 *
 * The VFS inode MUST be the last field so that container_of() works
 * correctly with the inode slab cache.
 */
struct xv6fs_inode_info {
	__u32  addrs[XV6FS_NDIRECT + 1]; /* Block addresses (host-endian)   */
	__s16  i_type;                   /* XV6 inode type (XV6FS_T_*)      */
	__s16  i_major;                  /* Device major (T_DEV only)       */
	__s16  i_minor;                  /* Device minor (T_DEV only)       */
	struct inode vfs_inode;          /* Embedded VFS inode — keep last! */
};

/* -----------------------------------------------------------------------
 * Inline accessors
 * --------------------------------------------------------------------- */

/** Return the xv6fs_inode_info that wraps @inode. */
static inline struct xv6fs_inode_info *xv6fs_i(struct inode *inode)
{
	return container_of(inode, struct xv6fs_inode_info, vfs_inode);
}

/** Return the xv6fs_sb_info attached to @sb. */
static inline struct xv6fs_sb_info *xv6fs_sb(struct super_block *sb)
{
	return sb->s_fs_info;
}

/* -----------------------------------------------------------------------
 * Kernel-version compatibility shims
 *
 * Kernel 6.6 made i_ctime opaque; use the accessor on newer kernels.
 * --------------------------------------------------------------------- */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
# define XV6FS_ZERO_CTIME(inode)  inode_set_ctime((inode), 0, 0)
#else
# define XV6FS_ZERO_CTIME(inode)  ((inode)->i_ctime = (struct timespec64){ 0, 0 })
#endif

/* -----------------------------------------------------------------------
 * Cross-file declarations
 * --------------------------------------------------------------------- */

/* super.c */
extern const struct super_operations xv6fs_super_ops;
int xv6fs_fill_super(struct super_block *sb, void *data, int silent);
void xv6fs_release_sbi(struct xv6fs_sb_info *sbi);

/* inode.c */
extern const struct inode_operations xv6fs_file_inode_ops;
extern const struct address_space_operations xv6fs_aops;
int  xv6fs_init_inode_cache(void);
void xv6fs_destroy_inode_cache(void);
struct inode *xv6fs_alloc_inode(struct super_block *sb);
void          xv6fs_destroy_inode(struct inode *inode);
struct inode *xv6fs_iget(struct super_block *sb, unsigned long ino);
int  xv6fs_get_block(struct inode *inode, sector_t iblock,
		     struct buffer_head *bh_result, int create);

/* dir.c */
extern const struct inode_operations xv6fs_dir_inode_ops;
extern const struct file_operations  xv6fs_dir_fops;

/* file.c */
extern const struct file_operations xv6fs_file_fops;

#endif /* _XV6FS_H */
