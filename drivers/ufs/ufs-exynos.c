// SPDX-License-Identifier: GPL-2.0+
/*
 * UFS Host Controller Driver for Samsung Exynos SoCs (U-Boot)
 *
 * Copyright (C) 2026 Sean Hoyt <seanhoyt963@gmail.com>
 *
 * Modeled after the open-source Linux kernel drivers/ufs/host/ufs-exynos.c,
 * phy-exynos9610-ufs.c.
 */

#include <asm/io.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <ufs.h>

#include "ufs.h"
#include "ufshci.h"
#include "unipro.h"

/* Exynos Vendor-Specific UFSHCI registers (at vs_hci base, 0x13521100) */
#define HCI_TXPRDT_ENTRY_SIZE		0x00
#define PRDT_PREFETCH_EN		BIT(31)
#define HCI_RXPRDT_ENTRY_SIZE		0x04
#define HCI_1US_TO_CNT_VAL		0x0C
#define VS_IS				0x38
#define HCI_UTRL_NEXUS_TYPE		0x40
#define HCI_UTMRL_NEXUS_TYPE		0x44
#define HCI_SW_RST			0x50
#define VS_GPIO_OUT			0x70
#define VS_CLKSTOP_CTRL			0xB0
#define VS_FORCE_HCS			0xB4
#define VS_UFS_ACG_DISABLE		0xFC
#define VS_CPORT_CONFIG			0x114
#define HCI_DATA_REORDER		0x60
#define HCI_UNIPRO_APB_CLK_CTRL		0x68
#define HCI_AXIDMA_RWDATA_BURST_LEN	0x6C

/* Exynos PMU & SYSREG offsets */
#define EXYNOS9610_PMU_PHY_ISO		0x0724
#define EXYNOS9610_SYSREG_COHERENCY	0x0010
#define EXYNOS9610_UFS_COHERENCY_EN	(0x3 << 8)

/* Exynos GPIO FSYS (GPF0) offsets */
#define GPF0CON				0x0000
#define GPF0PUD				0x0008

/* Exynos GPIO ALIVE (GPG0) offsets */
#define GPG0DAT				0x0144

struct exynos_ufs_priv {
	void __iomem *reg_hci;
	void __iomem *reg_vs;
	void __iomem *reg_unipro;
	void __iomem *reg_pma;
	void __iomem *reg_pmu;
	void __iomem *reg_sysreg;
	void __iomem *reg_gpio_fsys;
	void __iomem *reg_gpio_alive;
	u32 mclk_rate;
};

static int exynos_ufs_init(struct ufs_hba *hba)
{
	struct exynos_ufs_priv *priv = dev_get_priv(hba->dev);
	u32 reg;
	printf("exynos_ufs_init: setting up PMU/GPIO/SYSREG\n");

	/* Set required quirks */
	hba->quirks |= UFSHCI_QUIRK_BROKEN_REQ_LIST_CLR |
		       UFSHCI_QUIRK_SKIP_RESET_INTR_AGGR;

	/* Release UFS PHY isolation in PMU (1 = Isolation bypassed, PMU MPHY ON) */
	if (priv->reg_pmu) {
		reg = readl(priv->reg_pmu + EXYNOS9610_PMU_PHY_ISO);
		reg |= BIT(0);
		writel(reg, priv->reg_pmu + EXYNOS9610_PMU_PHY_ISO);
	}

	/* Power on device via ALIVE GPIO (GPG0_2 / bit 0 of 0x139B0144) */
	if (priv->reg_gpio_alive) {
		reg = readl(priv->reg_gpio_alive + GPG0DAT);
		reg |= BIT(0);
		writel(reg, priv->reg_gpio_alive + GPG0DAT);
		udelay(1000);
	}

	/* Configure FSYS GPIO (GPF0) for UFS_RST_N and UFS_REFCLK */
	if (priv->reg_gpio_fsys) {
		/* Clear pull-up/pull-down on pins 0 & 1 */
		reg = readl(priv->reg_gpio_fsys + GPF0PUD);
		reg &= ~0xFF;
		writel(reg, priv->reg_gpio_fsys + GPF0PUD);

		/* Set pin function to 3 (UFS RST_N / REFCLK) */
		reg = readl(priv->reg_gpio_fsys + GPF0CON);
		reg &= ~0xFF;
		reg |= 0x33;
		writel(reg, priv->reg_gpio_fsys + GPF0CON);
	}

	/* Enable UFS IO cache coherency in SYSREG */
	if (priv->reg_sysreg) {
		reg = readl(priv->reg_sysreg + EXYNOS9610_SYSREG_COHERENCY);
		reg |= EXYNOS9610_UFS_COHERENCY_EN;
		writel(reg, priv->reg_sysreg + EXYNOS9610_SYSREG_COHERENCY);
	}

	/* Configure vendor-specific registers */
	if (priv->reg_vs) {
		/* Set 1us tick counter based on 166 MHz MCLK */
		writel(166, priv->reg_vs + HCI_1US_TO_CNT_VAL);

		/* Enable PRDT prefetch with 4KB block size (12) */
		writel(PRDT_PREFETCH_EN | 12, priv->reg_vs + HCI_TXPRDT_ENTRY_SIZE);
		writel(PRDT_PREFETCH_EN | 12, priv->reg_vs + HCI_RXPRDT_ENTRY_SIZE);

		/* Data reorder (0xa = little endian DMA) and burst length */
		writel(0xa, priv->reg_vs + HCI_DATA_REORDER);
		writel(0x03030303, priv->reg_vs + HCI_AXIDMA_RWDATA_BURST_LEN);

		/* Set Nexus type */
		writel(0xFFFFFFFF, priv->reg_vs + HCI_UTRL_NEXUS_TYPE);
		writel(0xFFFFFFFF, priv->reg_vs + HCI_UTMRL_NEXUS_TYPE);
	}

	return 0;
}

