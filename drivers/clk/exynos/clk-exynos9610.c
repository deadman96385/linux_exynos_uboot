// SPDX-License-Identifier: GPL-2.0+
/*
 * Exynos9610 clock driver (U-Boot).
 *
 * Copyright (c) 2026 Sean Hoyt <seanhoyt963@gmail.com>
 *
 * Deliberately minimal: U-Boot's s5p serial driver
 * (drivers/serial/serial_s5p.c, s5p_serial_setbrg()) only ever calls
 * clk_get_by_index(dev, 1, &clk) -- it needs the UART's ipclk rate for
 * baud-rate math and nothing else (no bus/pclk lookup, no enable calls,
 * matching how U-Boot assumes clocks are already left configured/enabled
 * by the stock bootloader, the same assumption the rest of this bring-up
 * has made throughout).
 *
 * The real hardware clock chain for this ipclk is a two-domain
 * PLL -> mux -> mux path (CMU_TOP's dout_clkcmu_peri_uart feeding
 * CMU_PERI's mout_cmu_peri_uart_user mux) that isn't modeled here at
 * all. Instead this is a plain FRATE fixed-rate clock set to
 * 133250000 Hz -- read live off a running LineageOS kernel's
 * /sys/kernel/debug/clk/UART/clk_rate on real troika hardware, not
 * computed from the PLL formula. If that measured rate ever turns out
 * to depend on a mux state the bootloader doesn't always leave the
 * same way, this will need the real chain modeled instead.
 *
 * No CMU_TOP driver exists here since nothing in this file needs one:
 * a fixed-rate clock has no parent to resolve.
 */

#include <clk-uclass.h>
#include <dm.h>
#include <asm/io.h>
#include <dt-bindings/clock/samsung,exynos9610-cmu.h>
#include "clk.h"

enum exynos9610_cmu_id {
	CMU_PERI,
};

static const struct samsung_fixed_rate_clock peri_fixed_rate_clks[] = {
	FRATE(CLK_GOUT_PERI_UART_IPCLK, "peri_uart_ipclk", 133250000),
};

static const struct samsung_clk_group peri_cmu_clks[] = {
	{ S_CLK_FRATE, peri_fixed_rate_clks, ARRAY_SIZE(peri_fixed_rate_clks) },
};

static int exynos9610_cmu_peri_probe(struct udevice *dev)
{
	return samsung_register_cmu(dev, CMU_PERI, peri_cmu_clks,
				    exynos9610_cmu_peri);
}

static const struct udevice_id exynos9610_cmu_peri_ids[] = {
	{ .compatible = "samsung,exynos9610-cmu-peri" },
	{ }
};

SAMSUNG_CLK_OPS(exynos9610_cmu_peri, CMU_PERI);

U_BOOT_DRIVER(exynos9610_cmu_peri) = {
	.name		= "exynos9610-cmu-peri",
	.id		= UCLASS_CLK,
	.of_match	= exynos9610_cmu_peri_ids,
	.ops		= &exynos9610_cmu_peri_clk_ops,
	.probe		= exynos9610_cmu_peri_probe,
	.flags		= DM_FLAG_PRE_RELOC,
};
