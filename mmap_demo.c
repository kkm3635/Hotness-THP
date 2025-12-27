// mmap_demo.c
#include <linux/module.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/export.h>
#include <linux/vmalloc.h>  
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/minmax.h>

#include <linux/mmap_demo.h>
#define DEV_NAME "mmap_demo"

#define BIN_SHIFT   21                     /* 2MiB */
#define NODE_SIZE   (30ULL << 30)         
#define K_TOP   (NODE_SIZE >> BIN_SHIFT) 

static struct dentry *demo_dbgdir;

static u64   *demo_buf;
static size_t demo_len_bytes;

const u64 *mmap_demo_get_buf(size_t *len_bytes)
{
    if (len_bytes) *len_bytes = demo_len_bytes;
    return demo_buf;                     
}
EXPORT_SYMBOL_GPL(mmap_demo_get_buf);

int mmap_demo_arr_len(void) { return K_TOP; }
EXPORT_SYMBOL_GPL(mmap_demo_arr_len);
/* ---------------------------------------------- */

#if IS_ENABLED(CONFIG_DEBUG_FS)
static int mmap_demo_topk_show(struct seq_file *m, void *v)
{
    size_t len_bytes;
    const u64 *a = mmap_demo_get_buf(&len_bytes);
    if (!a || len_bytes < 2 * sizeof(u64))
        return 0;

    u64 s1, s2;
    size_t n64, nkeys, show;

retry:
    s1 = READ_ONCE(a[0]);
    if (unlikely(s1 & 1)) {
        cpu_relax();
        goto retry;
    }

    n64   = len_bytes / sizeof(u64);
    nkeys = (n64 > 0) ? (n64 - 1) : 0;

    show  = min_t(size_t, nkeys, (size_t)K_TOP);

    seq_printf(m, "seq=%llu nkeys=%zu show=%zu (K_TOP=%zu)\n",
               (unsigned long long)s1, nkeys, show, (size_t)K_TOP);

    for (size_t i = 0; i < show; i++) {
        u64 vpn = READ_ONCE(a[1 + i]);
        u64 va  = vpn << BIN_SHIFT; 
        seq_printf(m, "%6zu: vpn=0x%016llx va=0x%016llx\n",
                   i,
                   (unsigned long long)vpn,
                   (unsigned long long)va);
    }

    if (nkeys > K_TOP) {
        size_t pad = nkeys - K_TOP;
        int all_zero = 1;
        for (size_t i = K_TOP; i < nkeys; i++) {
            if (READ_ONCE(a[1 + i]) != 0) { all_zero = 0; break; }
        }
        seq_printf(m, "[pad] %zu extra slots beyond K_TOP%s\n",
                   pad, all_zero ? " (all zero)" : "");
    }

    s2 = READ_ONCE(a[0]);
    if (unlikely((s2 & 1) || s1 != s2)) {
        seq_puts(m, "--- retry due to concurrent update ---\n");
        goto retry;
    }
    return 0;
}
#endif

 static int mmap_demo_topk_open(struct inode *inode, struct file *file)
 {
     return single_open(file, mmap_demo_topk_show, NULL);
 }

 static const struct file_operations mmap_demo_topk_fops = {

     .owner   = THIS_MODULE,
     .open    = mmap_demo_topk_open,
     .read    = seq_read,
     .llseek  = seq_lseek,
     .release = single_release,
 };

static int demo_mmap(struct file *filp, struct vm_area_struct *vma)
{
    size_t size = vma->vm_end - vma->vm_start;
    if (size > demo_len_bytes)
        vma->vm_end = vma->vm_start + demo_len_bytes;

    if(vma->vm_pgoff) vma->vm_pgoff = 0;

    vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
    return remap_vmalloc_range(vma, demo_buf, 0);
}

static const struct file_operations demo_fops = {
    .owner = THIS_MODULE,
    .mmap  = demo_mmap,
};

static struct miscdevice demo_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = DEV_NAME,
    .fops  = &demo_fops,
    .mode  = 0666, /* 데모용 */
};

static int __init demo_init(void)
{
    demo_len_bytes = PAGE_ALIGN((1+ K_TOP) * sizeof(u64));
    demo_buf = vmalloc_user(demo_len_bytes);

    if(!demo_buf)
	return -ENOMEM;
    memset(demo_buf,0,demo_len_bytes);

     /* /dev 등록 */
     int ret = misc_register(&demo_miscdev);
     if (ret)
         return ret;

#if IS_ENABLED(CONFIG_DEBUG_FS)
     demo_dbgdir = debugfs_create_dir("mmap_demo", NULL);
     if (demo_dbgdir)
         debugfs_create_file("topk", 0444, demo_dbgdir, NULL, &mmap_demo_topk_fops);
#endif
     return 0;
}

static void __exit demo_exit(void)
{
#if IS_ENABLED(CONFIG_DEBUG_FS)
     debugfs_remove_recursive(demo_dbgdir);
     demo_dbgdir = NULL;
#endif
    misc_deregister(&demo_miscdev);
    vfree(demo_buf);
}

module_init(demo_init);
module_exit(demo_exit);
MODULE_LICENSE("GPL");
