/*
 *  Copyright (C) 2008 Steve Shoecraft
 *
 *  Cooperative Linux SCSI Driver implementation
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/delay.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

#include <linux/cooperative.h>
#include <linux/cooperative_internal.h>
#include <linux/cooperative_pci.h>

#include <linux/cdrom.h>
#include <scsi/coscsi.h>

/* Special pass through type */
#define TYPE_PASS 0x1f

#define COSCSI_VERSION "1.02"

MODULE_AUTHOR("Steve Shoecraft <sshoecraft@earthlink.net>");
MODULE_DESCRIPTION("Cooperative Linux SCSI Driver " COSCSI_VERSION);
MODULE_LICENSE("GPL");

#define COSCSI_DUMP_STATS 0
#define COSCSI_DUMP_CONFIG 0
#define COSCSI_DUMP_PARAMS 0

#ifdef min
#undef min
#endif
#define min(a,b) ((a) < (b) ? (a) : (b))

/* Keep sg size to <= 1 page */
#define COSCSI_SGSIZE ( 4096 / sizeof(struct scatterlist) )

#define COSCSI_DEBUG 0
#define COSCSI_DEBUG_PCI 0
#define COSCSI_DEBUG_ISR 0
#define COSCSI_DEBUG_HOST 0
#define COSCSI_DEBUG_XFER 0
#define COSCSI_DEBUG_COMM 0
#define COSCSI_DEBUG_INQ 0
#define COSCSI_DEBUG_SENSE 0
#define COSCSI_DEBUG_PASS 0

#if COSCSI_DEBUG_XFER || COSCSI_DEBUG_COMM || COSCSI_DEBUG_SENSE
#define DUMP_DATA 1
#else
#define DUMP_DATA 0
#endif

/* OPs not found in scsi.h, use from cdrom.h */
#define GET_CONFIGURATION	GPCMD_GET_CONFIGURATION
#define GET_EVENT_STATUS	GPCMD_GET_EVENT_STATUS_NOTIFICATION
#define READ_DISC_INFO		GPCMD_READ_DISC_INFO

/* Sense codes */
#define LOGICAL_UNIT_NOT_READY 0x4
#define INVALID_FIELD_IN_CDB 0x24
#define MEDIUM_NOT_PRESENT 0x3a

#include "coscsi_rom.h"

struct coscsi_device {
	int unit;
	int type;
	coscsi_rom_t *rom;
	unsigned long flags;
	unsigned long long max_lba;
	unsigned long long size;
	void *os_handle;
	int prevent;
	int key;
	int asc;
	int asq;
	int debug;
	char msg[192];
};
typedef struct coscsi_device coscsi_device_t;

/* Device flags */
enum DEVICE_FLAG {
	DFLAG_NONE,
	DFLAG_DEBUG,			/* Allow debugging output for this dev */
	DFLAG_OPEN,			/* Prevent medium removal */
	DFLAG_CHECK,			/* Check condition */
	DFLAG_PREVENT,			/* Prevent medium removal */
	DFLAG_EVENT,			/* Outstanding Event */
};

#define DFLAG(d,f) ((d->flags & f) != 0)
#define dprintk(m) if (DFLAG(dp, DFLAG_DEBUG)) printk(KERN_INFO "scsi%d: ", dp->unit), printk(KERN_INFO m)

struct coscsi_worker {
	coscsi_device_t *dp;
	struct scsi_cmnd *scp;
};
typedef struct coscsi_worker coscsi_worker_t;

/* Private info */
char scsi_rev[5];
static coscsi_device_t devices[CO_MODULE_MAX_COSCSI];

#if DUMP_DATA
static void _dump_data(int unit, char *str, void *data, int data_len) {
	unsigned char *p;
	int x,y,len;

	printk(KERN_INFO "scsi%d: %s(%d bytes):\n",unit,str,data_len);
	len = data_len;
	p = data;
	for(x=y=0; x < len; x++) {
		printk(KERN_INFO " %02x", p[x]);
		y++;
		if (y > 15) {
			printk(KERN_INFO "\n");
			y = 0;
		}
	}
	if (y) printk(KERN_INFO "\n");
}
#define dump_data(u, s,a,b) _dump_data(u,s,a,b)
#else
#define dump_data(u, s,a,b) /* noop */
#endif

static spinlock_t coscsi_isr_lock;

static irqreturn_t coscsi_isr(int irq, void *dev_id)
{
	co_message_node_t *node_message;
	co_linux_message_t *message;
	co_scsi_intr_t *info;
	struct scsi_cmnd *scp;

	spin_lock(&coscsi_isr_lock);
#if COSCSI_DEBUG_ISR
	printk(KERN_INFO "coscsi_isr: getting messages!\n");
#endif
	while (co_get_message(&node_message, CO_DEVICE_SCSI)) {

		message = (co_linux_message_t *)&node_message->msg.data;

		info = (co_scsi_intr_t *) &message->data;
		scp = info->ctx;
		scp->result = info->result;
		scsi_set_resid(scp, info->delta);
#if COSCSI_DEBUG_ISR
		printk(KERN_INFO "coscsi_isr: scp: %p result: %d, delta: %d\n", scp, info->result, info->delta);
#endif
		scp->scsi_done(scp);
		co_free_message(node_message);
	}
	spin_unlock(&coscsi_isr_lock);

	return IRQ_HANDLED;
}

