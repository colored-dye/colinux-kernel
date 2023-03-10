
#ifndef __COSCSI_ROM_H
#define __COSCSI_ROM_H

/* Mode/Inq page data */
struct coscsi_page {
	int num;
	unsigned char *page;
	int size;
};
typedef struct coscsi_page coscsi_page_t;

#define COSCSI_ROM_PAGE(n,p) { n, p, sizeof(p) }

struct coscsi_rom {
        char *name;
        coscsi_page_t std;
        coscsi_page_t *vpd;
	coscsi_page_t *mode;
};
typedef struct coscsi_rom coscsi_rom_t;

/*
 * Disk pages
*/

/* Standard Inquiry page */
static unsigned char disk_std_page[] = {
	0x00,0x00,0x05,0x02,0x5c,0x00,0x01,0x20,	/* 00 - 07 */
	0x63,0x6f,0x4c,0x69,0x6e,0x75,0x78,0x00,	/* 08 - 15 */
	0x43,0x4f,0x44,0x49,0x53,0x4b,0x00,0x00,	/* 16 - 23 */
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 24 - 31 */
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 32 - 39 */
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 40 - 47 */
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 48 - 55 */
	0x00,0x00,0x00,0x77,0x00,0x14,0x03,0x3d,	/* 56 - 63 */
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 64 - 71 */
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 72 - 79 */
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 80 - 87 */
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 88 - 95 */
};

#if 0
/* Supported VPD Pages */
static unsigned char disk_vpd_00[] = {
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 00 - 07 */
};
#endif

#if 0
/* Unit Serial Number VPD page */
static unsigned char disk_vpd_80[] = {
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 00 - 07 */
};
#endif

/* Device Identification VPD page */
static unsigned char disk_vpd_83[] = {
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 00 - 07 */
};

/* Block Limits VPD page */
static unsigned char disk_vpd_b0[] = {
	0x00,0xB0,0x00,0x10,0x00,0x00,0x00,0x00,	/* 00 - 07 */
};

static coscsi_page_t disk_vpd_pages[] = {
#if 0
	COSCSI_ROM_PAGE(0x80, disk_vpd_80),		/* Unit Serial Number */
#endif
	COSCSI_ROM_PAGE(0x83, disk_vpd_83),		/* Device Identification */
	COSCSI_ROM_PAGE(0xb0, disk_vpd_b0),		/* Block limits (SBC) */
	{ 0, 0, 0 }
};

static unsigned char disk_mode_08[] = {
	0x08, 0x12, 0x14, 0x00, 0xff, 0xff, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x80, 0x14, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static coscsi_page_t disk_mode_pages[] = {
	COSCSI_ROM_PAGE(0x08, disk_mode_08),
	{ 0, 0, 0 }
};

static coscsi_rom_t disk_rom = {
	.name =		"CODISK",
	.std =		COSCSI_ROM_PAGE(0, disk_std_page),
	.vpd =		disk_vpd_pages,
	.mode =		disk_mode_pages,
};

/*
 * CD pages
*/

static unsigned char cd_std_page[] = {
	0x05,0x80,0x02,0x02,0x1f,0x00,0x00,0x10,	/* 00 - 07 */
	0x4f,0x50,0x30,0x34,0x32,0x5a,0x20,0x49,	/* 08 - 15 */
	0x52,0x53,0x30,0x36,0x50,0x20,0x20,0x20,	/* 16 - 23 */
	0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,	/* 24 - 31 */
	0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,	/* 24 - 31 */
	0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,	/* 32 - 39 */
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 40 - 47 */
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 48 - 55 */
	0x00,0x00,0x00,0x77,0x00,0x14,0x03,0x3d,	/* 56 - 63 */
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 64 - 71 */
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 72 - 79 */
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 80 - 87 */
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 88 - 95 */
};

/* Device Identification VPD page */
static unsigned char cd_vpd_83[] = {
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	/* 00 - 07 */
};

static coscsi_page_t cd_vpd_pages[] = {
	COSCSI_ROM_PAGE(0x83, cd_vpd_83),		/* Device Identification */
	{ 0, 0, 0 }
};

unsigned char cd_mode_2a[] = {
	0x2a,0x18,0x3f,0x00,0x75,0x7f,0x29,0x00, 	/* 00 - 07 */
	0x16,0x00,0x01,0x00,0x02,0x00,0x16,0x00,	/* 08 - 15 */
	0x00,0x18,0x00,0x00,0x00,0x00,0x00,0x01,	/* 16 - 23 */
};

static coscsi_page_t cd_mode_pages[] = {
	COSCSI_ROM_PAGE(0x2a, cd_mode_2a),
	{ 0, 0, 0 }
};

static coscsi_rom_t cd_rom = {
	.name =		"COCD",
	.std =		COSCSI_ROM_PAGE(0, cd_std_page),
	.vpd = 		cd_vpd_pages,
	.mode =		cd_mode_pages,
};

#endif
