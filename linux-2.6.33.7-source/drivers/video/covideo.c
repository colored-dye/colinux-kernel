
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fb.h>

#include <linux/cooperative.h>
#include <linux/cooperative_internal.h>
#include <linux/cooperative_pci.h>

#include <linux/covideo.h>

#include <stdarg.h>

MODULE_AUTHOR("Steve Shoecraft <sshoecraft@earthlink.net>");
MODULE_DESCRIPTION("Cooperative Linux Video Driver");
MODULE_LICENSE("GPL");

#define COVIDEO_DEBUG 0
#define COVIDEO_MEMTEST 0

#if COVIDEO_DEBUG
#define dprintk(m) printk m
#else
#define dprintk(m) /* noop */
#endif

/* Our info */
struct covideo_par {
	int unit;
	void *buffer;
	int size;
	struct pci_dev *pdev;
	struct mutex open_lock;
};
typedef struct covideo_par covideo_par_t;

static void uprintk(int unit, char *fmt, ...) {
	char line[1024],*p;
	va_list ap;

	sprintf(line,"covideo%d: ", unit);
	p = line + strlen(line);

	va_start(ap,fmt);
	vsprintf(p,fmt,ap);
	va_end(ap);
}

static struct fb_var_screeninfo covideo_default = {
        .xres =         640,
        .yres =         480,
        .xres_virtual = 640,
        .yres_virtual = 480,
        .bits_per_pixel = 8,
        .red =          { 0, 8, 0 },
        .green =        { 0, 8, 0 },
        .blue =         { 0, 8, 0 },
        .activate =     FB_ACTIVATE_TEST,
        .height =       -1,
        .width =        -1,
        .pixclock =     20000,
        .left_margin =  64,
        .right_margin = 64,
        .upper_margin = 32,
        .lower_margin = 32,
        .hsync_len =    64,
        .vsync_len =    2,
        .vmode =        FB_VMODE_NONINTERLACED,
};

static struct fb_fix_screeninfo covideo_fix = {
        .id =           "CoVideo",
        .type =         FB_TYPE_PACKED_PIXELS,
        .visual =       FB_VISUAL_PSEUDOCOLOR,
        .xpanstep =     1,
        .ypanstep =     1,
        .ywrapstep =    1,
        .accel =        FB_ACCEL_NONE,
};

static int covideo_open(struct fb_info *info, int user)
{
	covideo_par_t *par = info->par;

	printk(KERN_INFO "covideo%d: open: user: %d\n", par->unit, user);
	return 0;
}

static int covideo_release(struct fb_info *info, int user)
{
	covideo_par_t *par = info->par;

	printk(KERN_INFO "covideo%d: close: user: %d\n", par->unit, user);
	return 0;
}

static u_long get_line_length(int xres_virtual, int bpp)
{
        u_long length;

        length = xres_virtual * bpp;
        length = (length + 31) & ~31;
        length >>= 3;
        return (length);
}

static int covideo_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct covideo_par *par = info->par;
        u_long line_length;

//	uprintk("checking var...\n");

//	printk("check_var: xres: %d, yres: %d, bpp: %d\n", var->xres, var->yres, var->bits_per_pixel);

	/* We only do 32 BPP */
