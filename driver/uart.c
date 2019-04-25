// SPDX-License-Identifier: GPL-2.0-or-later
/*

Linux Driver for the
Spectral Instruments 3097 Camera Interface

Copyright (C) 2006  Jeffrey R Hagen
*/

#include <linux/version.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/poll.h>
#include <linux/pci.h>

#include "si3097.h"
#include "si3097_module.h"

void si_get_serial_params(struct SIDEVICE *dev, struct SI_SERIAL_PARAM *sp)
{
	sp->timeout = dev->Uart.timeout;
	sp->flags = dev->Uart.block;
	sp->baud = dev->Uart.baud;
	sp->bits = dev->Uart.bits;
	sp->parity = dev->Uart.parity;
	sp->stopbits = dev->Uart.stopbits;
	sp->fifotrigger = dev->Uart.fifotrigger;
	sp->buffersize = dev->Uart.serialbufsize;
}

void si_uart_clear(struct SIDEVICE *dev)
{
	unsigned long flags;
	int clr;

	spin_lock_irqsave(&dev->uart_lock, flags);
	dev->Uart.rxput = 0;
	dev->Uart.rxget = 0;
	clr = dev->Uart.rxcnt;
	dev->Uart.rxcnt = 0;
	dev->Uart.txput = 0;
	dev->Uart.txget = 0;
	dev->Uart.txcnt = dev->Uart.serialbufsize;
	spin_unlock_irqrestore(&dev->uart_lock, flags);

	/* warn if clearing data */
	if (clr > 0)
		si_info(dev, "UART clear rxcnt %d\n", clr);
	else
		si_serial_dbg(dev, "UART clear\n");
}

/* setup the serial parameters for the UART control of the SI card */

