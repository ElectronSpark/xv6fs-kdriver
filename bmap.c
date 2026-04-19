// block bitmap

#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/bitmap.h>
#include "xv6fs.h"

long xv6fs_b_count_free(struct super_block *sb) {
    struct xv6fs_sb_info *sbi = xv6fs_sb(sb);
    __u64 total = sbi->raw_sb.size;
    __u64 dstart = XV6FS_DSTART(sbi);
    __u64 bstart = round_down(dstart, XV6FS_BPB);
    __u64 non_data_blocks = 0;
    int used = 0;
    struct buffer_head *bh = NULL;

    if (dstart % XV6FS_BPB != 0) {
        bh = sb_bread(sb, XV6FS_BBLOCK(bstart, sbi));
        if (!bh) {
            pr_err("xv6fs: failed to read bitmap block for bit %llu\n", bstart);
            return -EIO;
        }
        non_data_blocks = bitmap_weight((const unsigned long *)bh->b_data, dstart - bstart);
    }

    for (__u64 b = bstart; b < total; b += XV6FS_BPB) {
        if (!bh) {
            bh = sb_bread(sb, XV6FS_BBLOCK(b, sbi));
        }
        if (!bh) {
            pr_err("xv6fs: failed to read bitmap block for bit %llu\n", b);
            return -EIO;
        }

        __u64 bits = min_t(__u64, XV6FS_BPB, total - b);
        used += bitmap_weight((const unsigned long *)bh->b_data, bits);
        brelse(bh);
        bh = NULL;
    }

    return total - used - dstart + non_data_blocks;
}

long xv6fs_balloc(struct super_block *sb) {
    struct xv6fs_sb_info *sbi = xv6fs_sb(sb);
    __u64 total = sbi->raw_sb.size;

    for (__u64 b = 0; b < total; b += XV6FS_BPB) {
        struct buffer_head *bh = sb_bread(sb, XV6FS_BBLOCK(b, sbi));
        if (!bh) {
            pr_err("xv6fs: failed to read bitmap block for bit %llu\n", b);
            return -EIO;
        }

        lock_buffer(bh);
        __u64 bits = min_t(__u64, XV6FS_BPB, total - b);
        __u64 offset = b < XV6FS_BPB ? XV6FS_DSTART(sbi) : 0;
        unsigned long *bitmap = (unsigned long *)bh->b_data;
        __u64 bit = find_next_zero_bit_le(bitmap, bits, offset);

        if (bit < bits) {
            set_bit_le(bit, bitmap);
            mark_buffer_dirty(bh);
            unlock_buffer(bh);
            brelse(bh);
            return b + bit;
        }

        unlock_buffer(bh);
        brelse(bh);
    }

    return -ENOSPC;
}

void xv6fs_bfree(struct super_block *sb, sector_t b) {
    struct xv6fs_sb_info *sbi = xv6fs_sb(sb);
    __u64 total = sbi->raw_sb.size;

    if (b >= total) {
        pr_err("xv6fs: block number %llu out of range (total blocks: %llu)\n", (unsigned long long)b, total);
        return;
    }

    if (b < XV6FS_DSTART(sbi)) {
        pr_err("xv6fs: block number %llu is reserved for metadata and cannot be freed\n", (unsigned long long)b);
        return;
    }

    struct buffer_head *bh = sb_bread(sb, XV6FS_BBLOCK(b, sbi));
    if (!bh) {
        pr_err("xv6fs: failed to read bitmap block for bit %llu\n", (unsigned long long)b);
        return;
    }
    lock_buffer(bh);
    clear_bit_le(XV6FS_BOFFSET(b), (unsigned long *)bh->b_data);
    mark_buffer_dirty(bh);
    unlock_buffer(bh);
    brelse(bh);
}

