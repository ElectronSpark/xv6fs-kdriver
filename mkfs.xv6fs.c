// SPDX-License-Identifier: GPL-2.0
/*
 * mkfs.xv6fs — Create an XV6 filesystem image.
 *
 * Usage:  mkfs.xv6fs [-s size] <image> [file ...]
 *
 * This is the userspace mkfs tool for xv6fs.  Once installed to /sbin/,
 * it integrates with the standard mkfs(8) dispatcher:
 *
 *     mkfs -t xv6fs /dev/sdX
 *     mkfs.xv6fs -s 1000 disk.img README.md main.c
 *
 * On-disk layout produced (each cell is one 512-byte block):
 *
 *   [ 0: boot | 1: super | 2..nlog+1: log | inodes | bitmap | data ]
 *
 * The tool computes the layout sizes automatically from the requested
 * filesystem size (in blocks) and the number of inodes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>

/* -----------------------------------------------------------------------
 * XV6 FS constants — must match xv6fs.h in the kernel module
 * --------------------------------------------------------------------- */

#define XV6FS_MAGIC      0x10203040
#define XV6FS_BSIZE      512
#define XV6FS_DIRSIZ     14
#define XV6FS_NDIRECT    12
#define XV6FS_NINDIRECT  (XV6FS_BSIZE / sizeof(uint32_t))
#define XV6FS_MAXFILE    (XV6FS_NDIRECT + XV6FS_NINDIRECT)
#define XV6FS_ROOTINO    1
#define XV6FS_IPB        (XV6FS_BSIZE / sizeof(struct xv6fs_dinode))
#define XV6FS_BPB        (XV6FS_BSIZE * 8)

#define XV6FS_T_DIR      1
#define XV6FS_T_FILE     2
#define XV6FS_T_DEV      3

/* Default filesystem size (blocks) and log size */
#define FSSIZE_DEFAULT   1000
#define LOGSIZE          30
#define NINODES_DEFAULT  200

/* -----------------------------------------------------------------------
 * On-disk structures — native endian (host == little-endian on x86)
 * --------------------------------------------------------------------- */

struct xv6fs_superblock {
	uint32_t magic;
	uint32_t size;
	uint32_t nblocks;
	uint32_t ninodes;
	uint32_t nlog;
	uint32_t logstart;
	uint32_t inodestart;
	uint32_t bmapstart;
};

struct xv6fs_dinode {
	uint16_t type;
	uint16_t major;
	uint16_t minor;
	uint16_t nlink;
	uint32_t size;
	uint32_t addrs[XV6FS_NDIRECT + 1];
};

struct xv6fs_dirent {
	uint16_t inum;
	char     name[XV6FS_DIRSIZ];
};

/* -----------------------------------------------------------------------
 * Globals
 * --------------------------------------------------------------------- */

static int                    fsfd;          /* image file descriptor     */
static struct xv6fs_superblock sb;           /* in-memory superblock      */
static uint32_t               freeinode = 1; /* next inode to allocate    */
static uint32_t               freeblock;     /* next data block to alloc  */
static char                   zeroes[XV6FS_BSIZE];

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

/** Convert to little-endian (no-op on x86, but makes intent explicit). */
static uint32_t xint(uint32_t x)
{
	/* TODO: implement proper host-to-LE conversion for big-endian hosts.
	 * On x86/x86_64 this is a no-op. */
	return x;
}

static uint16_t xshort(uint16_t x)
{
	/* TODO: same as xint — no-op on LE hosts. */
	return x;
}

/** Write @buf (exactly XV6FS_BSIZE bytes) to block @bnum. */
static void wsect(uint32_t bnum, const void *buf)
{
	if (lseek(fsfd, (off_t)bnum * XV6FS_BSIZE, SEEK_SET) < 0) {
		perror("lseek");
		exit(1);
	}
	if (write(fsfd, buf, XV6FS_BSIZE) != XV6FS_BSIZE) {
		perror("write");
		exit(1);
	}
}