int si_set_serial_params(struct SIDEVICE *dev, struct SI_SERIAL_PARAM *sp)
{
	__u8 c, reg;
	__u8 *cp;
	int x;
	unsigned long flags;

	si_serial_dbg(dev, "serialparamsSCC\n");
	si_serial_dbg(dev, "baud rate %d\n", sp->baud);
	si_serial_dbg(dev, "timeout %d\n", sp->timeout);
	si_serial_dbg(dev, "number of bits %d\n", sp->bits);
	si_serial_dbg(dev, "parity %d\n", sp->parity);
	si_serial_dbg(dev, "stop bits %d\n", sp->stopbits);
	si_serial_dbg(dev, "set fifo trigger %d\n", sp->fifotrigger);
	spin_lock_irqsave(&dev->uart_lock, flags);

	dev->Uart.timeout = sp->timeout;
	dev->Uart.block = sp->flags & SI_SERIAL_FLAGS_BLOCK;
	dev->Uart.baud = sp->baud;

	x = 1000000 / dev->Uart.baud; // 16 MHz oscillator
	if (sp->baud == 57600) // make it go really fast
		x = 4;

	c = UART_REG_READ(dev, SERIAL_LCR);
	UART_REG_WRITE(dev, SERIAL_LCR, (__u8)(c | 0x80));
	UART_REG_WRITE(dev, SERIAL_DLL, (__u8)(x & 0xff));
	UART_REG_WRITE(dev, SERIAL_DLH, (__u8)((x >> 8) & 0xff));
	UART_REG_WRITE(dev, SERIAL_LCR, c);

	// make sure the loop register is NOT set
	UART_REG_WRITE(dev, SERIAL_MCR, 0);

	dev->Uart.bits = sp->bits;
	x = (sp->bits - 1) & 3;
	c = UART_REG_READ(dev, SERIAL_LCR) & ~3;
	UART_REG_WRITE(dev, SERIAL_LCR, (__u8)(c | x));

	dev->Uart.parity = sp->parity;
	if (sp->parity == 'E' || sp->parity == 'e')
		x = 0x18;
	else if (sp->parity == 'O' || sp->parity == 'o')
		x = 0x08;
	else if (sp->parity == 'M' || sp->parity == 'm')
		x = 0x28;
	else if (sp->parity == 'S' || sp->parity == 's')
		x = 0x38;
	else
		x = 0;

	c = UART_REG_READ(dev, SERIAL_LCR) & ~0x38;
	UART_REG_WRITE(dev, SERIAL_LCR, (__u8)(c | x));

	dev->Uart.stopbits = sp->stopbits;
	x = (sp->stopbits == 2) ? 4 : 0;
	c = UART_REG_READ(dev, SERIAL_LCR) & ~4;
	UART_REG_WRITE(dev, SERIAL_LCR, (__u8)(c | x));

	switch (sp->fifotrigger) {
	case 1:
	case 4:
	case 8:
	case 14:
		x = sp->fifotrigger;
		break;
	default:
		x = 0;
		break;
	}
	dev->Uart.fifotrigger = x;
	x &= 0x0c;
	x <<= 4;
	UART_REG_WRITE(dev, SERIAL_FCR, (__u8)(x | 7));

	reg = UART_REG_READ(dev, SERIAL_FCR);
	si_serial_dbg(dev, "UART SERIAL_FCR 0x%x\n", reg);

	UART_REG_WRITE(dev, SERIAL_IER, 0); /* disable all serial ints */
	cp = dev->Uart.rxbuf; /* save old pointer just in case */

	/* allocate one large buffer and split it in half.
     Allocatate new before deallocating old one.
   */

	si_serial_dbg(dev, "sp->buffersize %d\n", (int)sp->buffersize);
	if (sp->buffersize <= 0)
		sp->buffersize = 8192;

	if (sp->buffersize % 8192) {
		sp->buffersize += 8192 - (sp->buffersize % 8192);
	}

	spin_unlock_irqrestore(&dev->uart_lock, flags);
	dev->Uart.rxbuf = (char *)kmalloc(sp->buffersize * 2, GFP_KERNEL);
	spin_lock_irqsave(&dev->uart_lock, flags);

	if (dev->Uart.rxbuf == 0) {
		dev->Uart.rxbuf = cp;
		sp->buffersize = dev->Uart.serialbufsize;
	} else {
		if (cp) /* txbuf is part of rxbuf */
			kfree(cp);
	}
	dev->Uart.serialbufsize = sp->buffersize;
	dev->Uart.txbuf = dev->Uart.rxbuf + dev->Uart.serialbufsize;

	x = dev->Uart.fifotrigger;
	if (x) {
		x &= 0x0c;
		x <<= 4;
		UART_REG_WRITE(dev, SERIAL_FCR,
			       (__u8)(x | 7)); /* enable rx and tx fifos */
	} else
		UART_REG_WRITE(dev, SERIAL_FCR,
			       0); /* disable rx and tx fifos */

	UART_REG_READ(dev, SERIAL_IIR);
	UART_REG_READ(dev, SERIAL_RX);
	reg = UART_REG_READ(dev, SERIAL_LSR);
	si_serial_dbg(dev, "UART SERIAL_LSR 0x%x\n", reg);
	UART_REG_READ(dev, SERIAL_MSR);
	UART_REG_WRITE(dev, SERIAL_IER, RX_INT | TX_INT); /* tx ints enabled */

	spin_unlock_irqrestore(&dev->uart_lock, flags);

	reg = UART_REG_READ(dev, SERIAL_IER);
	si_serial_dbg(dev, "UART SERIAL_IER 0x%x\n", reg);

	reg = UART_REG_READ(dev, SERIAL_LCR);
	si_serial_dbg(dev, "UART SERIAL_LCR 0x%x\n", reg);

	reg = UART_REG_READ(dev, SERIAL_FCR);
	si_serial_dbg(dev, "UART SERIAL_FCR 0x%x\n", reg);

	si_uart_clear(dev);
	return TRUE;
}

/* inititalize UART for comm */

int si_init_uart(struct SIDEVICE *dev)
{
	struct SI_SERIAL_PARAM param;

	/* default serial paramters  */
	param.baud = 57600;
	param.bits = 8;
	param.timeout = 1000;
	param.flags = SI_SERIAL_FLAGS_BLOCK;
	param.parity = 'N';
	param.stopbits = 1;
	param.fifotrigger = 8;
	param.buffersize = PAGE_SIZE * 2; /* 16384 */

	si_set_serial_params(dev, &param);

	si_serial_dbg(dev, "exit initUART\n");

	return TRUE;
}

/*  close out UART comm */

void si_cleanup_serial(struct SIDEVICE *dev)
{
	unsigned long flags;
	__u32 reg;

	if (dev->test) {
		si_info(dev, "cleanup_serial TEST\n");
		return;
	}

	spin_lock_irqsave(&dev->uart_lock, flags);
	UART_REG_WRITE(dev, SERIAL_FCR, 0); // disable rx and tx fifos
	UART_REG_WRITE(dev, SERIAL_IER, 0); // disable all ints
	spin_unlock_irqrestore(&dev->uart_lock, flags);
	//  txbuf is part of rxbuf
	if (dev->Uart.rxbuf) {
		kfree(dev->Uart.rxbuf);
		dev->Uart.rxbuf = NULL;
	}

	/* turn off interrupts */

	reg = PLX_REG_READ(dev, PCI9054_INT_CTRL_STAT);
	PLX_REG_WRITE(dev, PCI9054_INT_CTRL_STAT, reg & ~((1 << 11)));
}