/****************************************************************************************************
 *
 *
 * HOST functions
 *
 *
 ****************************************************************************************************/

/*
 * Open handle
*/
static int host_open(coscsi_device_t *dp) {
	unsigned long flags;
	int rc = 0;

#if COSCSI_DEBUG_HOST
	if (dp->debug) printk(KERN_INFO "host_open: handle: %p\n", dp->os_handle);
#endif
	if (!dp->os_handle) {
		co_passage_page_assert_valid();
		co_passage_page_acquire(&flags);
		co_passage_page->operation = CO_OPERATION_DEVICE;
		co_passage_page->params[0] = CO_DEVICE_SCSI;
		co_passage_page->params[1] = CO_SCSI_OPEN;
		co_passage_page->params[2] = dp->unit;

		co_switch_wrapper();

		rc = co_passage_page->params[0];
		if (!rc) dp->os_handle = (void *) co_passage_page->params[1];
		co_passage_page_release(flags);
	}

#if COSCSI_DEBUG_HOST
	if (dp->debug) printk(KERN_INFO "host_open: rc: %d, handle: %p\n", rc, dp->os_handle);
#endif
	if (rc) printk(KERN_ERR "coscsi%d: unable to open device! rc: %x\n", dp->unit, rc);
	return rc;
}

/*
 * Close handle
*/
static int host_close(coscsi_device_t *dp) {
	unsigned long flags;
	int rc;

#if COSCSI_DEBUG_HOST
	if (dp->debug) printk(KERN_INFO "host_close: handle: %p\n", dp->os_handle);
#endif
	if (!dp->os_handle) return 0;
	co_passage_page_assert_valid();
	co_passage_page_acquire(&flags);
	co_passage_page->operation = CO_OPERATION_DEVICE;
	co_passage_page->params[0] = CO_DEVICE_SCSI;
	co_passage_page->params[1] = CO_SCSI_CLOSE;
	co_passage_page->params[2] = dp->unit;

	co_switch_wrapper();

	rc = co_passage_page->params[0];
	co_passage_page_release(flags);

	dp->os_handle = 0;
#if COSCSI_DEBUG_HOST
	if (dp->debug) printk(KERN_INFO "host_close: rc: %d\n", rc);
#endif
	return rc;
}

#if COSCSI_DUMP_STATS
static unsigned int max_segs = 1;
static unsigned int max_xfer = 4096;
#endif

static inline int coscsi_map_sg(struct scatterlist *sgl, int sg_count)
{
	unsigned char *virt;
	size_t sg_len = 0;
	struct scatterlist *sg;
	int i;

	// Set dma_address for old host driver
	for_each_sg(sgl, sg, sg_count, i) {
		virt = kmap_atomic(sg_page(sg), KM_SOFTIRQ0);
		sg->dma_address = __pa(virt) + sg->offset;
		sg_len += sg->length;
#if COSCSI_DEBUG_HOST
		printk(KERN_INFO "coscsi_map_sg: sg:%p virt:%p sg->len:%d i:%d sg_count:%d sg->offset:%zx\n",
			                            sg,     virt,     sg->length, i, sg_count, sg->offset);
#endif
		BUG_ON(!virt);
	}

	return i;
}

static inline void coscsi_unmap_sg(struct scatterlist *sgl, int sg_count)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, sg_count, i) {
		scsi_kunmap_atomic_sg(__va(sgl->dma_address - sg->offset));
	}
}

/*
 * Read/Write block(s)
*/

static int host_rw(coscsi_worker_t *wp, unsigned long long lba, unsigned long num, int write)
{
	struct scatterlist *sg = scsi_sglist(wp->scp);
	struct scsi_cmnd *scp = wp->scp;
	unsigned long flags;
	co_scsi_io_t *iop;
	int count,rc,total;

#if COSCSI_DEBUG_HOST
	if (wp->dp->debug) printk(KERN_INFO "host_rw: sg:%p count:%d lba: %lld, sector_size: %d, num: %ld, write: %d\n",
		sg, scsi_sg_count(scp),
		lba, scp->device->sector_size, num, write);
#endif

	if (!wp->dp->os_handle) {
		if (host_open(wp->dp))
			return 1;
	}

	/* XXX needed when clustering is enabled */
	local_irq_save(flags);
	count = coscsi_map_sg(sg, scsi_sg_count(scp));
	BUG_ON(!count);

	/* Get passage page */
	co_passage_page_assert_valid();
	co_passage_page_ref_up(); /* aka co_passage_page_acquire */
	co_passage_page->operation = CO_OPERATION_DEVICE;
	co_passage_page->params[0] = CO_DEVICE_SCSI;
	co_passage_page->params[1] = CO_SCSI_IO;
	co_passage_page->params[2] = wp->dp->unit;

	/* Setup iop */
	iop = (co_scsi_io_t *) &co_passage_page->params[3];
	iop->scp = scp;
	iop->offset = lba * scp->device->sector_size;
	iop->count = count;
	iop->write = write;
	iop->sg = scsi_sglist(scp);
	iop->reqlen = total = num * scp->device->sector_size;

	/* Do it */
	co_switch_wrapper();

	rc = co_passage_page->params[0];
	co_passage_page_ref_down(); /* aka co_passage_page_release */

	coscsi_unmap_sg(sg, scsi_sg_count(scp));
	local_irq_restore(flags);

#if COSCSI_DUMP_STATS
	if (rc == GOOD) {
		if (count > max_segs) {
			max_segs = count;
			printk(KERN_WARN "COSCSI: max_segs: %d\n", max_segs);
		}

		if (total > max_xfer) {
			max_xfer = total;
			printk(KERN_WARN "COSCSI: max_xfer: %dKB\n", max_xfer >> 10);
		}
	}
#endif

#if COSCSI_DEBUG_HOST
	if (wp->dp->debug) printk(KERN_INFO "host_rw: rc: %d\n", rc);
#endif
	return rc;
}

