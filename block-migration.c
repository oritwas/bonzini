/*
 * QEMU live block migration
 *
 * Copyright IBM, Corp. 2009
 *
 * Authors:
 *  Liran Schour   <lirans@il.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu-common.h"
#include "block_int.h"
#include "hw/hw.h"
#include "qemu-queue.h"
#include "qemu-timer.h"
#include "block-migration.h"
#include "migration.h"
#include "blockdev.h"
#include <assert.h>

#define BLOCK_SIZE (BDRV_SECTORS_PER_DIRTY_CHUNK << BDRV_SECTOR_BITS)

#define BLK_MIG_FLAG_DEVICE_BLOCK       0x01
#define BLK_MIG_FLAG_EOS                0x02
#define BLK_MIG_FLAG_PROGRESS           0x04

#define MAX_IS_ALLOCATED_SEARCH 65536

//#define DEBUG_BLK_MIGRATION

#ifdef DEBUG_BLK_MIGRATION
#define DPRINTF(fmt, ...) \
    do { printf("blk_migration: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

typedef struct BlkMigDevState {
    BlockDriverState *bs;
    int bulk_completed;
    int shared_base;
    int64_t cur_sector;
    int64_t cur_dirty;
    int64_t completed_sectors;
    int64_t total_sectors;
    int64_t dirty;
    QSIMPLEQ_ENTRY(BlkMigDevState) entry;
    unsigned long *aio_bitmap;
} BlkMigDevState;

typedef struct BlkMigBlock {
    uint8_t *buf;
    BlkMigDevState *bmds;
    int64_t sector;
    int nr_sectors;
    struct iovec iov;
    QEMUIOVector qiov;
    BlockDriverAIOCB *aiocb;
    int ret;
    QSIMPLEQ_ENTRY(BlkMigBlock) entry;
} BlkMigBlock;

typedef struct BlkMigState {
    int blk_enable;
    int shared_base;
    QSIMPLEQ_HEAD(bmds_list, BlkMigDevState) bmds_list;
    QSIMPLEQ_HEAD(blk_list, BlkMigBlock) blk_list;
    int submitted;
    int read_done;
    int transferred;
    int64_t total_sector_sum;
    int prev_progress;
    int bulk_completed;
    long double prev_time_offset;
} BlkMigState;

static BlkMigState block_mig_state;

static void blk_send(QEMUFile *f, BlkMigBlock * blk)
{
    int len;

    /* sector number and flags */
    qemu_put_be64(f, (blk->sector << BDRV_SECTOR_BITS)
                     | BLK_MIG_FLAG_DEVICE_BLOCK);

    /* device name */
    len = strlen(blk->bmds->bs->device_name);
    qemu_put_byte(f, len);
    qemu_put_buffer(f, (uint8_t *)blk->bmds->bs->device_name, len);

    qemu_put_buffer(f, blk->buf, BLOCK_SIZE);
}

int blk_mig_active(void)
{
    return !QSIMPLEQ_EMPTY(&block_mig_state.bmds_list);
}

uint64_t blk_mig_bytes_transferred(void)
{
    BlkMigDevState *bmds;
    uint64_t sum = 0;

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        sum += bmds->completed_sectors;
    }
    return sum << BDRV_SECTOR_BITS;
}

uint64_t blk_mig_bytes_remaining(void)
{
    return blk_mig_bytes_total() - blk_mig_bytes_transferred();
}

uint64_t blk_mig_bytes_total(void)
{
    BlkMigDevState *bmds;
    uint64_t sum = 0;

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        sum += bmds->total_sectors;
    }
    return sum << BDRV_SECTOR_BITS;
}

static int bmds_aio_inflight(BlkMigDevState *bmds, int64_t sector)
{
    int64_t chunk = sector / (int64_t)BDRV_SECTORS_PER_DIRTY_CHUNK;

    if ((sector << BDRV_SECTOR_BITS) < bdrv_getlength(bmds->bs)) {
        return !!(bmds->aio_bitmap[chunk / (sizeof(unsigned long) * 8)] &
            (1UL << (chunk % (sizeof(unsigned long) * 8))));
    } else {
        return 0;
    }
}

static void bmds_set_aio_inflight(BlkMigDevState *bmds, int64_t sector_num,
                             int nb_sectors, int set)
{
    int64_t start, end;
    unsigned long val, idx, bit;

    start = sector_num / BDRV_SECTORS_PER_DIRTY_CHUNK;
    end = (sector_num + nb_sectors - 1) / BDRV_SECTORS_PER_DIRTY_CHUNK;

    for (; start <= end; start++) {
        idx = start / (sizeof(unsigned long) * 8);
        bit = start % (sizeof(unsigned long) * 8);
        val = bmds->aio_bitmap[idx];
        if (set) {
            val |= 1UL << bit;
        } else {
            val &= ~(1UL << bit);
        }
        bmds->aio_bitmap[idx] = val;
    }
}

