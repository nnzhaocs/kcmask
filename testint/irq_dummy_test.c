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

static int test_thread(void *arg)
{

	set_user_nice(current, -20);
	//set_current_state(TASK_INTERRUPTIBLE);
	__set_current_state(TASK_RUNNING);
	while (!kthread_should_stop()) {
		schedule();
		printk("sleep...");
		msleep(5000);
    {
        __asm__ __volatile__ ("int $238");
    }
	//	__set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);

	return 0;
}

#include <linux/swap.h>
extern void call_interrupt(void);
#define RESERVED_MEMORY_OFFSET  0x100000000     /* Offset is 4GB */
extern u8 *reserved_memory;
//EXPORT_SYMBOL_GPL(reserved_memory);

extern struct workqueue_struct *test_workqueue;
//EXPORT_SYMBOL_GPL(test_workqueue);
extern struct acc_work_struct test_work;
//EXPORT_SYMBOL_GPU(test_work);
extern void do_save_page(struct work_struct *p_work);
//EXPORT_SYMBOL_GPL(do_save_page);



struct task_struct *thread = NULL;
static int __init init_dummy_irq_test(void)
{
    printk("initing test dummy irq.\n");
    test_workqueue = create_workqueue("test_work_queue");
    if (NULL == test_workqueue){
        printk("cannot create test work queue.\n");
        return -1;
    }
    INIT_WORK(&(test_work.save_page), do_save_page);

    printk("ioremap reserved mem.\n");
    
    reserved_memory = ioremap_nocache(RESERVED_MEMORY_OFFSET, 0x2000);
        if (NULL == reserved_memory) {
            printk("Get NULL piointerrr...\n");
        }
    reserved_memory[0] = 0xff;
    printk("reserved_memory %lx = %d...\n", (long unsigned int) reserved_memory , reserved_memory[0]);
    return 0;  
//    call_interrupt();
//    {
//        __asm__ __volatile__ ("int $160");
//    }
//

      thread = kthread_create_on_node(test_thread,
			(void *)NULL, cpu_to_node(0),
			"test_thread%d", 0);
/* bind thread to the cpu */
	if (likely(!IS_ERR(thread))) {
		kthread_bind(thread, 1);
		wake_up_process(thread);
	}

    return 0;
}

static void __exit exit_dummy_irq_test(void)
{
    printk("exit test dummy irq %lx\n", (long unsigned int) reserved_memory);
        if (NULL == reserved_memory) {
            return;
        }
    if (test_workqueue){
        destroy_workqueue(test_workqueue);
    }
    if (reserved_memory)
        iounmap(reserved_memory);

    return;

    if (thread)
	kthread_stop(thread); 
   return ;
}


module_init(init_dummy_irq_test);
module_exit(exit_dummy_irq_test);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nannan Zhao <znannan1@vt.edu>");
MODULE_DESCRIPTION("Kernel support for CMASK");
