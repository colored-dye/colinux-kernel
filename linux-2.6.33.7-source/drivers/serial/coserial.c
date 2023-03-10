/*
 *  Copyright (C) 2004 Dan Aloni <da-x@colinux.org>
 *
 *  Cooperative Linux Serial Line implementation
 *
 *  Compatible with UML, also based on some code from there.
 *  Also based on The tiny_tty.c example driver by Greg Kroah-Hartman (greg@kroah.com).
 */

#include <linux/major.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/device.h>
#include <linux/console.h>
#include <linux/interrupt.h>

#include <linux/tty.h>
#include <linux/tty_flip.h>

#include <linux/cooperative_internal.h>

static irqreturn_t cocd_interrupt(int irq, void *dev_id);

struct cocd_tty {
	struct tty_struct	*tty;		/* tty for this device */
	unsigned		open_count;	/* open()/close() tally */
};

static struct tty_driver *cocd_driver;
DECLARE_MUTEX(cocd_sem);

static int cocd_open(struct tty_struct *tty, struct file * filp)
{
	struct cocd_tty *cocd;

	down(&cocd_sem);

	if (!(cocd = (struct cocd_tty *)tty->driver_data)) {
		int ret;

		ret = request_irq(SERIAL_IRQ, &cocd_interrupt, IRQF_SAMPLE_RANDOM, "coserial", NULL);
		if (ret) {
			printk(KERN_ERR "COSERIAL: unable to get irq %d", SERIAL_IRQ);
			up(&cocd_sem);
			return ret;
		}

		if(!(cocd = kmalloc(sizeof(*cocd), GFP_KERNEL))) {
			up(&cocd_sem);
			return -ENOMEM;
		}

		cocd->open_count = 0;
		cocd->tty = tty;
		tty->driver_data = cocd;
		tty->low_latency = 1;
	}

	cocd->open_count++;

	up(&cocd_sem);

	return 0;
}

static void cocd_close(struct tty_struct *tty, struct file * filp)
{
	struct cocd_tty *cocd;

	down(&cocd_sem);

	cocd = (struct cocd_tty *)tty->driver_data;
	if (!cocd) {
		printk(KERN_ERR "cocd: close no attached struct\n");
		goto out;
	}

	if (--cocd->open_count == 0) {
		/* last close, clear all */
		free_irq(SERIAL_IRQ, NULL);
		tty->driver_data = NULL;
		cocd->tty = NULL;
		kfree(cocd);
	}

out:
	up(&cocd_sem);
}

static irqreturn_t cocd_interrupt(int irq, void *dev_id)
{
	co_message_node_t *input;
	co_linux_message_t *message;
	struct tty_struct *tty;
	struct cocd_tty *cocd;
	int len;

	if (!cocd_driver)
		return IRQ_NONE;

	if (down_trylock(&cocd_sem))
		return IRQ_NONE;

	if(!co_get_message(&input, CO_DEVICE_SERIAL))
		goto out;

	if(!input)
		goto out;

	message = (co_linux_message_t *)&input->msg.data;
	if (message->unit < CO_MODULE_MAX_SERIAL
	 && (tty = cocd_driver->ttys[message->unit])
	 && (cocd = (struct cocd_tty *)tty->driver_data)) {
		up(&cocd_sem);

		len = tty_insert_flip_string(tty, message->data, message->size);
		if (len)
			tty_flip_buffer_push(tty);

		co_free_message(input);
		return IRQ_HANDLED;
	}
	co_free_message(input); /* message lost */
out:
	up(&cocd_sem);
	return IRQ_NONE;
}

static int do_cocd_write(int index, const char *buf, unsigned count)
{
	const char *kbuf_scan = buf;
	int count_left = count;

	while (count_left > 0) {
		int count_partial = count_left;
		if (count_partial > 1000)
			count_partial = 1000;

		co_send_message(CO_MODULE_LINUX,
				CO_MODULE_SERIAL0 + index,
				CO_PRIORITY_DISCARDABLE,
				CO_MESSAGE_TYPE_STRING,
				count_partial,
				kbuf_scan);

		count_left -= count_partial;
		kbuf_scan += count_partial;
	}

	return count;
}


static int cocd_write(struct tty_struct * tty, const unsigned char *buf, int count)
{
	return do_cocd_write(tty->index, buf, count);
}

static int cocd_write_room(struct tty_struct *tty)
{
	struct cocd_tty *cocd = (struct cocd_tty *)tty->driver_data;
	if (!cocd)
		return -ENODEV;
	return 255;
}

static void cocd_set_termios(struct tty_struct *tty, struct ktermios *old_termios)
{
}

static struct tty_operations cocd_ops = {
	.open = cocd_open,
	.close = cocd_close,
	.write = cocd_write,
	.write_room = cocd_write_room,
	.set_termios = cocd_set_termios,
};

static int __init cocd_init(void)
{
	int retval;

	cocd_driver = alloc_tty_driver(CO_MODULE_MAX_SERIAL);
	if (!cocd_driver) {
		printk(KERN_ERR "Couldn't allocate cocd driver");
		return -ENOMEM;
	}

	cocd_driver->owner = THIS_MODULE;
	cocd_driver->driver_name = "coserial";
	cocd_driver->name = "ttyS";
	cocd_driver->major = TTY_MAJOR;
	cocd_driver->minor_start = 64;
	cocd_driver->type = TTY_DRIVER_TYPE_SERIAL;
	cocd_driver->subtype = SERIAL_TYPE_NORMAL;
	cocd_driver->init_termios = tty_std_termios;
	cocd_driver->flags = 0;

	tty_set_operations(cocd_driver, &cocd_ops);

	retval = tty_register_driver(cocd_driver);
	if (retval) {
		printk(KERN_ERR "Couldn't register cocd driver");
		put_tty_driver(cocd_driver);
		return retval;
	}

	return 0;
}

static void __exit cocd_exit(void)
{
	tty_unregister_driver(cocd_driver);
	put_tty_driver(cocd_driver);
}

module_init(cocd_init);
module_exit(cocd_exit);

/*
 * ------------------------------------------------------------
 * Serial console driver
 * ------------------------------------------------------------
 */
#ifdef CONFIG_SERIAL_COOPERATIVE_CONSOLE
static void cocd_console_write(struct console *c, const char *buf,  unsigned count)
{
	do_cocd_write(c->index, buf, count);
}

static struct tty_driver *cocd_console_device(struct console *c, int *index)
{
        *index = c->index;
        return cocd_driver;
}

static struct console cocd_cons = {
        name:           "ttyS",
        write:          cocd_console_write,
        device:         cocd_console_device,
        flags:          CON_PRINTBUFFER,
        index:          -1,
};

/*
 *	Register console.
 */
static int __init cocd_console_init(void)
{
	register_console(&cocd_cons);
	return 0;
}
console_initcall(cocd_console_init);
#endif /* CONFIG_SERIAL_COOPERATIVE_CONSOLE */