static void alloc_aio_bitmap(BlkMigDevState *bmds)
{
    BlockDriverState *bs = bmds->bs;
    int64_t bitmap_size;

    bitmap_size = (bdrv_getlength(bs) >> BDRV_SECTOR_BITS) +
            BDRV_SECTORS_PER_DIRTY_CHUNK * 8 - 1;
    bitmap_size /= BDRV_SECTORS_PER_DIRTY_CHUNK * 8;

    bmds->aio_bitmap = g_malloc0(bitmap_size);
}

static void blk_mig_read_cb(void *opaque, int ret)
{
    long double curr_time = qemu_get_clock_ns(rt_clock);
    BlkMigBlock *blk = opaque;

    blk->ret = ret;

    block_mig_state.prev_time_offset = curr_time;

    QSIMPLEQ_INSERT_TAIL(&block_mig_state.blk_list, blk, entry);
    bmds_set_aio_inflight(blk->bmds, blk->sector, blk->nr_sectors, 0);

    block_mig_state.submitted--;
    block_mig_state.read_done++;
    assert(block_mig_state.submitted >= 0);
}

static int mig_save_device_bulk(QEMUFile *f, BlkMigDevState *bmds)
{
    int64_t total_sectors = bmds->total_sectors;
    int64_t cur_sector = bmds->cur_sector;
    BlockDriverState *bs = bmds->bs;
    BlkMigBlock *blk;
    int nr_sectors;

    if (bmds->shared_base) {
        while (cur_sector < total_sectors &&
               !bdrv_is_allocated(bs, cur_sector, MAX_IS_ALLOCATED_SEARCH,
                                  &nr_sectors)) {
            cur_sector += nr_sectors;
        }
    }

    if (cur_sector >= total_sectors) {
        bmds->cur_sector = bmds->completed_sectors = total_sectors;
        return 1;
    }

    bmds->completed_sectors = cur_sector;

    cur_sector &= ~((int64_t)BDRV_SECTORS_PER_DIRTY_CHUNK - 1);

    /* we are going to transfer a full block even if it is not allocated */
    nr_sectors = BDRV_SECTORS_PER_DIRTY_CHUNK;

    if (total_sectors - cur_sector < BDRV_SECTORS_PER_DIRTY_CHUNK) {
        nr_sectors = total_sectors - cur_sector;
    }

    blk = g_malloc(sizeof(BlkMigBlock));
    blk->buf = g_malloc(BLOCK_SIZE);
    blk->bmds = bmds;
    blk->sector = cur_sector;
    blk->nr_sectors = nr_sectors;

    blk->iov.iov_base = blk->buf;
    blk->iov.iov_len = nr_sectors * BDRV_SECTOR_SIZE;
    qemu_iovec_init_external(&blk->qiov, &blk->iov, 1);

    if (block_mig_state.submitted == 0) {
        block_mig_state.prev_time_offset = qemu_get_clock_ns(rt_clock);
    }

    blk->aiocb = bdrv_aio_readv(bs, cur_sector, &blk->qiov,
                                nr_sectors, blk_mig_read_cb, blk);
    block_mig_state.submitted++;

    bdrv_reset_dirty(bs, cur_sector, nr_sectors);
    bmds->cur_sector = cur_sector + nr_sectors;

    return (bmds->cur_sector >= total_sectors);
}

static void set_dirty_tracking(int enable)
{
    BlkMigDevState *bmds;

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        bdrv_set_dirty_tracking(bmds->bs, enable);
    }
}

static void init_blk_migration_it(void *opaque, BlockDriverState *bs)
{
    BlkMigDevState *bmds;
    int64_t sectors;

    if (!bdrv_is_read_only(bs)) {
        sectors = bdrv_getlength(bs) >> BDRV_SECTOR_BITS;
        if (sectors <= 0) {
            return;
        }

        bmds = g_malloc0(sizeof(BlkMigDevState));
        bmds->bs = bs;
        bmds->bulk_completed = 0;
        bmds->total_sectors = sectors;
        bmds->completed_sectors = 0;
        bmds->shared_base = block_mig_state.shared_base;
        alloc_aio_bitmap(bmds);
        drive_get_ref(drive_get_by_blockdev(bs));
        bdrv_set_in_use(bs, 1);

        block_mig_state.total_sector_sum += sectors;

        if (bmds->shared_base) {
            DPRINTF("Start migration for %s with shared base image\n",
                    bs->device_name);
        } else {
            DPRINTF("Start full migration for %s\n", bs->device_name);
        }

        QSIMPLEQ_INSERT_TAIL(&block_mig_state.bmds_list, bmds, entry);
    }
}