//	if (var->bits_per_pixel != 32) return -EINVAL;

        /*
         *  FB_VMODE_CONUPDATE and FB_VMODE_SMOOTH_XPAN are equal!
         *  as FB_VMODE_SMOOTH_XPAN is only used internally
         */
        if (var->vmode & FB_VMODE_CONUPDATE) {
                var->vmode |= FB_VMODE_YWRAP;
                var->xoffset = info->var.xoffset;
                var->yoffset = info->var.yoffset;
        }

        /*
         *  Some very basic checks
         */
        if (!var->xres)
                var->xres = 1;
        if (!var->yres)
                var->yres = 1;
        if (var->xres > var->xres_virtual)
                var->xres_virtual = var->xres;
        if (var->yres > var->yres_virtual)
                var->yres_virtual = var->yres;
        if (var->bits_per_pixel <= 1)
                var->bits_per_pixel = 1;
        else if (var->bits_per_pixel <= 8)
                var->bits_per_pixel = 8;
        else if (var->bits_per_pixel <= 16)
                var->bits_per_pixel = 16;
        else if (var->bits_per_pixel <= 24)
                var->bits_per_pixel = 24;
        else if (var->bits_per_pixel <= 32)
                var->bits_per_pixel = 32;
        else
                return -EINVAL;

        if (var->xres_virtual < var->xoffset + var->xres)
                var->xres_virtual = var->xoffset + var->xres;
        if (var->yres_virtual < var->yoffset + var->yres)
                var->yres_virtual = var->yoffset + var->yres;

        line_length =
            get_line_length(var->xres_virtual, var->bits_per_pixel);
        if (line_length * var->yres_virtual > par->size)
                return -ENOMEM;

        /*
         * Now that we checked it we alter var. The reason being is that the video
         * mode passed in might not work but slight changes to it might make it
         * work. This way we let the user know what is acceptable.
         */
        switch (var->bits_per_pixel) {
        case 1:
        case 8:
                var->red.offset = 0;
                var->red.length = 8;
                var->green.offset = 0;
                var->green.length = 8;
                var->blue.offset = 0;
                var->blue.length = 8;
                var->transp.offset = 0;
                var->transp.length = 0;
                break;
        case 16:                /* RGBA 5551 */
                if (var->transp.length) {
                        var->red.offset = 0;
                        var->red.length = 5;
                        var->green.offset = 5;
                        var->green.length = 5;
                        var->blue.offset = 10;
                        var->blue.length = 5;
                        var->transp.offset = 15;
                        var->transp.length = 1;
                } else {        /* RGB 565 */
                        var->red.offset = 0;
                        var->red.length = 5;
                        var->green.offset = 5;
                        var->green.length = 6;
                        var->blue.offset = 11;
                        var->blue.length = 5;
                        var->transp.offset = 0;
                        var->transp.length = 0;
                }
                break;
        case 24:                /* RGB 888 */
                var->red.offset = 0;
                var->red.length = 8;
                var->green.offset = 8;
                var->green.length = 8;
                var->blue.offset = 16;
                var->blue.length = 8;
                var->transp.offset = 0;
                var->transp.length = 0;
                break;
        case 32:                /* RGBA 8888 */
                var->red.offset = 0;
                var->red.length = 8;
                var->green.offset = 8;
                var->green.length = 8;
                var->blue.offset = 16;
                var->blue.length = 8;
                var->transp.offset = 24;
                var->transp.length = 8;
                break;
        }
        var->red.msb_right = 0;
        var->green.msb_right = 0;
        var->blue.msb_right = 0;
        var->transp.msb_right = 0;

	return 0;
}

static int covideo_set_par(struct fb_info *info)
{
//	struct covideo_par *par = info->par;

//	uprintk(par->unit, "setting par...\n");

	printk("set_par: xres: %d, yres: %d, bpp: %d\n", info->var.xres, info->var.yres, info->var.bits_per_pixel);
        info->fix.line_length = get_line_length(info->var.xres_virtual, info->var.bits_per_pixel);
	return 0;
}

static int covideo_setcolreg(unsigned regno, unsigned red, unsigned green, unsigned blue, unsigned transp, struct fb_info *info)
{
	struct covideo_par *par = info->par;

	uprintk(par->unit, "setting coloreg...\n");

        if (regno >= 256)       /* no. of hw registers */
                return 1;
        /*
         * Program hardware... do anything you want with transp
         */

        /* grayscale works only partially under directcolor */
        if (info->var.grayscale) {
                /* grayscale = 0.30*R + 0.59*G + 0.11*B */
                red = green = blue =
                    (red * 77 + green * 151 + blue * 28) >> 8;
        }

        /* Directcolor:
         *   var->{color}.offset contains start of bitfield
         *   var->{color}.length contains length of bitfield
         *   {hardwarespecific} contains width of RAMDAC
         *   cmap[X] is programmed to (X << red.offset) | (X << green.offset) | (X << blue.offset)
         *   RAMDAC[X] is programmed to (red, green, blue)
         *
         * Pseudocolor:
         *    uses offset = 0 && length = RAMDAC register width.
         *    var->{color}.offset is 0
         *    var->{color}.length contains widht of DAC
         *    cmap is not used
         *    RAMDAC[X] is programmed to (red, green, blue)
         * Truecolor:
         *    does not use DAC. Usually 3 are present.
         *    var->{color}.offset contains start of bitfield
         *    var->{color}.length contains length of bitfield
         *    cmap is programmed to (red << red.offset) | (green << green.offset) |
         *                      (blue << blue.offset) | (transp << transp.offset)
         *    RAMDAC does not exist
         */
#define CNVT_TOHW(val,width) ((((val)<<(width))+0x7FFF-(val))>>16)
        switch (info->fix.visual) {
        case FB_VISUAL_TRUECOLOR:
        case FB_VISUAL_PSEUDOCOLOR:
                red = CNVT_TOHW(red, info->var.red.length);
                green = CNVT_TOHW(green, info->var.green.length);
                blue = CNVT_TOHW(blue, info->var.blue.length);
                transp = CNVT_TOHW(transp, info->var.transp.length);
                break;
        case FB_VISUAL_DIRECTCOLOR:
                red = CNVT_TOHW(red, 8);        /* expect 8 bit DAC */
                green = CNVT_TOHW(green, 8);
                blue = CNVT_TOHW(blue, 8);
                /* hey, there is bug in transp handling... */
                transp = CNVT_TOHW(transp, 8);
                break;
        }
#undef CNVT_TOHW
        /* Truecolor has hardware independent palette */
        if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
                u32 v;

                if (regno >= 16)
                        return 1;

                v = (red << info->var.red.offset) |
                    (green << info->var.green.offset) |
                    (blue << info->var.blue.offset) |
                    (transp << info->var.transp.offset);
                switch (info->var.bits_per_pixel) {
                case 8:
                        break;
                case 16:
                        ((u32 *) (info->pseudo_palette))[regno] = v;
                        break;
                case 24:
                case 32:
                        ((u32 *) (info->pseudo_palette))[regno] = v;
                        break;
                }
        }

	return 0;
}

