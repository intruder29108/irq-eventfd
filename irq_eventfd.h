/*
 * irq_eventfd.h
 *
 */
#ifndef __IRQ_EVENTFD_H__
#define __IRQ_EVENTFD_H__

#include <linux/list.h>

struct irqefd_devdata {
	struct inode *inode;
	struct eventfd_ctx **ctx;
	int num_events;
	struct list_head node;
};

struct irqefd_ioctl_arg {
	int dfd;	/* fd to the device which owns the interrupt */
	int efd;	/* eventfd to be attached */
	int eidx;	/* index to event to be attached */
	int nevts;	/* number events supported/attached */
};

int irqefd_register_device(struct irqefd_devdata *devdata);
int irqefd_unregister_device(struct irqefd_devdata *devdata);

#define ATTACH_EVENT		(0x0600)
#define DETACH_EVENT		(0x0601)
#define GET_NUM_EVENTS		(0x0602)

#endif
