// Author: Jiaqi Lou
// 03/11/2024

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/sched/task_stack.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/export.h>
#include <linux/list.h>

#include <rdma/peer_mem.h>
#include <linux/pci-p2pdma.h>

#include "base/dlb2_mbox.h"
#include "base/dlb2_resource.h"
#include "dlb2_file.h"
#include "dlb2_intr.h"
#include "dlb2_ioctl.h"
#include "dlb2_main.h"
#include "dlb2_sriov.h"
#include "dlb2_perf.h"

#define DRV_NAME	"dlb2_peer_mem"
#define DRV_VERSION	"1.0"
#define DRV_RELDATE	__DATE__

MODULE_AUTHOR("Jiaqi Lou");
MODULE_DESCRIPTION("DLB2 peer memory");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(DRV_VERSION);

static void *reg_handle;
extern struct list_head dlb2_dev_list;

struct dlb2_context {
	unsigned long virt_addr;
	size_t size;
	struct vm_area_struct *vma;
    
    struct sg_table sg_head;
	u64 core_context;
	u64 page_virt_start;
	u64 page_virt_end;
	size_t mapped_size;
	unsigned long npages;
	int nmap;
	unsigned long page_size;
	int writable;
	int dirty;
};

/**
* acquire - Begin working with a user space virtual address range
*
* @addr - Virtual address to be checked whether belongs to peer.
* @size - Length of the virtual memory area starting at addr.
* @peer_mem_private_data - Obsolete, always NULL
* @peer_mem_name - Obsolete, always NULL
* @client_context - Returns an opaque value for this acquire use in
*                   other APIs
*
* Returns 1 if the peer_memory_client supports the entire virtual
* address range, 0 or -ERRNO otherwise.  If 1 is returned then
* release() will be called to release the acquire().
*/
static int dlb2_mem_acquire(unsigned long addr, size_t size, void *peer_mem_private_data, 
                            char *peer_mem_name, void **client_context)
{         
    printk("Try dlb2_mem_acquire ...\n");
    int ret = 0;
	unsigned long pfn, end;
	struct vm_area_struct *vma = NULL;
    struct dlb2_context *dlb2_ctx = kzalloc(sizeof(*dlb2_ctx), GFP_KERNEL);
    if (!dlb2_ctx)
		/* Error case handled as not mine */
		return 0;

	dlb2_ctx->page_virt_start = addr & PAGE_MASK;
	dlb2_ctx->page_virt_end   = (addr + size + PAGE_SIZE - 1) & PAGE_MASK;
	dlb2_ctx->mapped_size  = dlb2_ctx->page_virt_end - dlb2_ctx->page_virt_start;
    printk("[dlb2_mem_acquire]: mapped_size is %d\n", dlb2_ctx->mapped_size);

    end = addr + size;

	vma = find_vma(current->mm, addr);
    if (!vma || vma->vm_end < end)
		goto err;

    if (follow_pfn(vma, addr, &pfn))
		goto err;

    // check whether the virtual address corresponding to DLB's memory space
	struct dlb2 *dlb2_dev = list_first_entry(&dlb2_dev_list, struct dlb2, list);
    if (!(dlb2_dev->hw.func_phys_addr >> PAGE_SHIFT <= pfn && pfn < (dlb2_dev->hw.func_phys_addr + pci_resource_len(dlb2_dev->pdev, DLB2_FUNC_BAR)) >> PAGE_SHIFT)) {
        // not belong to DLB
        printk("Not belong to DLB start: virt_addr is %lx, phys_addr is %lx, pfn is %lx\n", addr, virt_to_phys(addr), pfn);
        printk("Not belong to DLB end: virt_addr is %lx, phys_addr is %lx\n", end, virt_to_phys(end));
        goto err;
    }

	printk("DLB2 peer memory acquire: virt_addr is %lx, virt_to_phys is %lx, phys_addr is %lx, pfn is %lx\n", addr, virt_to_phys(addr), pfn << PAGE_SHIFT, pfn);

    *client_context = dlb2_ctx;
    __module_get(THIS_MODULE);
    return 1;

err:
    kfree(dlb2_ctx);
    return 0;
}