static int get_bs_bits(coscsi_device_t *dp, int sector_size) {
	unsigned long mask = 0x80000000;
	int bs_bits;

	bs_bits = 31;
	while(mask) {
		if (sector_size & mask)
			break;
		mask >>= 1;
		bs_bits--;
	}

	return bs_bits;
}

/*
 * File/Device size
*/
static int host_size(coscsi_device_t *dp, struct scsi_cmnd *scp) {
	unsigned long long s;
	unsigned long flags;
	int rc, bits;

#if COSCSI_DEBUG_HOST
	if (dp->debug) printk(KERN_INFO "host_size: getting size...\n");
#endif
	if (!dp->os_handle) {
		if (host_open(dp))
			return 1;
	}

	co_passage_page_assert_valid();
	co_passage_page_acquire(&flags);
	co_passage_page->operation = CO_OPERATION_DEVICE;
	co_passage_page->params[0] = CO_DEVICE_SCSI;
	co_passage_page->params[1] = CO_SCSI_SIZE;
	co_passage_page->params[2] = dp->unit;

	co_switch_wrapper();

	rc = co_passage_page->params[0];
	dp->size = *((unsigned long long *)&co_passage_page->params[1]);
	co_passage_page_release(flags);

	bits = get_bs_bits(dp, scp->device->sector_size);

	s = dp->size >> bits;
	s *= scp->device->sector_size;
	if (s < dp->size) s += scp->device->sector_size;

	dp->max_lba = (s >> bits) - 1;

#if COSCSI_DEBUG_HOST
	if (dp->debug) printk(KERN_INFO "host_size: rc: %d, size: %lld, max_lba: %lld\n", rc, dp->size, dp->max_lba);
#endif

	return rc;
}

/*
 * Pass-through
*/
static int host_pass(coscsi_device_t *dp, struct scsi_cmnd *scp) {
	unsigned long flags;
	void *buffer;
	unsigned long buflen;
	co_scsi_pass_t *pass;
	int rc;

	if (!dp->os_handle) {
		if (host_open(dp))
			return 1;
	}

	/* Scatter/Gather */
	if (scsi_sg_count(scp)) {
		struct scatterlist *sg;

		/* Should never be more than 1 for non r/w transfers */
		if (scsi_sg_count(scp) > 1) panic("COSCSI: host_pass: use_sg (%d) > 1!\n", scsi_sg_count(scp));

		sg = scsi_sglist(scp);
#if COSCSI_DEBUG_HOST
		if (dp->debug) printk(KERN_INFO "host_pass: sg: page: %p, offset: %d, length: %d\n",
			sg_page(sg), sg->offset, sg->length);
#endif
		buffer = sg_virt(sg);
		buflen = sg->length;
	/* Direct */
	} else {
		buffer = scsi_sglist(scp);
		buflen = scsi_bufflen(scp);
	}

	co_passage_page_assert_valid();
	co_passage_page_acquire(&flags);
	co_passage_page->operation = CO_OPERATION_DEVICE;
	co_passage_page->params[0] = CO_DEVICE_SCSI;
	co_passage_page->params[1] = CO_SCSI_PASS;
	co_passage_page->params[2] = dp->unit;

	pass = (co_scsi_pass_t *) &co_passage_page->params[3];
	memcpy(&pass->cdb, &scp->cmnd, 16);
	pass->cdb_len = scp->cmd_len;
	pass->write = (scp->sc_data_direction == DMA_TO_DEVICE);
	pass->buffer = buffer;
	pass->buflen = buflen;

	co_switch_wrapper();

	rc = co_passage_page->params[0];
	co_passage_page_release(flags);

#if COSCSI_DEBUG_PASS
	if (rc == GOOD && dp->debug) dump_data(dp->unit, "host_pass", buffer, buflen);
#endif

#if COSCSI_DEBUG_HOST
	if (dp->debug) printk(KERN_INFO "host_pass: rc: %d\n", rc);
#endif

	return rc;
}

/****************************************************************************************************
 *
 *
 * SCSI functions
 *
 *
 ****************************************************************************************************/

static int check_condition(struct coscsi_device *dp, int key, int asc, int asq) {
	dp->key = key;
	dp->asc = asc;
	dp->asq = asq;
	return CHECK_CONDITION;
}

