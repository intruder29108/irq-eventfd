/*
 * irq_eventfd.c
 * Framework for binding device interrupt vectors to userspace eventfds.
 *
 */
#include <linux/device.h>
#include <linux/moduleparam.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/eventfd.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "irq_eventfd.h"

#define irqefd_err(fmt, ...)						\
do {									\
	printk(KERN_ERR "[ERR ]: "fmt"\n", ##__VA_ARGS__);		\
} while(0)

#define irqefd_dbg(drv, fmt, ...)					\
do {									\
	if(drv->verbose) {						\
		dev_info(&drv->dev, "[INFO]: "fmt"\n", ##__VA_ARGS__);	\
	}								\
} while(0)

#define IRQEFD_MAXDEVS		(1)

/*
 * Module load parameters.
 */
static bool debug = false;
module_param(debug, bool, 0);

struct irqefd_driver_data {
	struct list_head list;
	struct mutex lock;
	struct cdev *c_dev;
	dev_t dev_num;
	bool verbose;
	struct device dev;
};

static struct irqefd_driver_data *irqefd_drvdata;

static int irqefd_open(struct inode *inode, struct file *filep) {

	return 0;
}

static int irqefd_release(struct inode *inode, struct file *filep) {

	return 0;
}


static long irqefd_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg) {

	int ret;
	struct irqefd_ioctl_arg argp;
	struct file *fp;
	struct irqefd_devdata *devdata, *hit;
	struct eventfd_ctx *efd_ctx;

	(void)copy_from_user(&argp, (void *)arg, sizeof(argp));
	fp = fget(argp.dfd);
	if (!fp) {
		irqefd_err("fdget failed for fd(%d)", argp.dfd);
		return -1;
	}
	irqefd_dbg(irqefd_drvdata,
			"ioctl: cmd(%d) dfd(%d) efd(%d) eidx(%d) inode(%p)",
			cmd, argp.dfd, argp.efd, argp.eidx, fp->f_inode);

	switch (cmd) {
		case ATTACH_EVENT:
			hit = NULL;
			list_for_each_entry(devdata, &irqefd_drvdata->list,
					node) {
				if (devdata->inode == fp->f_inode) {
					hit = devdata;
					break;
				}
			}
			if (!hit) {
				irqefd_err("device not registered");
				ret = -1;

				goto fail;
			}
			if (hit->ctx[argp.eidx]) {
				irqefd_err("already registered vector(%d)",
						argp.eidx);
				ret = -1;

				goto fail;
			}
			hit->ctx[argp.eidx] = eventfd_ctx_fdget(argp.efd);

			break;

		case DETACH_EVENT:

			hit = NULL;
			list_for_each_entry(devdata, &irqefd_drvdata->list,
					node) {
				if (devdata->inode == fp->f_inode) {
					hit = devdata;
					break;
				}
			}
			if (!hit) {
				irqefd_err("device not registered");
				ret = -1;

				goto fail;
			}
			if (!hit->ctx[argp.eidx]) {
				irqefd_err("event(%d) not set", argp.eidx);
				ret = -1;

				goto fail;
			}
			efd_ctx = hit->ctx[argp.eidx];
			hit->ctx[argp.eidx] = NULL;
			eventfd_ctx_put(efd_ctx);

			break;

		case GET_NUM_EVENTS:
			hit = NULL;
			list_for_each_entry(devdata, &irqefd_drvdata->list, node) {
				if (devdata->inode == fp->f_inode) {
					hit = devdata;
					break;
				}
			}
			if (!hit) {
				irqefd_err("device not registered");
				ret = -1;

				goto fail;
			}
			argp.nevts = hit->num_events;

			break;

		default:
			irqefd_err("unknown cmd(%d)", cmd);
			ret = -1;

			goto fail;

	}

	fput(fp);
	return (copy_to_user((void *)arg, &argp, sizeof(argp)));

fail:
	if (fp) {
		fput(fp);
	}

	return ret;
}

static const struct file_operations irqefd_fops = {
	.owner			= THIS_MODULE,
	.open			= irqefd_open,
	.release		= irqefd_release,
	.unlocked_ioctl		= irqefd_ioctl,
};

static int irqefd_cdevadd(struct irqefd_driver_data *drvdata) {

	struct cdev *cdev = NULL;
	dev_t dev_num = 0;
	int result;

	result = alloc_chrdev_region(&dev_num, 0,
			IRQEFD_MAXDEVS, "irqefd");
	if (result) {
		result = -1;
		irqefd_err("failed to get chrdev region");

		goto out;
	}
	cdev = cdev_alloc();
	if (!cdev) {
		result = -ENOMEM;
		irqefd_err("failed to alloc cdev\n");

		goto out_unregister;
	}

	cdev->owner = THIS_MODULE;
	cdev->ops = &irqefd_fops;
	kobject_set_name(&cdev->kobj, "%s", "irqefd");

	result = cdev_add(cdev, dev_num, IRQEFD_MAXDEVS);
	if (result) {
		goto out_put;
	}

	drvdata->dev_num = dev_num;
	drvdata->c_dev  = cdev;
	irqefd_dbg(drvdata, "added chrdev(%p) with major(%d)",
			cdev, MAJOR(dev_num));

	return 0;

out_put:
	kobject_put(&cdev->kobj);
out_unregister:
	unregister_chrdev_region(dev_num, IRQEFD_MAXDEVS);
out:
	return result;
}

static void irqefd_cdev_remove(struct irqefd_driver_data *drvdata) {

	irqefd_dbg(drvdata, "removing chrdev(%p) with major(%d)",
			drvdata->c_dev, MAJOR(drvdata->dev_num));
	unregister_chrdev_region(drvdata->dev_num, IRQEFD_MAXDEVS);
	cdev_del(drvdata->c_dev);
}

static int irqefd_init(void) {

	int ret;

	irqefd_drvdata = kzalloc(sizeof *irqefd_drvdata, GFP_KERNEL);
	if (!irqefd_drvdata) {
		ret = -ENOMEM;
		goto fail_nomem;
	}

	INIT_LIST_HEAD(&irqefd_drvdata->list);
	mutex_init(&irqefd_drvdata->lock);
	irqefd_drvdata->verbose = debug;

	dev_set_name(&irqefd_drvdata->dev, "irq_eventfd");
	if ((ret = device_register(&irqefd_drvdata->dev)) != 0) {
		irqefd_err("failed to register device");
		goto fail_device_register;
	}

	ret = irqefd_cdevadd(irqefd_drvdata);
	if (ret != 0) {
		goto fail_cdevadd;
	}
	return 0;

fail_cdevadd:
	device_del(&irqefd_drvdata->dev);
fail_device_register:
	kfree(irqefd_drvdata);
fail_nomem:
	return ret;
}

static void irqefd_remove(void) {

	irqefd_cdev_remove(irqefd_drvdata);
	device_del(&irqefd_drvdata->dev);
	kfree(irqefd_drvdata);
}

int irqefd_register_device(struct irqefd_devdata *devdata) {

	devdata->ctx = kzalloc((sizeof *(devdata->ctx)) * devdata->num_events,
			GFP_KERNEL);
	if (!devdata->ctx) {
		irqefd_err("failed to allocate event contexts\n");
		return -1;
	}
	irqefd_dbg(irqefd_drvdata, "allocated event context(%p) with %d events\n",
			devdata->ctx, devdata->num_events);
	mutex_lock(&irqefd_drvdata->lock);
	list_add(&devdata->node, &irqefd_drvdata->list);
	mutex_unlock(&irqefd_drvdata->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(irqefd_register_device);

int irqefd_unregister_device(struct irqefd_devdata *devdata) {

	void *tmp = devdata->ctx;

	devdata->ctx = NULL;
	kfree(tmp);

	mutex_lock(&irqefd_drvdata->lock);
	list_del(&devdata->node);
	mutex_unlock(&irqefd_drvdata->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(irqefd_unregister_device);

module_init(irqefd_init);
module_exit(irqefd_remove);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Antony Clince Alex");
