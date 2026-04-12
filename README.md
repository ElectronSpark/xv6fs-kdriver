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
5. [Development VM](#development-vm)
6. [Creating a Test Image](#creating-a-test-image)
7. [Roadmap](#roadmap)
8. [XV6 FS Limitations](#xv6-fs-limitations)

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
├── Makefile       — build rules (targets: all, clean, load, unload,
│                    install-mkfs, uninstall-mkfs)
├── mkfs.xv6fs.c   — userspace mkfs tool (creates xv6fs images)
├── xv6fs.h        — on-disk structs, in-memory types, constants,
│                    cross-file declarations
├── xv6fs_main.c   — module init/exit; filesystem type registration
├── super.c        — superblock operations  (stage 1)
├── inode.c        — inode cache, block mapping  (stages 2 & 4)
├── dir.c         — directory readdir & lookup  (stage 3)
├── file.c        — file read/write operations  (stages 4 & 5)
└── vm/            — QEMU development VM scripts
    ├── setup.sh   — create a Debian trixie VM with kdump + 9p share
    └── run.sh     — launch the VM
```

---

## Prerequisites

Install the build tools:

```bash
sudo apt-get update
sudo apt-get install build-essential dwarves libelf-dev libssl-dev bc flex bison
```

For the [development VM](#development-vm) (recommended on WSL2):

```bash
sudo apt-get install debootstrap qemu-system-x86 qemu-utils
```

On a **native Linux** system (VM or dual-boot), also install the kernel
headers:

```bash
sudo apt-get install linux-headers-$(uname -r)
```

> **WSL2 note:** WSL2 uses a Microsoft-supplied kernel.  The standard
> `linux-headers-$(uname -r)` package does not exist.  You must build the
> WSL2 kernel from source:
>
> ```bash
> # 1. Clone the Microsoft WSL2 kernel
> git clone --depth=1 https://github.com/microsoft/WSL2-Linux-Kernel.git
> cd WSL2-Linux-Kernel
>
> # 2. Check out the tag that matches your running kernel
> #    (find it with: uname -r)
> git fetch --depth=1 origin tag linux-msft-wsl-$(uname -r | sed 's/-microsoft-standard-WSL2//')
> git checkout linux-msft-wsl-$(uname -r | sed 's/-microsoft-standard-WSL2//')
>
> # 3. Configure and do a full build (needed for Module.symvers)
> cp Microsoft/config-wsl .config
> make olddefconfig
> make -j$(nproc)
>
> # 4. Create a symlink so the default KDIR works without extra flags
> sudo mkdir -p /lib/modules/$(uname -r)
> sudo ln -s $(pwd) /lib/modules/$(uname -r)/build
> ```
>
> With the symlink in place, `make`, `make clean`, etc. work without
> passing `KDIR=...`.  Alternatively, pass it explicitly each time:
> `make KDIR=/path/to/WSL2-Linux-Kernel`.
>
> **Note:** WSL2 resets `/lib/modules` on reboot, so the symlink must be
> recreated after each `wsl --shutdown`.  To automate this, add to
> `/etc/wsl.conf`:
> ```ini
> [boot]
> command = mkdir -p /lib/modules/$(uname -r) && ln -sf /home/es/reps/WSL2-Linux-Kernel /lib/modules/$(uname -r)/build
> ```
>
> **Important:** A full kernel build (not just `make modules_prepare`) is
> required so that `Module.symvers` is populated with all exported symbols.
> The kernel source version must exactly match `uname -r`, otherwise
> `insmod` will reject the module with "Invalid module format".

The module targets kernel **5.19 or later** (uses the folio-based page-cache
API).  It has been tested on Ubuntu 22.04 HWE (5.19+), Ubuntu 24.04 (6.8),
and WSL2 (6.6).  See [XV6 FS Limitations](#xv6-fs-limitations) for API
compatibility notes.

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

## Development VM

A kernel panic during module development crashes the entire WSL2 instance.
To avoid this, use the included QEMU/KVM development VM — panics stay
contained, and crash logs survive reboots.

### Prerequisites

```bash
sudo apt install debootstrap qemu-system-x86 qemu-utils
```

For KVM acceleration on WSL2, enable nested virtualisation in
`%USERPROFILE%\.wslconfig`:

```ini
[wsl2]
nestedVirtualization=true
```

Then restart WSL (`wsl --shutdown`).  Without KVM the VM still works but
uses software emulation.

### Create the VM

```bash
./vm/setup.sh          # creates vm/disk.img, vm/vmlinuz, vm/initrd.img
```

This bootstraps a Debian trixie (kernel 6.12) rootfs with:
- `linux-headers-amd64` and `build-essential` for in-guest builds
- **9p share**: the repo is mounted at `/mnt/src` inside the guest
- **kdump**: crash dumps saved to `/var/crash/`
- **Persistent journald**: info-level logs in `/var/log/journal/`
- Serial console with root autologin

### Launch

```bash
./vm/run.sh            # boots with serial console; exit: Ctrl-A, X

# Override defaults:
QEMU_MEM=4G QEMU_SMP=4 ./vm/run.sh
```

### Inside the VM

| Alias | Action |
|---|---|
| `reload` | `make clean && make && insmod xv6fs.ko` |
| `testmount` | mount `test.img` at `/mnt/test` |
| `testumount` | unmount `/mnt/test` |
| `klog` | recent kernel log |
| `prevboot` | previous boot's full journal (useful after a crash) |
| `lastcrash` | list crash dumps in `/var/crash/` |

---

## Creating a Test Image

You need an XV6-formatted disk image to test the driver.  You can use the
included `mkfs.xv6fs` tool:

```bash
# Build everything (kernel module + mkfs tool)
make

# Create a test image with sample files
echo "Hello from xv6fs" > testfile
./mkfs.xv6fs -s 1000 test.img testfile
```

To make `mkfs -t xv6fs` work system-wide:

```bash
sudo make install-mkfs    # installs to /sbin/mkfs.xv6fs
sudo make uninstall-mkfs  # removes it
```

**Alternative:** You can also use the `mkfs` utility from the
[xv6 source](https://github.com/mit-pdos/xv6-public):

```bash
# 1. Clone xv6 and build its mkfs tool
git clone --depth=1 https://github.com/mit-pdos/xv6-public.git
cd xv6-public
gcc -Wall -o mkfs mkfs.c    # only build mkfs (full xv6 build may fail on modern GCC)
cd ..
```

> **⚠️ Magic number:** Some versions of xv6-public do not include a `magic`
> field in the superblock.  The driver expects `0x10203040` as the first
> 4 bytes of block 1.  If your `fs.h` struct starts with `size` instead of
> `magic`, you must patch it:
>
> ```c
> // In fs.h — add before the size field:
> #define FSMAGIC 0x10203040
> struct superblock {
>   uint magic;        // Must be FSMAGIC    ← ADD THIS
>   uint size;
>   // ... rest unchanged
> };
>
> // In mkfs.c — add before sb.size = xint(FSSIZE):
> sb.magic = xint(FSMAGIC);
> ```

```bash
# 2. Create a test image with sample files
cd xv6-public
echo "Hello from xv6fs" > testfile
./mkfs /tmp/xv6test.img testfile
cd ..
```

Then mount it (once the driver is functional enough to mount):

```bash
# Attach the image to a loop device
sudo losetup -fP /tmp/xv6test.img
LOOP=$(losetup -j /tmp/xv6test.img | cut -d: -f1)

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
   **⚠️ Returns the block size on success or 0 on failure** (not a negative
   errno).  Check with `if (!sb_set_blocksize(...))`.
2. `sb_bread(sb, 1)` — read block 1 (the superblock).  Returns `NULL` on
   I/O error.
3. Check `le32_to_cpu(raw->magic) == XV6FS_MAGIC`; return `-EINVAL` if not.
4. `kzalloc(sizeof(struct xv6fs_sb_info), GFP_KERNEL)` — allocate `sbi`;
   copy the raw superblock data into `sbi->raw_sb`.  The `raw_sb` fields
   remain `__le32` — use `le32_to_cpu()` each time you **read** a field.
   Store `bh` in `sbi->bh`; assign `sb->s_fs_info = sbi`.
5. Set `sb->s_magic`, `sb->s_op`, `sb->s_maxbytes`.
6. `xv6fs_iget(sb, XV6FS_ROOTINO)` — get root inode (implemented in
   stage 2).  **⚠️ Returns `ERR_PTR()` on failure** — check with
   `IS_ERR(root_inode)` and return `PTR_ERR(root_inode)`.
7. `d_make_root(root_inode)` — create the root dentry; assign to
   `sb->s_root`.  **⚠️ Returns `NULL` on failure** (and iput's the inode
   for you) — return `-ENOMEM`.

**Cleanup on failure:** Any error after allocating `sbi` or reading `bh`
must free those resources and clear `sb->s_fs_info`.  Use `goto` labels
for cleanup (e.g. `goto out_free_sbi`).  Otherwise you leak memory on
every failed mount.

**Key APIs** (`#include <linux/buffer_head.h>`, `<linux/slab.h>`):

```c
sb_set_blocksize(sb, size);           // returns size on success, 0 on failure
struct buffer_head *bh = sb_bread(sb, block_nr); // read one block; NULL on error
brelse(bh);                           // release when done
void *sbi = kzalloc(size, GFP_KERNEL);
le32_to_cpu(val);                     // little-endian to host
d_make_root(inode);                   // create root dentry; NULL on failure
IS_ERR(ptr);                          // check ERR_PTR return values
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
| *(derived)* | `inode->i_blocks` | set to `(i_size + 511) / 512` |
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


// ctx->pos is the byte offset; advance it after each emitted entry:
ctx->pos += sizeof(struct xv6fs_dirent);

// In lookup: return the child dentry (or a negative dentry)
return d_splice_alias(inode, dentry); // inode==NULL → not found
```

**Important:** Do **not** use `dir_emit_dots()` — XV6 directories store
`.` and `..` as regular entries, so emitting them from the on-disk data
is correct.  Using `dir_emit_dots()` would emit them twice.

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

// Read the indirect block to resolve indirect addresses.
// ei->addrs[] is already host-endian (converted in xv6fs_iget),
// but the indirect block contents are raw on-disk data — use le32_to_cpu.
struct buffer_head *ibh = sb_bread(sb, ei->addrs[XV6FS_NDIRECT]);
__u32 disk_block = le32_to_cpu(((__le32 *)ibh->b_data)[idx]);
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