static int response(coscsi_worker_t *wp, void *data, int len) {
	struct scsi_cmnd *scp = wp->scp;
	void *buffer;
	unsigned long buflen;
	int act_len;

	/* Scatter/Gather */
	if (scsi_sg_count(scp)) {
		struct scatterlist *sg;
		int i;

		/* scatter-gather list too long? */
		BUG_ON(scsi_sg_count(scp) > COSCSI_SGSIZE);

		scsi_for_each_sg(scp, sg, scsi_sg_count(scp), i) {
#if COSCSI_DEBUG
			if (wp->dp->debug) printk(KERN_INFO "response: sg: page: %p, offset: %d, length: %d\n",
				sg_page(sg), sg->offset, sg->length);
#endif
			buffer = sg_virt(sg);
			buflen = sg->length;
			act_len = min(buflen, len);
#if COSCSI_DEBUG_COMM
			if (wp->dp->debug) dump_data(wp->dp->unit, "response", data, act_len);
#endif
			memcpy(buffer, data, act_len);
			data += act_len;
			len -= act_len;
		}
	/* Direct */
	} else {
		buffer = scsi_sglist(scp);
		buflen = scsi_bufflen(scp);
		if (!buflen) return GOOD;
		act_len = min(buflen, len);
#if COSCSI_DEBUG_COMM
		if (wp->dp->debug) dump_data(wp->dp->unit, "response", data, act_len);
#endif
		memcpy(buffer, data, act_len);
	}

	return GOOD;
}

static int unit_ready(coscsi_worker_t *wp) {
	int error, rc;

	rc = GOOD;
	error = (wp->dp->os_handle == 0 ? host_open(wp->dp) : GOOD);
	if (error) {
		switch(wp->dp->type) {
		case TYPE_ROM:
		case TYPE_TAPE:
			rc = check_condition(wp->dp, NOT_READY, MEDIUM_NOT_PRESENT, 0x2);
			break;
		default:
			rc = check_condition(wp->dp, NOT_READY, LOGICAL_UNIT_NOT_READY, 0x2);
			break;
		}
	}

	return rc;
}

static int inquiry(coscsi_worker_t *wp) {
	int x, alloc_len;
	struct scsi_cmnd *scp = wp->scp;

	alloc_len = (scp->cmnd[3] << 8) + scp->cmnd[4];
#if COSCSI_DEBUG_INQ
	if (wp->dp->debug) printk(KERN_INFO "scsi_inq: alloc_len: %d, buflen: %d\n", alloc_len, scsi_bufflen(scp));
#endif

	/* EVPD? */
	if (scp->cmnd[1] & 1) {
		coscsi_page_t *vpd = wp->dp->rom->vpd;
		int page = scp->cmnd[2];

#if COSCSI_DEBUG_INQ
		if (wp->dp->debug) printk(KERN_INFO "scsi_inq: sending VPD page %d\n", page);
#endif
		/* For page 00, generate dynamically */
		if (page == 0) {
			unsigned char data[32];
			int i;

			memset(data, 0, sizeof(data));
			data[0] = wp->dp->rom->std.page[0];
			i = 4;
			for(x=0; vpd[x].page; x++) data[i++] = vpd[x].num;
			data[3] = i - 3;

			return response(wp, data, min(alloc_len, sizeof(data)));
		} else {
			for(x=0; vpd[x].page; x++) {
				if (vpd[x].num == page)
					return response(wp, vpd[x].page, min(alloc_len, vpd[x].size));
			}
			return check_condition(wp->dp, ILLEGAL_REQUEST, INVALID_FIELD_IN_CDB, 0);
		}

	/* Standard page */
	} else {
		unsigned char *std = wp->dp->rom->std.page;

#if COSCSI_DEBUG_INQ
		printk(KERN_INFO "scsi_inq: sending STD page\n");
#endif
		strcpy((char *)&std[8], "coLinux");
		std[1] = ((wp->dp->type == TYPE_ROM || wp->dp->type == TYPE_TAPE) ? 0x80 : 0);
		memcpy(&std[16], wp->dp->rom->name, strlen(wp->dp->rom->name)+1);
		memcpy(&std[32], scsi_rev, min(4, strlen(scsi_rev)+1));
		return response(wp, std, min(alloc_len, wp->dp->rom->std.size));
	}
}

static int read_capacity(coscsi_worker_t *wp) {
	coscsi_device_t *dp = wp->dp;
	struct scsi_cmnd *scp = wp->scp;

	/* Get the size */
	if (host_size(dp, scp)) return check_condition(dp, HARDWARE_ERROR, 0x3e, 1);

	/* Convert to read_capacity format */
	if (dp->max_lba > 0xfffffffe || scp->cmnd[8] & 1) {
		dp->msg[0] = 0xff;
		dp->msg[1] = 0xff;
		dp->msg[2] = 0xff;
		dp->msg[3] = 0xff;
	} else {
		dp->msg[0] = (wp->dp->max_lba >> 24);
		dp->msg[1] = (wp->dp->max_lba >> 16) & 0xff;
		dp->msg[2] = (wp->dp->max_lba >> 8) & 0xff;
		dp->msg[3] = wp->dp->max_lba & 0xff;
	}
	dp->msg[4] = (scp->device->sector_size >> 24);
	dp->msg[5] = (scp->device->sector_size >> 16) & 0xff;
	dp->msg[6] = (scp->device->sector_size >> 8) & 0xff;
	dp->msg[7] = scp->device->sector_size & 0xff;

	return response(wp, &dp->msg, 8);
}

