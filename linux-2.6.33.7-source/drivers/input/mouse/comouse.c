
/*
 * Cooperative mouse driver
 *
 * Copyright (c) 2007 Steve Shoecraft <sshoecraft@earthlink.net>
 * Copyright (c) 2005 Nuno Lucas <nuno.lucas@zmail.pt>
 * Copyright (c) 2004 Dan Aloni
 * Copyright (c) 1999-2001 Vojtech Pavlik
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/cooperative_internal.h>

#define COMOUSE_DEBUG 0

MODULE_AUTHOR("Steve Shoecraft <sshoecraft@earthlink.net");
MODULE_DESCRIPTION("Cooperative Mouse Driver");
MODULE_LICENSE("GPL");

static struct input_dev *comouse_dev;

static irqreturn_t comouse_isr(int irq, void *dev_id) {
	co_message_node_t *node_message;

	while ( co_get_message(&node_message, CO_DEVICE_MOUSE) ) {
		co_linux_message_t *message = (co_linux_message_t *)&node_message->msg.data;
		co_mouse_data_t* data = (co_mouse_data_t*) message->data;
		unsigned len = message->size;

		if ( sizeof(*data) != len ) {
			printk( KERN_ERR "comouse: Invalid data packet!\n" );
		} else {
			/* Movement */
			input_report_abs( comouse_dev, ABS_X, data->abs_x );
			input_report_abs( comouse_dev, ABS_Y, data->abs_y );

			/* Wheel */
			if ( data->rel_z ) input_report_rel( comouse_dev, REL_WHEEL, -data->rel_z );

			/* Buttons */
			input_report_key( comouse_dev, BTN_TOUCH, data->btns & 1 );
			input_report_key( comouse_dev, BTN_LEFT, data->btns & 1 );
			input_report_key( comouse_dev, BTN_RIGHT, data->btns & 2 );
			input_report_key( comouse_dev, BTN_MIDDLE, data->btns & 4 );

			input_sync( comouse_dev );
#if COMOUSE_DEBUG
			printk( KERN_DEBUG "comouse: x:%d y:%d buttons:%u wheel: %d.\n",
				data->abs_x, data->abs_y, data->btns, data->rel_z );
#endif
		}
		co_free_message(node_message);
	}

	return IRQ_HANDLED;
}

#if 0
static int comouse_open(struct input_dev *dev) {
	return 0;
}

static void comouse_close(struct input_dev *dev) {
	return;
}
#endif

static int __init comouse_init(void) {
	int err;

	comouse_dev = input_allocate_device();
	if (!comouse_dev) {
		printk(KERN_ERR "comouse.c: Not enough memory for input device\n");
		return -ENOMEM;
	}

	comouse_dev->name = "Cooperative Mouse";
	comouse_dev->phys = "comouse/input0";
	comouse_dev->id.bustype = BUS_HOST;
	comouse_dev->id.vendor  = 0x0001;
	comouse_dev->id.product = 0x0001;
	comouse_dev->id.version = 0x0100;
#if 0
	comouse_dev->open  = comouse_open;
	comouse_dev->close = comouse_close;
#endif

	/* Buttons */
	set_bit( EV_KEY, comouse_dev->evbit );
	set_bit( BTN_TOUCH , comouse_dev->keybit );
	set_bit( BTN_LEFT  , comouse_dev->keybit );
	set_bit( BTN_RIGHT , comouse_dev->keybit );
	set_bit( BTN_MIDDLE, comouse_dev->keybit );

	/* Movement */
	set_bit( EV_ABS, comouse_dev->evbit );
	input_set_abs_params(comouse_dev, ABS_X, 0, CO_MOUSE_MAX_X, 0, 0);
	input_set_abs_params(comouse_dev, ABS_Y, 0, CO_MOUSE_MAX_Y, 0, 0);

	/* Wheel */
	set_bit( EV_REL, comouse_dev->evbit );
	set_bit( REL_WHEEL, comouse_dev->relbit );

	err = input_register_device(comouse_dev);
	if (err) {
		printk(KERN_ERR "comouse: device registration failed!\n");
		input_free_device(comouse_dev);
		return err;
	}

	if (request_irq(MOUSE_IRQ, comouse_isr, 0, "comouse", NULL)) {
		printk(KERN_ERR "comouse: unable to allocate irq %d!\n", MOUSE_IRQ);
		return -EBUSY;
	}

#if COMOUSE_DEBUG
	printk(KERN_INFO "comouse: initialized.\n");
#endif
	return 0;
}

static void __exit comouse_exit(void) {
	free_irq(MOUSE_IRQ, NULL);
	input_unregister_device(comouse_dev);
}

module_init(comouse_init);
module_exit(comouse_exit);
