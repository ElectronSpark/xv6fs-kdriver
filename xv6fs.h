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
#include <linux/list.h>

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
#define XV6FS_IPB        (XV6FS_BSIZE / sizeof(union xv6fs_dinode))

/** Number of data-block bits tracked by one bitmap block. */
#define XV6FS_BPB        (XV6FS_BSIZE * 8)

/* -----------------------------------------------------------------------
 * On-disk structures  (use __leNN types — XV6 is little-endian)
 * --------------------------------------------------------------------- */

/**
 * struct xv6fs_dsuperblock - on-disk superblock (block 1).
 *
 * Block 0 is the boot block and is never read by the driver.
 */
struct xv6fs_dsuperblock {
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
 * struct xv6fs_superblock - in-memory superblock (host-endian).
 *
 * Converted from the on-disk xv6fs_dsuperblock by xv6fs_sb_to_cpu().
 * All fields are native __u32 — no le32_to_cpu() needed when reading.
 */
struct xv6fs_superblock {
	__u32 magic;
	__u32 size;
	__u32 nblocks;
	__u32 ninodes;
	__u32 nlog;
	__u32 logstart;
	__u32 inodestart;
	__u32 bmapstart;
};

/**
 * union xv6fs_dinode - on-disk inode.
 *
 * Packed tightly; XV6FS_IPB of these fit in one XV6FS_BSIZE block.
 */
union xv6fs_dinode {
	struct {
		__le16 type;                      /* File type (XV6FS_T_*)              */
		__le16 major;                     /* Device major (T_DEV only)          */
		__le16 minor;                     /* Device minor (T_DEV only)          */
		__le16 nlink;                     /* Number of hard links               */
		__le32 size;                      /* File size in bytes                 */
		__le32 addrs[XV6FS_NDIRECT + 1];  /* Direct block addrs + 1 indirect    */
	};
	struct {
		__le16 type_padding;
		struct list_head list; /* For free inode list in xv6fs_sb_info */
	};
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
	struct xv6fs_superblock raw_sb;  /* Host-endian copy of the on-disk superblock */
	struct buffer_head *bh;     /* Buffer head that holds the superblock block */
	
	union xv6fs_dinode *dinodes; /* In-memory copy of the inode blocks (array of xv6fs_dinode) */
	struct list_head free_inodes_list; /* List of free inodes (for ialloc) */
	__u32 free_inodes; 	 /* Count of free inodes (for bmap) */
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
 * Derived block-number macros
 *
 * These accept a pointer to the in-memory superblock copy (xv6fs_superblock).
 * --------------------------------------------------------------------- */
/** Block number of the inode block that holds inode @ino. */
#define XV6FS_IBLOCK(ino, sbi)  ((ino) / XV6FS_IPB + (sbi)->raw_sb.inodestart)
#define XV6FS_IOFFSET(ino)      ((ino) % XV6FS_IPB)

/** Block number of the bitmap block that tracks data block @b. */
#define XV6FS_BBLOCK(b, sbi)    ((b) / XV6FS_BPB + (sbi)->raw_sb.bmapstart)

/** Block number of the first data block (bmapstart + nbitmap). */
#define XV6FS_DSTART(sbi)  ((sbi)->raw_sb.bmapstart + (sbi)->raw_sb.size / XV6FS_BPB + 1)

/** Block number of the data block corresponding to the logical block @d in a file. */
#define XV6FS_DBLOCK(d, sbi)   ((d) + XV6FS_DSTART(sbi))

/* -----------------------------------------------------------------------
 * Endian-conversion helpers
 *
 * XV6 stores all integers in little-endian format on disk.  The kernel
 * works in host-endian (CPU-native) format.  These helpers centralise
 * the conversion so callers don't have to sprinkle le*_to_cpu / cpu_to_le*
 * everywhere.  On LE hosts the swap macros are no-ops, so there is no
 * runtime cost.
 *
 * "dinode_to_cpu"  — disk  → host  (used when reading an inode from disk)
 * "dinode_to_disk" — host  → disk  (used when writing an inode to disk)
 * "dirent_to_cpu"  — disk  → host  (used when reading a dir entry)
 * "dirent_to_disk" — host  → disk  (used when writing a dir entry)
 *
 * The name field in xv6fs_dirent is a byte array and does not need
 * endian conversion, so only the @inum field is swapped.
 *
 * For union xv6fs_dinode — ALL fields are converted:
 *   __le16: type, major, minor, nlink
 *   __le32: size, addrs[0..XV6FS_NDIRECT]   (13 entries)
 *
 * nlink and size live in the VFS inode, so the helpers take a
 * struct inode * alongside xv6fs_inode_info * to access them.
 * --------------------------------------------------------------------- */

/**
 * xv6fs_dinode_to_cpu - convert an on-disk inode to host-endian.
 * @raw:   pointer to the on-disk inode (fields are __le16 / __le32).
 * @ei:    pointer to the in-memory xv6fs_inode_info to populate.
 *
 * Converts every field.  The VFS inode is accessed via ei->vfs_inode.
 * Does NOT set i_mode, i_op, i_fop —
 * the caller must do that based on ei->i_type after this returns.
 */
static inline void xv6fs_dinode_to_cpu(const union xv6fs_dinode *raw,
				       struct xv6fs_inode_info *ei)
{
	struct inode *inode = &ei->vfs_inode;
	int i;

