// SPDX-License-Identifier: GPL-2.0
/*
 * vdisk - dynamically created RAM-backed block devices, managed through a
 * single control file.
 *
 * Control interface: /proc/vdisk
 *   write "create <name> <size_bytes>"  -- creates /dev/<name>
 *   write "remove <name>"               -- tears it down
 *   read                                 -- lists active disks
 *
 * Backing store is a sparse per-page xarray, allocated on first write to
 * each page -- an unused disk costs nothing beyond its bookkeeping struct.
 * This mirrors the in-tree brd.c ramdisk driver's page-store scheme
 * (bio handling in particular is adapted from it almost directly); what
 * this module adds on top is dynamic, named disks controlled from
 * userspace at runtime instead of a fixed count sized at module load.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/xarray.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/highmem.h>
#include <linux/ctype.h>
#include <linux/slab.h>

#define VDISK_NAME_MAX  24
#define VDISK_PROC_NAME "vdisk"

struct vdisk_device {
    struct list_head list;
    char name[VDISK_NAME_MAX + 1];
    struct gendisk *disk;
    struct xarray pages;
    u64 nr_pages;   /* pages actually allocated so far, for the listing */
    u64 size_bytes;
};

static LIST_HEAD(vdisk_list);
static DEFINE_MUTEX(vdisk_lock);
static int vdisk_major;
static int vdisk_next_minor;

/* ---- backing store: sparse per-page xarray (same scheme as brd.c) ---- */

static struct page *vdisk_lookup_page(struct vdisk_device *v, sector_t sector)
{
    struct page *page;
    XA_STATE(xas, &v->pages, sector >> PAGE_SECTORS_SHIFT);

    rcu_read_lock();
repeat:
    page = xas_load(&xas);
    if (xas_retry(&xas, page)) {
        xas_reset(&xas);
        goto repeat;
    }
    if (!page)
        goto out;
    if (!get_page_unless_zero(page)) {
        xas_reset(&xas);
        goto repeat;
    }
    if (unlikely(page != xas_reload(&xas))) {
        put_page(page);
        xas_reset(&xas);
        goto repeat;
    }
out:
    rcu_read_unlock();
    return page;
}

static struct page *vdisk_insert_page(struct vdisk_device *v, sector_t sector,
                                       blk_opf_t opf)
{
    gfp_t gfp = (opf & REQ_NOWAIT) ? GFP_NOWAIT : GFP_NOIO;
    struct page *page, *ret;

    page = alloc_page(gfp | __GFP_ZERO | __GFP_HIGHMEM);
    if (!page)
        return ERR_PTR(-ENOMEM);

    xa_lock(&v->pages);
    ret = __xa_cmpxchg(&v->pages, sector >> PAGE_SECTORS_SHIFT, NULL, page, gfp);
    if (!ret) {
        v->nr_pages++;
        get_page(page);
        xa_unlock(&v->pages);
        return page;
    }
    if (!xa_is_err(ret)) {
        get_page(ret);
        xa_unlock(&v->pages);
        put_page(page);
        return ret;
    }
    xa_unlock(&v->pages);
    put_page(page);
    return ERR_PTR(xa_err(ret));
}

static void vdisk_free_pages(struct vdisk_device *v)
{
    struct page *page;
    pgoff_t idx;

    xa_for_each(&v->pages, idx, page) {
        put_page(page);
        cond_resched();
    }
    xa_destroy(&v->pages);
}

/* Processes a single bio_vec segment, capped to one page in both the bio
 * and the backing store, exactly as brd_rw_bvec() does. */
