/*
 *  linux/drivers/video/cocon.c -- Cooperative Linux console VGA driver
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive for
 *  more details.
 *
 *  Based on code copied from vgacon.c.
 *
 *  Dan Aloni <da-x@gmx.net>, 2003-2004 (c)
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <linux/console.h>
#include <linux/string.h>
#include <linux/kd.h>
#include <linux/slab.h>
#include <linux/vt_kern.h>
#include <linux/selection.h>
#include <linux/init.h>

#include <linux/cooperative_internal.h>

/*
 *  Interface used by the world
 */

static int cocon_cols = 80;
static int cocon_rows = 25;
static int cocon_attr = 0x07; /* fg=white, bg=black */

static const char __init *cocon_startup(void)
{
	unsigned long flags;
	co_console_message_t *message;
	co_message_t *co_message;

	co_message = co_send_message_save(&flags);
	if (co_message) {
		message = (co_console_message_t *)co_message->data;
		co_message->from = CO_MODULE_LINUX;
		co_message->to = CO_MODULE_CONSOLE;
		co_message->priority = CO_PRIORITY_DISCARDABLE;
		co_message->type = CO_MESSAGE_TYPE_STRING;
		co_message->size = ((char *)(&message->type + 1)) - ((char *)message);
		message->type = CO_OPERATION_CONSOLE_STARTUP;
		co_switch_wrapper();
		if (message->type == CO_OPERATION_CONSOLE_CONFIG) {
			cocon_cols = message->config.cols;
			cocon_rows = message->config.rows;
			cocon_attr = message->config.attr;
		}
		co_passage_page_release(flags);
	}

	return "CoCON";
}

static void cocon_init(struct vc_data *c, int init)
{
	unsigned long flags;
	co_console_message_t *message;
	co_message_t *co_message;

	/* We cannot be loaded as a module, therefore init is always 1 */
	c->vc_can_do_color = 1;
	c->vc_cols = cocon_cols;
	c->vc_rows = cocon_rows;

	/* drivers/char/vt.c: Must hack vc_init() for vc_def_color */
	c->vc_def_color = cocon_attr;

	c->vc_complement_mask = 0x7700;
	c->vc_visible_origin = 0;
	c->vc_origin = 0;

	co_message = co_send_message_save(&flags);
	if (!co_message)
		return;

	message = (co_console_message_t *)co_message->data;
	co_message->from = CO_MODULE_LINUX;
	co_message->to = CO_MODULE_CONSOLE;
	co_message->priority = CO_PRIORITY_DISCARDABLE;
	co_message->type = CO_MESSAGE_TYPE_STRING;
	co_message->size = ((char *)(&message->type + 1)) - ((char *)message);
	message->type = CO_OPERATION_CONSOLE_INIT;
	co_send_message_restore(flags);
}

static void cocon_deinit(struct vc_data *c)
{
	unsigned long flags;
	co_console_message_t *message;
	co_message_t *co_message;

	co_message = co_send_message_save(&flags);
	if (!co_message)
		return;

	message = (co_console_message_t *)co_message->data;
	co_message->from = CO_MODULE_LINUX;
	co_message->to = CO_MODULE_CONSOLE;
	co_message->priority = CO_PRIORITY_DISCARDABLE;
	co_message->type = CO_MESSAGE_TYPE_STRING;
	co_message->size = ((char *)(&message->type + 1)) - ((char *)message);
	message->type = CO_OPERATION_CONSOLE_DEINIT;
	co_send_message_restore(flags);

}

static void cocon_clear(struct vc_data *c, int top, int left, int rows, int cols)
{
	unsigned long flags;
	co_console_message_t *message;
	co_message_t *co_message;

	co_message = co_send_message_save(&flags);
	if (!co_message)
		return;

	message = (co_console_message_t *)co_message->data;
	co_message->from = CO_MODULE_LINUX;
	co_message->to = CO_MODULE_CONSOLE;
	co_message->priority = CO_PRIORITY_DISCARDABLE;
	co_message->type = CO_MESSAGE_TYPE_STRING;
	co_message->size = ((char *)(&message->clear + 1)) - ((char *)message);
	message->type = CO_OPERATION_CONSOLE_CLEAR;
	message->clear.top = top;
	message->clear.left = left;
	message->clear.bottom = top + rows - 1;
	message->clear.right = left + cols - 1;
	message->clear.charattr = c->vc_video_erase_char;
	co_send_message_restore(flags);
}

