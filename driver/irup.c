// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Linux Driver for the
 * Spectral Instruments 3097 Camera Interface
 *
 * Copyright (C) 2006  Jeffrey R Hagen
 */

/* This is the interrupt code for the si3097 */

#include <linux/version.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/poll.h>
#include <linux/pci.h>

#include "si3097.h"
#include "si3097_module.h"

/* The Interrupt Service Routine for the PLX chip on the SI camera controller
 */
irqreturn_t si_interrupt(int irq, struct SIDEVICE *dev)
{
	__u32 reg;
	__u32 ctrl_stat;
	__u32 source;

	// Read interrupt status register
	ctrl_stat = PLX_REG_READ(dev, PCI9054_INT_CTRL_STAT);
	si_dbg(dev, "interrupt ctrl_stat 0x%x\n", ctrl_stat);

	/*
	 * If the chip is in a low power state, then local
	 * register reads are disabled and will always return
	 * 0xFFFFFFFF.  If the PLX chip's interrupt is shared
	 * with another PCI device, the PXL ISR may continue
	 * to be called.  This case is handled to avoid
	 * erroneous reporting of an active interrupt.
	 */

	if (ctrl_stat == 0xFFFFFFFF)
		return IRQ_HANDLED;

	// Check for master PCI interrupt enable
	if ((ctrl_stat & (1 << 8)) == 0)
		return IRQ_HANDLED;

	// Verify that an interrupt is truly active

	// Clear the interrupt type flag
	source = INTR_TYPE_NONE;

	// Check if PCI Doorbell Interrupt is active and not masked
	if ((ctrl_stat & (1 << 13)) && (ctrl_stat & (1 << 9)))
		source |= INTR_TYPE_DOORBELL;

	// Check if PCI Abort Interrupt is active and not masked
	if ((ctrl_stat & (1 << 14)) && (ctrl_stat & (1 << 10)))
		source |= INTR_TYPE_PCI_ABORT;

	// Check if Local->PCI Interrupt is active and not masked
	if ((ctrl_stat & (1 << 15)) && (ctrl_stat & (1 << 11))) {
		source |= INTR_TYPE_LOCAL_1;
		//      si_info(dev, "local interrupt\n");
	}

	// Check if DMA Channel 0 Interrupt is active and not masked
	if ((ctrl_stat & (1 << 21)) && (ctrl_stat & (1 << 18))) {
		// Verify the DMA interrupt is routed to PCI
		reg = dev->irup_reg = PLX_REG_READ(dev, PCI9054_DMA0_MODE);
		//      si_info(dev, "DMA0 interrupt, mode 0x%x\n", reg);

		if (reg & (1 << 17))
			source |= INTR_TYPE_DMA_0;
	}

	// Check if DMA Channel 1 Interrupt is active and not masked
	if ((ctrl_stat & (1 << 22)) && (ctrl_stat & (1 << 19))) {
		// Verify the DMA interrupt is routed to PCI
		reg = PLX_REG_READ(dev, PCI9054_DMA1_MODE);

		if (reg & (1 << 17))
			source |= INTR_TYPE_DMA_1;
	}

	// Return if no interrupts are active
	if (source == INTR_TYPE_NONE)
		return IRQ_HANDLED;

	// Mask the PCI Interrupt reenabled in bottom half
	PLX_REG_WRITE(dev, PCI9054_INT_CTRL_STAT, ctrl_stat & ~(1 << 8));

	// Schedule bottom half to complete interrupt processing
	// Reset task structure

	//PREPARE_WORK( &dev->task, si_bottom_half );

	dev->source = source; // pass to bottom half

	// Add task to queue the processing of the irq
	queue_work(dev->bottom_half_wq, &dev->task);

	return IRQ_HANDLED;
}