static int covideo_blank(int blank_mode, struct fb_info *info)
{
	return 0;
}

static int covideo_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
        unsigned long start = vma->vm_start;
        unsigned long size  = vma->vm_end-vma->vm_start;
        unsigned long page, pos;

	if (size > info->screen_size) return -EINVAL;

        pos = (unsigned long) info->screen_base;
        while (size > 0) {
                page = vmalloc_to_pfn((void *)pos);
                if (remap_pfn_range(vma, start, page, PAGE_SIZE, PAGE_SHARED))
                        return -EAGAIN;

                start += PAGE_SIZE;
                pos += PAGE_SIZE;
                if (size > PAGE_SIZE)
                        size -= PAGE_SIZE;
                else
                        size = 0;
        }

        return 0;
}

/* Frame buffer operations */
static struct fb_ops covideo_ops = {
	.owner		= THIS_MODULE,
	.fb_open	= covideo_open,
	.fb_release	= covideo_release,
	.fb_read	= fb_sys_read,
	.fb_write	= fb_sys_write,
	.fb_check_var	= covideo_check_var,
	.fb_set_par	= covideo_set_par,
	.fb_setcolreg	= covideo_setcolreg,
	.fb_blank	= covideo_blank,
	.fb_fillrect	= sys_fillrect,
	.fb_copyarea	= sys_copyarea,
	.fb_imageblit	= sys_imageblit,
	.fb_mmap	= covideo_mmap,

#if 0
	/* XXX currently not implemented */
	.fb_setcmap	= covideo_setcmap,
	.fb_pan_display = covideo_pan_display,
	.fb_cursor	= covideo_cursor,
	.fb_rotate 	= covideo_rotate,
        .fb_get_caps    = svga_get_caps,
#endif
};

#if COVIDEO_MEMTEST
/* Simple page-level checkerboard test */
static int test_buffer(void *buffer, int unit, int size) {
	unsigned char *p, *t, *t0, *t1;
	unsigned long flags;
	int npages,rc;
	register int i;

	printk(KERN_INFO "covideo%d: testing buffer at 0x%p (size: %d)\n", unit, buffer, size);
	rc = 1;
	t1 = 0;
	if ((t0 = kmalloc(PAGE_SIZE, GFP_KERNEL)) == 0) goto test_out;
	memset(t0, 0, PAGE_SIZE);
	if ((t1 = kmalloc(PAGE_SIZE, GFP_KERNEL)) == 0) goto test_out;
	memset(t1, 0xFF, PAGE_SIZE);
	npages = size >> PAGE_SHIFT;

	p = buffer;
	for(i=0; i < npages; i++) {
		t = (i & 1 ? t1 : t0);
		memcpy(p, t, PAGE_SIZE);
		p += PAGE_SIZE;
	}

	co_passage_page_assert_valid();
	co_passage_page_acquire(&flags);
	co_passage_page->operation = CO_OPERATION_DEVICE;
	co_passage_page->params[0] = CO_DEVICE_VIDEO;
	co_passage_page->params[1] = CO_VIDEO_TEST;
	co_passage_page->params[2] = unit;
	co_switch_wrapper();
	rc = co_passage_page->params[0];
	co_passage_page_release(flags);

	/* Fail on host side? */
	if (rc) goto test_out;

	p = buffer;
	for(i=0; i < npages; i++) {
		t = (i & 1 ? t0 : t1);
		if (memcmp(p, t, PAGE_SIZE) != 0)
			goto test_out;
		p += PAGE_SIZE;
	}

	rc = 0;

test_out:
	if (t0) kfree(t0);
	if (t1) kfree(t1);
	printk(KERN_INFO "covideo%d: test %s!\n", unit, (rc ? "failed" : "passed"));
	return rc;
}
#endif