static int mode_sense(coscsi_worker_t *wp) {
	unsigned char data[256],*ap;
	int offset, bd_len, page;
	coscsi_page_t *pages = wp->dp->rom->mode;
	coscsi_device_t *dp = wp->dp;
	struct scsi_cmnd *scp = wp->scp;
	register int x;

	memset(data, 0, sizeof(data));
	offset = 4;
	bd_len = 8;

	data[2] = 0x10; /* DPOFUA */
	data[3] = bd_len;

	ap = data + offset;
	if (dp->max_lba > 0xfffffffe) {
		ap[0] = 0xff;
		ap[1] = 0xff;
		ap[2] = 0xff;
		ap[3] = 0xff;
	} else {
		ap[0] = (dp->max_lba >> 24) & 0xff;
		ap[1] = (dp->max_lba >> 16) & 0xff;
		ap[2] = (dp->max_lba >> 8) & 0xff;
		ap[3] = dp->max_lba & 0xff;
	}
	ap[5] = (scp->device->sector_size >> 16) & 0xff;
	ap[6] = (scp->device->sector_size >> 8) & 0xff;
	ap[7] = scp->device->sector_size & 0xff;

	offset += bd_len;
	ap = data + offset;
#if COSCSI_DEBUG_SENSE
	if (dp->debug) printk(KERN_INFO "mode_sense: ap: %p, offset: %d\n", ap, offset);
#endif
	page = scp->cmnd[2] & 0x3f;
	if (page == 0x3f) {
		/* All pages */
		if (scp->cmnd[3] == 0 || scp->cmnd[3] == 0xFF) {
			for(x=0; pages[x].page; x++) {
#if COSCSI_DEBUG_SENSE
				if (dp->debug) dump_data(dp->unit, "page", pages[x].page, pages[x].size);
#endif
				memcpy(ap, pages[x].page, pages[x].size);
				ap += pages[x].size;
				offset += pages[x].size;
			}
		} else {
			return check_condition(wp->dp, ILLEGAL_REQUEST, INVALID_FIELD_IN_CDB, 0);
		}
	} else {
		/* Specific page */
		int found = 0;
		for(x=0; pages[x].page; x++) {
#if COSCSI_DEBUG_SENSE
			if (dp->debug) printk(KERN_INFO "mode_sense: pages[%d].num: %d, page: %d\n", x, pages[x].num, page);
#endif
			if (pages[x].num == page) {
#if COSCSI_DEBUG_SENSE
				if (dp->debug) dump_data(dp->unit, "page", pages[x].page, pages[x].size);
#endif
				memcpy(ap, pages[x].page, pages[x].size);
				offset += pages[x].size;
				found = 1;
				break;
			}
		}
		if (!found) return check_condition(wp->dp, ILLEGAL_REQUEST, INVALID_FIELD_IN_CDB, 0);
	}
#if COSCSI_DEBUG_SENSE
	if (dp->debug) printk(KERN_INFO "scsi_mode_sense: offset: %d\n", offset);
#endif
	data[0] = offset - 1;
	return response(wp, data, min(scp->cmnd[4], offset));
}

/*
The Logical Block Address field contains the LBA of the first block from which data shall be returned. If the
Logical Block Address is beyond the range of recorded data, the Drive shall terminate the command with
CHECK CONDITION status and SK/ASC/ASCQ values shall be set to ILLEGAL REQUEST/LOGICAL BLOCK
ADDRESS OUT OF RANGE.
*/

static int read_write(coscsi_worker_t *wp) {
	unsigned long long lba;
	unsigned long num;
	register unsigned char *p = wp->scp->cmnd;

	lba = num = 0;
	switch(*p) {
	case READ_16:
	case WRITE_16:
		{
			register int x;

			for (x = 0; x < 8; x++) {
				if (x) lba <<= 8;
				lba |= (u32)(*(p+2+x));
			}
		}
		num = *(p+10) << 24 | *(p+11) << 16 | *(p+12) << 8 | *(p+13);
		break;
	case READ_12:
	case WRITE_12:
		lba = (u32)(*(p+2) << 24 | *(p+3) << 16 | *(p+4) << 8 | *(p+5));
		num = *(p+6) << 24 | *(p+7) << 16 | *(p+8) << 8 | *(p+9);
		break;
	case READ_10:
	case WRITE_10:
		lba = (u32)(*(p+2) << 24 | *(p+3) << 16 | *(p+4) << 8 | *(p+5));
		num = *(p+7) << 8 | *(p+8);
		break;
	case READ_6:
	case WRITE_6:
		lba = (u32)((*(p+1) & 0x1f) << 16 | *(p+2) << 8 | *(p+3));
		num = *(p+4) ? *(p+4) : 0xff;
		break;
	default:
		printk(KERN_ERR "scsi%d: read_write: unknown opcode: %x\n", wp->dp->unit, *p);
		return check_condition(wp->dp, ILLEGAL_REQUEST, INVALID_FIELD_IN_CDB, 0);
	}

#if COSCSI_DEBUG_XFER
	if (wp->dp->debug) printk(KERN_INFO "read_write: lba: %lld, num: %ld\n", lba, num);
#endif

	if (host_rw(wp, lba, num, (wp->scp->cmnd[0] & 2) >> 1))
		return check_condition(wp->dp, HARDWARE_ERROR, 0x3e, 1);
	else
		return GOOD;
}

