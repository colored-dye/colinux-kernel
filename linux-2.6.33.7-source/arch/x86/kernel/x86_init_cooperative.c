/*
 * Copyright (C) 2009 Thomas Gleixner <tglx@linutronix.de>
 *
 *  For licencing details see kernel-base/COPYING
 */
#include <linux/init.h>
#include <linux/kernel.h>

#include <asm/bios_ebda.h>
#include <asm/paravirt.h>
#include <asm/mpspec.h>
#include <asm/setup.h>
#include <asm/apic.h>
#include <asm/e820.h>
#include <asm/time.h>
#include <asm/irq.h>
#include <asm/pat.h>
#include <asm/tsc.h>
#include <asm/iommu.h>

void __cpuinit x86_init_noop(void) { }
void __init x86_init_uint_noop(unsigned int unused) { }
void __init x86_init_pgd_noop(pgd_t *unused) { }
int __init iommu_init_noop(void) { return 0; }
void iommu_shutdown_noop(void) { }

static void __init reserve_standard_io_resources_noop(void) { }
static unsigned long mach_get_cmos_time_noop(void) { return 0; }
static int mach_set_rtc_mmss_noop(unsigned long unused) { return 0; }
static void __init hpet_time_init_noop(void) { }
static void __init native_init_IRQ_noop(void) { }
static char *__init default_machine_specific_memory_setup_noop(void) { return "CO-dummy"; }
static unsigned long native_calibrate_tsc_noop(void) { return 0; }

/*
 * The platform setup functions are preset with the default functions
 * for standard PC hardware.
 */
struct x86_init_ops x86_init __initdata = {

	.resources = {
		.probe_roms		= x86_init_noop,
		.reserve_resources	= reserve_standard_io_resources_noop,
		.memory_setup		= default_machine_specific_memory_setup_noop,
	},

	.mpparse = {
		.mpc_record		= x86_init_uint_noop,
		.setup_ioapic_ids	= x86_init_noop,
		.mpc_apic_id		= default_mpc_apic_id,
		.smp_read_mpc_oem	= default_smp_read_mpc_oem,
		.mpc_oem_bus_info	= default_mpc_oem_bus_info,
		.find_smp_config	= default_find_smp_config,
		.get_smp_config		= default_get_smp_config,
	},

	.irqs = {
		.pre_vector_init	= init_ISA_irqs,
		.intr_init		= native_init_IRQ_noop,
		.trap_init		= x86_init_noop,
	},

	.oem = {
		.arch_setup		= x86_init_noop,
		.banner			= default_banner,
	},

	.paging = {
		.pagetable_setup_start	= native_pagetable_setup_start,
		.pagetable_setup_done	= native_pagetable_setup_done,
	},

	.timers = {
		.setup_percpu_clockev	= setup_boot_APIC_clock,
		.tsc_pre_init		= x86_init_noop,
		.timer_init		= hpet_time_init_noop,
	},

	.iommu = {
		.iommu_init		= iommu_init_noop,
	},
};

struct x86_cpuinit_ops x86_cpuinit __cpuinitdata = {
	.setup_percpu_clockev		= setup_secondary_APIC_clock,
};

struct x86_platform_ops x86_platform = {
	.calibrate_tsc			= native_calibrate_tsc_noop,
	.get_wallclock			= mach_get_cmos_time_noop,
	.set_wallclock			= mach_set_rtc_mmss_noop,
	.iommu_shutdown			= iommu_shutdown_noop,
	.is_untracked_pat_range		= is_ISA_range,
};
