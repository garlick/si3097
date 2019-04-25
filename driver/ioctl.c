/*

Linux Driver for the
Spectral Instruments 3097 Camera Interface

Copyright (C) 2006  Jeffrey R Hagen

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/

/* define the ioctl calls */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/poll.h>
#include <linux/pci.h>
#include <asm/atomic.h>

#include "si3097.h"
#include "si3097_module.h"

/* si_ioctl:  Processes the IOCTL messages sent to this device */

long si_ioctl(struct file *filp, unsigned int cmd, unsigned long args)
{
	int ret;
	struct SI_DMA_STATUS dma_status;
	struct SI_SERIAL_PARAM serial_param;
	struct SIDEVICE *dev;
	unsigned long flags;

	dev = (struct SIDEVICE *)filp->private_data;
	if (!dev)
		return -EIO;

	//  if( dev->verbose )
	//    si_dbg(dev, "ioctl %d\n",  _IOC_SIZE(cmd));

	/*  Si Interface only */

	ret = 0;
	switch (cmd) {
	case SI_IOCTL_RESET:
		ret = si_reset(dev); /* parameter ignored */
		break;

	case SI_IOCTL_SERIAL_IN_STATUS: {
		int status;

		spin_lock_irqsave(&dev->uart_lock, flags);
		status = dev->Uart.rxcnt;
		spin_unlock_irqrestore(&dev->uart_lock, flags);

		ret = put_user(status, (int __user *)args);
		si_serial_dbg(dev, "SI_IOCTL_SERIAL_IN_STATUS %d\n", status);
	} break;

	case SI_IOCTL_SERIAL_OUT_STATUS: {
		int ul;

		spin_lock_irqsave(&dev->uart_lock, flags);
		ul = dev->Uart.serialbufsize - dev->Uart.txcnt;
		spin_unlock_irqrestore(&dev->uart_lock, flags);
		ret = put_user(ul, (int __user *)args);
		si_serial_dbg(dev, "SI_IOCTL_SERIAL_OUT_STATUS %d\n", ul);
	} break;

	case SI_IOCTL_GET_SERIAL:
		si_get_serial_params(dev, &serial_param);
		if (copy_to_user((struct SI_SERIAL_PARAM __user *)args,
				 &serial_param, sizeof(struct SI_SERIAL_PARAM)))
			ret = -EFAULT;
		break;
	case SI_IOCTL_SET_SERIAL:

		if (copy_from_user(&serial_param,
				   (struct SI_SERIAL_PARAM *)args,
				   sizeof(struct SI_SERIAL_PARAM)))
			return -EFAULT;

		si_serial_dbg(dev, "SI_IOCTL_SERIAL_PARAMS, baud %d\n",
				serial_param.baud);

		si_set_serial_params(dev, &serial_param);
		ret = 0;

		break;
	case SI_IOCTL_SERIAL_CLEAR:
		si_uart_clear(dev);
		ret = 0;
		break;

	case SI_IOCTL_SERIAL_BREAK: {
		int tim;

		if ((ret = get_user(tim, (int __user *)args)) < 0)
			break;

		if (tim < 0 || tim > 1000)
			tim = 1000; // limit to 1 second

		si_serial_dbg(dev, "SI_IOCTL_SERIAL_BREAK %d\n", tim);

		si_uart_break(dev, tim);
	} break;

	// DMA related entries
	case SI_IOCTL_DMA_INIT:
		si_dbg(dev, "SI_IOCTL_DMA_INIT\n");

		if (copy_from_user(&dev->dma_cfg, (struct SI_DMA_CONFIG *)args,
				   sizeof(struct SI_DMA_CONFIG))) {
			ret = -EFAULT;
			break;
		}
		ret = si_config_dma(dev);

		break;

	case SI_IOCTL_DMA_START:
		si_dbg(dev, "SI_IOCTL_DMA_START\n");

		if ((ret = si_start_dma(dev)) < 0)
			break;

		if ((ret = si_dma_status(dev, &dma_status)) < 0)
			break;

		if (args &&
		    copy_to_user((struct SI_DMA_STATUS *)args, &dma_status,
				 sizeof(struct SI_DMA_STATUS)))
			ret = -EFAULT;
		break;

	case SI_IOCTL_DMA_STATUS:
		si_dbg(dev, "SI_IOCTL_DMA_STATUS\n");
		if ((ret = si_dma_status(dev, &dma_status)) < 0)
			break;

		if (copy_to_user((struct SI_DMA_STATUS *)args, &dma_status,
				 sizeof(struct SI_DMA_STATUS)))
			ret = -EFAULT;
		break;

	case SI_IOCTL_DMA_NEXT:
		si_dbg(dev, "SI_IOCTL_DMA_NEXT\n");

		ret = si_dma_next(dev, &dma_status);
		if (copy_to_user((struct SI_DMA_STATUS *)args, &dma_status,
				 sizeof(struct SI_DMA_STATUS)))
			ret = -EFAULT;
		break;

	case SI_IOCTL_DMA_ABORT:
		si_dbg(dev, "SI_IOCTL_DMA_ABORT\n");

		if ((ret = si_stop_dma(dev, &dma_status)) < 0)
			break;

		if (args &&
		    copy_to_user((struct SI_DMA_STATUS *)args, &dma_status,
				 sizeof(struct SI_DMA_STATUS)))
			ret = -EFAULT;
		break;
	case SI_IOCTL_VERBOSE:
		ret = get_user(dev->verbose, (int __user *)args);
		break;

	case SI_IOCTL_SETPOLL:
		ret = get_user(dev->setpoll, (int __user *)args);
		if (dev->setpoll == SI_SETPOLL_UART)
			si_dbg(dev, "setpoll set to uart\n");
		else
			si_dbg(dev, "setpoll set to dma\n");
		break;

	case SI_IOCTL_FREEMEM:
		si_dbg(dev, "freemem\n");

		if ((ret = si_wait_vmaclose(dev))) { /* make sure munmap before free */
			si_info(dev, "freemem timeout waiting for munmap\n");
			return ret;
		}

		if (dev->sgl) {
			si_stop_dma(dev, NULL);
			si_free_sgl(dev);
		} else {
			si_dbg(dev, "freemem no data allocated\n");
		}
		break;

	default:
		si_info(dev, "Unsupported SI_IOCTL_Xxx (%02d)\n", _IOC_NR(cmd));
		ret = -EINVAL;
		break;
	}

	// si_dbg(dev, "completed ioctl %d\n", ret );

	return ret;
}

int si_reset(struct SIDEVICE *dev)
{
	si_dbg(dev, "master local reset\n");
	/* do the master reset local bus */
	LOCAL_REG_WRITE(dev, LOCAL_COMMAND, 0);
	return 0;
}
