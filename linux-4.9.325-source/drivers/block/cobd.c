/*
 *  Copyright (C) 2003 Dan Aloni <da-x@colinux.org>
 *
 *  Cooperative Linux Block Device implementation
 */

#include <linux/major.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/major.h>
#include <linux/stat.h>
#include <linux/slab.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/cooperative_internal.h>
#include <linux/file.h>
#include <linux/ioctl.h>
#include <linux/ctype.h>
#include <linux/interrupt.h>

#include <asm/uaccess.h>
#include <asm/types.h>

static int const hardsect_size = 512;
static int const hardsect_size_shift = 9;
static int const cobd_max = CO_MODULE_MAX_COBD;
static spinlock_t cobd_lock = SPIN_LOCK_UNLOCKED;

struct cobd_device {
	int unit;
	int refcount;
	struct block_device *device;
};

static struct gendisk **cobd_disks;
static struct cobd_device cobd_devs[CO_MODULE_MAX_COBD];

static int cobd_request(struct cobd_device *cobd, co_block_request_type_t type, co_block_request_t *out_request)
{
	co_block_request_t *request;
	unsigned long flags;
	long rc;

	co_passage_page_assert_valid();
	co_passage_page_acquire(&flags);
	co_passage_page->operation = CO_OPERATION_DEVICE;
	co_passage_page->params[0] = CO_DEVICE_BLOCK;
	co_passage_page->params[1] = cobd->unit;
	request = (co_block_request_t *)&co_passage_page->params[2];
	request->type = type;
	request->rc = -1;
	co_switch_wrapper();
	rc = request->rc;
	*out_request = *request;
	co_passage_page_release(flags);

	return rc;
}

static int cobd_stat(struct cobd_device *cobd, co_block_request_t *out_request)
{
	return cobd_request(cobd, CO_BLOCK_STAT, out_request);
}

static int cobd_get_alias(struct cobd_device *cobd, co_block_request_t *out_request)
{
	return cobd_request(cobd, CO_BLOCK_GET_ALIAS, out_request);
}

static int cobd_ioctl(struct block_device *bdev, fmode_t mode,
		      unsigned int cmd, unsigned long arg)
{
	return -ENOTTY; /* unknown command */
}

static int cobd_open(struct block_device *bdev, fmode_t mode)
{
	struct cobd_device *cobd = bdev->bd_disk->private_data;
	co_block_request_t *co_request;
	co_block_request_t stat_request;
	unsigned long flags;
	int result;

	if (cobd->device && cobd->device != bdev)
		return -EBUSY;

	if (cobd->refcount == 0) {
		if (cobd_stat(cobd, &stat_request)) {
			return -ENODEV;
		}
	}

	result = 0;

	co_passage_page_assert_valid();
	co_passage_page_acquire(&flags);
	co_passage_page->operation = CO_OPERATION_DEVICE;
	co_passage_page->params[0] = CO_DEVICE_BLOCK;
	co_passage_page->params[1] = cobd->unit;
	co_request = (co_block_request_t *)&co_passage_page->params[2];
	co_request->type = CO_BLOCK_OPEN;
	co_switch_wrapper();
	if (co_request->rc)
		result = -EIO;
	else
		cobd->refcount++;
	co_passage_page_release(flags);

	if (result)
		return result;

	if (cobd->refcount == 1) {
		set_capacity(bdev->bd_disk, stat_request.disk_size >> hardsect_size_shift);
		cobd->device = bdev;
	}

	return 0;
}

static int cobd_release(struct gendisk *disk, fmode_t mode)
{
	struct cobd_device *cobd = disk->private_data;
	co_block_request_t *co_request;
	unsigned long flags;
	int ret = 0;

	co_passage_page_assert_valid();
	co_passage_page_acquire(&flags);
	co_passage_page->operation = CO_OPERATION_DEVICE;
	co_passage_page->params[0] = CO_DEVICE_BLOCK;
	co_passage_page->params[1] = cobd->unit;
	co_request = (co_block_request_t *)&co_passage_page->params[2];
	co_request->type = CO_BLOCK_CLOSE;
	co_switch_wrapper();
	if (co_request->rc)
		ret = -EIO;
	cobd->refcount--;
	co_passage_page_release(flags);

	if (cobd->refcount == 0)
		cobd->device = NULL;

	return ret;
}

/*
 * Handle an I/O request.
 */