	ei->i_type  = le16_to_cpu(raw->type);
	ei->i_major = le16_to_cpu(raw->major);
	ei->i_minor = le16_to_cpu(raw->minor);
	set_nlink(inode, le16_to_cpu(raw->nlink));
	inode->i_size = le32_to_cpu(raw->size);

	for (i = 0; i < XV6FS_NDIRECT + 1; i++)
		ei->addrs[i] = le32_to_cpu(raw->addrs[i]);
}

/**
 * xv6fs_dinode_to_disk - convert host-endian inode info back to on-disk format.
 * @ei:    pointer to the in-memory xv6fs_inode_info (host-endian fields).
 * @raw:   pointer to the on-disk inode buffer to fill.
 *
 * Converts every field including nlink and size from ei->vfs_inode.
 */
static inline void xv6fs_dinode_to_disk(const struct xv6fs_inode_info *ei,
					union xv6fs_dinode *raw)
{
	const struct inode *inode = &ei->vfs_inode;
	int i;

	raw->type  = cpu_to_le16(ei->i_type);
	raw->major = cpu_to_le16(ei->i_major);
	raw->minor = cpu_to_le16(ei->i_minor);
	raw->nlink = cpu_to_le16(inode->i_nlink);
	raw->size  = cpu_to_le32(inode->i_size);

	for (i = 0; i < XV6FS_NDIRECT + 1; i++)
		raw->addrs[i] = cpu_to_le32(ei->addrs[i]);
}

/**
 * xv6fs_dirent_to_cpu - convert an on-disk directory entry to host-endian.
 * @disk: pointer to the on-disk directory entry (inum is __le16).
 * @host: pointer to the host-endian output struct to populate.
 */
static inline void xv6fs_dirent_to_cpu(const struct xv6fs_dirent *disk,
					struct xv6fs_dirent *host)
{
	host->inum = le16_to_cpu(disk->inum);
	memcpy(host->name, disk->name, XV6FS_DIRSIZ);
}

/**
 * xv6fs_dirent_to_disk - convert a host-endian directory entry to on-disk format.
 * @host: pointer to the host-endian directory entry.
 * @disk: pointer to the on-disk buffer to fill (inum becomes __le16).
 */
static inline void xv6fs_dirent_to_disk(const struct xv6fs_dirent *host,
					 struct xv6fs_dirent *disk)
{
	disk->inum = cpu_to_le16(host->inum);
	memcpy(disk->name, host->name, XV6FS_DIRSIZ);
}

/**
 * xv6fs_sb_to_cpu - convert an on-disk superblock to host-endian.
 * @disk: pointer to the on-disk superblock (fields are __le32).
 * @host: pointer to the host-endian xv6fs_superblock to populate.
 */
static inline void xv6fs_sb_to_cpu(const struct xv6fs_dsuperblock *disk,
				   struct xv6fs_superblock *host)
{
	host->magic      = le32_to_cpu(disk->magic);
	host->size       = le32_to_cpu(disk->size);
	host->nblocks    = le32_to_cpu(disk->nblocks);
	host->ninodes    = le32_to_cpu(disk->ninodes);
	host->nlog       = le32_to_cpu(disk->nlog);
	host->logstart   = le32_to_cpu(disk->logstart);
	host->inodestart = le32_to_cpu(disk->inodestart);
	host->bmapstart  = le32_to_cpu(disk->bmapstart);
}

/**
 * xv6fs_sb_to_disk - convert a host-endian superblock to on-disk format.
 * @host: pointer to the host-endian xv6fs_superblock.
 * @disk: pointer to the on-disk buffer to fill.
 */
static inline void xv6fs_sb_to_disk(const struct xv6fs_superblock *host,
				    struct xv6fs_dsuperblock *disk)
{
	disk->magic      = cpu_to_le32(host->magic);
	disk->size       = cpu_to_le32(host->size);
	disk->nblocks    = cpu_to_le32(host->nblocks);
	disk->ninodes    = cpu_to_le32(host->ninodes);
	disk->nlog       = cpu_to_le32(host->nlog);
	disk->logstart   = cpu_to_le32(host->logstart);
	disk->inodestart = cpu_to_le32(host->inodestart);
	disk->bmapstart  = cpu_to_le32(host->bmapstart);
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
int  xv6fs_get_block(struct inode *inode, sector_t lblk,
		     struct buffer_head *bh_result, int create);
int xv6fs_load_dinodes(struct super_block *sb);
void xv6fs_release_dinodes(struct xv6fs_sb_info *sbi);

/* dir.c */
extern const struct inode_operations xv6fs_dir_inode_ops;
extern const struct file_operations  xv6fs_dir_fops;

/* file.c */
extern const struct file_operations xv6fs_file_fops;

/* bmap.c */
long xv6fs_b_count_free(struct super_block *sb);
long xv6fs_balloc(struct super_block *sb);
void xv6fs_bfree(struct super_block *sb, sector_t b);

#endif /* _XV6FS_H */
