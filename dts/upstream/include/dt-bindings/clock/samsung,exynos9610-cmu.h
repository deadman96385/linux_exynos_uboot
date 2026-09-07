/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Copyright (c) 2026 Sean Hoyt <seanhoyt963@gmail.com>
 *
 * Device Tree binding constants for the Exynos9610 U-Boot clock driver.
 *
 * NOT the same numbering as Linux's dt-bindings/clock/samsung,exynos9610.h:
 * U-Boot's samsung_register_cmu() framework packs a per-CMU local id
 * (0-255, see SAMSUNG_TO_CLK_ID() in drivers/clk/exynos/clk.h) rather than
 * using one flat global id space like Linux's CCF driver does, so Linux's
 * ids (in the hundreds) can't be reused here without silently getting
 * masked to the wrong value.
 */

#ifndef _DT_BINDINGS_CLOCK_EXYNOS9610_CMU_H
#define _DT_BINDINGS_CLOCK_EXYNOS9610_CMU_H

/* CMU_PERI */
#define CLK_GOUT_PERI_UART_IPCLK	1

#endif /* _DT_BINDINGS_CLOCK_EXYNOS9610_CMU_H */