static void init_blk_migration(QEMUFile *f)
{
    block_mig_state.submitted = 0;
    block_mig_state.read_done = 0;
    block_mig_state.transferred = 0;
    block_mig_state.total_sector_sum = 0;
    block_mig_state.prev_progress = -1;
    block_mig_state.bulk_completed = 0;

    bdrv_iterate(init_blk_migration_it, NULL);
}

static int blk_mig_save_bulked_block(QEMUFile *f)
{
    int64_t completed_sector_sum = 0;
    BlkMigDevState *bmds;
    int progress;
    int ret = 0;

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        if (bmds->bulk_completed == 0) {
            if (mig_save_device_bulk(f, bmds) == 1) {
                /* completed bulk section for this device */
                bmds->bulk_completed = 1;
            }
            completed_sector_sum += bmds->completed_sectors;
            ret = 1;
            break;
        } else {
            completed_sector_sum += bmds->completed_sectors;
        }
    }

    if (block_mig_state.total_sector_sum != 0) {
        progress = completed_sector_sum * 100 /
                   block_mig_state.total_sector_sum;
    } else {
        progress = 100;
    }
    if (progress != block_mig_state.prev_progress) {
        block_mig_state.prev_progress = progress;
        qemu_put_be64(f, (progress << BDRV_SECTOR_BITS)
                         | BLK_MIG_FLAG_PROGRESS);
        DPRINTF("Completed %d %%\r", progress);
    }

    return ret;
}

static void blk_mig_reset_dirty_cursor(void)
{
    BlkMigDevState *bmds;

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        bmds->cur_dirty = 0;
    }
}

static int mig_save_device_dirty(QEMUFile *f, BlkMigDevState *bmds,
                                 int is_async)
{
    BlkMigBlock *blk;
    int64_t total_sectors = bmds->total_sectors;
    int64_t sector;
    int nr_sectors;
    int ret = -EIO;

    for (sector = bmds->cur_dirty; sector < bmds->total_sectors;) {
        if (bmds_aio_inflight(bmds, sector)) {
            bdrv_drain_all();
        }
        if (bdrv_get_dirty(bmds->bs, sector)) {

            if (total_sectors - sector < BDRV_SECTORS_PER_DIRTY_CHUNK) {
                nr_sectors = total_sectors - sector;
            } else {
                nr_sectors = BDRV_SECTORS_PER_DIRTY_CHUNK;
            }
            blk = g_malloc(sizeof(BlkMigBlock));
            blk->buf = g_malloc(BLOCK_SIZE);
            blk->bmds = bmds;
            blk->sector = sector;
            blk->nr_sectors = nr_sectors;

            if (is_async) {
                blk->iov.iov_base = blk->buf;
                blk->iov.iov_len = nr_sectors * BDRV_SECTOR_SIZE;
                qemu_iovec_init_external(&blk->qiov, &blk->iov, 1);

                if (block_mig_state.submitted == 0) {
                    block_mig_state.prev_time_offset = qemu_get_clock_ns(rt_clock);
                }

                blk->aiocb = bdrv_aio_readv(bmds->bs, sector, &blk->qiov,
                                            nr_sectors, blk_mig_read_cb, blk);
                block_mig_state.submitted++;
                bmds_set_aio_inflight(bmds, sector, nr_sectors, 1);
            } else {
                ret = bdrv_read(bmds->bs, sector, blk->buf, nr_sectors);
                if (ret < 0) {
                    goto error;
                }
                blk_send(f, blk);

                g_free(blk->buf);
                g_free(blk);
            }

            bdrv_reset_dirty(bmds->bs, sector, nr_sectors);
            break;
        }
        sector += BDRV_SECTORS_PER_DIRTY_CHUNK;
        bmds->cur_dirty = sector;
    }

    return (bmds->cur_dirty >= bmds->total_sectors);

error:
    DPRINTF("Error reading sector %" PRId64 "\n", sector);
    g_free(blk->buf);
    g_free(blk);
    return ret;
}