/** Read block @bnum into @buf. */
static void rsect(uint32_t bnum, void *buf)
{
	if (lseek(fsfd, (off_t)bnum * XV6FS_BSIZE, SEEK_SET) < 0) {
		perror("lseek");
		exit(1);
	}
	if (read(fsfd, buf, XV6FS_BSIZE) != XV6FS_BSIZE) {
		perror("read");
		exit(1);
	}
}

/** Read inode @inum from disk into @ip. */
static void rinode(uint32_t inum, struct xv6fs_dinode *ip)
{
	/* TODO (stage: inode read)
	 *   1. Compute the block that holds this inode:
	 *        uint32_t bn = sb.inodestart + inum / XV6FS_IPB;
	 *   2. rsect(bn, buf);
	 *   3. Copy out the correct slot:
	 *        *ip = ((struct xv6fs_dinode *)buf)[inum % XV6FS_IPB];
	 */
	(void)inum;
	(void)ip;
}

/** Write inode @inum back to disk from @ip. */
static void winode(uint32_t inum, const struct xv6fs_dinode *ip)
{
	/* TODO (stage: inode write)
	 *   1. Compute the block (same formula as rinode).
	 *   2. rsect the block, overwrite the slot, wsect it back.
	 */
	(void)inum;
	(void)ip;
}

/** Allocate and return the next free inode number. */
static uint32_t ialloc(uint16_t type)
{
	/* TODO (stage: inode allocation)
	 *   1. uint32_t inum = freeinode++;
	 *   2. Zero a struct xv6fs_dinode, set din.type = xshort(type).
	 *   3. winode(inum, &din);
	 *   4. return inum;
	 */
	(void)type;
	return 0;
}

/** Mark block @used in the bitmap. */
static void balloc_mark(uint32_t used)
{
	/* TODO (stage: bitmap)
	 *   For each bitmap block that covers blocks 0..used-1:
	 *   1. Compute which bitmap block: sb.bmapstart + b / XV6FS_BPB
	 *   2. Set the appropriate bits to 1.
	 *   3. wsect() the bitmap block.
	 */
	(void)used;
}

/** Allocate a data block and return its block number. */
static uint32_t balloc(void)
{
	/* TODO (stage: block allocation)
	 *   return freeblock++;
	 */
	return 0;
}

/** Append @n bytes from @data to the file represented by inode @inum. */
static void iappend(uint32_t inum, const void *data, size_t n)
{
	/* TODO (stage: file write)
	 *   1. rinode(inum, &din) to get current state.
	 *   2. Walk din.addrs[] (direct then indirect) to find the block at
	 *      offset din.size, allocating new blocks with balloc() as needed.
	 *   3. Copy data into those blocks (one XV6FS_BSIZE chunk at a time).
	 *   4. Update din.size += n and winode(inum, &din).
	 */
	(void)inum;
	(void)data;
	(void)n;
}

/* -----------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------- */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-s size_in_blocks] <image> [file ...]\n"
		"\n"
		"  -s size   Total filesystem size in 512-byte blocks (default %d)\n"
		"  image     Output disk image path (or block device)\n"
		"  file ...  Optional files to copy into the root directory\n",
		prog, FSSIZE_DEFAULT);
	exit(1);
}

