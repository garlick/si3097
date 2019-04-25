// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Linux Driver for the
 * Spectral Instruments 3097 Camera Interface
 *
 * Copyright (C) 2006  Jeffrey R Hagen
 */

#include <linux/version.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/poll.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <asm/atomic.h>

#include "si3097.h"
#include "si3097_module.h"

MODULE_AUTHOR(
	"Jeff Hagen, jhagen@as.arizona.edu Univ of Arizona, H-J. Meyer, Spectral Instruments");
MODULE_DESCRIPTION("Driver for Spectral Instruments 3097 Camera Interface");
MODULE_LICENSE("GPL");

/* module parameters */

/* if maxever is not zero on module load,
 * configure memory based on buflen and maxever
 */

int buflen = 1048576;
module_param(buflen, int, 0);

int maxever = 33554432; /* this is for lotis */
module_param(maxever, int, 0);

int timeout = 5000; /* default jiffies */
module_param(timeout, int, 0);

int verbose;
module_param(verbose, int, 0);

#define SI_MAX_CARDS 3
static struct SIDEVICE si_devices[SI_MAX_CARDS];
static int si_count;

static struct pci_device_id si_pci_tbl[] __initdata = {
	{ 0x10b5, 0x2679, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{
		0,
	}
};

static spinlock_t spin_multi_devs; /* use this for configure_device */
static struct pci_driver si_driver;
static dev_t si_dev;
static struct class *si_class;
static struct cdev si_cdev;

MODULE_DEVICE_TABLE(pci, si_pci_tbl);

/* The proc filesystem: function to read and entry */

static struct proc_dir_entry *si_proc;

int si_show_proc(struct seq_file *seq, void *private)
{
	struct SIDEVICE *d;
	struct pci_dev *pci;
	int nr;

	for (nr = 0; nr < si_count; nr++) {
		d = &si_devices[nr];
		pci = d->pci;
		if (pci) {
			seq_printf(seq,
			"SI %s, major %d minor %d devfn %d irq %d isopen %d\n",
			pci_name(pci), MAJOR(si_dev), nr, pci->devfn,
			pci->irq, atomic_read(&d->isopen));

		} else {
			seq_printf(seq, "SI TEST major %d minor %d\n",
				   MAJOR(si_dev), nr);
		}
	}
	return 0;
}

static int si_open_proc(struct inode *inode, struct file *file)
{
	return single_open(file, si_show_proc, NULL);
}

/* proc file operations */

const struct file_operations si_proc_fops = {
	.owner = THIS_MODULE,
	.open = si_open_proc,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* The different file operations */

const struct file_operations si_fops = {
	.owner = THIS_MODULE, /* owner */
	.read = si_read, /* read  */
	.write = si_write, /* write */
	.poll = si_poll, /* poll */
	.unlocked_ioctl = si_ioctl, /* ioctl */
	.mmap = si_mmap, /* mmap */
	.open = si_open, /* open */
	.release = si_close, /* release */
};

static int si_configure_device(struct pci_dev *pci,
			       const struct pci_device_id *id)
{
	struct SIDEVICE *dev;
	unsigned char irup;
	unsigned int error;
	int len, i;
	__u32 reg;
	int nr = si_count;

	if (nr == SI_MAX_CARDS) {
		dev_info(&pci->dev, "ignoring card - max %d\n", SI_MAX_CARDS);
		return -EINVAL;
	}

	if (pci_set_dma_mask(pci, 0xffffffff) != 0) {
		dev_info(&pci->dev, "pci_set_dma_mask failed\n");
		return -EIO;
	}

	/* in case of multiple devices on a SMP machine */

	spin_lock(&spin_multi_devs);

	dev = &si_devices[nr];
	memset(dev, 0, sizeof(struct SIDEVICE));
	si_count++;

	spin_unlock(&spin_multi_devs);

	error = pci_enable_device(pci);
	if (error < 0)
		goto out;

	dev->pci = pci;
	pci_read_config_byte(dev->pci, PCI_INTERRUPT_LINE, &irup);
	error = pci_request_regions(dev->pci, "SI3097");
	if (error < 0)
		goto out;

	for (i = 0; i < 4; i++) {
		len = pci_resource_len(pci, i);

		if (!len)
			continue;

		dev->bar_len[i] = len;
		dev->bar[i] = pci_iomap(pci, i, len);

		si_dbg(dev, "address of bar %d: 0x%lx\n", i,
			(unsigned long)dev->bar[i]);
	}

	if (pci->irq) {
		error = request_irq(pci->irq, (void *)si_interrupt,
					 IRQF_SHARED, "SI", dev);
		if (error) {
			si_info(dev, "failed to get irq %d error %d\n",
				pci->irq, error);
			si_info(dev, "skipping device\n");
			error = -ENODEV;
			goto out;
		}
	} else
		si_info(dev, "no pci interupt\n");

	si_info(dev, "irup 0x%x irq 0x%x id 0x%x\n",
		irup, pci->irq, id->device);

	pci_set_master(dev->pci);

	dev->bottom_half_wq = create_workqueue("SI3097");
	INIT_WORK(&dev->task, si_bottom_half);

	spin_lock_init(&dev->uart_lock);
	spin_lock_init(&dev->dma_lock);
	spin_lock_init(&dev->nopage_lock);

	init_waitqueue_head(&dev->dma_block);
	init_waitqueue_head(&dev->uart_wblock);
	init_waitqueue_head(&dev->uart_rblock);
	init_waitqueue_head(&dev->mmap_block);

	/* do the master reset local bus */
	//  LOCAL_REG_WRITE(dev, LOCAL_COMMAND, 0 );
	//  UART_REG_WRITE(dev, SERIAL_IER, 0);   /* disable all serial ints */
	//  mdelay(1);
	//  LOCAL_REG_WRITE(dev, LOCAL_COMMAND, (LC_FIFO_MRS_L) );

	//  reg = PLX_REG8_READ(dev, PCI9054_DMA_COMMAND_STAT);
	//  si_info(dev, "dma cmd stat 0x%x\n", reg);
	//if( reg & 1 ) {
	// si_stop_dma( dev, NULL );
	// }

	/* turn on interrupts */
	reg = PLX_REG_READ(dev, PCI9054_INT_CTRL_STAT);
	//  si_info(dev, "intr stat 0x%x\n", reg);
	PLX_REG_WRITE(dev, PCI9054_INT_CTRL_STAT, reg | (1 << 8) | (1 << 11));
	reg = PLX_REG_READ(dev, PCI9054_INT_CTRL_STAT);

	//  si_info(dev, "LOCAL_ID_NUMBER = 0x%x 0x%x\n",
	//    LOCAL_REG_READ(dev, LOCAL_ID_NUMBER), reg);

	//  reg = PLX_REG_READ( dev, PCI9054_INT_CTRL_STAT);

	//  if( reg & ( 1<<7 ) ) {
	//    si_info(dev, "local bus parity error 0x%x\n", reg);
	//    PLX_REG_WRITE( dev, PCI9054_INT_CTRL_STAT, reg | (1<<7) );
	//  }
	//  reg = PLX_REG_READ( dev, PCI9054_INT_CTRL_STAT );
	//  si_info(dev, "LOCAL_REV_NUMBER = 0x%x 0x%x\n",
	//    LOCAL_REG_READ(dev, LOCAL_REV_NUMBER), reg);

	//  reg = PLX_REG_READ( dev, PCI9054_INT_CTRL_STAT);

	//  reg = PLX_REG_READ(dev, PCI9054_DMA0_MODE );
	//  si_info(dev, "mode 0x%x\n", reg);

	/* assign verbose flag as module parameter */

	dev->verbose = verbose;

	/* on init, configure memory if module parameter maxever is non zero */

	if (maxever > 0) {
		if (buflen > maxever)
			buflen = maxever;

		dev->dma_cfg.total = maxever;
		dev->dma_cfg.buflen = buflen;
		dev->dma_cfg.timeout = timeout;
		dev->dma_cfg.maxever = maxever;
		dev->dma_cfg.config = SI_DMA_CONFIG_WAKEUP_ONEND;
		si_config_dma(dev);
	}
	device_create(si_class, NULL, MKDEV(MAJOR(si_dev), nr), NULL,
		      "sicamera%d", nr);

	return 0;
out:
	si_count--;
	return error;
}

/* the module stuff */

static int __init si_init_module(void)
{
	spin_lock_init(&spin_multi_devs);

	memset(&si_driver, 0, sizeof(struct pci_driver));
	si_driver.name = "si3097";
	si_driver.id_table = si_pci_tbl;
	si_driver.probe = si_configure_device;

	if (alloc_chrdev_region(&si_dev, 0, SI_MAX_CARDS, "si3097") < 0) {
		pr_err("SI alloc_chrdev_region failed\n");
		return -1;
	}
	si_class = class_create(THIS_MODULE, "chardrv");
	if (!si_class) {
		pr_err("SI class_create failed\n");
		goto out_reg;
	}
	cdev_init(&si_cdev, &si_fops);
	if (cdev_add(&si_cdev, si_dev, SI_MAX_CARDS) < 0) {
		pr_err("SI cdev_add failed\n");
		goto out_class;
	}

	si_proc = proc_create("si3097", 0, NULL, &si_proc_fops);
	if (!si_proc) {
		pr_err("SI proc_create failed\n");
		goto out_cdev;
	}

	//#define NO_HW_TEST 1

#ifdef NO_HW_TEST
	si_count = 1;
	dev = &si_devices[0];
	pr_info("SI TEST device configured\n");
	memset(dev, 0, sizeof(struct SIDEVICE));
	dev->test = 1;
	spin_lock_init(&dev->uart_lock);
	spin_lock_init(&dev->dma_lock);
#else
	if (pci_register_driver(&si_driver) < 0) {
		pr_err("SI pci_register_driver failed\n");
		goto out_proc;
	}
	if (si_count == 0) {
		pci_unregister_driver(&si_driver);
		pr_info("SI no cards found\n");
		goto out_proc;
	}
#endif

	return 0; /* succeed */

out_proc:
	remove_proc_entry("si3097", 0);
out_cdev:
	cdev_del(&si_cdev);
out_class:
	class_destroy(si_class);
out_reg:
	unregister_chrdev_region(si_dev, SI_MAX_CARDS);
	return -1;
}

static void __exit si_cleanup_module(void)
{
	struct SIDEVICE *dev;
	int nr;

	cdev_del(&si_cdev);

	for (nr = 0; nr < si_count; nr++) {
		dev = &si_devices[nr];
		si_stop_dma(dev, NULL);
		si_free_sgl(dev);
		si_cleanup_serial(dev);
		if (dev->pci) {
			if (dev->pci->irq)
				free_irq(dev->pci->irq, dev);

			pci_release_regions(dev->pci);
			pci_disable_device(dev->pci);
		}
		device_destroy(si_class, MKDEV(MAJOR(si_dev), nr));
	}

	class_destroy(si_class);
	unregister_chrdev_region(si_dev, SI_MAX_CARDS);

#ifndef NO_HW_TEST
	pci_unregister_driver(&si_driver);
#endif

	if (si_proc)
		remove_proc_entry("si3097", 0);
	si_proc = NULL;
}

module_init(si_init_module);
module_exit(si_cleanup_module);

int si_open(struct inode *inode, struct file *filp)
{
	int minor = MINOR(inode->i_rdev);
	int op;
	struct SIDEVICE *dev; /* device information */
	__u32 int_stat;

	if (minor >= si_count) {
		pr_err("SI bad minor number %d in open\n", minor);
		return -EBADF;
	}
	dev = &si_devices[minor];

	try_module_get(THIS_MODULE);

	op = atomic_read(&dev->isopen);
	if (op)
		si_info(dev, "minor %d already open %d, thats ok\n", op, minor);

	filp->private_data = dev;
	atomic_inc(&dev->isopen);

	int_stat = PLX_REG_READ(dev, PCI9054_INT_CTRL_STAT);

	return 0; /* success */
}

int si_close(struct inode *inode, struct file *filp) /* close */
{
	int minor = MINOR(inode->i_rdev);
	struct SIDEVICE *dev;

	if (minor >= si_count) {
		pr_err("SI bad minor number %d in close\n", minor);
		return -EBADF;
	}
	dev = &si_devices[minor];

	atomic_dec(&dev->isopen);

	if (atomic_read(&dev->isopen) <= 0) {
		//if (si_wait_vmaclose(dev)) {
		//	si_err(dev,
		//	"last close, but vma is still open %d\n", minor);
		//}
		si_stop_dma(dev, NULL);
	}

	if (atomic_read(&dev->isopen) <= 0 && atomic_read(&dev->vmact) != 0) {
		si_err(dev, "close without vma_close, %d\n",
		       atomic_read(&dev->vmact));
		atomic_set(&dev->vmact, 0);
	}

	filp->private_data = NULL;
	module_put(THIS_MODULE);

	return 0;
}

/* si_read polls data from uart */
/* si_read does not block */

ssize_t si_read(struct file *filp, char __user *buf, size_t count, loff_t *off)
{
	struct SIDEVICE *dev;
	int i, blocking, ret;
	__u8 ch;

	dev = filp->private_data;
	blocking = (dev->Uart.block & SI_SERIAL_FLAGS_BLOCK) != 0;

	if (dev->test) {
		for (i = 0; i < count; i++) { // for all characters
			if (put_user(0, (char __user *)&buf[i]))
				return -EFAULT;
		}
		return count;
	}

	for (i = 0; i < count; i++) { // for all characters

		while (si_receive_serial(dev, &ch) == FALSE) {
			if (!blocking) {
				si_serial_dbg(dev, "read, count %d rxcnt %d\n",
				       i, dev->Uart.rxcnt);
				return i;
			}

			ret = dev->Uart.timeout;
			wait_event_interruptible_timeout(
				dev->uart_rblock, si_uart_read_ready(dev), ret);
			if (si_uart_read_ready(dev)) {
				continue;
			} else {
				si_serial_dbg(dev, "read, count %d rxcnt %d\n",
				       i, dev->Uart.rxcnt);
				return i;
			}
		}

		if (put_user(ch, (char __user *)&buf[i]))
			return -EFAULT;
	}

	si_serial_dbg(dev, "read, count %d rxcnt %d\n", i, dev->Uart.rxcnt);

	return i;
}

/* si_write operates on the SI uart interface                       */
/* if blocking then it blocks till done writing */

ssize_t si_write(struct file *filp, const char __user *buf, size_t count,
		 loff_t *off)
{
	int i, ret, blocking;
	struct SIDEVICE *dev;
	__u8 ch;

	dev = filp->private_data;
	blocking = (dev->Uart.block & SI_SERIAL_FLAGS_BLOCK) != 0;

	si_serial_dbg(dev, "write, count %lu\n", (unsigned long)count);

	if (dev->test) {
		for (i = 0; i < count; i++) { // for all characters
			if (get_user(ch, (char __user *)&buf[i]))
				return -EFAULT;
		}
		return count;
	}

	for (i = 0; i < count; i++) { // for all characters
		if (get_user(ch, (char __user *)&buf[i]))
			return -EFAULT;

		if (si_transmit_serial(dev, ch) == FALSE) {
			if (blocking) {
				ret = dev->Uart.timeout;
				wait_event_interruptible_timeout(
					dev->uart_wblock, si_uart_tx_empty(dev),
					ret);
				/* care with wait_event_.., dual use third parameter */
				if (ret < 0)
					return ret;
				else
					si_transmit_serial(dev, ch);
			} else {
				return -EWOULDBLOCK;
			}
		}
	}

	if (blocking) {
		ret = dev->Uart.timeout;
		wait_event_interruptible_timeout(dev->uart_wblock,
						 si_uart_tx_empty(dev), ret);
		/* care with wait_event_.., dual use third parameter */
		if (ret >= 0)
			ret = count;
	} else {
		ret = i;
	}

	return ret;
}

/* true when UART transmit buffer is empty */

int si_uart_tx_empty(struct SIDEVICE *dev)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&dev->uart_lock, flags);
	ret = (dev->Uart.txcnt == dev->Uart.serialbufsize);
	spin_unlock_irqrestore(&dev->uart_lock, flags);
	return ret;
}