static void cobd_transfer(struct request_queue *q, struct request *req)
{
	struct cobd_device *cobd = (struct cobd_device *)(req->rq_disk->private_data);
	co_block_request_t *co_request;
	unsigned long flags;
	int async;
	int ret;

next_segment:

	co_passage_page_assert_valid();
	co_passage_page_acquire(&flags);
	co_passage_page->operation = CO_OPERATION_DEVICE;
	co_passage_page->params[0] = CO_DEVICE_BLOCK;
	co_passage_page->params[1] = cobd->unit;
	co_request = (co_block_request_t *)&co_passage_page->params[2];
	co_request->type = (rq_data_dir(req) == READ) ? CO_BLOCK_READ : CO_BLOCK_WRITE;
	co_request->irq_request = req;
	co_request->offset = ((unsigned long long)blk_rq_pos(req)) << hardsect_size_shift;
	co_request->size = blk_rq_cur_bytes(req);
	co_request->address = req->buffer;
	co_request->rc = 0;
	co_request->async = 0;
	co_switch_wrapper();
	async = co_request->async;
	ret = co_request->rc;
	co_passage_page_release(flags);

	/*
	 * OK:   ret ==  0
	 * FAIL: ret == -1
	 */
	if (ret == CO_BLOCK_REQUEST_RETCODE_OK) {
		if (async)
			return; /* wait for interrupt */

		if (__blk_end_request_cur(req, 0))
			goto next_segment;

	} else {
		__blk_end_request_all(req, -EIO);
	}
}

static void do_cobd_request(struct request_queue *q)
{
        struct request *req;

        while ((req = blk_fetch_request(q)) != NULL) {
		if (!blk_fs_request(req)) {
			__blk_end_request_all(req, -EIO);
			continue;
		}

		cobd_transfer(q, req);
        }
}

static irqreturn_t cobd_interrupt(int irq, void *dev_id)
{
	co_message_node_t *input;

	while (co_get_message(&input, CO_DEVICE_BLOCK)) {
		co_linux_message_t *message;
		co_block_intr_t *intr;
		struct request *req;

		message = (co_linux_message_t *)&input->msg.data;
		if (message->unit >= CO_MODULE_MAX_COBD) {
			printk("cobd interrupt: buggy unit reception: %x\n", message->unit);
			goto goto_next_message;
		}

		BUG_ON(message->size != sizeof(co_block_intr_t));
		intr = (co_block_intr_t *)message->data;
		req = intr->irq_request;
		BUG_ON(!req);

		spin_lock(&cobd_lock);
		if (__blk_end_request_cur(req, intr->uptodate ? 0 : -EIO))
			cobd_transfer(req->q, req); /* next segment */
		else
			do_cobd_request(req->q);
		spin_unlock(&cobd_lock);

goto_next_message:
		co_free_message(input);
	}

	return IRQ_HANDLED;
}

static struct block_device_operations cobd_fops = {
	.owner   = THIS_MODULE,
	.open    = cobd_open,
	.release = cobd_release,
	.ioctl   = cobd_ioctl,
};

static int __init cobd_drives_init(void)
{
	int result, i;

	if (request_irq(BLOCKDEV_IRQ, &cobd_interrupt, 0, "cobd", NULL)) {
		printk("cobd: unable to get IRQ%d\n", BLOCKDEV_IRQ);
		return -EBUSY;
	}

	if (register_blkdev(COLINUX_MAJOR, "cobd")) {
		printk(KERN_WARNING "Unable to get major number %d for cobd device\n", COLINUX_MAJOR);
		result = -EIO;
		goto fail_irq;
	}

	result = -ENOMEM; /* for the possible errors */

	cobd_disks = kmalloc(cobd_max * sizeof(struct gendisk *), GFP_KERNEL);
	if (!cobd_disks)
		goto fail_malloc;

	for (i=0; i < cobd_max; i++) {
		cobd_disks[i] = alloc_disk(1);
		if (!cobd_disks[i])
			goto fail_malloc3;
	}

	for (i=0; i < cobd_max; i++) {
		struct cobd_device *cobd = &cobd_devs[i];
		struct gendisk *disk = cobd_disks[i];

		disk->queue = blk_init_queue(do_cobd_request, &cobd_lock);
		if (!disk->queue)
			goto fail_malloc4;

		blk_queue_logical_block_size(disk->queue, hardsect_size);

		cobd->unit = i;
		disk->major = COLINUX_MAJOR;
		disk->first_minor = i;
		disk->fops = &cobd_fops;
		sprintf(disk->disk_name, "cobd%d", i);
		disk->private_data = cobd;
	}

	for (i=0; i < cobd_max; i++)
		add_disk(cobd_disks[i]);

	printk(KERN_INFO "cobd: loaded (max %d devices)\n", cobd_max);
	return 0;

/* error path */
fail_malloc4:
	while (i--)
		blk_cleanup_queue(cobd_disks[i]->queue);
	i = cobd_max;

fail_malloc3:
	while (i--)
		if (cobd_disks[i] != NULL)
			put_disk(cobd_disks[i]);

	kfree(cobd_disks);

fail_malloc:
	unregister_blkdev(COLINUX_MAJOR, "cobd");

fail_irq:
	free_irq(BLOCKDEV_IRQ, NULL);
	return result;
}