static void dlb2_mem_put_pages(struct sg_table *sg_head, void *context);


/**
 * get_pages - Fill in the first part of a sg_table for a virtual
 *             address range
 *
 * @addr - Virtual address to be checked whether belongs to peer.
 * @size - Length of the virtual memory area starting at addr.
 * @write - Always 1
 * @force - 1 if write is required
 * @sg_head - Obsolete, always NULL
 * @client_context - Value returned by acquire()
 * @core_context - Value to be passed to invalidate_peer_memory for
 *                 this get
 *
 * addr/size are passed as the raw virtual address range requested by
 * the user, it is not aligned to any page size. get_pages() is always
 * followed by dma_map().
 *
 * Upon return the caller can call the invalidate_callback().
 *
 * Returns 0 on success, -ERRNO on failure. After success put_pages()
 * will be called to return the pages.
 */
static int dlb2_mem_get_pages(unsigned long addr, size_t size, int write, int force,
				 struct sg_table *sg_head, void *client_context, u64 core_context)
{
    printk("Try dlb2_mem_get_pages ...\n");
	int ret;
	unsigned long cur_base;
	struct page **page_list;
	struct scatterlist *sg, *sg_list_start;
    struct sg_table *sg_table = NULL;
	int i;
	struct dlb2_context *dlb2_ctx;

	dlb2_ctx = (struct dlb2_context *)client_context;
	dlb2_ctx->core_context = core_context;
	dlb2_ctx->page_size = PAGE_SIZE;
	dlb2_ctx->writable = write;
	dlb2_ctx->npages = (dlb2_ctx->mapped_size + PAGE_SIZE - 1) / PAGE_SIZE;
    printk("[dlb2_mem_get_pages]: mapped_size is %d\n", dlb2_ctx->mapped_size);
    printk("[dlb2_mem_get_pages]: page_size is %d\n", dlb2_ctx->page_size);
    printk("[dlb2_mem_get_pages]: number of page is %d\n", dlb2_ctx->npages);

	if (dlb2_ctx->npages <= 0)
		return -EINVAL;

    // get pages for DLB based on the BAR address
	page_list = (struct page **)__get_free_page(GFP_KERNEL);
	if (!page_list) {
		ret = -ENOMEM;
	}

	/* mark that pages were exposed from the peer memory */
	// dlb2_ctx->dirty = 1;

	return 0;
}


/**
 * dma_map - Create a DMA mapped sg_table
 *
 * @sg_head - The sg_table to allocate
 * @client_context - Value returned by acquire()
 * @dma_device - The device that will be doing DMA from these addresses
 * @dmasync - Obsolete, always 0
 * @nmap - Returns the number of dma mapped entries in the sg_head
 *
 * Must be called after get_pages(). This must fill in the sg_head with
 * DMA mapped SGLs for dma_device. Each SGL start and end must meet a
 * minimum alignment of at least PAGE_SIZE, though individual sgls can
 * be multiples of PAGE_SIZE, in any mixture. Since the user virtual
 * address/size are not page aligned, the implementation must increase
 * it to the logical alignment when building the SGLs.
 *
 * Returns 0 on success, -ERRNO on failure. After success dma_unmap()
 * will be called to unmap the pages. On failure sg_head must be left
 * untouched or point to a valid sg_table.
 */
