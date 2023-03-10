/*
 *  Cooperative Linux virtual keyboard controller driver
 *
 *  Copyright (c) 1999-2002 Dan Aloni <da-x@colinux.org)
 *    Based on 98kbd-io.c written by Osamu Tomita>
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/serio.h>
#include <linux/sched.h>
#include <linux/kbd_kern.h>
#include <linux/cooperative_internal.h>

#include <asm/io.h>

MODULE_AUTHOR("Dan Aloni <da-x@colinux.org>");
MODULE_DESCRIPTION("Cooperative Linux virtual keyboard controller driver");
MODULE_LICENSE("GPL");

/*
 * Names.
 */

#define COKBD_PHYS_DESC        "cokbd"

static struct serio cokbd_port;

static irqreturn_t cokbdio_interrupt(int irq, void *dev_id);

/*
 * cokbd_write() sends a byte out through the keyboard interface.
 */

#define ATKBD_CMD_GETID		0x02f2

static int cokbd_write(struct serio *port, unsigned char c)
{
	return 0;
}

/*
 * cokbd_open() is called when a port is open by the higher layer.
 * It allocates the interrupt and enables in in the chip.
 */

static int cokbd_open(struct serio *port)
{
	if (request_irq(KEYBOARD_IRQ, cokbdio_interrupt, 0, "cokbd", NULL)) {
		printk(KERN_ERR "cobkd.c: Can't get irq %d for %s, unregistering the port.\n", KEYBOARD_IRQ, "KBD");
		serio_unregister_port(port);
		return -1;
	}

	return 0;
}

static void cokbd_close(struct serio *port)
{
	printk(KERN_INFO "cokbd closed\n");

	free_irq(KEYBOARD_IRQ, NULL);
}

/*
 * Structures for registering the devices in the serio.c module.
 */

static struct serio cokbd_port =
{
	.id.type =	SERIO_8042_XL,
	.write =	cokbd_write,
	.open =		cokbd_open,
	.close =	cokbd_close,
	.name =		"cokbd port",
	.phys =		COKBD_PHYS_DESC,
};

/*
 * cokbdio_interrupt() is the most important function in this driver -
 * it handles the interrupts from keyboard, and sends incoming bytes
 * to the upper layers.
 */

static irqreturn_t cokbdio_interrupt(int irq, void *dev_id)
{
	co_message_node_t *node_message;
	while (co_get_message(&node_message, CO_DEVICE_KEYBOARD)) {
		co_linux_message_t *message = (co_linux_message_t *)&node_message->msg.data;
		co_scan_code_t *sc = (co_scan_code_t *)message->data;
		unsigned long scancode = sc->code;

		switch (sc->mode)
		{
		    case CO_KBD_SCANCODE_RAW:
			serio_interrupt(&cokbd_port, scancode, 0);
			break;

		    case CO_KBD_SCANCODE_ASCII:
			keyboard_inject_utf8(scancode);
			break;
		}
		co_free_message(node_message);
	}

	return IRQ_HANDLED;
}

int __init cokbdio_init(void)
{
	serio_register_port(&cokbd_port);

	printk(KERN_INFO "serio: cokbd at irq %d\n", KEYBOARD_IRQ);

	return 0;
}

void __exit cokbdio_exit(void)
{
	serio_unregister_port(&cokbd_port);
}

module_init(cokbdio_init);
module_exit(cokbdio_exit);