void transmit_fifo_empty(struct SIDEVICE *dev)
{
	int i;

	if (dev->Uart.txcnt < dev->Uart.serialbufsize) { // i is fifo ctr
		if (dev->Uart.fifotrigger) { // using the FIFO?
			i = dev->Uart.serialbufsize - dev->Uart.txcnt;
			if (i > 16)
				i = 16;
		} else
			i = 1; // just get 1 at a time
		while (i--) { // fill fifo as much as possible
			UART_REG_WRITE(dev, SERIAL_TX,
					dev->Uart.txbuf[dev->Uart.txget++]);
			if (dev->Uart.txget == dev->Uart.serialbufsize)
				dev->Uart.txget = 0;
			dev->Uart.txcnt++;
		}
		if (dev->Uart.txget == dev->Uart.serialbufsize) /* empty */
			if (waitqueue_active(&dev->uart_wblock))
				wake_up_interruptible(&dev->uart_wblock);
	} else {
		; // nothing to send - no action needed
	}
}

void receive_fifo_timeout(struct SIDEVICE *dev)
{
	__u8 c;

	do {
		c = UART_REG_READ(dev, SERIAL_RX);
		dev->Uart.rxbuf[dev->Uart.rxput++] = c;
		dev->Uart.rxcnt++;
		si_serial_dbg(dev, "receive 0x%x rxcnt %d\n",
			       c, dev->Uart.rxcnt);
		if (dev->Uart.rxput == dev->Uart.serialbufsize)
			dev->Uart.rxput = 0;
		// Don't let the receive buffer overrun
		//  itself - newest byte is tossed
		if (dev->Uart.rxput == dev->Uart.rxget) {
			dev->Uart.rxput--;
			dev->Uart.rxcnt--;
			if (dev->Uart.rxput == -1)
				dev->Uart.rxput = dev->Uart.serialbufsize - 1;
		}
	} while (UART_REG_READ(dev, SERIAL_LSR) & 1); // empty the fifo
	if (waitqueue_active(&dev->uart_rblock))
		wake_up_interruptible(&dev->uart_rblock);
}


/* This routine is scheduled by the ISR to efficiently serivce the
 * interrupt
 */