/* return value:
 * 0: too much data for max_downtime
 * 1: few enough data for max_downtime
*/
static int blk_mig_save_dirty_block(QEMUFile *f, int is_async)
{
    BlkMigDevState *bmds;
    int ret = 1;

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        ret = mig_save_device_dirty(f, bmds, is_async);
        if (ret <= 0) {
            break;
        }
    }

    return ret;
}

static int flush_blks(QEMUFile *f)
{
    BlkMigBlock *blk;
    int ret = 0;

    DPRINTF("%s Enter submitted %d read_done %d transferred %d\n",
            __FUNCTION__, block_mig_state.submitted, block_mig_state.read_done,
            block_mig_state.transferred);

    while ((blk = QSIMPLEQ_FIRST(&block_mig_state.blk_list)) != NULL) {
        if (qemu_file_rate_limit(f)) {
            break;
        }
        if (blk->ret < 0) {
            ret = blk->ret;
            break;
        }
        blk_send(f, blk);

        QSIMPLEQ_REMOVE_HEAD(&block_mig_state.blk_list, entry);
        g_free(blk->buf);
        g_free(blk);

        block_mig_state.read_done--;
        block_mig_state.transferred++;
        assert(block_mig_state.read_done >= 0);
    }

    DPRINTF("%s Exit submitted %d read_done %d transferred %d\n", __FUNCTION__,
            block_mig_state.submitted, block_mig_state.read_done,
            block_mig_state.transferred);
    return ret;
}

static int64_t get_remaining_dirty(void)
{
    BlkMigDevState *bmds;
    int64_t dirty = 0;

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        dirty += bdrv_get_dirty_count(bmds->bs);
    }

    return dirty * BLOCK_SIZE;
}

static void blk_mig_cleanup(void)
{
    BlkMigDevState *bmds;
    BlkMigBlock *blk;

    bdrv_drain_all();

    set_dirty_tracking(0);

    while ((bmds = QSIMPLEQ_FIRST(&block_mig_state.bmds_list)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&block_mig_state.bmds_list, entry);
        bdrv_set_in_use(bmds->bs, 0);
        drive_put_ref(drive_get_by_blockdev(bmds->bs));
        g_free(bmds->aio_bitmap);
        g_free(bmds);
    }

    while ((blk = QSIMPLEQ_FIRST(&block_mig_state.blk_list)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&block_mig_state.blk_list, entry);
        g_free(blk->buf);
        g_free(blk);
    }
}

static void block_migration_cancel(void *opaque)
{
    blk_mig_cleanup();
}

static int block_save_setup(QEMUFile *f, void *opaque)
{
    int ret;

    DPRINTF("Enter save live setup submitted %d transferred %d\n",
            block_mig_state.submitted, block_mig_state.transferred);

    init_blk_migration(f);

    /* start track dirty blocks */
    set_dirty_tracking(1);

    ret = flush_blks(f);
    if (ret) {
        blk_mig_cleanup();
        return ret;
    }

    blk_mig_reset_dirty_cursor();

    qemu_put_be64(f, BLK_MIG_FLAG_EOS);

    return 0;
}

static int block_save_iterate_locked(QEMUFile *f, void *opaque)
{
    int ret;

    DPRINTF("Enter save live iterate submitted %d transferred %d\n",
            block_mig_state.submitted, block_mig_state.transferred);

    ret = flush_blks(f);
    if (ret) {
        blk_mig_cleanup();
        return ret;
    }

    blk_mig_reset_dirty_cursor();

    /* control the rate of transfer */
    while ((block_mig_state.submitted +
            block_mig_state.read_done) * BLOCK_SIZE <
           qemu_file_get_rate_limit(f)) {
        if (block_mig_state.bulk_completed == 0) {
            /* first finish the bulk phase */
            if (blk_mig_save_bulked_block(f) == 0) {
                /* finished saving bulk on all devices */
                block_mig_state.bulk_completed = 1;
            }
        } else {
            ret = blk_mig_save_dirty_block(f, 1);
            if (ret != 0) {
                /* no more dirty blocks */
                break;
            }
        }
    }
    if (ret) {
        blk_mig_cleanup();
        return ret;
    }

    ret = flush_blks(f);
    if (ret) {
        blk_mig_cleanup();
        return ret;
    }

    qemu_put_be64(f, BLK_MIG_FLAG_EOS);

    return 0;
}

static int block_save_iterate(QEMUFile *f, void *opaque)
{
    int ret;
    qemu_mutex_lock_iothread();
    ret = block_save_iterate_locked(f, opaque);
    qemu_mutex_unlock_iothread();
    return ret;
}

