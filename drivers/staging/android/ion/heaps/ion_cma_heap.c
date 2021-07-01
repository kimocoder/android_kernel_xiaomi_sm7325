// SPDX-License-Identifier: GPL-2.0
/*
 * ION Memory Allocator CMA heap exporter
 *
 * Copyright (C) Linaro 2012
 * Author: <benjamin.gaignard@linaro.org> for ST-Ericsson.
 */

#include <linux/device.h>
#include <linux/ion.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/cma.h>
#include <linux/scatterlist.h>
#include <linux/highmem.h>

struct ion_cma_heap {
	struct ion_heap heap;
	struct cma *cma;
} cma_heaps[MAX_CMA_AREAS];

#define to_cma_heap(x) container_of(x, struct ion_cma_heap, heap)

/* ION CMA heap operations functions */
static int ion_cma_allocate(struct ion_heap *heap, struct ion_buffer *buffer,
			    unsigned long len,
			    unsigned long flags)
{
	struct ion_cma_heap *cma_heap = to_cma_heap(heap);
	struct sg_table *table;
	struct ion_cma_buffer_info *info;
	struct page *pages = NULL;
	unsigned long size = PAGE_ALIGN(len);
	unsigned long nr_pages = size >> PAGE_SHIFT;
	unsigned long align = get_order(size);
	int ret;

	if (align > CONFIG_CMA_ALIGNMENT)
		align = CONFIG_CMA_ALIGNMENT;

	if (!ion_cma_has_kernel_mapping(cma_heap)) {
		flags &= ~((unsigned long)ION_FLAG_CACHED);
		buffer->flags = flags;

	if (PageHighMem(pages)) {
		unsigned long nr_clear_pages = nr_pages;
		struct page *page = pages;

		while (nr_clear_pages > 0) {
			void *vaddr = kmap_atomic(page);

			memset(vaddr, 0, PAGE_SIZE);
			kunmap_atomic(vaddr);
			page++;
			nr_clear_pages--;
		}
	} else {
		memset(page_address(pages), 0, size);
	}

	table = kmalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		goto err_alloc;

	ret = sg_alloc_table(table, 1, GFP_KERNEL);
	if (ret)
		goto free_table;

	sg_set_page(table->sgl, pages, size, 0);

	buffer->sg_table = table;
	ion_buffer_prep_noncached(buffer);

	return 0;

free_table:
	kfree(table);
err_alloc:
	if (info->cpu_addr)
		dma_free_attrs(dev, size, info->cpu_addr, info->handle, 0);
	else
		cma_release(cma_heap->cma, pages, nr_pages);
free_info:
	kfree(info);
	return -ENOMEM;
}

static void ion_cma_free(struct ion_buffer *buffer)
{
	struct ion_cma_heap *cma_heap = to_cma_heap(buffer->heap);
	struct msm_ion_buf_lock_state *lock_state =
			(struct msm_ion_buf_lock_state *)buffer->priv_virt;
	struct ion_cma_buffer_info *info = container_of(lock_state,
			struct ion_cma_buffer_info, lock_state);

	if (info->cpu_addr) {
		struct device *dev = cma_heap->heap.dev;

		dma_free_attrs(dev, PAGE_ALIGN(buffer->size), info->cpu_addr,
			       info->handle, 0);
	} else {
		struct page *pages = sg_page(buffer->sg_table->sgl);

		unsigned long nr_pages = PAGE_ALIGN(buffer->size) >> PAGE_SHIFT;
		/* release memory */
		cma_release(cma_heap->cma, pages, nr_pages);
	}
	/* release sg table */
	sg_free_table(buffer->sg_table);
	kfree(buffer->sg_table);
	kfree(info);
}

static struct ion_heap_ops ion_cma_ops = {
	.allocate = ion_cma_allocate,
	.free = ion_cma_free,
};

static int __ion_add_cma_heap(struct cma *cma, void *data)
{
	int *cma_nr = data;
	struct ion_cma_heap *cma_heap;
	int ret;

	if (*cma_nr >= MAX_CMA_AREAS)
		return -EINVAL;

	cma_heap = &cma_heaps[*cma_nr];
	cma_heap->heap.ops = &ion_cma_ops;
	cma_heap->heap.type = ION_HEAP_TYPE_DMA;
	cma_heap->heap.name = cma_get_name(cma);

	ret = ion_device_add_heap(&cma_heap->heap);
	if (ret)
		goto out;

	cma_heap->cma = cma;
	*cma_nr += 1;
out:
	return 0;
}

static int __init ion_cma_heap_init(void)
{
	int ret;
	int nr = 0;

	ret = cma_for_each_area(__ion_add_cma_heap, &nr);
	if (ret) {
		for (nr = 0; nr < MAX_CMA_AREAS && cma_heaps[nr].cma; nr++)
			ion_device_remove_heap(&cma_heaps[nr].heap);
	}

	return ret;
}

static void __exit ion_cma_heap_exit(void)
{
	int nr;

	for (nr = 0; nr < MAX_CMA_AREAS && cma_heaps[nr].cma; nr++)
		ion_device_remove_heap(&cma_heaps[nr].heap);
}

module_init(ion_cma_heap_init);
module_exit(ion_cma_heap_exit);
MODULE_LICENSE("GPL v2");