static int request_sense(coscsi_worker_t *wp) {
	coscsi_device_t *dp = wp->dp;

	if (wp->scp->cmnd[1] & 1) {
		dp->msg[0] = 0x72;
		dp->msg[1] = dp->key;
		dp->msg[2] = dp->asc;
		dp->msg[3] = dp->asq;
	} else {
		dp->msg[0] = 0x70;
		dp->msg[2] = dp->key;
		dp->msg[7] = 0xa;
		dp->msg[12] = dp->asc;
		dp->msg[13] = dp->asq;
	}
	return response(wp, &dp->msg, min(wp->scp->cmnd[4], 18));
}

static int prevent_allow(coscsi_worker_t *wp) {
	wp->dp->prevent = wp->scp->cmnd[4] & 1;
	return GOOD;
}

static int get_config(coscsi_worker_t *wp) {
	char buf[] = {
		0x00,0x00,0x00,0x7c,0x00,0x00,0x00,0x10,
		0x00,0x00,0x03,0x08,0x00,0x10,0x01,0x00,
		0x00,0x08,0x00,0x00,0x00,0x01,0x03,0x04,
		0x00,0x00,0x00,0x01,0x00,0x02,0x03,0x04
	};

	return response(wp, buf, sizeof(buf));
}

static int read_toc(coscsi_worker_t *wp) {
	int msf = ((wp->scp->cmnd[1] >> 1) & 1);
	int len = wp->scp->cmnd[7] << 8 | wp->scp->cmnd[8];
	int start;
	unsigned char data[12];

	/* We only support format 0 when MSF is set */
	if (msf && wp->scp->cmnd[2] & 0x0f) return check_condition(wp->dp, ILLEGAL_REQUEST, INVALID_FIELD_IN_CDB, 0);

	start = (msf ? 32 : 0);

	/* TOC header */
	data[0] = 0;		/* Len MSB */
	data[1] = sizeof(data);	/* Len LSB */
	data[2] = 1;		/* 1st track */
	data[3] = 1;		/* Last track */

	/* Track 1 descriptor */
	data[4] = 0;		/* Reserved */
	data[5] = 0x14;		/* ADR & CONTROL */
	data[6] = 1;		/* Track # */
	data[7] = 0;		/* Reserved */
	data[8] = (start >> 24) & 0xff;	/* Start */
	data[9] = (start >> 16) & 0xff;
	data[10] = (start >> 8) & 0xff;
	data[11] = start & 0xff;

	return response(wp, data, min(len, sizeof(data)));
}

/*
The Polled bit is used to select operational mode. When Polled is set to zero, the Host is requesting
asynchronous operation. If the Drive does not support asynchronous operation, the command shall be
terminated with CHECK CONDITION status and the values for SK/ASC/ASCQ shall be set to ILLEGAL
REQUEST/INVALID FIELD IN CDB.
Note 12. If Polled is zero while a Group 2 timeout command is executing, the GET EVENT STATUS
NOTIFICATION command may be queued, but it never terminates.
When Polled is set to one, the Host is requesting polled operation. The Drive shall return event information for
the highest priority requested event. If no event has occurred, the Drive shall report the .No Change. event for
the highest priority requested event class.
*/

static int event_status(coscsi_worker_t *wp) {
	return check_condition(wp->dp, ILLEGAL_REQUEST, INVALID_FIELD_IN_CDB, 0);
}

