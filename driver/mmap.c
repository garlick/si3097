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

/* mmap
   allocate the driver buffers and
   map it to the application
*/

#include <linux/version.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/poll.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <asm/atomic.h>

#include "si3097.h"
#include "si3097_module.h"

void *jeff_alloc(int size, dma_addr_t *pphy);

/* use the vma mechanism for mapping data */

void si_vmaopen(struct vm_area_struct *area)
{
	struct SIDEVICE *dev;

	dev = (struct SIDEVICE *)area->vm_file->private_data;
	si_dbg(dev, "vmaopen vmact %d\n", atomic_read(&dev->vmact));

	atomic_inc(&dev->vmact);
}

void si_vmaclose(struct vm_area_struct *area)
{
	struct SIDEVICE *dev;

	dev = (struct SIDEVICE *)area->vm_file->private_data;

	si_dbg(dev, "vmaclose vmact %d\n", atomic_read(&dev->vmact));

	if (atomic_dec_and_test(&dev->vmact))
		; //wake_up_interruptible( &dev->mmap_block );
}

/* when the application faults this routine is called to map the data */

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
int si_vmafault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
#else
int si_vmafault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
#endif
	unsigned int loff, off;
	int nbuf;
	void *vaddr;
	struct SIDEVICE *dev;
	//  struct page *pg;
	unsigned long flags;

	dev = (struct SIDEVICE *)vma->vm_file->private_data;

	if (!dev) {
		pr_err("SI fault failed, dev NULL\n");
		return VM_FAULT_SIGBUS;
	}

	if (!dev->sgl) {
		si_err(dev, "fault, sgl NULL\n");
		return VM_FAULT_SIGBUS;
	}

	spin_lock_irqsave(&dev->nopage_lock, flags);

	//  off = vmf->virtual_address - vma->vm_start; /* vma->vm_offset byte offset, must be fixed */
	off = (vmf->pgoff << PAGE_SHIFT);

	nbuf = off / dev->alloc_sm_buflen;
	loff = off % dev->alloc_sm_buflen;
	if (nbuf >= dev->dma_nbuf) {
		si_err(dev,
		    "fault, requested more mmap than data: nbuf %d max %d\n",
		    nbuf, dev->dma_nbuf);
		spin_unlock_irqrestore(&dev->nopage_lock, flags);
		return VM_FAULT_SIGBUS;
	}

	vaddr = ((unsigned char *)dev->sgl[nbuf].cpu) + loff;

	vmf->page = virt_to_page(vaddr);
	get_page(vmf->page);
	spin_unlock_irqrestore(&dev->nopage_lock, flags);

	return 0;
}

static struct vm_operations_struct si_vm_ops = { .open = si_vmaopen,
						 .close = si_vmaclose,
						 .fault = si_vmafault };

int si_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct SIDEVICE *dev;

	dev = (struct SIDEVICE *)filp->private_data;

	si_dbg(dev, "mmap vmact %d ptr 0x%lx\n", atomic_read(&dev->vmact),
		       (unsigned long)vma->vm_file);

	vma->vm_ops = &si_vm_ops;
	vma->vm_file = filp;
	vma->vm_flags |= (VM_DONTEXPAND | VM_DONTDUMP); /* Don't swap */

	si_vmaopen(vma);
	return 0;
}

/* setup dma channel */

/* alloc all the memory for a dma transfer
   dev->sgl holds an array of scatted gather tables
   which holds the pointers to the dma.
   An additional element, cpu is added to hold
   the kernel side virt address of the hardware address
*/