static bool vdisk_rw_bvec(struct vdisk_device *v, struct bio *bio)
{
    struct bio_vec bv = bio_iter_iovec(bio, bio->bi_iter);
    sector_t sector = bio->bi_iter.bi_sector;
    u32 offset = (sector & (PAGE_SECTORS - 1)) << SECTOR_SHIFT;
    blk_opf_t opf = bio->bi_opf;
    struct page *page;
    void *kaddr;

    bv.bv_len = min_t(u32, bv.bv_len, PAGE_SIZE - offset);

    page = vdisk_lookup_page(v, sector);
    if (!page && op_is_write(opf)) {
        page = vdisk_insert_page(v, sector, opf);
        if (IS_ERR(page))
            goto out_error;
    }

    kaddr = bvec_kmap_local(&bv);
    if (op_is_write(opf)) {
        memcpy_to_page(page, offset, kaddr, bv.bv_len);
    } else {
        if (page)
            memcpy_from_page(kaddr, page, offset, bv.bv_len);
        else
            memset(kaddr, 0, bv.bv_len); /* unwritten page reads as zero */
    }
    kunmap_local(kaddr);

    bio_advance_iter_single(bio, &bio->bi_iter, bv.bv_len);
    if (page)
        put_page(page);
    return true;

out_error:
    if (PTR_ERR(page) == -ENOMEM && (opf & REQ_NOWAIT))
        bio_wouldblock_error(bio);
    else
        bio_io_error(bio);
    return false;
}

static void vdisk_submit_bio(struct bio *bio)
{
    struct vdisk_device *v = bio->bi_bdev->bd_disk->private_data;

    do {
        if (!vdisk_rw_bvec(v, bio))
            return;
    } while (bio->bi_iter.bi_size);

    bio_endio(bio);
}

static const struct block_device_operations vdisk_fops = {
    .owner      = THIS_MODULE,
    .submit_bio = vdisk_submit_bio,
};

/* ---- create / remove, keyed by name ---- */

static bool vdisk_valid_name(const char *name)
{
    size_t len = strlen(name);

    if (len == 0 || len > VDISK_NAME_MAX)
        return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!isalnum((unsigned char)c) && c != '_' && c != '-')
            return false;
    }
    return true;
}

static struct vdisk_device *vdisk_find_locked(const char *name)
{
    struct vdisk_device *v;

    list_for_each_entry(v, &vdisk_list, list)
        if (strcmp(v->name, name) == 0)
            return v;
    return NULL;
}

static int vdisk_create(const char *name, u64 size_bytes)
{
    struct vdisk_device *v;
    struct gendisk *disk;
    int err;
    struct queue_limits lim = {
        .physical_block_size = PAGE_SIZE,
        .features = BLK_FEAT_SYNCHRONOUS | BLK_FEAT_NOWAIT,
    };

    if (!vdisk_valid_name(name))
        return -EINVAL;
    /* must be sector-aligned; cap at 1 TiB as a sanity limit, not a hard
     * architectural one -- this is memory-backed, real RAM is the actual
     * ceiling long before that. */
    if (size_bytes == 0 || (size_bytes & (SECTOR_SIZE - 1)) || size_bytes > (1ULL << 40))
        return -EINVAL;

    v = kzalloc(sizeof(*v), GFP_KERNEL);
    if (!v)
        return -ENOMEM;
    strscpy(v->name, name, sizeof(v->name));
    v->size_bytes = size_bytes;
    xa_init(&v->pages);

    disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
    if (IS_ERR(disk)) {
        err = PTR_ERR(disk);
        kfree(v);
        return err;
    }

    mutex_lock(&vdisk_lock);
    if (vdisk_find_locked(name)) {
        mutex_unlock(&vdisk_lock);
        put_disk(disk);
        kfree(v);
        return -EEXIST;
    }

    disk->major = vdisk_major;
    disk->first_minor = vdisk_next_minor++;
    disk->minors = 1; /* no partition support in v1 */
    disk->fops = &vdisk_fops;
    disk->private_data = v;
    strscpy(disk->disk_name, name, DISK_NAME_LEN);
    set_capacity(disk, size_bytes >> SECTOR_SHIFT);
    v->disk = disk;

    err = add_disk(disk);
    if (err) {
        mutex_unlock(&vdisk_lock);
        put_disk(disk);
        kfree(v);
        return err;
    }

    list_add_tail(&v->list, &vdisk_list);
    mutex_unlock(&vdisk_lock);
    return 0;
}