static int block_save_complete(QEMUFile *f, void *opaque)
{
    int ret;

    DPRINTF("Enter save live complete submitted %d transferred %d\n",
            block_mig_state.submitted, block_mig_state.transferred);

    ret = flush_blks(f);
    if (ret) {
        blk_mig_cleanup();
        return ret;
    }

    blk_mig_reset_dirty_cursor();

    /* we know for sure that save bulk is completed and
       all async read completed */
    assert(block_mig_state.submitted == 0);

    do {
        ret = blk_mig_save_dirty_block(f, 0);
    } while (ret == 0);

    blk_mig_cleanup();
    if (ret) {
        return ret;
    }
    /* report completion */
    qemu_put_be64(f, (100 << BDRV_SECTOR_BITS) | BLK_MIG_FLAG_PROGRESS);

    DPRINTF("Block migration completed\n");

    qemu_put_be64(f, BLK_MIG_FLAG_EOS);

    return 0;
}

static uint64_t block_save_pending(QEMUFile *f, void *opaque, uint64_t max_size)
{

    DPRINTF("Enter save live pending  %ld\n", get_remaining_dirty());

    return get_remaining_dirty();
}

static int block_load(QEMUFile *f, void *opaque, int version_id)
{
    static int banner_printed;
    int len, flags;
    char device_name[256];
    int64_t addr;
    BlockDriverState *bs, *bs_prev = NULL;
    uint8_t *buf;
    int64_t total_sectors = 0;
    int nr_sectors;
    int ret;

    do {
        addr = qemu_get_be64(f);

        flags = addr & ~BDRV_SECTOR_MASK;
        addr >>= BDRV_SECTOR_BITS;

        if (flags & BLK_MIG_FLAG_DEVICE_BLOCK) {
            /* get device name */
            len = qemu_get_byte(f);
            qemu_get_buffer(f, (uint8_t *)device_name, len);
            device_name[len] = '\0';

            bs = bdrv_find(device_name);
            if (!bs) {
                fprintf(stderr, "Error unknown block device %s\n",
                        device_name);
                return -EINVAL;
            }

            if (bs != bs_prev) {
                bs_prev = bs;
                total_sectors = bdrv_getlength(bs) >> BDRV_SECTOR_BITS;
                if (total_sectors <= 0) {
                    error_report("Error getting length of block device %s",
                                 device_name);
                    return -EINVAL;
                }
            }

            if (total_sectors - addr < BDRV_SECTORS_PER_DIRTY_CHUNK) {
                nr_sectors = total_sectors - addr;
            } else {
                nr_sectors = BDRV_SECTORS_PER_DIRTY_CHUNK;
            }

            buf = g_malloc(BLOCK_SIZE);

            qemu_get_buffer(f, buf, BLOCK_SIZE);
            ret = bdrv_write(bs, addr, buf, nr_sectors);

            g_free(buf);
            if (ret < 0) {
                return ret;
            }
        } else if (flags & BLK_MIG_FLAG_PROGRESS) {
            if (!banner_printed) {
                printf("Receiving block device images\n");
                banner_printed = 1;
            }
            printf("Completed %d %%%c", (int)addr,
                   (addr == 100) ? '\n' : '\r');
            fflush(stdout);
        } else if (!(flags & BLK_MIG_FLAG_EOS)) {
            fprintf(stderr, "Unknown flags\n");
            return -EINVAL;
        }
        ret = qemu_file_get_error(f);
        if (ret != 0) {
            return ret;
        }
    } while (!(flags & BLK_MIG_FLAG_EOS));

    return 0;
}

static void block_set_params(const MigrationParams *params, void *opaque)
{
    block_mig_state.blk_enable = params->blk;
    block_mig_state.shared_base = params->shared;

    /* shared base means that blk_enable = 1 */
    block_mig_state.blk_enable |= params->shared;
}

static bool block_is_active(void *opaque)
{
    return block_mig_state.blk_enable == 1;
}

SaveVMHandlers savevm_block_handlers = {
    .set_params = block_set_params,
    .save_live_setup = block_save_setup,
    .save_live_iterate = block_save_iterate,
    .save_live_complete = block_save_complete,
    .save_live_pending = block_save_pending,
    .load_state = block_load,
    .cancel = block_migration_cancel,
    .is_active = block_is_active,
};

void blk_mig_init(void)
{
    QSIMPLEQ_INIT(&block_mig_state.bmds_list);
    QSIMPLEQ_INIT(&block_mig_state.blk_list);

    register_savevm_live(NULL, "block", 0, 1, &savevm_block_handlers,
                         &block_mig_state);
}
