/*
 * kcmask.c - kcmask driver file
 *
 * kcmask is a backend for frontswap that takes pages that are in the process
 * of being swapped out and attempts to compress and store them in a
 * RAM-based memory pool.  This can result in a significant I/O reduction on
 * the swap device and, in the case where decompressing from RAM is faster
 * than reading from the swap device, can also improve workload performance.
 *
 * Copyright (C) 2012  Seth Jennings <sjenning@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
*/

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/frontswap.h>
#include <linux/rbtree.h>
#include <linux/swap.h>
#include <linux/crypto.h>
#include <linux/mempool.h>
#include <linux/zpool.h>

#include <linux/mm_types.h>
#include <linux/page-flags.h>
#include <linux/swapops.h>
#include <linux/writeback.h>
#include <linux/pagemap.h>
//NANNAN
#include <linux/unistd.h>
#include <linux/mm.h>
#include <asm/unistd.h>
#include <linux/bio.h>
#include <linux/kernel_stat.h>
#include <linux/gfp.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/uio.h>
#include <asm/pgtable.h>

#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>

#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/acpi.h>
#include <linux/swap.h>
/*********************************
* statistics
**********************************/

#define RESERVED_MEMORY_OFFSET  0x100000000     /* Offset is 4GB */

extern u8 *reserved_memory;

/*********************************
* frontswap_kcmask_ops->store hooks
**********************************/

static int kcmask_frontswap_store(unsigned type, pgoff_t offset,
				struct page *page)
{
	int ret = 0;
	u8 *src;
	unsigned int dlen = PAGE_SIZE;
	sector_t sector = (sector_t)__page_file_index(page) << (PAGE_SHIFT - 9);//swap_page_sector(page);

	pr_info("kcmask_frontswap_store!\n");
	
	pr_info("map reserved memory to this virtual memory space\n");
	//reserved_memory = ioremap_nocache(RESERVED_MEMORY_OFFSET, dlen+sizeof(sector_t)+sizeof(struct page));

	src = kmap_atomic(page);
	pr_info("cp data to reserved memory\n");
	memcpy(reserved_memory, src, dlen);
	kunmap_atomic(src);

	
	memcpy(reserved_memory+dlen, &sector, sizeof(sector_t));

	//memcpy(reserved_memory+dlen+sizeof(sector_t), page, sizeof(struct page));

	pr_info("sector: %ld\n", sector);

	pr_info("call asm interrupt to insert data to cache\n");
	//asm_call_interrupt();

	{
        	__asm__ __volatile__ ("int $238");
    }

	pr_info("finished interrupt handler\n");
	return ret;
}

//static int kcmask_frontswap_load(unsigned type, pgoff_t offset,
//				struct page *page)
//{
//
//	return 0; //meaning that we can successfully read from frontswap and there is no need to read from swap device.
//}
//
///* frees an entry in kcmask */
//static void kcmask_frontswap_invalidate_page(unsigned type, pgoff_t offset)
//{
//	return;
//}
//
///* frees all kcmask entries for the given swap type */
//static void kcmask_frontswap_invalidate_area(unsigned type)
//{
//	return;
//}

//static void kcmask_frontswap_init(unsigned type)
//{
//
//}

/*********************************
 kcmask ops:
 Only store is implemented
**********************************/

static struct frontswap_ops kcmask_frontswap_ops = {
	.store = kcmask_frontswap_store,
	.load = NULL,
	.invalidate_page = NULL,
	.invalidate_area = NULL,
	.init = NULL
};

/*********************************
* module init and exit
**********************************/
static int __init init_kcmask(void)
{
	unsigned int dlen = PAGE_SIZE;
	pr_info("start register front swap ops\n");
	frontswap_register_ops(&kcmask_frontswap_ops);
	reserved_memory = ioremap_nocache(RESERVED_MEMORY_OFFSET, dlen*2);
    return 0;
}

static void __exit exit_kcmask(void)
{
    iounmap(reserved_memory);
	pr_info("start unregister front swap ops done\n");
}

/* must be late so crypto has time to come up */
//late_initcall(init_kcmask);
module_init(init_kcmask);
module_exit(exit_kcmask);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nannan Zhao <znannan1@vt.edu>");
MODULE_DESCRIPTION("Kernel support for CMASK");