static int dlb2_mem_dma_map(struct sg_table *sg_head, void *context,
			       struct device *dma_device, int dmasync,
			       int *nmap)
{
    printk("Try dlb2_mem_dma_map ...\n");
    int ret, i;
    struct dlb2_context *dlb2_ctx =
		(struct dlb2_context *)context;
	struct scatterlist *sg;

    // allocate sg table
    printk("[dlb2_mem_dma_map]: number of page is %d\n", dlb2_ctx->npages);
    ret = sg_alloc_table(sg_head, dlb2_ctx->npages, GFP_KERNEL);
	if (ret) {
        printk("Failed to sg_alloc_table, ret=%d\n", ret);
		return ret;
    }

    
	struct dlb2 *dlb2_dev = list_first_entry(&dlb2_dev_list, struct dlb2, list);
    unsigned long pgoff = dlb2_dev->hw.func_phys_addr;
	if (dlb2_dev->type == DLB2_PF || dlb2_dev->type == DLB2_5_PF) {
		// if (port->is_ldb)
			pgoff += DLB2_DRV_LDB_PP_OFFS(9);
		// else
			// pgoff += DLB2_DRV_DIR_PP_OFFS(port->id);
	} else {
		// if (port->is_ldb)
			pgoff += DLB2_LDB_PP_OFFS(9);
		// else
			// pgoff += DLB2_DIR_PP_OFFS(port->id);
	}

    for_each_sg(sg_head->sgl, sg, dlb2_ctx->npages, i) {
		sg_set_page(sg, NULL, dlb2_ctx->page_size, 0);
		sg->dma_address = pgoff;
		sg->dma_length = dlb2_ctx->page_size;
	}

    dlb2_ctx->sg_head = *sg_head;
	printk("allocated sg_head.sgl=%p\n", dlb2_ctx->sg_head.sgl);

    // return number of page added
	*nmap = dlb2_ctx->npages;

	return 0;
}

static int dlb2_mem_dma_unmap(struct sg_table *sg_head, void *context,
				 struct device  *dma_device)
{
    printk("Try dlb2_mem_dma_unmap ...\n");
	struct dlb2_context *dlb2_ctx =
		(struct dlb2_context *)context;

	if (!context) {
		printk("dlb2_mem_dma_unmap: invalid context\n");
		return -EINVAL;
	}

	return 0;
}

static void dlb2_mem_put_pages(struct sg_table *sg_head, void *context)
{
    printk("Try dlb2_mem_put_pages ...\n");
	struct scatterlist *sg;
	struct page *page;
	int i;

	struct dlb2_context *dlb2_ctx =
		(struct dlb2_context *)context;

	sg_free_table(sg_head);
}

static void dlb2_mem_release(void *context)
{
    printk("Try dlb2_mem_release ...\n");
	struct dlb2_context *dlb2_ctx =
		(struct dlb2_context *)context;

	kfree(dlb2_ctx);
	module_put(THIS_MODULE);
}

static unsigned long dlb2_mem_get_page_size(void *context)
{
	struct dlb2_context *dlb2_ctx =
				(struct dlb2_context *)context;

	return dlb2_ctx->page_size;
}


static struct peer_memory_client dlb2_peer_mem_client = {
	.name           = DRV_NAME,
	.version        = DRV_VERSION,
	.acquire	    = dlb2_mem_acquire,
	.get_pages	    = dlb2_mem_get_pages,
	.dma_map	    = dlb2_mem_dma_map,
	.dma_unmap	    = dlb2_mem_dma_unmap,
	.put_pages	    = dlb2_mem_put_pages,
	.get_page_size	= dlb2_mem_get_page_size,
	.release	    = dlb2_mem_release,
};

static int __init dlb2_mem_client_init(void)
{
	reg_handle = ib_register_peer_memory_client(&dlb2_peer_mem_client, NULL);
	if (!reg_handle)
		return -EINVAL;

	printk(DRV_NAME ": dlb2_pmem kernel module loaded\n");

	return 0;
}

static void __exit dlb2_mem_client_cleanup(void)
{
	ib_unregister_peer_memory_client(reg_handle);
}


module_init(dlb2_mem_client_init);
module_exit(dlb2_mem_client_cleanup);