struct cobd_alias_major {
	const char *name;
	int registered;
	int number;
};

struct cobd_alias {
	const char *name;
	struct cobd_alias_major *major;
	int minor_start;
	int minor_count;
	struct gendisk **gendisk;
};

struct cobd_alias_major cobd_aliases_major_ide0 = {
	.name = "ide0",
	.number = IDE0_MAJOR,
};

struct cobd_alias_major cobd_aliases_major_ide1 = {
	.name = "ide1",
	.number = IDE1_MAJOR,
};

struct cobd_alias_major cobd_aliases_major_ide2 = {
	.name = "ide2",
	.number = IDE2_MAJOR,
};

struct cobd_alias_major cobd_aliases_major_ide3 = {
	.name = "ide3",
	.number = IDE3_MAJOR,
};

struct cobd_alias_major cobd_aliases_major_sd = {
	.name = "sd",
	.number = SCSI_DISK0_MAJOR,
};

struct cobd_alias cobd_aliases[] = {
	{"hda", &cobd_aliases_major_ide0, 0x00, 21, },
	{"hdb", &cobd_aliases_major_ide0, 0x40, 21, },
	{"hdc", &cobd_aliases_major_ide1, 0x00, 21, },
	{"hdd", &cobd_aliases_major_ide1, 0x40, 21, },
	{"hde", &cobd_aliases_major_ide2, 0x00, 21, },
	{"hdf", &cobd_aliases_major_ide2, 0x40, 21, },
	{"hdg", &cobd_aliases_major_ide3, 0x00, 21, },
	{"hdh", &cobd_aliases_major_ide3, 0x40, 21, },
	{"sda", &cobd_aliases_major_sd, 0x00, 0x10, },
	{"sdb", &cobd_aliases_major_sd, 0x10, 0x10, },
	{"sdc", &cobd_aliases_major_sd, 0x20, 0x10, },
	{"sdd", &cobd_aliases_major_sd, 0x30, 0x10, },
	{"sde", &cobd_aliases_major_sd, 0x40, 0x10, },
	{"sdf", &cobd_aliases_major_sd, 0x50, 0x10, },
	{"sdg", &cobd_aliases_major_sd, 0x60, 0x10, },
	{"sdh", &cobd_aliases_major_sd, 0x70, 0x10, },
	{"sdi", &cobd_aliases_major_sd, 0x80, 0x10, },
	{"sdj", &cobd_aliases_major_sd, 0x90, 0x10, },
	{"sdk", &cobd_aliases_major_sd, 0xa0, 0x10, },
	{"sdl", &cobd_aliases_major_sd, 0xb0, 0x10, },
	{"sdm", &cobd_aliases_major_sd, 0xc0, 0x10, },
	{"sdn", &cobd_aliases_major_sd, 0xd0, 0x10, },
	{"sdo", &cobd_aliases_major_sd, 0xe0, 0x10, },
	{"sdp", &cobd_aliases_major_sd, 0xf0, 0x10, },
	{NULL, },
};

static int __init skip_atoi(const char **s)
{
	/* lib/spprintf.h */

        int i=0;

        while (isdigit(**s))
                i = i*10 + *((*s)++) - '0';

        return i;
}