/* send one character over the serial bus
   if ready to send, send it, if not, queue
   and let the isr handle it
 */

int si_transmit_serial(struct SIDEVICE *dev, __u8 data)
{
	int ret = FALSE;
	unsigned long flags;
	__u8 reg;

	if (dev->test) {
		si_info(dev, "transmit_serial TEST\n");
		return 0;
	}

	spin_lock_irqsave(&dev->uart_lock, flags);
	//  print_UART_stat(dev);
	reg = 0;
	if (dev->Uart.txcnt == dev->Uart.serialbufsize) {
		reg = UART_REG_READ(dev, SERIAL_LSR);
		if ((reg & 0x20)) {
			UART_REG_WRITE(dev, SERIAL_TX, data);
			ret = TRUE;
		}
	}
	if (!ret) {
		if (dev->Uart.txcnt > 1) { // room in queue?
			dev->Uart.txbuf[dev->Uart.txput++] = data;
			if (dev->Uart.txput == dev->Uart.serialbufsize)
				dev->Uart.txput = 0;
			dev->Uart.txcnt--;
			ret = TRUE;
		}
	}
	spin_unlock_irqrestore(&dev->uart_lock, flags);

	// si_dbg(dev,
	//	"transmit_serial 0x%x txcnt %d size %d reg 0x%x ret %d\n",
	//	data, dev->Uart.txcnt, dev->Uart.serialbufsize, reg, ret);

	return ret;
}

/* read one character from UART

   Remove 1 character from the receive queue, copy
   it into pdata, and return TRUE. If the queue
   is empty, return FALSE.
*/

int si_receive_serial(struct SIDEVICE *dev, __u8 *pdata)
{
	int ret;
	unsigned long flags;
	__u8 reg;

	if (dev->test) {
		si_info(dev, "receive_serial TEST\n");
		return 0;
	}

	spin_lock_irqsave(&dev->uart_lock, flags);
	reg = UART_REG_READ(dev, SERIAL_LSR);

	if (dev->Uart.rxput != dev->Uart.rxget) {
		*pdata = dev->Uart.rxbuf[dev->Uart.rxget++];
		if (dev->Uart.rxget == dev->Uart.serialbufsize)
			dev->Uart.rxget = 0;
		dev->Uart.rxcnt--;
		ret = TRUE;
	} else {
		*pdata = 0;
		ret = FALSE;
	}

	spin_unlock_irqrestore(&dev->uart_lock, flags);

	if (ret)
		; // si_serial_dbg(dev, "receive_serial char 0x%x ret %d\n",
		  //	*pdata, ret);
	else
		si_serial_dbg(dev, "receive_serial no data, reg 0x%x\n", reg);

	return ret;
}

int si_print_uart_stat(struct SIDEVICE *dev)
{
	si_info(dev, "UART status\n");
	si_info(dev, "serialbufsize: %d\n", dev->Uart.serialbufsize);
	si_info(dev, "rxput:         %d\n", dev->Uart.rxput);
	si_info(dev, "rxget:         %d\n", dev->Uart.rxget);
	si_info(dev, "rxcnt:         %d\n", dev->Uart.rxcnt);
	si_info(dev, "txput:         %d\n", dev->Uart.txput);
	si_info(dev, "txget:         %d\n", dev->Uart.txget);
	si_info(dev, "txcnt:         %d\n", dev->Uart.txcnt);
	si_info(dev, "baud:          %d\n", dev->Uart.baud);
	si_info(dev, "bits:          %d\n", dev->Uart.bits);
	si_info(dev, "parity:        %d\n", dev->Uart.parity);
	si_info(dev, "stopbits:      %d\n", dev->Uart.stopbits);
	si_info(dev, "fifotrigger:   %d\n", dev->Uart.fifotrigger);

	return 0;
}

/* send a break to the UART, busy wait delay */

int si_uart_break(struct SIDEVICE *dev, int break_time)
{
	unsigned char uc;
	unsigned long flags;

	if (dev->test) {
		si_info(dev, "uart_break TEST\n");
		return 0;
	}

	spin_lock_irqsave(&dev->uart_lock, flags);
	uc = UART_REG_READ(dev, SERIAL_LCR); // assert break (signal low)
	UART_REG_WRITE(dev, SERIAL_LCR, (__u8)(uc | 0x40));

	mdelay(break_time); // wait break time

	uc = UART_REG_READ(dev, SERIAL_LCR); // de-assert break (signal high)
	UART_REG_WRITE(dev, SERIAL_LCR, (__u8)(uc & ~0x40));
	spin_unlock_irqrestore(&dev->uart_lock, flags);

	return 0;
}