int main(int argc, char *argv[])
{
	uint32_t fssize = FSSIZE_DEFAULT;
	int opt;

	while ((opt = getopt(argc, argv, "s:")) != -1) {
		switch (opt) {
		case 's':
			fssize = (uint32_t)atoi(optarg);
			if (fssize < 100) {
				fprintf(stderr, "error: size too small (min 100 blocks)\n");
				exit(1);
			}
			break;
		default:
			usage(argv[0]);
		}
	}

	if (optind >= argc)
		usage(argv[0]);

	const char *imgpath = argv[optind++];
	int nfiles = argc - optind;

	/* Open / create the image file */
	fsfd = open(imgpath, O_RDWR | O_CREAT | O_TRUNC, 0666);
	if (fsfd < 0) {
		perror(imgpath);
		exit(1);
	}

	/* TODO (stage: layout computation)
	 *
	 * Compute the on-disk layout:
	 *
	 *   uint32_t ninodeblocks = NINODES_DEFAULT / XV6FS_IPB + 1;
	 *   uint32_t nbitmap      = fssize / XV6FS_BPB + 1;
	 *   uint32_t nmeta        = 2 + LOGSIZE + ninodeblocks + nbitmap;
	 *   uint32_t nblocks      = fssize - nmeta;
	 *
	 *   sb.magic      = xint(XV6FS_MAGIC);
	 *   sb.size       = xint(fssize);
	 *   sb.nblocks    = xint(nblocks);
	 *   sb.ninodes    = xint(NINODES_DEFAULT);
	 *   sb.nlog       = xint(LOGSIZE);
	 *   sb.logstart   = xint(2);
	 *   sb.inodestart = xint(2 + LOGSIZE);
	 *   sb.bmapstart  = xint(2 + LOGSIZE + ninodeblocks);
	 *
	 *   printf("mkfs.xv6fs: %d blocks (%d data), %d inodes, %d log\n",
	 *          fssize, nblocks, NINODES_DEFAULT, LOGSIZE);
	 */
	(void)fssize;
	(void)nfiles;

	/* TODO (stage: zero all blocks)
	 *   for (uint32_t i = 0; i < fssize; i++)
	 *       wsect(i, zeroes);
	 */

	/* TODO (stage: write superblock)
	 *   char buf[XV6FS_BSIZE];
	 *   memset(buf, 0, sizeof(buf));
	 *   memcpy(buf, &sb, sizeof(sb));
	 *   wsect(1, buf);
	 */

	/* TODO (stage: create root directory)
	 *   uint32_t rootino = ialloc(XV6FS_T_DIR);
	 *   assert(rootino == XV6FS_ROOTINO);
	 *
	 *   // Add "." entry
	 *   struct xv6fs_dirent de;
	 *   memset(&de, 0, sizeof(de));
	 *   de.inum = xshort(rootino);
	 *   strncpy(de.name, ".", XV6FS_DIRSIZ);
	 *   iappend(rootino, &de, sizeof(de));
	 *
	 *   // Add ".." entry
	 *   memset(&de, 0, sizeof(de));
	 *   de.inum = xshort(rootino);
	 *   strncpy(de.name, "..", XV6FS_DIRSIZ);
	 *   iappend(rootino, &de, sizeof(de));
	 */

	/* TODO (stage: copy input files)
	 *   for (int i = 0; i < nfiles; i++) {
	 *       const char *path = argv[optind + i];
	 *       // Extract basename for the directory entry name
	 *       const char *name = strrchr(path, '/');
	 *       name = name ? name + 1 : path;
	 *
	 *       uint32_t inum = ialloc(XV6FS_T_FILE);
	 *
	 *       // Add directory entry in root
	 *       struct xv6fs_dirent de;
	 *       memset(&de, 0, sizeof(de));
	 *       de.inum = xshort(inum);
	 *       strncpy(de.name, name, XV6FS_DIRSIZ);
	 *       iappend(rootino, &de, sizeof(de));
	 *
	 *       // Copy file contents
	 *       int fd = open(path, O_RDONLY);
	 *       if (fd < 0) { perror(path); exit(1); }
	 *       char buf[XV6FS_BSIZE];
	 *       ssize_t n;
	 *       while ((n = read(fd, buf, sizeof(buf))) > 0)
	 *           iappend(inum, buf, (size_t)n);
	 *       close(fd);
	 *
	 *       // Set nlink = 1
	 *       struct xv6fs_dinode din;
	 *       rinode(inum, &din);
	 *       din.nlink = xshort(1);
	 *       winode(inum, &din);
	 *   }
	 */

	/* TODO (stage: fix up root inode nlink)
	 *   struct xv6fs_dinode din;
	 *   rinode(rootino, &din);
	 *   din.nlink = xshort(1);
	 *   winode(rootino, &din);
	 */

	/* TODO (stage: write bitmap)
	 *   balloc_mark(freeblock);
	 */

	close(fsfd);
	printf("mkfs.xv6fs: image written to %s\n", imgpath);
	return 0;
}