void si_bottom_half(struct work_struct *work)
{
	__u32 int_stat;
	__u32 reg;
	__u32 source;
	__u8 iir, lsr, msr;
	struct SIDEVICE *dev;
	int done;
	unsigned long flags;

	dev = container_of(work, struct SIDEVICE, task);
	int_stat = PLX_REG_READ(dev, PCI9054_INT_CTRL_STAT);

	// Copy interrupt source
	source = dev->source;
	//  si_info(dev, "bottom half source %d\n", source);

	// Local Interrupt 1
	if (source & INTR_TYPE_LOCAL_1) {
		// Synchronize access to Interrupt Control/Status Register
		spin_lock_irqsave(&dev->uart_lock, flags);

		// it's a local interrupt - so it must be from the UART
		//
		while (!((iir = UART_REG_READ(dev, SERIAL_IIR)) &
			 1)) { // more ints?
			// si_info(dev, "irup src %d uart reg 0x%x\n",
			//	source, iir & 0xe);

			switch (iir & 0xe) {
			case 0x6: // receiver line status interrupt
				// clear int, do nothing
				lsr = UART_REG_READ(dev, SERIAL_LSR);
				break;

			case 0x4: // receive fifo trigger level reached
			case 0xc: // receive fifo timeout
				receive_fifo_timeout(dev);
				break;

			case 0x2: // transmitter (fifo) empty
				transmit_fifo_empty(dev);
				break;

			case 0x0: // modem status interrupt
				// clear int, do nothing
				msr = UART_REG_READ(dev, SERIAL_MSR);
				break;
			}
		}
		spin_unlock_irqrestore(&dev->uart_lock, flags);
	}

	// Doorbell Interrupt
	if (source & INTR_TYPE_DOORBELL) { // Get Doorbell register
		reg = PLX_REG_READ(dev, PCI9054_PCI_DOORBELL);

		// Clear Doorbell interrupt
		PLX_REG_WRITE(dev, PCI9054_PCI_DOORBELL, reg);

		// Save this value in case it is requested later
		// dev->doorbell = reg;
	}

	// PCI Abort interrupt
	if (source & INTR_TYPE_PCI_ABORT) {
		// Get the PCI Command register
		pci_read_config_dword(dev->pci, PCI9054_COMMAND, &reg);

		// Write to back to clear PCI Abort
		pci_write_config_dword(dev->pci, PCI9054_COMMAND, reg);
	}

	// DMA Channel 0 interrupt
	if (source & INTR_TYPE_DMA_0) { // Get DMA Control/Status
		spin_lock_irqsave(&dev->dma_lock, flags);
		reg = PLX_REG8_READ(dev, PCI9054_DMA_COMMAND_STAT);

		done = ((reg & SI_DMA_STATUS_DONE) != 0);
		atomic_set(&dev->dma_done, reg);
		if (done) {
			__u32 rb_count;
			/* Clear DMA interrupt and disable if done */
			PLX_REG8_WRITE(dev, PCI9054_DMA_COMMAND_STAT, (1 << 3));
			/* careful not to read local bus during DMA */
			LOCAL_REG_WRITE(dev, LOCAL_COMMAND, LC_FIFO_MRS_L);
			rb_count = LOCAL_REG_READ(dev, LOCAL_PIX_CNT_LL) & 0xff;
			rb_count +=
				(LOCAL_REG_READ(dev, LOCAL_PIX_CNT_ML) & 0xff)
				<< 8;
			rb_count +=
				(LOCAL_REG_READ(dev, LOCAL_PIX_CNT_MH) & 0xff)
				<< 16;
			rb_count +=
				(LOCAL_REG_READ(dev, LOCAL_PIX_CNT_HH) & 0xff)
				<< 24;
			if (!dev->abort_active && rb_count)
				si_info(dev,
					"bh DMA0 irup, rb_count not zero %d\n",
					rb_count);
			dev->rb_count = rb_count;

		} else {
			PLX_REG8_WRITE(dev, PCI9054_DMA_COMMAND_STAT,
				       (1 << 3) | (1 << 0));
		}

		si_dbg(dev,
			"bh DMA0 irup, int_stat 0x%x mode 0x%x dma_stat 0x%x\n",
			int_stat, dev->irup_reg, reg);

		dev->dma_cur++;

		if (dev->dma_cfg.config & SI_DMA_CONFIG_WAKEUP_EACH) {
			if (waitqueue_active(&dev->dma_block)) {
				si_dbg(dev, "irup wakeup on each\n");
				wake_up_interruptible(&dev->dma_block);
			}
		} else {
			if (done && waitqueue_active(&dev->dma_block)) {
				si_dbg(dev, "irup wakeup on done\n");
				wake_up_interruptible(&dev->dma_block);
			}
		}
		spin_unlock_irqrestore(&dev->dma_lock, flags);
	}

	// DMA Channel 1 interrupt
	if (source & INTR_TYPE_DMA_1) {
		// Get DMA Control/Status
		reg = PLX_REG8_READ(dev, PCI9054_DMA_COMMAND_STAT);

		// Clear DMA interrupt
		PLX_REG8_WRITE(dev, PCI9054_DMA_COMMAND_STAT, reg | (1 << 11));
		reg = PLX_REG_READ(dev, PCI9054_DMA1_MODE);
		//    si_info(dev, "DMA1 interrupt, mode 0x%x\n", reg);
	}

	// Outbound post FIFO interrupt
	if (source & INTR_TYPE_OUTBOUND_POST) {
		// Mask Outbound Post interrupt
		PLX_REG_WRITE(dev, PCI9054_OUTPOST_INT_MASK, (1 << 3));
	}
	dev->source = 0;
	PLX_REG_WRITE(dev, PCI9054_INT_CTRL_STAT, int_stat | (1 << 8));
}