int si_config_dma(struct SIDEVICE *dev)
{
	int nchains, nb, nbytes;
	unsigned int end_mask;
	struct SIDMA_SGL *ch;
	dma_addr_t ch_dma, last; /* pci address */
	__u32 local_addr, cmd_stat;
	int buflen, nbuf, sm_buflen, isalloc;
	unsigned char setb;
	unsigned long flags;

	/* stop the dma if its running */

	cmd_stat = PLX_REG8_READ(dev, PCI9054_DMA_COMMAND_STAT);
	if (cmd_stat & 1)
		si_stop_dma(dev, NULL);

	si_dbg(dev, "si_config_dma alloc_maxever %d alloc_buflen %d\n",
		       dev->alloc_maxever, dev->alloc_buflen);

	if (dev->alloc_maxever == 0) { /* allocate the memory */
		if (dev->dma_cfg.total <= 0) {
			si_info(dev, "config.total %d\n", dev->dma_cfg.total);
			return -EIO;
		}
		if (dev->dma_cfg.maxever <= 0)
			dev->dma_cfg.maxever = dev->dma_cfg.total;

		dev->alloc_maxever = dev->dma_cfg.maxever;
		dev->alloc_buflen = dev->dma_cfg.buflen;
		si_alloc_memory(dev);
		isalloc = 1;
	} else {
		isalloc = 0;
	}
	si_dbg(dev, "si_config_dma2 alloc_maxever %d alloc_buflen %d\n",
		       dev->alloc_maxever, dev->alloc_buflen);

	if (dev->dma_cfg.maxever > dev->alloc_maxever) {
		si_info(dev,
		    "config need to freemem, maxever: asked for %d have %d\n",
		    dev->dma_cfg.maxever, dev->alloc_maxever);
		return -EIO;
	}

	if (dev->dma_cfg.buflen != dev->alloc_buflen) {
		si_info(dev,
		    "config need to freemem, buflen: asked for %d have %d\n",
		    dev->dma_cfg.buflen, dev->alloc_buflen);
		return -EIO;
	}

	/* sm_buflen is size of the mmapped buffer */
	/* buflen is the size of the dma buffer (likely the same) */
	/* nbytes is the number of bytes to xfer in last buffer */

	nbuf = dev->dma_cfg.total / dev->dma_cfg.buflen;
	nbytes = dev->dma_cfg.total % dev->dma_cfg.buflen;
	if (nbytes)
		nbuf++;

	dev->dma_nbuf = nbuf;
	buflen = dev->dma_cfg.buflen;

	if (nbytes == 0)
		nbytes = buflen;

	dev->dma_nbuf = nbuf;

	if ((buflen % PAGE_SIZE) == 0)
		sm_buflen = buflen;
	else
		sm_buflen = buflen + PAGE_SIZE - (buflen % PAGE_SIZE);

	dev->alloc_sm_buflen = sm_buflen;

	/* check limits */
	if (nbuf < 1 || buflen < PAGE_SIZE) {
		si_info(dev, "si_dma_init nbuf %d buflen %d\n",
			nbuf, buflen);
		return -EIO;
	}

	if ((unsigned int)(nbuf * buflen) > 0x7fffffff) {
		si_info(dev, "si_dma_init too big nbuf %d buflen %d\n",
			nbuf, buflen);
		return -EIO;
	}

	if (buflen != sm_buflen) {
		si_info(dev,
		"WARNING buflen %d sm_buflen %d, not a multiple of page size\n",
		buflen, sm_buflen);
	}

	spin_lock_irqsave(&dev->dma_lock, flags);

	if (dev->dma_cfg.config & SI_DMA_CONFIG_WAKEUP_EACH)
		end_mask = SIDMA_DPR_PCI_SRC | SIDMA_DPR_IRUP | SIDMA_DPR_TOPCI;
	else
		end_mask = SIDMA_DPR_PCI_SRC | SIDMA_DPR_TOPCI;

	last = 0;
	si_dbg(dev, "buflen %d dma_nbuf %d\n", buflen, dev->dma_nbuf);

	local_addr = SI_LOCAL_BUSADDR;
	nchains = dev->dma_nbuf;
	for (nb = nchains - 1; nb >= 0; nb--) {
		ch = &dev->sgl[nb]; /* kernel virt address of this SGL */
		ch_dma = dev->sgl_pci +
			 sizeof(struct SIDMA_SGL) * nb; /*bus side address*/
		ch->siz = nbytes;
		nbytes = buflen;
		ch->dpr = last | end_mask;

		last = (dma_addr_t)ch_dma & 0xfffffff0;
		setb = ((nb + 1) & 0x7f); /* set mem to see mmap working */
		memset(ch->cpu, setb, sm_buflen);
	}
	/* always wake up at the end */
	end_mask = SIDMA_DPR_PCI_SRC | SIDMA_DPR_IRUP | SIDMA_DPR_TOPCI |
		   SIDMA_DPR_EOC;
	dev->sgl[dev->dma_nbuf - 1].dpr =
		last | end_mask; /* point last at first */
	spin_unlock_irqrestore(&dev->dma_lock, flags);

	si_dbg(dev,
	"si_config_dma sgl 0x%lx sgl_pci 0x%x buflen %d sm_buflen %d nbuf %d\n",
	(unsigned long)dev->sgl, (unsigned int)dev->sgl_pci,
	buflen, sm_buflen, nbuf);
	if (isalloc)
		si_print_memtable(dev);

	return 0;
}