static int coscsi_queue(struct scsi_cmnd *scp, void (*done)(struct scsi_cmnd *)) {
	coscsi_device_t *dp;
	coscsi_worker_t worker;
	int rc;

#if COSCSI_DEBUG
	printk(KERN_INFO "coscsi_queue: id: %d, lun: %d, cdb[0]: 0x%02x\n",
		scp->device->id, scp->device->lun, scp->cmnd[0]);
#endif

	/* Get device pointer */
	dp = &devices[scp->device->id];

#if COSCSI_DEBUG_COMM
	if (dp->debug) dump_data(dp->unit, "request", &scp->cmnd, sizeof(scp->cmnd));
#endif

	/* Setup worker */
	worker.dp = dp;
	worker.scp = scp;

	/* Do we have the requested device? */
	if ((scp->device->id >= CO_MODULE_MAX_COSCSI) || (dp->rom == 0)) {
		if (scp->cmnd[0] == INQUIRY) {
			char temp[96];
			memset(temp,0,sizeof(temp));
			temp[0] = 0x7f;
			temp[3] = 2;
			temp[4] = 92;
			scp->result = response(&worker, temp, min(scp->cmnd[4],96));
		} else
			scp->result = check_condition(dp, ILLEGAL_REQUEST, INVALID_FIELD_IN_CDB, 0);
		goto req_done;
	}

	/* Set done for async funcs */
	scp->scsi_done = done;

	/* Pass-through? */
	if (dp->type == SCSI_PTYPE_PASS) {
		switch(scp->cmnd[0]) {
		case READ_6:
		case READ_10:
		case READ_16:
		case WRITE_6:
		case WRITE_10:
		case WRITE_16:
			/* r/w may be async */
			rc = read_write(&worker);
#if COSCSI_ASYNC
			if (rc == GOOD) goto req_out;
#endif
			break;
		default:
			rc = host_pass(dp, scp);
			break;
		}
		scp->result = rc;
		goto req_done;
	}

	/* Process command */
	switch(scp->cmnd[0]) {
	case INQUIRY:
		scp->result = inquiry(&worker);
		break;
	case TEST_UNIT_READY:
		scp->result = unit_ready(&worker);
		break;
	case REQUEST_SENSE:
		scp->result = request_sense(&worker);
		break;
	case READ_CAPACITY:
		scp->result = read_capacity(&worker);
		break;
	case REPORT_LUNS:
		/* We only support 1 lun right now */
		memset(dp->msg, 0, 16);
		dp->msg[3] = 1;
		scp->result = response(&worker, &dp->msg, 16);
		break;
	case MODE_SENSE:
		scp->result = mode_sense(&worker);
		break;
	case ALLOW_MEDIUM_REMOVAL:
		scp->result = prevent_allow(&worker);
		break;
	case READ_TOC:
		scp->result = read_toc(&worker);
		break;
	case GET_CONFIGURATION:
		scp->result = get_config(&worker);
		break;
	case GET_EVENT_STATUS:
		scp->result = event_status(&worker);
		break;
	case READ_6:
	case READ_10:
	case READ_16:
	case WRITE_6:
	case WRITE_10:
	case WRITE_16:
		/* r/w may be async */
		rc = read_write(&worker);
#if COSCSI_ASYNC
		if (rc == GOOD) goto req_out;
#endif
		scp->result = rc;
		break;
	case SYNCHRONIZE_CACHE:
		scp->result = GOOD;
		break;
	case READ_DISC_INFO:
		{
			disc_information di = { 0, };

			di.disc_information_length = cpu_to_be16(1);
			/* di.erasable = 0; */
			scp->result = response(&worker, &di, sizeof(di.disc_information_length) + 1);
		}
		break;
	default:
		printk(KERN_NOTICE "scsi%d: unhandled opcode: %x\n", dp->unit, scp->cmnd[0]);
		scp->result = check_condition(dp, ILLEGAL_REQUEST, INVALID_FIELD_IN_CDB, 0);
	}

req_done:
	done(scp);
#if COSCSI_ASYNC
req_out:
#endif
#if COSCSI_DEBUG_COMM
	if (dp->debug) printk(KERN_INFO "coscsi_queue: scp->result: %02x (code: %x)\n", scp->result, scp->result & 0xffff);
	if (dp->debug) printk(KERN_INFO "------------------------------------------------------------------------\n");
#endif
	return 0;
}

static int coscsi_config(struct scsi_device *sdev) {
	switch(sdev->type) {
	case TYPE_ROM:
	case TYPE_WORM:
		/* XXX required to get rid of "unaligned transfer" errors */
	        blk_queue_logical_block_size(sdev->request_queue, 2048);
		break;
	default:
		break;
	}

	/* Don't have SAI_READ_CAPACITY_16 and other 16 byte commands at the moment */
	if (sdev->type != SCSI_PTYPE_PASS)
		sdev->scsi_level = SCSI_SPC_2;

	return 0;
}

struct scsi_host_template coscsi_template = {
	.module			= THIS_MODULE,
	.name			= "Cooperative Linux SCSI Adapter",
	.proc_name		= "coscsi",
	.queuecommand		= coscsi_queue,
	.slave_configure	= coscsi_config,
	.this_id		= -1,
	.sg_tablesize		= COSCSI_SGSIZE,
	.max_sectors		= 0xFFFF,
	.can_queue		= 65535,
	.cmd_per_lun		= 2048,
	.use_clustering		= ENABLE_CLUSTERING,
	.skip_settle_delay	= 1,
	.max_host_blocked	= 1,
};

/****************************************************************************************************
 *
 *
 * PCI functions
 *
 *
 ****************************************************************************************************/

