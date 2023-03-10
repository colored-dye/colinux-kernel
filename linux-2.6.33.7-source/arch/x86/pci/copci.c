/*
 *  Copyright (C) 2008 Steve Shoecraft
 *
 *  Cooperative Linux PCI Driver implementation
 */


#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/interrupt.h>
#include <linux/cooperative_pci.h>
#include <linux/cooperative_internal.h>
#include <asm/pci_x86.h>

#include <linux/copci.h>

#define COPCI_DEBUG 0
#define COPCI_DEBUG_IO 0

/* For PCI or other memory-mapped resources */
/* Symbol needed, dummy for coLinux. (copied from arch/x86/kernel/e820.c) */
unsigned long pci_mem_start = 0x10000000;
EXPORT_SYMBOL(pci_mem_start);

struct device_list {
	int type;
	int bus;
	int device;
	int func;
	unsigned char regs[256];
	struct device_list *next;
};

static struct device_list *devices = 0, *last_device;

#define pci_byte(r,l) *((unsigned char *)(&r[l]))
#define pci_short(r,l) *((unsigned short *)(&r[l]))
#define pci_long(r,l) *((unsigned long *)(&r[l]))

static int copci_read(unsigned int seg, unsigned int bus, unsigned int devfn, int reg, int len, u32 *value)
{
	int device, func;
	struct device_list *dp;

	/* Linux has encoded the device & func; split them */
	device = devfn >> 3;
	func = devfn & 7;

	if (reg + len > 255) {
		*value = -1;
		return -EINVAL;
	}

	spin_lock(&pci_config_lock);

	*value = 0;
	for(dp = devices; dp; dp = dp->next) {
		if (bus == dp->bus && device == dp->device && func == dp->func) {
#if COPCI_DEBUG_IO
			if (dp->type == CO_DEVICE_NETWORK) printk(KERN_INFO "copci_read: bus: %02x, devfn: %02x "
				"(device: %02x, func: %02x), reg: %02x, len: %d\n", bus, devfn, device, func, reg, len);
#endif
			switch(len) {
			case 1:
				*value = pci_byte(dp->regs, reg);
				break;
			case 2:
				*value = pci_short(dp->regs, reg);
				break;
			case 4:
				*value = pci_long(dp->regs, reg);
				break;
			}
#if COPCI_DEBUG_IO
			if (dp->type == CO_DEVICE_NETWORK) printk(KERN_INFO "copci_read: value: 0x%08x\n", *value);
#endif
		}
	}

	spin_unlock(&pci_config_lock);

	return 0;
}

static int copci_write(unsigned int seg, unsigned int bus, unsigned int devfn, int reg, int len, u32 value) {
	struct device_list *dp;
	int rc, device, func;

	device = devfn >> 3;
	func = devfn & 7;

	if (reg + len > 255) return -EINVAL;

	spin_lock(&pci_config_lock);

	rc = -EPERM;
	for(dp = devices; dp; dp = dp->next) {
		if (bus == dp->bus && device == dp->device && func == dp->func) {
#if COPCI_DEBUG_IO
			if (dp->type == CO_DEVICE_NETWORK) printk(KERN_INFO "copci_read: bus: %02x, devfn: %02x "
				"(device: %02x, func: %02x), reg: %02x, len: %d, value: %08X\n",
				bus, devfn, device, func, reg, len, value);
#endif
			switch(len) {
			case 1:
//				pci_byte(dp->regs, reg) = *value;
				break;
			case 2:
//				pci_short(dp->regs, reg) = *value;
				break;
			case 4:
//				pci_long(dp->regs, reg) = value;
				break;
			}
#if COPCI_DEBUG_IO
			if (dp->type == CO_DEVICE_NETWORK) printk(KERN_INFO "copci_read: value: 0x%08x\n", value);
#endif
		}
	}

	spin_unlock(&pci_config_lock);

	return rc;
}

struct pci_raw_ops copci_ops = {
	.read =         copci_read,
	.write =        copci_write,
};

static int get_mac(int unit, unsigned char *address)
{
	unsigned long flags;
	co_network_request_t *net_request;
	int result;

	co_passage_page_assert_valid();
	co_passage_page_acquire(&flags);
	co_passage_page->operation = CO_OPERATION_DEVICE;
	co_passage_page->params[0] = CO_DEVICE_NETWORK;
	net_request = (typeof(net_request))&co_passage_page->params[1];
	net_request->unit = unit;
	net_request->type = CO_NETWORK_GET_MAC;
	co_switch_wrapper();
	memcpy(address, net_request->mac_address, 6);
	result = net_request->result;
	co_passage_page_release(flags);

	return result;
}

#if 0
static int get_irq(int type) {
	unsigned long flags;
	co_network_request_t *net_request;
	int result, irq;

	co_passage_page_assert_valid();
	co_passage_page_acquire(&flags);
	co_passage_page->operation = CO_OPERATION_IRQ;
	co_passage_page->params[0] = type;
	co_switch_wrapper();
	irq = co_passage_page->params[0];
	co_passage_page_release(flags);
	result = co_passage_page->params[1];


	return result;
}
#endif