/* allocate memory */

int si_alloc_memory(struct SIDEVICE *dev)
{
	int nbuf, buflen, sm_buflen, nb;
	struct SIDMA_SGL *ch;
	void *cpu;
	struct page *page, *pend;
	dma_addr_t ch_dma, dma_buf;
	__u32 local_addr;

	unsigned char setb;

	if (dev->sgl) {
		si_info(dev, "alloc memory already allocated\n");
		return -EIO;
	}

	nbuf = dev->alloc_maxever / dev->alloc_buflen;
	if ((dev->alloc_maxever % dev->alloc_buflen))
		nbuf++;

	dev->alloc_nbuf = nbuf;
	buflen = dev->alloc_buflen;

	if ((dev->alloc_buflen % PAGE_SIZE) == 0)
		dev->alloc_sm_buflen = dev->alloc_buflen;
	else
		dev->alloc_sm_buflen = dev->alloc_buflen + PAGE_SIZE -
				       (dev->alloc_buflen % PAGE_SIZE);

	sm_buflen = dev->alloc_sm_buflen;
	local_addr = SI_LOCAL_BUSADDR;

	dev->sgl_len = nbuf * sizeof(struct SIDMA_SGL);
	dev->sgl = dma_alloc_coherent(&dev->pci->dev, dev->sgl_len,
				      &dev->sgl_pci, GFP_KERNEL);
	//  dev->sgl = jeff_alloc( dev->sgl_len, &dev->sgl_pci);

	//  spin_lock_irqsave( &dev->dma_lock, flags );

	if (!dev->sgl) {
		si_info(dev, "no memory allocating table\n");
		//    spin_unlock_irqrestore( &dev->dma_lock, flags );
		return -EIO;
	}
	memset(dev->sgl, 0,
	       dev->sgl_len); /* clear so cpu will be null if fail */

	dev->total_allocs++;
	dev->total_bytes += dev->sgl_len;

	for (nb = nbuf - 1; nb >= 0; nb--) {
		cpu = dma_alloc_coherent(&dev->pci->dev, sm_buflen, &dma_buf,
					 GFP_KERNEL);
		//cpu = jeff_alloc( sm_buflen, &dma_buf );
		if (!cpu) {
			si_info(dev, "no memory allocating buffer %d\n",
			       nbuf - nb);
			//      spin_unlock_irqrestore( &dev->dma_lock, flags );
			si_free_sgl(dev);
			return -EIO;
		}

		//   if I am already doing the pci_alloc_consistent, why do I need this
		pend = virt_to_page(cpu + buflen - 1);
		page = virt_to_page(cpu);
		while (page <= pend) {
			SetPageReserved(page);
			get_page(page); /* doit once too many so it sticks */
			page++;
		}

		ch = &dev->sgl[nb]; /* kernel virt address of this SGL */
		ch_dma = dev->sgl_pci +
			 sizeof(struct SIDMA_SGL) * nb; /*bus side address*/
		ch->padr = (__u32)dma_buf;
		ch->ladr = local_addr;
		ch->cpu = cpu;
		ch->siz = 0;
		ch->dpr = 0;
		dev->total_allocs++;
		dev->total_bytes += buflen;
		setb = ((nb + 1) & 0x7f); /* set mem to see mmap working */
		memset(cpu, setb, sm_buflen);
	}
	//  spin_unlock_irqrestore( &dev->dma_lock, flags );

	si_dbg(dev, "si_alloc_memory, %d allocates and %d bytes\n",
	       dev->total_allocs, dev->total_bytes);

	return 0;
}

