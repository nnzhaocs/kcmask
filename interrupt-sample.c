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

extern int asm_call_interrupt(void);

irqreturn_t int068_interrupt(int irq, void *dev_id)//, struct pt_regs *regs)
{
    printk("Interrupt should be handled there\n");
    //return 0;
    return IRQ_HANDLED;
}

static int __init
clcdint_init(void)
{
    unsigned int irq;
    unsigned int irqflags;
    int ret;

    irq=4;
    //irqflags=IRQF_SHARED | 
    irqflags = IRQF_SHARED;//IRQF_NO_SUSPEND;
    printk("START REGISTERING ...");
    ret = request_irq(irq, (irq_handler_t)int068_interrupt, IRQF_NO_SUSPEND, "clcdint-int068", NULL);

    if (ret!=0) {
            printk("ERROR: Cannot request IRQ %d", irq);
            printk(" - code %d , EIO %d , EINVAL %d\n", ret, EIO, EINVAL);
	    return -1;
    }

    printk("CLCDINT_INIT\n");
    printk("START CALLING INTERRUPT ...");
    asm_call_interrupt();
    printk("SUCCESSFULLY CALLED ...");
    return 0;
}

module_init(clcdint_init);

static void __exit
clcdint_exit(void)
{
    unsigned int irq;
    irq=68;
    free_irq(irq, NULL);
    printk("CLCDINT_EXIT\n");
}

module_exit(clcdint_exit);
