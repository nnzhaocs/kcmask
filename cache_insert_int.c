#include <linux/init.h>
#include <linux/module.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_address.h>

//#include <asm/exception.h>
//#include <asm/mach/irq.h>

#define  IRQNO  251

//void int250_interrupt(int irq, void *dev_id, struct pt_regs *regs)
//{
//    printk("cmask_cache_insert: Interrupt should be handled there.\n");
//}

static irqreturn_t cacheInsert_InterruptHandler(int IRQ_Channel, void *DeviceIdentifier)
{
	printk("cmask_cache_insert: Interrupt should be handled there.\n");
	return IRQ_HANDLED;
}

static int __init cache_insert_int_init(void)
{
    unsigned int irq;
    unsigned int irqflags;
    int ret;

    irq=IRQNO;
    irqflags=IRQF_NO_SUSPEND;
    printk("cmask_cache_insert: start register it.\n");
    ret = request_irq(irq, (irq_handler_t)cacheInsert_InterruptHandler,
            irqflags, "cacheinsertint-int068", NULL);

    if (ret!=0) {
            printk(KERN_INFO "ERROR: Cannot request IRQ %d", irq);
            printk(" - code %d , EIO %d , EINVAL %d\n", ret, EIO, EINVAL);
    }

    printk("CACHEINSERT_INIT\n");
    return 0;
}

module_init(cache_insert_int_init);

static void __exit cache_insert_int_exit(void)
{
    unsigned int irq;
    irq=IRQNO;
    free_irq(irq, NULL);
    printk("CACHEINSERT_EXIT\n");
}

module_exit(cache_insert_int_exit);