void si_print_memtable(struct SIDEVICE *dev)
{
	int nb;
	struct SIDMA_SGL *ch;

	if (!dev->sgl)
		return;

	si_dbg(dev, "si_print_memtable nbuf %d\n", dev->dma_nbuf);

	si_dbg(dev,
	  "  ch          padr       ladr       siz        dpr        cpu\n");
	for (nb = 0; nb < dev->dma_nbuf; nb++) {
		ch = &dev->sgl[nb];
		si_dbg(dev, "0x%lx 0x%x 0x%x 0x%x 0x%x 0x%lx\n",
			       (unsigned long)dev->sgl +
				       sizeof(struct SIDMA_SGL) * nb,
			       ch->padr, ch->ladr, ch->siz, ch->dpr,
			       (unsigned long)ch->cpu);
		ch++;
	}
}

void si_free_sgl(struct SIDEVICE *dev)
{
	int n, nchains;
	struct SIDMA_SGL *ch, *dchain;
	int total_frees, total_bytes, sm_buflen;
	struct page *page, *pend;
	unsigned long flags;

	if (!dev->sgl)
		return;

	if (atomic_read(&dev->vmact) != 0) /* what about multiple opens */
		return;

	dchain = dev->sgl;

	spin_lock_irqsave(&dev->dma_lock, flags);
	dev->sgl = 0;
	spin_unlock_irqrestore(&dev->dma_lock, flags);

	total_frees = 0;
	total_bytes = 0;
	nchains = dev->alloc_nbuf;
	si_dbg(dev, "free_sgl nbuf %d\n", nchains);

	if ((dev->alloc_buflen % PAGE_SIZE) == 0)
		sm_buflen = dev->alloc_buflen;
	else
		sm_buflen = dev->alloc_buflen + PAGE_SIZE -
			    (dev->alloc_buflen % PAGE_SIZE);

	for (n = 0; n < nchains; n++) {
		ch = &dchain[n];
		if (!ch->cpu)
			break;

		/* does free_consist do this */

		pend = virt_to_page(ch->cpu + sm_buflen - 1);
		page = virt_to_page(ch->cpu);
		while (page <= pend) {
			ClearPageReserved(page);
			put_page_testzero(page);
			page++;
		}

		dma_free_coherent(&dev->pci->dev, sm_buflen, ch->cpu, ch->padr);
		total_frees++;
		total_bytes += dev->dma_cfg.buflen;
	}
	dma_free_coherent(&dev->pci->dev, dev->sgl_len, dchain, dev->sgl_pci);
	total_frees++;
	total_bytes += dev->sgl_len;
	dev->dma_cfg.buflen = 0;
	dev->sgl_pci = 0;
	si_dbg(dev, "free_sgl allocates freed %d, bytes %d\n",
	       total_frees, total_bytes);

	dev->total_allocs = 0;
	dev->total_bytes = 0;

	dev->alloc_nbuf = 0;
	dev->alloc_buflen = 0;
	dev->alloc_sm_buflen = 0;
	dev->alloc_maxever = 0;
}

/* start configured dma */