/* true when UART has data */

int si_uart_read_ready(struct SIDEVICE *dev)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&dev->uart_lock, flags);
	ret = dev->Uart.rxcnt;
	spin_unlock_irqrestore(&dev->uart_lock, flags);
	return ret;
}

/* when app calls select or poll, block until dma_wake */

unsigned int si_poll(struct file *filp, poll_table *table)
{
	struct SIDEVICE *dev;
	int done, rr;
	unsigned int mask;

	dev = filp->private_data;
	mask = 0;
	done = 0;
	rr = 0;

	/* either function for dma or UART but not both */

	if (dev->setpoll == SI_SETPOLL_UART) { /* for UART */
		rr = si_uart_read_ready(dev);
		if (!rr) {
			poll_wait(filp, &dev->uart_rblock,
				  table); /* queue for read */
			rr = si_uart_read_ready(dev);
		}
		if (rr)
			mask |= POLLIN | POLLRDNORM;
	} else {
		done = si_dma_wakeup(dev);

		if (!done) {
			poll_wait(filp, &dev->dma_block,
				  table); /* queue for read */
			done = si_dma_wakeup(dev);
		}

		if (done)
			mask |= POLLIN | POLLRDNORM;
	}

	if (dev->verbose & SI_VERBOSE_SERIAL) {
		char buf[256];
		if (dev->setpoll == SI_SETPOLL_UART) {
			strcpy(buf, "poll uart");
			if (rr)
				strcat(buf, ", rx not empty");
			else
				strcat(buf, ", rx empty");
		} else {
			strcpy(buf, "poll dma");
			if (done)
				strcat(buf, ", dma ready");
			else
				strcat(buf, ", dma not ready");
		}
		dev_dbg(&dev->pci->dev, "%s\n", buf);
	}
	return mask;
}
