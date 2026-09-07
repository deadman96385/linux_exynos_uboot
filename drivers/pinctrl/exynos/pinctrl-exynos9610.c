// SPDX-License-Identifier: GPL-2.0+
/*
 * Exynos9610 pinctrl driver.
 *
 * Copyright (c) 2026 Sean Hoyt <seanhoyt963@gmail.com>
 *
 * Only the ALIVE controller's gpq0 and gpa1 banks are defined --
 * offsets taken directly from mainline Linux's already-merged
 * drivers/pinctrl/samsung/pinctrl-exynos-arm64.c
 * (exynos9610_pin_banks0[]: EXYNOS850_PIN_BANK_EINTN(5, 0x080, "gpq0"),
 * GS101_PIN_BANK_EINTW(8, 0x040, "gpa1")), not guessed. gpq0 is the
 * UART pinmux; gpa1 (pin 6) is the volume-down key, confirmed live on
 * real troika hardware via /sys/firmware/devicetree/base/pinctrl@
 * 11850000/key-voldown (samsung,pins = "gpa1-6") and cross-checked
 * against gpa1's own phandle matching button@1's gpios reference under
 * the live gpio_keys node. Every other bank/domain is still missing.
 */

#include <dm.h>
#include <errno.h>
#include <asm/io.h>
#include <dm/pinctrl.h>
#include <dm/root.h>
#include <fdtdec.h>
#include <asm/arch/pinmux.h>
#include "pinctrl-exynos.h"

static const struct pinctrl_ops exynos9610_pinctrl_ops = {
	.set_state	= exynos_pinctrl_set_state
};

/* pin banks of exynos9610 pin-controller 0 (ALIVE) */
static const struct samsung_pin_bank_data exynos9610_pin_banks0[] = {
	EXYNOS_PIN_BANK(8, 0x040, "gpa1"),
	EXYNOS_PIN_BANK(5, 0x080, "gpq0"),
};

const struct samsung_pin_ctrl exynos9610_pin_ctrl[] = {
	{
		/* pin-controller instance 0 ALIVE data */
		.pin_banks	= exynos9610_pin_banks0,
		.nr_banks	= ARRAY_SIZE(exynos9610_pin_banks0),
	},
	{/* list terminator */}
};

static const struct udevice_id exynos9610_pinctrl_ids[] = {
	{ .compatible = "samsung,exynos9610-pinctrl",
		.data = (ulong)exynos9610_pin_ctrl },
	{ }
};

U_BOOT_DRIVER(pinctrl_exynos9610) = {
	.name		= "pinctrl_exynos9610",
	.id		= UCLASS_PINCTRL,
	.of_match	= exynos9610_pinctrl_ids,
	.priv_auto = sizeof(struct exynos_pinctrl_priv),
	.ops		= &exynos9610_pinctrl_ops,
	.probe		= exynos_pinctrl_probe,
	.bind		= exynos_pinctrl_bind,
};