int si_start_dma(struct SIDEVICE *dev)
{
	int n_pixels;
	int rb_count;
	__u32 reg;
	unsigned long flags;

	n_pixels = dev->dma_cfg.total / 2;

	if (dev->test) {
		si_info(dev, "start_dma test mode\n");
		return 0;
	}

	reg = PLX_REG8_READ(dev, PCI9054_DMA_COMMAND_STAT);
	if (reg & 1) { /* already on stop */
		si_info(dev, "start_dma already on stopping first, dma_stat 0x%x\n",
		       reg);
		si_stop_dma(dev, NULL);
	}

	spin_lock_irqsave(&dev->dma_lock, flags);
	// Start DMA

	// load pixel counter with number of pixels
	LOCAL_REG_WRITE(dev, LOCAL_PIX_CNT_LL, n_pixels & 0xff);
	LOCAL_REG_WRITE(dev, LOCAL_PIX_CNT_ML, (n_pixels >> 8) & 0xff);
	LOCAL_REG_WRITE(dev, LOCAL_PIX_CNT_MH, (n_pixels >> 16) & 0xff);
	LOCAL_REG_WRITE(dev, LOCAL_PIX_CNT_HH, (n_pixels >> 24) & 0xff);

	// read back the counter value
	rb_count = LOCAL_REG_READ(dev, LOCAL_PIX_CNT_LL) & 0xff;
	rb_count += (LOCAL_REG_READ(dev, LOCAL_PIX_CNT_ML) & 0xff) << 8;
	rb_count += (LOCAL_REG_READ(dev, LOCAL_PIX_CNT_MH) & 0xff) << 16;
	rb_count += (LOCAL_REG_READ(dev, LOCAL_PIX_CNT_HH) & 0xff) << 24;

	if (rb_count != n_pixels)
		si_err(dev, "start_dma ERROR pixel register mismatch %d %d\n",
		       n_pixels, rb_count);

	// enable FIFOs
	LOCAL_REG_WRITE(dev, LOCAL_COMMAND, (LC_FIFO_MRS_L | LC_FIFO_PRS_L));

	atomic_set(&dev->dma_done, 0x1); // reflection of status bit
	dev->dma_cur = 0;
	dev->dma_next = 0;
	// setup DMA mode, turns on interrrupt DMA0
	PLX_REG_WRITE(dev, PCI9054_DMA0_MODE, (__u32)0x00021f43);

	// Write SGL physical address & set descriptors in PCI space
	PLX_REG_WRITE(dev, PCI9054_DMA0_DESC_PTR,
		      (__u32)dev->sgl_pci | (1 << 0));

	// Enable DMA channel
	PLX_REG8_WRITE(dev, PCI9054_DMA_COMMAND_STAT, ((1 << 0)));

	reg = PLX_REG_READ(dev, PCI9054_INT_CTRL_STAT);
	PLX_REG_WRITE(dev, PCI9054_INT_CTRL_STAT, reg | ((1 << 8) | (1 << 18)));
	// Start DMA
	PLX_REG8_WRITE(dev, PCI9054_DMA_COMMAND_STAT, (((1 << 0) | (1 << 1))));

	spin_unlock_irqrestore(&dev->dma_lock, flags);

	si_dbg(dev, "Starting DMA transfer, int_stat 0x%x rb_count %d\n",
	       reg, rb_count);

	return 0;
}

/* stop running dma */

int si_stop_dma(struct SIDEVICE *dev, struct SI_DMA_STATUS *status)
{
	unsigned long flags;
	int ret;
	__u32 cmd_stat;

	ret = 0;
	dev->abort_active = 1;
	spin_lock_irqsave(&dev->dma_lock, flags);

	cmd_stat = PLX_REG8_READ(dev, PCI9054_DMA_COMMAND_STAT);
	if (cmd_stat & 1) { /* if dma enabled, do abort sequence */
		PLX_REG8_WRITE(dev, PCI9054_DMA_COMMAND_STAT, 0x0); /* disable*/
		PLX_REG8_WRITE(dev, PCI9054_DMA_COMMAND_STAT,
			       (1 << 2)); /* abort */
		atomic_set(&dev->dma_done, cmd_stat);
	}
	spin_unlock_irqrestore(&dev->dma_lock, flags);
	si_dbg(dev, "stop_dma stat 0x%x\n", cmd_stat);

	if (cmd_stat & 1) {
		ret = 10; /* jiffies timeout */
		wait_event_interruptible_timeout(dev->dma_block,
						 (atomic_read(&dev->dma_done) &
						  SI_DMA_STATUS_DONE),
						 ret);
		if (!(atomic_read(&dev->dma_done) & SI_DMA_STATUS_DONE)) {
			si_info(dev, "timeout in abort sequence\n");
			ret = -EIO;
		} else
			ret = 0;
	}
	dev->abort_active = 0;

	si_dma_status(dev, status);
	return ret;
}

/* no block status request */

int si_dma_status(struct SIDEVICE *dev, struct SI_DMA_STATUS *stat)
{
	if (!stat)
		return 0;

	stat->status = atomic_read(&dev->dma_done);

	stat->transferred = si_dma_progress(dev);
	stat->next = dev->dma_next;
	stat->cur = dev->dma_cur;

	return 0;
}

/* block for next buffer complete */