static int __init cobd_spawn_alias(struct cobd_alias *alias,
				   const char *alias_name_requested,
				   int cobd_unit)
{
	const char *index_str_start = &alias_name_requested[strlen(alias->name)];
	const char *index_str_end = index_str_start;
	struct cobd_device *cobd;
	struct gendisk *disk;

	int index = skip_atoi(&index_str_end);

	if (!((index >= 0) && (index <= alias->minor_count))) {
		printk(KERN_WARNING "index out of bounds for alias %s (1 - %d)\n",
		       alias_name_requested, alias->minor_count);
		return -1;
	}

	if (alias->gendisk == NULL) {
		static struct gendisk **gendisks;

		gendisks = kzalloc(alias->minor_count * sizeof(struct gendisk *), GFP_KERNEL);
		if (!gendisks) {
			printk(KERN_WARNING "cannot allocate gendisk array for %s\n", alias->name);
			return -ENOMEM;
		}

		if (!alias->major->registered) {
			if (register_blkdev(alias->major->number, alias->major->name)) {
				printk(KERN_WARNING "unable to get major number %d for cobd alias device %s\n",
				       alias->major->number, alias_name_requested);
				kfree(gendisks);
				return -EIO;
			}

			alias->major->registered = 1;
		}

		alias->gendisk = gendisks;
	}

	if (alias->gendisk[index] != NULL) {
		printk(KERN_WARNING "alias %s already used\n", alias_name_requested);
		return -1;
	}

	disk = alloc_disk(1);
	if (!disk) {
		printk(KERN_WARNING "cannot allocate disk for alias %s\n", alias_name_requested);
		return -1;
	}

	disk->queue = blk_init_queue(do_cobd_request, &cobd_lock);
	if (!disk->queue) {
		printk(KERN_WARNING "cannot allocate init queue for alias %s\n", alias_name_requested);
		put_disk(disk);
		return -1;
	}

	cobd = &cobd_devs[cobd_unit];
	blk_queue_logical_block_size(disk->queue, hardsect_size);
	disk->major = alias->major->number;
	disk->first_minor = alias->minor_start + index;
	disk->fops = &cobd_fops;
	if (index)
		sprintf(disk->disk_name, "%s%d", alias->name, index);
	else
		sprintf(disk->disk_name, "%s", alias->name);
	disk->private_data = cobd;
	add_disk(disk);
	alias->gendisk[index] = disk;

	printk("cobd alias cobd%d -> %s created\n", cobd_unit, alias_name_requested);

	return 0;
}

static void __init cobd_aliases_init(void)
{
	int unit;
	co_block_request_t request;

	for (unit=0; unit < cobd_max; unit++) {
		struct cobd_alias *alias = cobd_aliases;
		int result = cobd_get_alias(&cobd_devs[unit], &request);
		if (result)
			continue;

		printk("alias for cobd%d is %s\n", unit, request.alias);

		while (alias->name) {
			const char *match = (strstr(request.alias, alias->name));
			if (match == request.alias) {
				cobd_spawn_alias(alias, request.alias, unit);
				break;
			}
			alias++;
		}

		if (alias->name == NULL)
			printk("alias %s is unknown (see cobd_aliases in cobd.c)\n", request.alias);
	}
}

static void cobd_drives_exit(void)
{
	int i;

	for (i = 0; i < cobd_max; i++) {
		blk_cleanup_queue(cobd_disks[i]->queue);
		del_gendisk(cobd_disks[i]);
		put_disk(cobd_disks[i]);
	}

	unregister_blkdev(COLINUX_MAJOR, "cobd");

	free_irq(BLOCKDEV_IRQ, NULL);
	kfree(cobd_disks);
}

static void cobd_aliases_exit(void)
{
	struct cobd_alias *alias = &cobd_aliases[0];
	while (alias->name != NULL) {
		int index;
		if (alias->gendisk == NULL) {
			alias++;
			continue;
		}

		for (index=0; index < alias->minor_count; index++) {
			struct gendisk *disk = alias->gendisk[index];
			if (!disk)
				return;

			blk_cleanup_queue(disk->queue);
			del_gendisk(disk);
			put_disk(disk);
		}

		if (!alias->major->registered) {
			unregister_blkdev(alias->major->number, alias->major->name);
			alias->major->registered = 0;
		}
		kfree(alias->gendisk);

		alias++;
	}
}

static int __init cobd_init(void)
{
	int result = cobd_drives_init();
	if (result)
		return result;

	cobd_aliases_init();

	return result;
}

static void cobd_exit(void)
{
	cobd_aliases_exit();
	cobd_drives_exit();
}

module_init(cobd_init);
module_exit(cobd_exit);


