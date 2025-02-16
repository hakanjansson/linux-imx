// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2014 Freescale Semiconductor, Inc.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/irqchip.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/pm_opp.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/mfd/syscon/imx6q-iomuxc-gpr.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include "common.h"
#include "cpuidle.h"

static int ar8031_phy_fixup(struct phy_device *dev)
{
	u16 val;

	/* Set RGMII IO voltage to 1.8V */
	phy_write(dev, 0x1d, 0x1f);
	phy_write(dev, 0x1e, 0x8);

	/* disable phy AR8031 SmartEEE function. */
	phy_write(dev, 0xd, 0x3);
	phy_write(dev, 0xe, 0x805d);
	phy_write(dev, 0xd, 0x4003);
	val = phy_read(dev, 0xe);
	val &= ~(0x1 << 8);
	phy_write(dev, 0xe, val);

	/* introduce tx clock delay */
	phy_write(dev, 0x1d, 0x5);
	val = phy_read(dev, 0x1e);
	val |= 0x0100;
	phy_write(dev, 0x1e, val);

	return 0;
}

#define PHY_ID_AR8031   0x004dd074
static void __init imx6sx_enet_phy_init(void)
{
	if (IS_BUILTIN(CONFIG_PHYLIB))
		phy_register_fixup_for_uid(PHY_ID_AR8031, 0xffffffff,
					   ar8031_phy_fixup);
}

static void __init imx6sx_enet_clk_sel(void)
{
	struct regmap *gpr;

	gpr = syscon_regmap_lookup_by_compatible("fsl,imx6sx-iomuxc-gpr");
	if (!IS_ERR(gpr)) {
		regmap_update_bits(gpr, IOMUXC_GPR1,
				   IMX6SX_GPR1_FEC_CLOCK_MUX_SEL_MASK, 0);
		regmap_update_bits(gpr, IOMUXC_GPR1,
				   IMX6SX_GPR1_FEC_CLOCK_PAD_DIR_MASK, 0);
	} else {
		pr_err("failed to find fsl,imx6sx-iomux-gpr regmap\n");
	}
}

static inline void imx6sx_enet_init(void)
{
	imx6_enet_mac_init("fsl,imx6sx-fec", "fsl,imx6sx-ocotp");
	imx6sx_enet_phy_init();
	imx6sx_enet_clk_sel();
}

#define OCOTP_CFG3                      0x440
#define OCOTP_CFG3_SPEED_SHIFT          16
#define OCOTP_CFG3_SPEED_996MHZ         0x2
static void __init imx6sx_opp_check_speed_grading(struct device *cpu_dev)
{
	struct device_node *np;
	struct clk *ocotp_clk;
	void __iomem *base;
	u32 val;
	int err;

	np = of_find_compatible_node(NULL, NULL, "fsl,imx6sx-ocotp");
	if (!np) {
		pr_warn("failed to find ocotp node\n");
		return;
	}

	base = of_iomap(np, 0);
	if (!base) {
		pr_warn("failed to map ocotp\n");
		goto put_node;
	}

	/*
	 * Make sure the OCOTP clock is enabled before OCOTP register
	 * is accessed
	 */

	ocotp_clk = of_clk_get(np, 0);
	if (IS_ERR(ocotp_clk)) {
		pr_warn("%s: failed to get ocotp clock\n", __func__);
		goto put_node;
	}

	err = clk_prepare_enable(ocotp_clk);
	if (err) {
		pr_warn("Unable to enable OCOTP clock\n");
		goto put_clock;
	}

	/*
	 * Speed GRADING[1:0] defines the max speed of ARM:
	 * 2b'00: 800000000Hz;
	 * 2b'01: Reserved;
	 * 2b'10: 1000000000Hz;
	 * 2b'11: Reserved;
	 * We need to set the max speed of ARM according to fuse map.
	 */
	val = readl_relaxed(base + OCOTP_CFG3);
	val >>= OCOTP_CFG3_SPEED_SHIFT;
	val &= 0x3;

	if (val != OCOTP_CFG3_SPEED_996MHZ) {
		if (dev_pm_opp_disable(cpu_dev, 996000000))
			pr_warn("Failed to disable 996MHz OPP\n");
	}
	iounmap(base);

	clk_disable_unprepare(ocotp_clk);

put_clock:
	clk_put(ocotp_clk);
put_node:
	of_node_put(np);
}

static void __init imx6sx_opp_init(void)
{
	struct device_node *np;
	struct device *cpu_dev = get_cpu_device(0);

	if (!cpu_dev) {
		pr_warn("failed to get cpu0 device\n");
		return;
	}
	np = of_node_get(cpu_dev->of_node);
	if (!np) {
		pr_warn("failed to find cpu0 node\n");
		return;
	}

	if (dev_pm_opp_of_add_table(cpu_dev)) {
		pr_warn("failed to init OPP table\n");
		goto put_node;
	}

	imx6sx_opp_check_speed_grading(cpu_dev);
put_node:
	of_node_put(np);
}

static void __init imx6sx_init_machine(void)
{
	of_platform_default_populate(NULL, NULL, NULL);

	imx6sx_enet_init();
	imx_anatop_init();
	imx6sx_pm_init();
}

static void __init imx6sx_init_irq(void)
{
	imx_gpc_check_dt();
	imx_init_revision_from_anatop();
	imx_init_l2cache();
	imx_src_init();
	irqchip_init();
	imx6_pm_ccm_init("fsl,imx6sx-ccm");
}

static void __init imx6sx_map_io(void)
{
	debug_ll_io_init();
	imx6_pm_map_io();
	imx_busfreq_map_io();
}

static void __init imx6sx_init_late(void)
{
	imx6sx_cpuidle_init();

	if (IS_ENABLED(CONFIG_ARM_IMX6Q_CPUFREQ)) {
		imx6sx_opp_init();
		platform_device_register_simple("imx6q-cpufreq", -1, NULL, 0);
	}
}

static const char * const imx6sx_dt_compat[] __initconst = {
	"fsl,imx6sx",
	NULL,
};

DT_MACHINE_START(IMX6SX, "Freescale i.MX6 SoloX (Device Tree)")
	.l2c_aux_val 	= 0,
	.l2c_aux_mask	= ~0,
	.map_io		= imx6sx_map_io,
	.init_irq	= imx6sx_init_irq,
	.init_machine	= imx6sx_init_machine,
	.dt_compat	= imx6sx_dt_compat,
	.init_late	= imx6sx_init_late,
MACHINE_END