int si_dma_next(struct SIDEVICE *dev, struct SI_DMA_STATUS *stat)
{
	int next, cur, ret, tmout;
	unsigned long flags;

	tmout = dev->dma_cfg.timeout; /* jiffies timeout */
	ret = 0;
	if (dev->dma_cfg.config & SI_DMA_CONFIG_WAKEUP_EACH) {
		spin_lock_irqsave(&dev->dma_lock, flags);
		next = dev->dma_next;
		cur = dev->dma_cur;
		spin_unlock_irqrestore(&dev->dma_lock, flags);
		if (next >= cur) {
			if (!si_dma_wakeup(dev)) {
				wait_event_interruptible_timeout(
					dev->dma_block, si_dma_wakeup(dev),
					tmout);
				if (si_dma_wakeup(dev))
					ret = 0;
				else
					ret = -EWOULDBLOCK;
			} else {
				ret = 0;
			}
		} else {
			ret = 0;
		}
	} else {
		if (!si_dma_wakeup(dev)) {
			wait_event_interruptible_timeout(
				dev->dma_block, si_dma_wakeup(dev), tmout);
			if (si_dma_wakeup(dev))
				ret = 0;
			else
				ret = -EWOULDBLOCK;
		} else {
			ret = 0;
		}
	}
	si_dma_status(dev, stat);
	//  if( stat->transferred == 0 ) {
	//    si_info(dev, "dma_next wakeup with transfer zero\n");
	//  }

	if (ret == 0)
		dev->dma_next++;

	return ret;
}

/* true if its time to wakeup dma_block */

int si_dma_wakeup(struct SIDEVICE *dev)
{
	int ret;
	int done;

	done = ((atomic_read(&dev->dma_done) & SI_DMA_STATUS_DONE) != 0);

	if (!dev->sgl) { /* if its not configured or enabled, always wakeup */
		ret = 1;
	} else {
		if (dev->dma_cfg.config & SI_DMA_CONFIG_WAKEUP_EACH)
			ret = ((dev->dma_next < dev->dma_cur) || (done != 0));
		else
			ret = done;
	}

	if (ret)
		si_dbg(dev, "wakeup done %d\n", ret);
	return ret;
}

/* wait for vma close */

int si_wait_vmaclose(struct SIDEVICE *dev)
{
	int tmout;

	tmout = VMACLOSE_TIMEOUT;

	si_info(dev, "si_wait_vmaclose waiting\n");
	wait_event_interruptible_timeout(
		dev->mmap_block, (atomic_read(&dev->vmact) == 0), tmout);

	si_info(dev, "si_wait_vmaclose wakeup\n");
	if (atomic_read(&dev->vmact) > 0) {
		si_dbg(dev, "si_wait_vmaclose timeout\n");
		return -EWOULDBLOCK;
	} else {
		si_dbg(dev, "si_wait_vmaclose ok\n");
		return 0;
	}
}

/* return byte count progress */

int si_dma_progress(struct SIDEVICE *dev)
{
	__u32 pci;
	int nb, nchains, prog;
	struct SIDMA_SGL *ch;

	pci = PLX_REG_READ(dev, PCI9054_DMA0_PCI_ADDR);

	prog = 0;
	nb = 0;
	nchains = dev->dma_nbuf;

	for (nb = 0; nb < nchains; nb++) {
		ch = &dev->sgl[nb]; /* kernel virt address of this SGL */
		prog += ch->siz;
		if (ch->padr == pci) {
			break;
		}
		// si_info(dev,
		//	"pci 0x%x prog %d %d\n", ch->padr, prog, ch->siz);
	}

	/* this can happen if its already done */

	if (prog > dev->dma_cfg.total) {
		si_info(dev, "prog %d total %d dma_done 0x%x\n", prog,
		       dev->dma_cfg.total, atomic_read(&dev->dma_done));

		prog = dev->dma_cfg.total;
	}

	return prog;
}

void *jeff_alloc(int size, dma_addr_t *pphy)
{
	unsigned char *km;
	//  unsigned int phy, phy_ix;
	//  int count;
	int order;

	order = get_order(size);
	pr_info("SI order %d\n", order);
	km = (unsigned char *)__get_free_pages(GFP_KERNEL, order);
	if (!km) {
		pr_err("SI TEST get_free_pages no memory\n");
		return NULL;
	} else {
		//int i;

		memset(km, 0, size);

		if (pphy)
			*pphy = (dma_addr_t)virt_to_phys(km);

		//    count = 0;

		//    for( i=0; i<TEST_SIZE; i+=8192 ) {
		//       phy_ix = (unsigned int)virt_to_phys( km+i );
		//       if( phy_ix != (phy + i ))
		//         count++;
		//    }
		//    if( count == 0 )
		//      pr_info("TEST worked count %d\n", count);
		//    else
		//      pr_info("TEST failed count %d\n", count);
		//
		//    free_pages((unsigned long)km, order);

		return (void *)km;
	}
}