static int exynos_ufs_device_reset(struct ufs_hba *hba)
{
	struct exynos_ufs_priv *priv = dev_get_priv(hba->dev);

	if (priv->reg_vs) {
		writel(0, priv->reg_vs + VS_GPIO_OUT);
		udelay(10);
		writel(1, priv->reg_vs + VS_GPIO_OUT);
		udelay(10);
	}

	return 0;
}

static int exynos_ufs_hce_enable_notify(struct ufs_hba *hba,
					enum ufs_notify_change_status status)
{
	struct exynos_ufs_priv *priv = dev_get_priv(hba->dev);
	u32 reg;

	if (status == PRE_CHANGE && priv->reg_vs) {
		/* Clear VS_FORCE_HCS if needed */
		if ((readl(priv->reg_vs + VS_FORCE_HCS) >> 4) & 0xf)
			writel(0x0, priv->reg_vs + VS_FORCE_HCS);

		/* Pulse software reset and wait for completion */
		writel(3, priv->reg_vs + HCI_SW_RST);
		while (readl(priv->reg_vs + HCI_SW_RST))
			udelay(1);

		/* Clear VS_IS[20] (UFS_IDLE_Indicator) */
		reg = readl(priv->reg_vs + VS_IS);
		if ((reg >> 20) & 0x1)
			writel(reg, priv->reg_vs + VS_IS);
	} else if (status == POST_CHANGE && priv->reg_vs) {
		/* Enable REFCLKOUT by clearing bit 4 of VS_CLKSTOP_CTRL */
		reg = readl(priv->reg_vs + VS_CLKSTOP_CTRL);
		reg &= ~(1 << 4);
		writel(reg, priv->reg_vs + VS_CLKSTOP_CTRL);

		/* Clock gating set */
		writel(0xde0, priv->reg_vs + VS_FORCE_HCS);

		/* Disable auto clock gating */
		reg = readl(priv->reg_vs + VS_UFS_ACG_DISABLE);
		writel(reg | 1, priv->reg_vs + VS_UFS_ACG_DISABLE);

		/* CPORT config */
		writel(0x22, priv->reg_vs + VS_CPORT_CONFIG);
	}

	return 0;
}

static int exynos_ufs_link_startup_notify(struct ufs_hba *hba,
					  enum ufs_notify_change_status status)
{
	struct exynos_ufs_priv *priv = dev_get_priv(hba->dev);
	printf("exynos_ufs_link_startup_notify: status=%d\n", status);
	u32 mclk_period = 1000000000L / priv->mclk_rate; /* 6 for 166 MHz */
	u32 hw_cap, peer_rx, max_rx;
	int i;

