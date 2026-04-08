# xv6fs-kdriver

A Linux kernel module that mounts XV6 file system images as a native Linux
filesystem.  This repository is a **starting-point skeleton** — the data
structures and module boilerplate are in place; the driver logic is left for
you to implement following the roadmap below.

---

## Table of Contents

1. [XV6 FS Overview](#xv6-fs-overview)
2. [Project Structure](#project-structure)
3. [Prerequisites](#prerequisites)
4. [Build & Load](#build--load)
5. [Creating a Test Image](#creating-a-test-image)
6. [Roadmap](#roadmap)
7. [XV6 FS Limitations](#xv6-fs-limitations)

---

## XV6 FS Overview

XV6 FS is the simple Unix-like file system used in the
[xv6 teaching OS](https://github.com/mit-pdos/xv6-public).  Its on-disk
layout is:

```
Block 0      : boot block (unused by the driver)
Block 1      : superblock
Blocks 2…    : log blocks
             : inode blocks
             : free-block bitmap
             : data blocks
```

All on-disk integers are **little-endian** 16- or 32-bit values.

---

## Project Structure

```
xv6fs-kdriver/
├── Makefile      — build rules (targets: all, clean, load, unload)
├── xv6fs.h       — on-disk structs, in-memory types, constants,
│                   cross-file declarations
├── xv6fs_main.c  — module init/exit; filesystem type registration
├── super.c       — superblock operations  (stage 1)
├── inode.c       — inode cache, block mapping  (stages 2 & 4)
├── dir.c         — directory readdir & lookup  (stage 3)
└── file.c        — file read/write operations  (stages 4 & 5)
```

---

## Prerequisites

Install the kernel headers and build tools for your **running** kernel:

```bash
sudo apt-get update
sudo apt-get install build-essential linux-headers-$(uname -r)
```

> **WSL2 note:** WSL2 uses a Microsoft-supplied kernel.  The standard
> `linux-headers-$(uname -r)` package may not exist.  You have two options:
>
> **Option A — build inside a native Linux VM / dual-boot:**
> The simplest and most reliable approach for kernel module development.
>
> **Option B — build the WSL2 kernel from source:**
> ```bash
> # 1. Clone the Microsoft WSL2 kernel
> git clone --depth=1 https://github.com/microsoft/WSL2-Linux-Kernel.git
> cd WSL2-Linux-Kernel
>
> # 2. Build it (use the WSL2 config as a starting point)
> cp Microsoft/config-wsl .config
> make -j$(nproc) KCONFIG_CONFIG=.config
>
> # 3. Point the Makefile at this tree
> make -C /path/to/xv6fs-kdriver KDIR=$(pwd)
> ```

The module targets kernel **5.19 or later** (uses the folio-based page-cache
API).  It has been tested on Ubuntu 22.04 HWE (5.19+) and Ubuntu 24.04
(6.8).  See [XV6 FS Limitations](#xv6-fs-limitations) for API compatibility
notes.

---

## Build & Load

### Build

```bash
# From the repository root
make

# Or point at a different kernel source tree
make KDIR=/path/to/linux
```

A successful build produces `xv6fs.ko`.

### Load the module

```bash
sudo insmod xv6fs.ko

# Verify it registered
cat /proc/filesystems | grep xv6fs

# Watch kernel messages
dmesg | tail
```

### Unload the module

```bash
sudo rmmod xv6fs
```

### Rebuild after changes

```bash
make clean && make
```

---

## Creating a Test Image

You need an XV6-formatted disk image to test the driver.  The easiest way is
to use the `mkfs` utility from the [xv6 source](https://github.com/mit-pdos/xv6-public):

```bash
# 1. Clone xv6 and build its mkfs tool
git clone https://github.com/mit-pdos/xv6-public.git
cd xv6-public
make fs.img       # produces fs.img (an XV6-formatted disk image)
cd ..
```

Then mount it (once the driver is functional enough to mount):

```bash
# Attach the image to a loop device
sudo losetup -fP xv6-public/fs.img
LOOP=$(losetup -j xv6-public/fs.img | cut -d: -f1)

# Create a mount point and mount
sudo mkdir -p /mnt/xv6
sudo mount -t xv6fs "$LOOP" /mnt/xv6

# Browse the filesystem
ls /mnt/xv6

# Unmount and detach
sudo umount /mnt/xv6
sudo losetup -d "$LOOP"
```

---

## Roadmap

Work through the stages in order.  Each stage builds on the previous one.
The files and functions you need to edit are called out explicitly.

---

### Stage 1 — Read-Only Mount (`super.c`)

**Goal:** `sudo mount -t xv6fs /dev/loop0 /mnt/xv6` succeeds without errors.

**Functions to implement:**

| Function | File | Notes |
|---|---|---|
| `xv6fs_fill_super` | `super.c` | Core mount logic |
| `xv6fs_put_super` | `super.c` | Cleanup on unmount |
| `xv6fs_statfs` | `super.c` | Powers `df -h` |

**Step-by-step for `xv6fs_fill_super`:**

1. `sb_set_blocksize(sb, XV6FS_BSIZE)` — set the 512-byte block size.
2. `sb_bread(sb, 1)` — read block 1 (the superblock).
3. Check `le32_to_cpu(raw->magic) == XV6FS_MAGIC`; return `-EINVAL` if not.
4. `kzalloc(sizeof(struct xv6fs_sb_info), GFP_KERNEL)` — allocate `sbi`; copy
   all fields with `le32_to_cpu`; store `bh` in `sbi->bh`; assign
   `sb->s_fs_info = sbi`.
5. Set `sb->s_magic`, `sb->s_op`, `sb->s_maxbytes`.
6. `xv6fs_iget(sb, XV6FS_ROOTINO)` — get root inode (implements in stage 2).
7. `d_make_root(root_inode)` — create the root dentry; assign to `sb->s_root`.

**Key APIs** (`#include <linux/buffer_head.h>`, `<linux/slab.h>`):

```c
sb_set_blocksize(sb, size);           // set logical block size
struct buffer_head *bh = sb_bread(sb, block_nr); // read one block
brelse(bh);                           // release when done
void *sbi = kzalloc(size, GFP_KERNEL);
le32_to_cpu(val);                     // little-endian to host
d_make_root(inode);                   // create root dentry
```

---

### Stage 2 — Inode Reads (`inode.c`)

**Goal:** The root directory inode is successfully read; `ls /mnt/xv6` does
not crash (it may return empty results — that is fine until stage 3).

**Functions to implement:**

| Function | File | Notes |
|---|---|---|
| `xv6fs_iget` | `inode.c` | Look up / read an inode by number |

**Key formula:**

```c
// Which block holds inode number ino?
__u32 block = XV6FS_IBLOCK(ino, &sbi->raw_sb);

// Which slot within that block?
struct xv6fs_dinode *raw = (struct xv6fs_dinode *)bh->b_data;
raw += ino % XV6FS_IPB;
```

**XV6 → VFS inode field mapping:**

| XV6 `dinode` field | VFS `inode` field | Notes |
|---|---|---|
| `type` | `i_mode` (S_IFDIR / S_IFREG / S_IFCHR) | also selects `i_op`, `i_fop` |
| `nlink` | `set_nlink(inode, n)` | |
| `size` | `inode->i_size` | |
| `major`, `minor` | `init_special_inode(...)` | T_DEV only |
| *(none)* | `i_uid`, `i_gid` | fake as `make_kuid/kgid(&init_user_ns, 0)` |
| *(none)* | `i_atime`, `i_mtime`, ctime | fake as zero (XV6 has no timestamps) |

**Key APIs** (`<linux/fs.h>`, `<linux/uidgid.h>`):

```c
struct inode *inode = iget_locked(sb, ino);
if (!(inode->i_state & I_NEW))
    return inode;             // already cached, return immediately

// … fill in inode fields …

unlock_new_inode(inode);      // announce to other waiters
// OR on error:
iget_failed(inode);           // discard the half-initialised inode
return ERR_PTR(-EIO);

set_nlink(inode, n);
make_kuid(&init_user_ns, 0);
XV6FS_ZERO_CTIME(inode);      // compat macro in xv6fs.h
init_special_inode(inode, mode, MKDEV(major, minor));
```

---

### Stage 3 — Directory Listing (`dir.c`)

**Goal:** `ls /mnt/xv6` prints the correct filenames.

**Functions to implement:**

| Function | File | Notes |
|---|---|---|
| `xv6fs_readdir` | `dir.c` | Powers `ls`, `find`, `getdents(2)` |
| `xv6fs_lookup` | `dir.c` | Powers every path component resolution |

**Key APIs** (`<linux/fs.h>`):

```c
// Emit one directory entry; returns false when the buffer is full
bool ok = dir_emit(ctx, name, namelen, ino, DT_UNKNOWN);

// Emit "." and ".." automatically (optional convenience)
dir_emit_dots(file, ctx);

// ctx->pos is the byte offset; advance it after each emitted entry:
ctx->pos += sizeof(struct xv6fs_dirent);

// In lookup: return the child dentry (or a negative dentry)
return d_splice_alias(inode, dentry); // inode==NULL → not found
```

**Important:** XV6 filenames are **not** null-terminated when they fill all
`XV6FS_DIRSIZ` bytes.  When matching a name in `lookup`, compare using both
the length from `dentry->d_name.len` and `strnlen` to avoid false positives
(e.g. "foo" must not match "foobar"):

```c
size_t dlen = dentry->d_name.len;
if (strnlen(dirent->name, XV6FS_DIRSIZ) == dlen &&
    strncmp(dirent->name, dentry->d_name.name, dlen) == 0)
    /* found */
```

---

### Stage 4 — File Reads (`inode.c`)

**Goal:** `cat /mnt/xv6/README` prints the correct file contents.

**Functions to implement:**

| Function | File | Notes |
|---|---|---|
| `xv6fs_get_block` | `inode.c` | Maps logical block → disk block |
| `xv6fs_read_folio` | `inode.c` | Feeds the page cache |
| `xv6fs_bmap` | `inode.c` | Used by `lsof`, swap, etc. |

The read data-flow is:
```
read(2)
  └─ generic_file_read_iter()   [page cache]
       └─ xv6fs_read_folio()
            └─ mpage_read_folio(folio, xv6fs_get_block)
                 └─ xv6fs_get_block()  ← you implement this
                      └─ disk I/O
```

**Key APIs** (`<linux/buffer_head.h>`, `<linux/mpage.h>`):

```c
// Map a logical block to a disk block and fill bh_result
map_bh(bh_result, sb, disk_block_nr);

// Read the indirect block to resolve indirect addresses
struct buffer_head *ibh = sb_bread(sb, ei->addrs[XV6FS_NDIRECT]);
__u32 disk_block = le32_to_cpu(((__u32 *)ibh->b_data)[idx]);
brelse(ibh);

// Delegate page reading to the mpage layer (kernel >= 5.19)
return mpage_read_folio(folio, xv6fs_get_block);

// Map a file block to a sector (for bmap)
return generic_block_bmap(mapping, block, xv6fs_get_block);
```

---

### Stage 5 — Write Support (`inode.c`, `dir.c`, `file.c`)

**Goal:** `cp file /mnt/xv6/` and `mkdir /mnt/xv6/dir` work correctly.

This stage has multiple sub-tasks; tackle them in this order:

#### 5a — Block allocation

Scan the free-block bitmap (blocks starting at `sbi->raw_sb.bmapstart`)
for a zero bit, mark it as used, and return the block number.

```c
// Read a bitmap block, find a free bit, set it, mark dirty
struct buffer_head *bbh = sb_bread(sb, bitmap_block_nr);
// scan bbh->b_data for a zero bit ...
mark_buffer_dirty(bbh);
brelse(bbh);
```

#### 5b — Inode allocation

Scan the inode blocks for a `dinode` with `type == 0` (free), write the
new type, and return the inode number.

#### 5c — Buffered file writes

```c
// In xv6fs_aops (inode.c) — needs a thin wrapper:
static int xv6fs_write_begin(struct file *file,
                             struct address_space *mapping,
                             loff_t pos, unsigned int len,
                             struct page **pagep, void **fsdata)
{
    return block_write_begin(mapping, pos, len, pagep, xv6fs_get_block);
}

.write_begin = xv6fs_write_begin,  // <linux/buffer_head.h>
.write_end   = generic_write_end,  // <linux/pagemap.h>

// In file.c:
.write_iter  = generic_file_write_iter,
```

#### 5d — Directory mutations

Add these callbacks to `xv6fs_dir_inode_ops` in `dir.c`:

```c
.create = xv6fs_create,   // create a regular file
.mkdir  = xv6fs_mkdir,    // create a directory
.unlink = xv6fs_unlink,   // remove a file
.rmdir  = xv6fs_rmdir,    // remove a directory
.link   = xv6fs_link,     // create a hard link
```

Key APIs for directory mutation:

```c
mark_inode_dirty(inode);       // schedule inode write-back
d_instantiate(dentry, inode);  // attach inode to a freshly created dentry
insert_inode_hash(inode);      // add inode to the inode cache hash
new_inode(sb);                 // allocate a VFS inode (calls alloc_inode)
```

---

### Stage 6 — XV6 Logging Layer (`log.c`)

**Goal:** Survive a crash mid-write without corrupting the filesystem.

XV6 uses a simple write-ahead log: all writes go through the log first, then
are committed atomically to their final locations.  This is the most complex
stage; consult the XV6 source code (`log.c`) for the on-disk protocol.

Sketch of the API you will build:

```c
void xv6fs_log_begin(struct super_block *sb);   // start a transaction
struct buffer_head *xv6fs_log_write(struct super_block *sb,
                                    struct buffer_head *bh); // pin a block
void xv6fs_log_commit(struct super_block *sb);  // commit + install
void xv6fs_log_recover(struct super_block *sb); // replay on mount
```

---

## XV6 FS Limitations

These are **by design** in XV6 and cannot be changed without modifying the
on-disk format:

| Feature | Status | Driver workaround |
|---|---|---|
| Timestamps (atime/mtime/ctime) | ❌ Not stored | Report as Unix epoch (0) |
| File permissions (mode bits) | ❌ Not stored | Report dirs as `0755`, files as `0644` |
| User/group ownership | ❌ Not stored | Report as `root:root` |
| Filename length | ⚠️ Max 14 bytes | Enforce in create/lookup; names not null-terminated at limit |
| Symbolic links | ❌ Not supported | — |
| Max file size | ⚠️ ~70 KB | (12 + 128) × 512 bytes; enforced by `xv6fs_get_block` |
| Hard link count | ✅ Stored in `nlink` | — |
| Special files (char/block dev) | ✅ Stored as T_DEV | Use `init_special_inode()` |

### Kernel version compatibility

| Kernel range | `address_space_operations` | `i_ctime` access |
|---|---|---|
| < 5.19 | `.readpage` + `block_read_full_page()` | `inode->i_ctime = …` |
| 5.19 – 6.5 | `.read_folio` + `mpage_read_folio()` | `inode->i_ctime = …` |
| ≥ 6.6 | `.read_folio` + `mpage_read_folio()` | `inode_set_ctime(inode, 0, 0)` |

The `XV6FS_ZERO_CTIME(inode)` macro in `xv6fs.h` handles the 6.6+ change
automatically.  The skeleton targets **≥ 5.19** (Ubuntu 22.04 HWE and later).