static int add_new(int b, int d, int f, int id, int class, int type, int irq) {
	struct device_list *newdev;

#if COPCI_DEBUG
	printk("add_new: d: %d, f: %d, id: %d, class: %x, type: %d, irq: %d\n", d, f, id, class, type, irq);
#endif
	newdev = kzalloc(sizeof(struct device_list), GFP_KERNEL);
	if (!newdev) {
		printk(KERN_ERR "COPCI: no memory for device info!\n");
		return -ENOMEM;
	}
	memset(newdev, 0, sizeof(*newdev));
	newdev->type = type;
	newdev->bus = b;
	newdev->device = d;
	newdev->func = f;
	pci_short(newdev->regs, PCI_VENDOR_ID) = PCI_VENDOR_ID_CO;
	pci_short(newdev->regs, PCI_DEVICE_ID) = id;
	pci_short(newdev->regs, PCI_COMMAND) = PCI_COMMAND_FAST_BACK;
	pci_short(newdev->regs, PCI_STATUS) = (PCI_STATUS_FAST_BACK | PCI_STATUS_DEVSEL_FAST);
	pci_short(newdev->regs, PCI_HEADER_TYPE) = 0x80;
	pci_short(newdev->regs, PCI_CLASS_DEVICE) = class;
	pci_byte(newdev->regs, PCI_INTERRUPT_LINE) = irq;
	pci_byte(newdev->regs, PCI_INTERRUPT_PIN) = 1;
	if (devices) {
		last_device->next = newdev;
		last_device = newdev;
	} else {
		devices = newdev;
		last_device = newdev;
	}

	return 0;
}

void pci_cooperative_init(void) {
	struct device_list *dp;
	copci_config_t *host_cp, *guest_cp, *cp;
	unsigned char addr[6];
	unsigned long flags;
	int x,id,class,count,unit,irq;
	const int max_count = COPCI_MAX_SLOTS * COPCI_MAX_FUNCS;

#if COPCI_DEBUG
	printk(KERN_INFO "COPCI: Initializing max slots:%d max func:%d size:%d\n",
		COPCI_MAX_SLOTS, COPCI_MAX_FUNCS, COPCI_MAX_SLOTS*COPCI_MAX_FUNCS*sizeof(*cp));
#endif

	guest_cp = kmalloc(max_count * sizeof(*cp), GFP_KERNEL);
	BUG_ON(!guest_cp);

	/* Get our config */
	co_passage_page_assert_valid();
	co_passage_page_acquire(&flags);
	co_passage_page->operation = CO_OPERATION_DEVICE;
	co_passage_page->params[0] = CO_DEVICE_PCI;
	co_passage_page->params[1] = COPCI_GET_CONFIG;
	co_passage_page->params[2] = 0;
	co_switch_wrapper();
	count = co_passage_page->params[0];
	BUG_ON(count>max_count);
	host_cp = (copci_config_t *) &co_passage_page->params[1];
	memcpy(guest_cp, host_cp, count * sizeof(*cp));
	co_passage_page_release(flags);

	cp = guest_cp;
	for(x=0; x < count; x++, cp++) {
		switch(cp->type) {
#ifdef CONFIG_COOPERATIVE_VIDEO
		case CO_DEVICE_VIDEO:
			id = PCI_DEVICE_ID_COVIDEO;
			class = PCI_CLASS_DISPLAY_OTHER;
			irq = 0;
			break;
#endif
#ifdef CONFIG_COOPERATIVE_AUDIO
		case CO_DEVICE_AUDIO:
			id = PCI_DEVICE_ID_COAUDIO;
			class = PCI_CLASS_MULTIMEDIA_AUDIO;
			irq = SOUND_IRQ;
			break;
#endif
		case CO_DEVICE_SCSI:
			id = PCI_DEVICE_ID_COSCSI;
			class = PCI_CLASS_STORAGE_SCSI;
			irq = SCSI_IRQ;
			break;
#ifdef CO_DEVICE_IDE
		case CO_DEVICE_IDE:
			id = PCI_DEVICE_ID_COIDE;
			class = PCI_CLASS_STORAGE_IDE;
			irq = 0x14;
			break;
#endif
		case CO_DEVICE_NETWORK:
			id = PCI_DEVICE_ID_CONET;
			class = PCI_CLASS_NETWORK_ETHERNET;
			irq = NETWORK_IRQ;
			break;
		default:
			id = class = irq = 0;
		}
		if (id) {
			add_new(0, cp->dev, cp->func, id, class, cp->type, irq);
			pci_byte(last_device->regs, PCI_CO_UNIT) = cp->unit;
		}
	}
	kfree(guest_cp);

#if COPCI_DEBUG
	printk(KERN_INFO "COPCI: config:\n");
	for(dp = devices; dp; dp = dp->next)
		printk(KERN_INFO "dev: %d, func: %d, type: %d\n", dp->device, dp->func, dp->type);
#endif

	/* For each network device, get the HW address */
	for(dp = devices; dp; dp = dp->next) {
		if (dp->type == CO_DEVICE_NETWORK) {
			unit = pci_byte(dp->regs, PCI_CO_UNIT);
			if (get_mac(unit, addr) != 0) {
#if COPCI_DEBUG
				printk(KERN_INFO "COPCI: got MAC for host unit %d\n", unit);
#endif
				pci_byte(dp->regs, PCI_CO_MAC1) = addr[0];
				pci_byte(dp->regs, PCI_CO_MAC2) = addr[1];
				pci_byte(dp->regs, PCI_CO_MAC3) = addr[2];
				pci_byte(dp->regs, PCI_CO_MAC4) = addr[3];
				pci_byte(dp->regs, PCI_CO_MAC5) = addr[4];
				pci_byte(dp->regs, PCI_CO_MAC6) = addr[5];
			}
		}
	}

	raw_pci_ops = &copci_ops;
}

int pci_set_dma_mask(struct pci_dev *dev, u64 mask)
{
	return -EIO;
}
EXPORT_SYMBOL(pci_set_dma_mask);

int pci_set_consistent_dma_mask(struct pci_dev *dev, u64 mask)
{
	return -EIO;
}
EXPORT_SYMBOL(pci_set_consistent_dma_mask);