static void cocon_putc(struct vc_data *c, int charattr, int y, int x)
{
	unsigned long flags;
	co_message_t *co_message;
	co_console_message_t *message;

	co_message = co_send_message_save(&flags);
	if (!co_message)
		return;

	message = (co_console_message_t *)co_message->data;
	co_message->from = CO_MODULE_LINUX;
	co_message->to = CO_MODULE_CONSOLE;
	co_message->priority = CO_PRIORITY_DISCARDABLE;
	co_message->type = CO_MESSAGE_TYPE_STRING;
	co_message->size = ((char *)(&message->putc + 1)) - ((char *)message);
	message->type = CO_OPERATION_CONSOLE_PUTC;
	message->putc.x = x;
	message->putc.y = y;
	message->putc.charattr = charattr;
	co_send_message_restore(flags);
}


static void cocon_putcs(struct vc_data *conp,
			const unsigned short *s, int count, int yy, int xx)
{
	unsigned long flags;
	co_console_message_t *message;
	co_message_t *co_message;

//	if (count > CO_MAX_PARAM_SIZE/2 - 16)
//		return;

	co_message = co_send_message_save(&flags);
	if (!co_message)
		return;

	message = (co_console_message_t *)co_message->data;
	co_message->from = CO_MODULE_LINUX;
	co_message->to = CO_MODULE_CONSOLE;
	co_message->priority = CO_PRIORITY_DISCARDABLE;
	co_message->type = CO_MESSAGE_TYPE_STRING;
	co_message->size = ((char *)(&message->putcs + 1)) - ((char *)message) +
		count * sizeof(unsigned short);
	message->type = CO_OPERATION_CONSOLE_PUTCS;
	message->putcs.x = xx;
	message->putcs.y = yy;
	message->putcs.count = count;
	memcpy(&message->putcs.data, s, count * sizeof(unsigned short));
	co_send_message_restore(flags);
}

static u8 cocon_build_attr(struct vc_data *c, u8 color, u8 intensity, u8 blink, u8 underline, u8 reverse, u8 italic)
{
	u8 attr = color;

	if (underline)
		attr = (attr & 0xf0) | c->vc_ulcolor;
	else if (intensity == 0)
		attr = (attr & 0xf0) | c->vc_halfcolor;
	if (reverse)
		attr = ((attr) & 0x88) | ((((attr) >> 4) | ((attr) << 4)) & 0x77);
	if (blink)
		attr ^= 0x80;
	if (intensity == 2)
		attr ^= 0x08;

	return attr;
}

static void cocon_invert_region(struct vc_data *c, u16 *p, int count)
{
	unsigned long flags;
	co_message_t *co_message;
	co_console_message_t *message;
	unsigned long x = (unsigned long)(p - c->vc_origin);  // UPDATE: vc_origin = 0; but not yet

	co_message = co_send_message_save(&flags);
	if (co_message) {
		message = (co_console_message_t *)co_message->data;
		co_message->from = CO_MODULE_LINUX;
		co_message->to = CO_MODULE_CONSOLE;
		co_message->priority = CO_PRIORITY_DISCARDABLE;
		co_message->type = CO_MESSAGE_TYPE_STRING;
		co_message->size = ((char *)(&message->invert + 1)) - ((char *)message);
		message->type = CO_OPERATION_CONSOLE_INVERT_REGION;
		message->invert.y = ((unsigned)x)/c->vc_cols;
		message->invert.x = ((unsigned)x)-(message->invert.y);
		message->invert.count = count;
		co_send_message_restore(flags);
	}

	while (count--) {
		u16 a = scr_readw(p);
		a = ((a) & 0x88ff) | (((a) & 0x7000) >> 4) | (((a) & 0x0700) << 4);
		scr_writew(a, p++);
        }

}