static int vdisk_remove(const char *name)
{
    struct vdisk_device *v;

    mutex_lock(&vdisk_lock);
    v = vdisk_find_locked(name);
    if (!v) {
        mutex_unlock(&vdisk_lock);
        return -ENOENT;
    }
    /* Refuse to pull a disk out from under a live mount/open fd instead of
     * silently yanking it -- same spirit as the Windows tool refusing to
     * drop a disk still backing an attached physical disk. */
    if (disk_openers(v->disk) > 0) {
        mutex_unlock(&vdisk_lock);
        return -EBUSY;
    }
    list_del(&v->list);
    mutex_unlock(&vdisk_lock);

    del_gendisk(v->disk);
    put_disk(v->disk);
    vdisk_free_pages(v);
    kfree(v);
    return 0;
}

/* ---- /proc/vdisk: write commands, read for status ---- */

static int vdisk_proc_show(struct seq_file *m, void *v_unused)
{
    struct vdisk_device *v;

    mutex_lock(&vdisk_lock);
    seq_printf(m, "%-24s %14s %14s\n", "NAME", "SIZE", "RAM_USED");
    list_for_each_entry(v, &vdisk_list, list) {
        seq_printf(m, "%-24s %14llu %14llu\n",
                   v->name, v->size_bytes, v->nr_pages * PAGE_SIZE);
    }
    mutex_unlock(&vdisk_lock);
    seq_puts(m, "commands: create <name> <size_bytes> | remove <name>\n");
    return 0;
}

static int vdisk_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, vdisk_proc_show, NULL);
}

static ssize_t vdisk_proc_write(struct file *file, const char __user *ubuf,
                                 size_t len, loff_t *off)
{
    char cmd[64];
    char name[VDISK_NAME_MAX + 1];
    size_t n = min(len, sizeof(cmd) - 1);
    int err;

    if (copy_from_user(cmd, ubuf, n))
        return -EFAULT;
    cmd[n] = '\0';
    if (n > 0 && cmd[n - 1] == '\n')
        cmd[n - 1] = '\0';

    if (!strncmp(cmd, "create ", 7)) {
        unsigned long long size_bytes;
        if (sscanf(cmd + 7, "%24s %llu", name, &size_bytes) != 2)
            return -EINVAL;
        err = vdisk_create(name, size_bytes);
    } else if (!strncmp(cmd, "remove ", 7)) {
        if (sscanf(cmd + 7, "%24s", name) != 1)
            return -EINVAL;
        err = vdisk_remove(name);
    } else {
        return -EINVAL;
    }

    return err ? err : (ssize_t)len;
}

static const struct proc_ops vdisk_proc_ops = {
    .proc_open    = vdisk_proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
    .proc_write   = vdisk_proc_write,
};

/* ---- module init/exit ---- */

static struct proc_dir_entry *vdisk_proc_entry;

static int __init vdisk_init(void)
{
    int ret;

    ret = register_blkdev(0, "vdisk");
    if (ret < 0) {
        pr_err("vdisk: register_blkdev failed: %d\n", ret);
        return ret;
    }
    vdisk_major = ret;

    vdisk_proc_entry = proc_create(VDISK_PROC_NAME, 0644, NULL, &vdisk_proc_ops);
    if (!vdisk_proc_entry) {
        unregister_blkdev(vdisk_major, "vdisk");
        pr_err("vdisk: failed to create /proc/%s\n", VDISK_PROC_NAME);
        return -ENOMEM;
    }

    pr_info("vdisk: loaded (major %d, control=/proc/%s)\n", vdisk_major, VDISK_PROC_NAME);
    return 0;
}

static void __exit vdisk_exit(void)
{
    struct vdisk_device *v, *next;

    remove_proc_entry(VDISK_PROC_NAME, NULL);

    mutex_lock(&vdisk_lock);
    list_for_each_entry_safe(v, next, &vdisk_list, list) {
        list_del(&v->list);
        del_gendisk(v->disk);
        put_disk(v->disk);
        vdisk_free_pages(v);
        kfree(v);
    }
    mutex_unlock(&vdisk_lock);

    unregister_blkdev(vdisk_major, "vdisk");
    pr_info("vdisk: unloaded\n");
}

module_init(vdisk_init);
module_exit(vdisk_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("vdisk project");
MODULE_DESCRIPTION("Dynamically created RAM-backed block devices");