/*
 * PCI Probe - probe for a single device
*/
static int __devinit coscsi_pci_probe( struct pci_dev *pdev, const struct pci_device_id *ent )
{
	struct Scsi_Host *shost;
	unsigned long flags;
	coscsi_device_t *dp;
	register int x;
	int rc;

#if COSCSI_DEBUG
	printk(KERN_INFO "coscsi_pci_probe: adding host...\n");
#endif

	/* Get our config from the host */
	memset(&devices, 0, sizeof(devices));
	co_passage_page_acquire(&flags);
	co_passage_page->operation = CO_OPERATION_DEVICE;
	co_passage_page->params[0] = CO_DEVICE_SCSI;
	co_passage_page->params[1] = CO_SCSI_GET_CONFIG;
	co_passage_page->params[2] = 0;
	co_switch_wrapper();

	/* Get the result */
	if (!co_passage_page->params[0]) {
		for(x=0; x < CO_MODULE_MAX_COSCSI; x++) {
			if ((co_passage_page->params[x+1] & COSCSI_DEVICE_ENABLED) == 0)
				continue;
			dp = &devices[x];
			dp->unit = x;
			dp->type = co_passage_page->params[x+1] & 0x1f;
			dp->debug = 1;
			switch(dp->type) {
			case TYPE_DISK:
				dp->rom = &disk_rom;
				break;
			case TYPE_ROM:
			case TYPE_WORM:
				dp->rom = &cd_rom;
				break;
			case TYPE_PASS:
				dp->rom = (void *) ~0L;
				break;
			case TYPE_MEDIUM_CHANGER:
//				dp->rom = &changer_rom;
				break;
			case TYPE_TAPE:
//				dp->rom = &tape_rom;
				break;
			default:
				dp->unit = -1;
				break;
			}
		}
	}

	/* Release the page */
	co_passage_page_release(flags);

#if COSCSI_DUMP_CONFIG
	printk(KERN_INFO "SCSI: device configuration:\n");
	for(x=0; x < CO_MODULE_MAX_COSCSI; x++) {
		dp = &devices[x];
		printk(KERN_INFO "scsi%02d: type: %02d, rom: %p\n", dp->unit, dp->type, dp->rom);
	}
#endif

	/* Get shost */
	shost = scsi_host_alloc(&coscsi_template, sizeof(void *));
	if (!shost) {
		printk(KERN_ERR "coscsi_pci_probe: scsi_host_alloc failed");
		return -ENOMEM;
	}

	/* Set params */
	shost->irq = SCSI_IRQ;
	shost->max_id = CO_MODULE_MAX_COSCSI;
	shost->max_lun = 1;
	shost->max_channel = 0;

#if COSCSI_DUMP_PARAMS
#define SDUMP(s,f) printk(KERN_INFO "  %16s: %d\n", #f, (s)->f)
	printk(KERN_INFO "COSCSI: host parameters:\n");
	SDUMP(shost,max_id);
	SDUMP(shost,max_lun);
	SDUMP(shost,max_channel);
	SDUMP(shost,unique_id);
	SDUMP(&coscsi_template,can_queue);
	SDUMP(&coscsi_template,cmd_per_lun);
	SDUMP(&coscsi_template,sg_tablesize);
	SDUMP(&coscsi_template,max_sectors);
	SDUMP(&coscsi_template,use_clustering);
	SDUMP(shost,use_blk_tcq);
	SDUMP(shost,reverse_ordering);
	SDUMP(&coscsi_template,ordered_tag);
	SDUMP(&coscsi_template,max_host_blocked);
#undef SDUMP
#endif

	/* Add host */
	rc = scsi_add_host(shost, &pdev->dev);
	if (rc) {
		printk(KERN_ERR "coscsi_pci_probe: scsi_add_host failed");
		goto err_put;
	}
	pci_set_drvdata(pdev, shost);

	/* Scan devs */
	scsi_scan_host(shost);

	return 0;

err_put:
	scsi_host_put(shost);
	return rc;
}

/*
 * PCI Remove - hotplug removal
*/
static void __devexit coscsi_pci_remove(struct pci_dev *pdev)
{
	pci_set_drvdata(pdev, NULL);
}

/* We only support the COSCSI adapter :) */
static struct pci_device_id coscsi_pci_ids[] __devinitdata = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CO, PCI_DEVICE_ID_COSCSI) },
	{ 0 }
};

static struct pci_driver coscsi_pci_driver = {
	.name		= "coscsi",
	.id_table	= coscsi_pci_ids,
	.probe		= coscsi_pci_probe,			/* FIXME: Use bus_type methods */
	.remove		= __devexit_p(coscsi_pci_remove),	/* FIXME: Use bus_type methods */
};

extern int coio_test(void);

/*
 * PCI Init - module load
*/
static int __init coscsi_pci_init(void) {
	int rc;

	/* XXX COSCSI_VERSION better be <= 4 bytes */
	strncpy(scsi_rev, COSCSI_VERSION, 4);

	memset(&devices, 0, sizeof(devices));

	rc = request_irq(SCSI_IRQ, &coscsi_isr, IRQF_SAMPLE_RANDOM, "coscsi", NULL);
	if (rc) {
		printk(KERN_ERR "coscsi_pci_init: unable to get irq %d", SCSI_IRQ);
		return rc;
	}
	spin_lock_init(&coscsi_isr_lock);

#if COSCSI_DEBUG_PCI
	printk(KERN_INFO "coscsi_pci_init: registering...\n");
#endif
	return pci_register_driver(&coscsi_pci_driver);
}

/*
 * PCI Exit - module unload
*/
static void __exit coscsi_pci_exit(void) {
	register int x;

#if COSCSI_DEBUG_PCI
	printk(KERN_INFO "coscsi_pci_exit: closing handles\n");
#endif

	/* Close the handles */
	for(x=0; x < CO_MODULE_MAX_COSCSI; x++) host_close(&devices[x]);

	/* Unmap the page */

#if COSCSI_DEBUG_PCI
	printk(KERN_INFO "coscsi_pci_exit: exiting\n");
#endif
	pci_unregister_driver(&coscsi_pci_driver);
}

module_init(coscsi_pci_init);
module_exit(coscsi_pci_exit);