	if (status == PRE_CHANGE) {
		/*
		 * 1. Configure M-PHY PMA registers (at reg_pma = 0x13524000).
		 * Values from Linux phy-exynos9610-ufs.c.
		 */
		void __iomem *pma = priv->reg_pma;

		if (pma) {
			writel(0x80, pma + 0x8C);
			writel(0x10, pma + 0x74);
			for (i = 0; i < 2; i++) {
				u32 lane_off = i * 0x140;

				writel(0xB5, pma + 0x110 + lane_off);
				writel(0x43, pma + 0x134 + lane_off);
				writel(0x20, pma + 0x16C + lane_off);
				writel(0xC0, pma + 0x178 + lane_off);
				writel(0x94, pma + 0x1B0 + lane_off);
				writel(0x12, pma + 0xE0 + lane_off);
				writel(0x58, pma + 0x164 + lane_off);
			}
			writel(0xC0, pma + 0x8C);
			writel(0x00, pma + 0x8C);
		}

		/*
		 * 2. Configure UniPro and M-PHY PCS via DME UIC commands.
		 */
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x9514), mclk_period);
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x200), 0x40);

		for (i = 0; i < 2; i++) {
			/* TX lane MIBs (TX lane 0 + i) */
			ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0xAA, i), mclk_period);
			ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x04, i), 0x1);
			ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x8F, i), 0x3E);

			/* RX lane MIBs (RX lane 4 + i) */
			ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x12, 4 + i), mclk_period);
			ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x5C, 4 + i), 0x38);
			ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x0F, 4 + i), 0x0);
			ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x65, 4 + i), 0x01);
			ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x69, 4 + i), 0x1);
			ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x21, 4 + i), 0x0);
			ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x22, 4 + i), 0x0);
			ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x84, 4 + i), 0x1);
		}

		ufshcd_dme_set(hba, UIC_ARG_MIB(0x200), 0x0);
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x9536), 0x4E20);
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x9564), 0x2E820183);
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x155E), 0x0);
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x3000), 0x0);
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x3001), 0x1);
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x4021), 0x1);
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x4020), 0x1);
	} else if (status == POST_CHANGE) {
		/*
		 * Post link startup configuration
		 */
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x9529), 0x1);
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x15A4), 0xFA);
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x9529), 0x0);
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x200), 0x40);

		for (i = 0; i < 2; i++) {
			ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x35, 4 + i), 0x05);
			ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x73, 4 + i), 0x01);
			ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x41, 4 + i), 0x02);
			ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x42, 4 + i), 0xAC);
		}
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x200), 0x0);

		/* Calibrate hibern8 values */
		hw_cap = 0;
		peer_rx = 0;
		max_rx = 0;
		ufshcd_dme_get(hba, UIC_ARG_MIB_SEL(0x8F, 4), &hw_cap);
		ufshcd_dme_get(hba, UIC_ARG_MIB(0x15A8), &peer_rx);
		ufshcd_dme_get(hba, UIC_ARG_MIB(0x15A7), &max_rx);

		if (peer_rx >= hw_cap)
			ufshcd_dme_peer_set(hba, UIC_ARG_MIB(0x15A8), peer_rx + 1);
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x15A7), max_rx + 1);

		/* Re-assert vendor-specific registers */
		writel(0xa, priv->reg_vs + HCI_DATA_REORDER);
		writel(PRDT_PREFETCH_EN | 12, priv->reg_vs + HCI_TXPRDT_ENTRY_SIZE);
		writel(PRDT_PREFETCH_EN | 12, priv->reg_vs + HCI_RXPRDT_ENTRY_SIZE);
		writel(0xFFFFFFFF, priv->reg_vs + HCI_UTRL_NEXUS_TYPE);
		writel(0xFFFFFFFF, priv->reg_vs + HCI_UTMRL_NEXUS_TYPE);

		/* Ensure DME is enabled */
		ufshcd_dme_enable(hba);
	}

	return 0;
}

static struct ufs_hba_ops exynos_ufs_hba_ops = {
	.init			= exynos_ufs_init,
	.device_reset		= exynos_ufs_device_reset,
	.hce_enable_notify	= exynos_ufs_hce_enable_notify,
	.link_startup_notify	= exynos_ufs_link_startup_notify,
};

static int exynos_ufs_probe(struct udevice *dev)
{
	struct exynos_ufs_priv *priv = dev_get_priv(dev);
	printf("exynos_ufs_probe: probing %s\n", dev->name);
	fdt_addr_t base;

	base = dev_read_addr_index(dev, 0);
	if (base == FDT_ADDR_T_NONE)
		base = 0x13520000;
	priv->reg_hci = (void __iomem *)base;

	base = dev_read_addr_index(dev, 1);
	if (base == FDT_ADDR_T_NONE)
		base = (fdt_addr_t)priv->reg_hci + 0x1100;
	priv->reg_vs = (void __iomem *)base;

	base = dev_read_addr_index(dev, 2);
	if (base == FDT_ADDR_T_NONE)
		base = (fdt_addr_t)priv->reg_hci - 0x10000;
	priv->reg_unipro = (void __iomem *)base;

	priv->reg_pma = (void __iomem *)((fdt_addr_t)priv->reg_hci + 0x4000);
	priv->reg_pmu = (void __iomem *)0x11860000;
	priv->reg_sysreg = (void __iomem *)0x13411000;
	priv->reg_gpio_fsys = (void __iomem *)0x13490000;
	priv->reg_gpio_alive = (void __iomem *)0x139B0000;
	priv->mclk_rate = 166000000;

	return ufshcd_probe(dev, &exynos_ufs_hba_ops);
}

static const struct udevice_id exynos_ufs_ids[] = {
	{ .compatible = "samsung,exynos9610-ufs" },
	{ .compatible = "samsung,exynos7-ufs" },
	{},
};

U_BOOT_DRIVER(exynos_ufs) = {
	.name		= "exynos-ufs",
	.id		= UCLASS_UFS,
	.of_match	= exynos_ufs_ids,
	.probe		= exynos_ufs_probe,
	.priv_auto	= sizeof(struct exynos_ufs_priv),
};
