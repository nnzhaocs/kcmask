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

#define RESERVED_MEMORY_OFFSET  0x180000000     /* Offset is 4GB */

extern u8 *reserved_memory;
extern struct amb_area amb_test;
//extern enum swap_rw_modified_ops;
extern u8 flag_handle;

/*
 * Initialize the flag to SWAP_WRITEPAGE_TOBUFFER. So we can write swap-out pages to buffer.
 * TODO:
 * kcmask should be load with mem/swap kernel modules at booting.
 * Remove unused header files.
 * Remove unused lines of code from the files in the project.
 * */

/*********************************
* module init and exit
**********************************/
extern u8 flag_len;
extern struct amb_area amb_test;

static int __init init_kcmask(void)
{
	unsigned int dlen = PAGE_SIZE;
	enum swap_rw_modified_ops flag = SWAP_WRITEPAGE_TOBUFFER;

	pr_info("start register front swap ops\n");

	reserved_memory = ioremap_nocache(RESERVED_MEMORY_OFFSET, dlen*2);
	spin_lock_init(&amb_test.lock);

	amb_test.entry.flag = SWAP_WRITEPAGE_TOBUFFER; //write to buffer
	memcpy(reserved_memory+flag_handle, &flag, flag_len);

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
MODULE_DESCRIPTION("Kernel support for Hardware compression");