static void cocon_cursor(struct vc_data *c, int mode)
{
	unsigned long flags;
	co_console_message_t *message;
	co_message_t *co_message;

	co_message = co_send_message_save(&flags);
	if (!co_message)
		return;

	message = (co_console_message_t *)co_message->data;
	co_message->from = CO_MODULE_LINUX;
	co_message->to = CO_MODULE_CONSOLE;
	co_message->priority = CO_PRIORITY_DISCARDABLE;
	co_message->type = CO_MESSAGE_TYPE_STRING;
	co_message->size = ((char *)(&message->cursor + 1)) - ((char *)message);;
	if (mode==CM_ERASE) {
		message->type = CO_OPERATION_CONSOLE_CURSOR_ERASE;
		message->cursor.height = CUR_NONE;
		co_send_message_restore(flags);
		return;
	}

	if(mode==CM_MOVE) {
		message->type = CO_OPERATION_CONSOLE_CURSOR_MOVE;
	} else /*(mode==CM_DRAW)*/ {
		message->type = CO_OPERATION_CONSOLE_CURSOR_DRAW;
	}
	message->cursor.x = c->vc_x;
	message->cursor.y = c->vc_y;
	message->cursor.height = c->vc_cursor_type & CUR_HWMASK;

	co_send_message_restore(flags);
}

static int cocon_switch(struct vc_data *c)
{
	unsigned long flags;
	co_console_message_t *message;
	co_message_t *co_message;

	co_message = co_send_message_save(&flags);
	if (co_message) {
		message = (co_console_message_t *)co_message->data;
		co_message->from = CO_MODULE_LINUX;
		co_message->to = CO_MODULE_CONSOLE;
		co_message->priority = CO_PRIORITY_DISCARDABLE;
		co_message->type = CO_MESSAGE_TYPE_STRING;
		co_message->size = ((char *)(&message->type + 1)) - ((char *)message);
		message->type = CO_OPERATION_CONSOLE_SWITCH;
		co_send_message_restore(flags);
	}

	return 1;	/* Redrawing not needed */
}

static int cocon_set_palette(struct vc_data *c, unsigned char *table)
{
	unsigned long flags;
	co_console_message_t *message;
	co_message_t *co_message;

	co_message = co_send_message_save(&flags);
	if (co_message) {
		message = (co_console_message_t *)co_message->data;
		co_message->from = CO_MODULE_LINUX;
		co_message->to = CO_MODULE_CONSOLE;
		co_message->priority = CO_PRIORITY_DISCARDABLE;
		co_message->type = CO_MESSAGE_TYPE_STRING;
		co_message->size = ((char *)(&message->type + 1)) - ((char *)message);
		message->type = CO_OPERATION_CONSOLE_SET_PALETTE;
		co_send_message_restore(flags);
	}

	return 1;
}

static int cocon_blank(struct vc_data *c, int blank, int mode_switchg)
{
	unsigned long flags;
	co_console_message_t *message;
	co_message_t *co_message;

	co_message = co_send_message_save(&flags);
	if (co_message) {
		message = (co_console_message_t *)co_message->data;
		co_message->from = CO_MODULE_LINUX;
		co_message->to = CO_MODULE_CONSOLE;
		co_message->priority = CO_PRIORITY_DISCARDABLE;
		co_message->type = CO_MESSAGE_TYPE_STRING;
		co_message->size = ((char *)(&message->type + 1)) - ((char *)message);
		message->type = CO_OPERATION_CONSOLE_BLANK;
		co_send_message_restore(flags);
	}

	return 1;
}


static int cocon_scrolldelta(struct vc_data *c, int lines)
{
	unsigned long flags;
	co_console_message_t *message;
	co_message_t *co_message;

	co_message = co_send_message_save(&flags);
	if (co_message) {
		message = (co_console_message_t *)co_message->data;
		co_message->from = CO_MODULE_LINUX;
		co_message->to = CO_MODULE_CONSOLE;
		co_message->priority = CO_PRIORITY_DISCARDABLE;
		co_message->type = CO_MESSAGE_TYPE_STRING;
		co_message->size = ((char *)(&message->type + 1)) - ((char *)message);
		message->type = CO_OPERATION_CONSOLE_SCROLLDELTA;
		co_send_message_restore(flags);
	}

	return 1;
}

