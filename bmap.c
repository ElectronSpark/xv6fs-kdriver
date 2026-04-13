// block bitmap

#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/bitmap.h>
#include "xv6fs.h"

long xv6fs_b_count_free(struct super_block *sb) {
    struct xv6fs_sb_info *sbi = xv6fs_sb(sb);
    __u64 total = sbi->raw_sb.size;
    int used = 0;

    for (__u64 b = 0; b < total; b += XV6FS_BPB) {
        struct buffer_head *bh = sb_bread(sb, XV6FS_BBLOCK(b, sbi));
        if (!bh) {
            pr_err("xv6fs: failed to read bitmap block for bit %u\n", b);
            return -EIO;
        }

        __u64 bits = min_t(__u64, XV6FS_BPB, total - b);
        used += bitmap_weight((const unsigned long *)bh->b_data, bits);
        brelse(bh);
    }

    return total - used;
}

long xv6fs_balloc(struct super_block *sb) {
    struct xv6fs_sb_info *sbi = xv6fs_sb(sb);
    __u64 total = sbi->raw_sb.size;

    for (__u64 b = 0; b < total; b += XV6FS_BPB) {
        struct buffer_head *bh = sb_bread(sb, XV6FS_BBLOCK(b, sbi));
        if (!bh) {
            pr_err("xv6fs: failed to read bitmap block for bit %u\n", b);
            return -EIO;
        }

        __u64 bits = min_t(__u64, XV6FS_BPB, total - b);
        unsigned long *bitmap = (unsigned long *)bh->b_data;
        __u64 bit = find_first_zero_bit(bitmap, bits);

        if (bit < bits) {
            set_bit(bit, bitmap);
            mark_buffer_dirty(bh);
            brelse(bh);
            return b + bit;
        }

        brelse(bh);
    }

    return -ENOSPC;
}

void xv6fs_bfree(struct super_block *sb, sector_t b) {
    struct xv6fs_sb_info *sbi = xv6fs_sb(sb);
    __u64 total = sbi->raw_sb.size;
    struct buffer_head *bh = sb_bread(sb, XV6FS_BBLOCK(b, sbi));
    if (!bh) {
        pr_err("xv6fs: failed to read bitmap block for bit %lu\n", (unsigned long)b);
        return;
    }
    clear_bit(XV6FS_IOFFSET(b), (unsigned long *)bh->b_data);
    mark_buffer_dirty(bh);
    brelse(bh);
}