/*
 * PCI Probe - probe for a single device
*/
static int __devinit covideo_pci_probe( struct pci_dev *pdev, const struct pci_device_id *id )
{
	unsigned long flags;
	covideo_config_t *cp;
        struct fb_info *info;
        covideo_par_t *par;
	void *host_buffer, *buffer;
	u8 unit;
        int rc, size;

	/* Get our host unit */
	pci_read_config_byte(pdev, PCI_CO_UNIT, &unit);

	/* Get our config */
	co_passage_page_assert_valid();
	co_passage_page_acquire(&flags);
	co_passage_page->operation = CO_OPERATION_DEVICE;
	co_passage_page->params[0] = CO_DEVICE_VIDEO;
	co_passage_page->params[1] = CO_VIDEO_GET_CONFIG;
	co_passage_page->params[2] = unit;
	co_switch_wrapper();
	rc = co_passage_page->params[0];
	cp = (covideo_config_t *) &co_passage_page->params[1];
	host_buffer = cp->buffer;
	size = cp->size;
	co_passage_page_release(flags);

	/* If unable to get size, silently skip this device */
	if (rc) return 0;

	/* Map host buffer into our space */
	buffer = co_map_buffer(host_buffer, size);
	if (!buffer) {
		printk(KERN_ERR "covideo%d: unable to map video buffer!\n", unit);
		return 0;
	}

#if COVIDEO_DEBUG
	printk(KERN_INFO "covideo%d: buffer: %p, size: %d\n", unit, buffer, size);
#endif

#if COVIDEO_MEMTEST
	/* Test buffer */
	if (test_buffer(buffer, unit, size)) return -EIO;
#endif

	/* Allocate and fill driver data structure */
	info = framebuffer_alloc(sizeof(covideo_par_t), &pdev->dev);
	if (!info) {
		printk(KERN_ERR "covideo%d: framebuffer alloc failed!!", unit);
		return -ENOMEM;
	}

	/* Need to set the base and ops before find_mode */
	info->screen_base = (char __iomem *)buffer;
	info->screen_size = size;
	info->fbops = &covideo_ops;

#if COVIDEO_DEBUG
	printk(KERN_INFO "covideo%d: calling find_mode...\n", unit);
#endif
	rc = fb_find_mode(&info->var, info, NULL, NULL, 0, NULL, 8);
	if (!rc || (rc == 4)) info->var = covideo_default;

	info->fix = covideo_fix;
	info->fix.smem_start = (unsigned long) buffer;
	info->fix.smem_len = size;
	info->pseudo_palette = NULL;
	info->flags = FBINFO_FLAG_DEFAULT;

	rc = fb_alloc_cmap(&info->cmap, 256, 0);
	if (rc < 0) goto err1;

	rc = register_framebuffer(info);
	if (rc < 0) goto err2;

	par = info->par;
	par->pdev = pdev;
	par->unit = unit;
	par->buffer = buffer;
	par->size = size;
	mutex_init(&par->open_lock);

	printk(KERN_INFO "fb%d: Cooperative video at: %p, size: %dK\n", info->node,
		buffer, size >> 10);

	pci_set_drvdata(pdev, info);
	return 0;

err2:
	fb_dealloc_cmap(&info->cmap);

err1:
        framebuffer_release(info);

	return 0;
}


/*
 * PCI Remove - hotplug removal
*/
static void __devexit covideo_pci_remove(struct pci_dev *pdev)
{
	pci_set_drvdata(pdev, NULL);
}

static struct pci_device_id covideo_pci_ids[] __devinitdata = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CO, PCI_DEVICE_ID_COVIDEO) },
	{ 0 }
};

static struct pci_driver covideo_pci_driver = {
	.name		= "covideo",
	.id_table	= covideo_pci_ids,
	.probe		= covideo_pci_probe,
	.remove		= __devexit_p(covideo_pci_remove),
};

extern int coio_test(void);

/*
 * PCI Init - module load
*/
static int __init covideo_pci_init(void) {
#if 0
	rc = request_irq(VIDEO_IRQ, &covideo_isr, IRQF_SAMPLE_RANDOM, "covideo", NULL);
	if (rc) {
		printk(KERN_ERR "covideo_pci_init: unable to get irq %d", VIDEO_IRQ);
		return rc;
	}
#endif
#if COVIDEO_DEBUG
	printk(KERN_INFO "covideo_pci_init: registering...\n");
#endif
	return pci_register_driver(&covideo_pci_driver);
}

/*
 * PCI Exit - module unload
*/
static void __exit covideo_pci_exit(void) {
#if COVIDEO_DEBUG
	printk(KERN_INFO "covideo_pci_exit: exiting\n");
#endif
        pci_unregister_driver(&covideo_pci_driver);
}

module_init(covideo_pci_init);
module_exit(covideo_pci_exit);