static int cocon_set_origin(struct vc_data *c)
{
	unsigned long flags;
	co_console_message_t *message;
	co_message_t *co_message;

	co_message = co_send_message_save(&flags);
	if (co_message) {
		message = (co_console_message_t *)co_message->data;
		co_message->from = CO_MODULE_LINUX;
		co_message->to = CO_MODULE_CONSOLE;
		co_message->priority = CO_PRIORITY_DISCARDABLE;
		co_message->type = CO_MESSAGE_TYPE_STRING;
		co_message->size = ((char *)(&message->type + 1)) - ((char *)message);
		message->type = CO_OPERATION_CONSOLE_SET_ORIGIN;
		co_send_message_restore(flags);
	}

	return 1;
}

static void cocon_save_screen(struct vc_data *c)
{
	unsigned long flags;
	co_console_message_t *message;
	co_message_t *co_message;

	co_message = co_send_message_save(&flags);
	if (co_message) {
		message = (co_console_message_t *)co_message->data;
		co_message->from = CO_MODULE_LINUX;
		co_message->to = CO_MODULE_CONSOLE;
		co_message->priority = CO_PRIORITY_DISCARDABLE;
		co_message->type = CO_MESSAGE_TYPE_STRING;
		co_message->size = ((char *)(&message->type + 1)) - ((char *)message);
		message->type = CO_OPERATION_CONSOLE_SAVE_SCREEN;
		co_send_message_restore(flags);
	}
}

static int cocon_scroll(struct vc_data *c, int t, int b, int dir, int lines)
{
	unsigned long flags;
	co_console_message_t *message;
	co_message_t *co_message;

	co_message = co_send_message_save(&flags);
	if (co_message) {
		message = (co_console_message_t *)co_message->data;
		co_message->from = CO_MODULE_LINUX;
		co_message->to = CO_MODULE_CONSOLE;
		co_message->priority = CO_PRIORITY_DISCARDABLE;
		co_message->type = CO_MESSAGE_TYPE_STRING;
		co_message->size = ((char *)(&message->scroll + 1)) - ((char *)message);
		if (dir == SM_UP)
			message->type = CO_OPERATION_CONSOLE_SCROLL_UP;
		else
			message->type = CO_OPERATION_CONSOLE_SCROLL_DOWN;
		message->scroll.top = t;
		message->scroll.bottom = b-1;
		message->scroll.lines = lines;
		message->scroll.charattr = c->vc_video_erase_char;
		co_send_message_restore(flags);
	}

	return 0;
}

static void cocon_bmove(struct vc_data *c, int sy, int sx, int dy, int dx, int h, int w)
{
	unsigned long flags;
	co_console_message_t *message;
	co_message_t *co_message;

	co_message = co_send_message_save(&flags);
	if (!co_message)
		return;

	message = (co_console_message_t *)co_message->data;
	co_message->from = CO_MODULE_LINUX;
	co_message->to = CO_MODULE_CONSOLE;
	co_message->priority = CO_PRIORITY_DISCARDABLE;
	co_message->type = CO_MESSAGE_TYPE_STRING;
	co_message->size = ((char *)(&message->bmove + 1)) - ((char *)message);
	message->type = CO_OPERATION_CONSOLE_BMOVE;
	message->bmove.row = dy;
	message->bmove.column = dx;
	message->bmove.top = sy;
	message->bmove.left = sx;
	message->bmove.bottom = sy + h - 1;
	message->bmove.right = sx + w - 1;
	co_send_message_restore(flags);
}

static int cocon_resize(struct vc_data *vc, unsigned int width,
			unsigned int height, unsigned int user)
{
	return -EINVAL;
}

/*
 *  The console `switch' structure for the VGA based console
 */

const struct consw colinux_con = {
	con_startup:		cocon_startup,
	con_init:		cocon_init,
	con_deinit:		cocon_deinit,
	con_clear:		cocon_clear,
	con_putc:		cocon_putc,
	con_putcs:		cocon_putcs,
	con_cursor:		cocon_cursor,
	con_scroll:		cocon_scroll,
	con_bmove:		cocon_bmove,
	con_switch:		cocon_switch,
	con_blank:		cocon_blank,
	con_resize: 		cocon_resize,
	con_set_palette:	cocon_set_palette,
	con_scrolldelta:	cocon_scrolldelta,
	con_set_origin:		cocon_set_origin,
	con_save_screen:	cocon_save_screen,
	con_build_attr:		cocon_build_attr,
	con_invert_region:	cocon_invert_region,
};

MODULE_LICENSE("GPL");
