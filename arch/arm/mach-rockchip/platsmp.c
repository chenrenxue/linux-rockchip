/*
 * Copyright (C) 2013 ROCKCHIP, Inc.
 * Copyright (c) 2013 MundoReader S.L.
 * Author: Heiko Stuebner <heiko@sntech.de>
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
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>

#include <asm/cacheflush.h>
#include <asm/smp_scu.h>
#include <asm/smp_plat.h>
#include <asm/mach/map.h>

#include "core.h"

#define SCU_CTRL		0x00
#define   SCU_STANDBY_EN	(1 << 5)

static int ncores;

/*
 * temporary PMU handling
 */

#define PMU_PWRDN_CON		0x08
#define PMU_PWRDN_ST		0x0c

static void __iomem *pmu_base_addr;

extern void secondary_startup(void);
extern void v7_invalidate_l1(void);
static void __naked rockchip_a9_secondary_startup(void)
{
	v7_invalidate_l1();
	secondary_startup();
}

static inline bool pmu_power_domain_is_on(int pd)
{
	return !(readl_relaxed(pmu_base_addr + PMU_PWRDN_ST) & BIT(pd));
}

static void pmu_set_power_domain(int pd, bool on)
{
	u32 val = readl_relaxed(pmu_base_addr + PMU_PWRDN_CON);
	if (on)
		val &= ~BIT(pd);
	else
		val |=  BIT(pd);
	writel(val, pmu_base_addr + PMU_PWRDN_CON);

	while (pmu_power_domain_is_on(pd) != on) { }
}

/*
 * Handling of CPU cores
 */

static int __cpuinit rockchip_boot_secondary(unsigned int cpu,
					     struct task_struct *idle)
{
	if (cpu >= ncores) {
		pr_err("%s: cpu %d outside maximum number of cpus %d\n",
							__func__, cpu, ncores);
		return -EINVAL;
	}

	/* start the core */
	pmu_set_power_domain(0 + cpu, true);

	return 0;
}

/**
 * rockchip_smp_prepare_sram - populate necessary sram block
 * Starting cores execute the code residing at the start of the on-chip sram
 * after power-on. Therefore make sure, this sram region is reserved and
 * big enough. After this check, copy the trampoline code that directs the
 * core to the real startup code in ram into the sram-region.
 */
static int __init rockchip_smp_prepare_sram(void)
{
	void __iomem *sram_base_addr;
	unsigned int trampoline_sz = &rockchip_secondary_trampoline_end -
					    &rockchip_secondary_trampoline;

	sram_base_addr = ioremap_nocache(0, trampoline_sz);
	if (!sram_base_addr) {
		pr_err("%s: could not map sram\n", __func__);
		BUG();
	}

	/* set the boot function for the sram code */
	if (read_cpuid_part_number() == ARM_CPU_PART_CORTEX_A9)
		rockchip_boot_fn = virt_to_phys(rockchip_a9_secondary_startup);
	else
		rockchip_boot_fn = virt_to_phys(secondary_startup);

	/* copy the trampoline to sram, that runs during startup of the core */
	memcpy(sram_base_addr, &rockchip_secondary_trampoline, trampoline_sz);

	iounmap(sram_base_addr);

	return 0;
}

static void __init rockchip_smp_prepare_cpus(unsigned int max_cpus)
{
	void __iomem *scu_base_addr = NULL;
	struct device_node *node;
	unsigned int i, cpu;

	if (scu_a9_has_base())
		scu_base_addr = ioremap(scu_a9_get_base(), 0x100);

	if (!scu_base_addr) {
		pr_err("%s: could not map scu registers\n", __func__);
		BUG();
	}

	if (rockchip_smp_prepare_sram())
		return;

	node = of_find_compatible_node(NULL, NULL, "rockchip,pmu");
	if (!node) {
		pr_err("%s: could not find sram dt node\n", __func__);
		return;
	}

	pmu_base_addr = of_iomap(node, 0);

	/*
	 * While the number of cpus is gathered from dt, also get the number
	 * of cores from the scu to verify this value when booting the cores.
	 */
	ncores = scu_get_core_count(scu_base_addr);

	writel_relaxed(readl_relaxed(scu_base_addr + SCU_CTRL) | SCU_STANDBY_EN, scu_base_addr + SCU_CTRL);
	scu_enable(scu_base_addr);

	cpu = MPIDR_AFFINITY_LEVEL(read_cpuid_mpidr(), 0);
	/* Make sure that all cores except myself are really off */
	for (i = 0; i < ncores; i++) {
		if (i == cpu)
			continue;
		pmu_set_power_domain(i, false);
	}

	iounmap(scu_base_addr);
}

struct smp_operations rockchip_smp_ops __initdata = {
	.smp_prepare_cpus	= rockchip_smp_prepare_cpus,
	.smp_boot_secondary	= rockchip_boot_secondary,
};