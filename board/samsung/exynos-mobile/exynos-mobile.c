// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Exynos Generic Board Source (for mobile devices)
 *
 * Copyright (c) 2025 Kaustabh Chakraborty <kauschluss@disroot.org>
 */

#include <asm/armv8/mmu.h>
#include <asm/io.h>
#include <blk.h>
#include <bootflow.h>
#include <ctype.h>
#include <dm.h>
#include <dm/ofnode.h>
#include <efi.h>
#include <efi_loader.h>
#include <env.h>
#include <errno.h>
#include <fastboot.h>
#include <init.h>
#include <linux/sizes.h>
#include <linux/delay.h>
#include <lmb.h>
#include <part.h>
#include <stdbool.h>
#include <string.h>
#include <video.h>
#if IS_ENABLED(CONFIG_UFS)
#include <ufs.h>
#endif
#if IS_ENABLED(CONFIG_SCSI)
#include <scsi.h>
#endif

#include "j7y17lte-panel-diagnostic.h"

DECLARE_GLOBAL_DATA_PTR;

#define lmb_alloc(size, addr) \
	lmb_alloc_mem(LMB_MEM_ALLOC_ANY, SZ_2M, addr, size, LMB_NONE)

#define EXYNOS_BOOT_PARTITION		"boot"
#define EXYNOS_USERDATA_PARTITION	"userdata"

#define EXYNOS7870_PMU_BASE		0x10480000UL
#define EXYNOS7870_PMU_INFORM2		0x0808
#define EXYNOS7870_PMU_INFORM3		0x080c
#define EXYNOS7870_SEC_POWER_RESET	0x12345678
#define EXYNOS7870_SEC_RECOVERY		0x12345674

#define EXYNOS9610_PMU_BASE		0x11860000UL
#define EXYNOS9610_PMU_SYSIP_DAT3	0x081c
#define MOTOROLA_REBOOT_FASTBOOT	0x77665500
#define MOTOROLA_REBOOT_RECOVERY	0x77665502

int fastboot_set_reboot_flag(enum fastboot_reboot_reason reason)
{
	void __iomem *pmu;

	if (of_machine_is_compatible("motorola,troika")) {
		pmu = (void __iomem *)EXYNOS9610_PMU_BASE;
		if (reason == FASTBOOT_REBOOT_REASON_BOOTLOADER) {
			writel(MOTOROLA_REBOOT_FASTBOOT,
			       pmu + EXYNOS9610_PMU_SYSIP_DAT3);
		} else if (reason == FASTBOOT_REBOOT_REASON_RECOVERY ||
			   reason == FASTBOOT_REBOOT_REASON_FASTBOOTD) {
			writel(MOTOROLA_REBOOT_RECOVERY,
			       pmu + EXYNOS9610_PMU_SYSIP_DAT3);
		} else {
			return -ENOTSUPP;
		}
		return 0;
	}

	if (!of_machine_is_compatible("samsung,j7y17lte") ||
	    reason != FASTBOOT_REBOOT_REASON_RECOVERY)
		return -ENOTSUPP;

	pmu = (void __iomem *)EXYNOS7870_PMU_BASE;
	writel(EXYNOS7870_SEC_POWER_RESET,
	       pmu + EXYNOS7870_PMU_INFORM2);
	writel(EXYNOS7870_SEC_RECOVERY,
	       pmu + EXYNOS7870_PMU_INFORM3);

	if (readl(pmu + EXYNOS7870_PMU_INFORM2) !=
			EXYNOS7870_SEC_POWER_RESET ||
	    readl(pmu + EXYNOS7870_PMU_INFORM3) !=
			EXYNOS7870_SEC_RECOVERY)
		return -EIO;

	return 0;
}

struct efi_fw_image fw_images[] = {
	{
		.fw_name = u"UBOOT_BOOT_PARTITION",
		.image_index = 1,
	},
};

struct efi_capsule_update_info update_info = {
	.dfu_string = NULL,
	.images = fw_images,
	.num_images = ARRAY_SIZE(fw_images),
};

bool efi_gop_register_allowed(void)
{
	/*
	 * Keep U-Boot's local video console, but do not publish its inherited
	 * S-Boot framebuffer to the EFI payload. Linux has a native Exynos
	 * DECON/DSIM driver and must be the only DRM owner of the scanout.
	 */
	return !of_machine_is_compatible("samsung,j7y17lte");
}

/*
 * The memory mapping includes all DRAM banks, along with the
 * peripheral block, and a sentinel at the end. This is filled in
 * dynamically.
 */
static struct mm_region exynos_mem_map[CONFIG_NR_DRAM_BANKS + 2] = {
	{
		/* Peripheral MMIO block */
		.virt = 0x10000000UL,
		.phys = 0x10000000UL,
		.size = 0x10000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE | PTE_BLOCK_PXN | PTE_BLOCK_UXN,
	},
};

struct mm_region *mem_map = exynos_mem_map;

static const char *exynos_prev_bl_get_bootargs(void)
{
	void *prev_bl_fdt_base = (void *)get_prev_bl_fdt_addr();
	int chosen_node_offset, ret;
	const struct fdt_property *bootargs_prop;

	ret = fdt_check_header(prev_bl_fdt_base);
	if (ret < 0) {
		log_err("%s: FDT is invalid (FDT_ERR %d)\n", __func__, ret);
		return NULL;
	}

	ret = fdt_path_offset(prev_bl_fdt_base, "/chosen");
	chosen_node_offset = ret;
	if (ret < 0) {
		log_err("%s: /chosen node not found (FDT_ERR %d)\n", __func__,
			ret);
		return NULL;
	}

	bootargs_prop = fdt_get_property(prev_bl_fdt_base, chosen_node_offset,
					 "bootargs", &ret);
	if (!bootargs_prop) {
		log_err("%s: /chosen/bootargs property not found (FDT_ERR %d)\n",
			__func__, ret);
		return NULL;
	}

	return bootargs_prop->data;
}

static void exynos_parse_dram_banks(const void *fdt_base)
{
	u64 mem_addr, mem_size = 0;
	u32 na, ns, i;
	int index = 1;
	int offset;

	if (fdt_check_header(fdt_base) < 0)
		return;

	/* #address-cells and #size-cells as defined in the fdt root. */
	na = fdt_address_cells(fdt_base, 0);
	ns = fdt_size_cells(fdt_base, 0);

	fdt_for_each_subnode(offset, fdt_base, 0) {
		if (strncmp(fdt_get_name(fdt_base, offset, NULL), "memory", 6))
			continue;

		for (i = 0; ; i++) {
			if (index > CONFIG_NR_DRAM_BANKS)
				break;

			mem_addr = fdtdec_get_addr_size_fixed(fdt_base, offset,
							      "reg", i, na, ns,
							      &mem_size, false);
			if (mem_addr == FDT_ADDR_T_NONE)
				break;

			if (!mem_size)
				continue;

			mem_map[index].phys = mem_addr;
			mem_map[index].virt = mem_addr;
			mem_map[index].size = mem_size;
			mem_map[index].attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
					       PTE_BLOCK_INNER_SHARE;
			index++;
		}
	}
}

static void exynos_env_setup(void)
{
	const char *bootargs = exynos_prev_bl_get_bootargs();
	const char *dev_compatible, *soc_compatible;
	char *ptr;
	char buf[128];
	int nr_compatibles;
	int offset;
	int ret;

	if (bootargs) {
		/* Read the cmdline property which stores the serial number. */
		ret = cmdline_get_arg(bootargs, "androidboot.serialno", &offset);
		if (ret > 0) {
			strlcpy(buf, bootargs + offset, ret);
			env_set("serial#", buf);
		}
	}

	nr_compatibles = ofnode_read_string_count(ofnode_root(), "compatible");
	if (nr_compatibles < 2) {
		log_warning("%s: expected 2 or more compatible strings\n",
			    __func__);
		return;
	}

	ret = ofnode_read_string_index(ofnode_root(), "compatible",
				       nr_compatibles - 1, &soc_compatible);
	if (ret) {
		log_warning("%s: failed to read SoC compatible\n",
			    __func__);
		return;
	}

	ret = ofnode_read_string_index(ofnode_root(), "compatible", 0,
				       &dev_compatible);
	if (ret) {
		log_warning("%s: failed to read device compatible\n",
			    __func__);
		return;
	}

	/* <manufacturer>,<soc> => platform = <soc> */
	ptr = strchr(soc_compatible, ',');
	if (ptr)
		soc_compatible = ptr + 1;
	env_set("platform", soc_compatible);

	/* <manufacturer>,<codename> => board = <manufacturer>-<codename> */
	strlcpy(buf, dev_compatible, sizeof(buf) - 1);
	ptr = strchr(buf, ',');
	if (ptr)
		*ptr = '-';
	env_set("board", buf);

	/*
	 * NOTE: Board name usually goes as <manufacturer>-<codename>, but
	 * upstream device trees for Exynos SoCs are <soc>-<codename>.
	 * Extraction of <codename> from the board name is required.
	 */
	ptr = strchr(dev_compatible, ',');
	if (ptr)
		dev_compatible = ptr + 1;

	/* EFI booting requires the path to correct DTB, specify it here. */
	snprintf(buf, sizeof(buf), "exynos/%s-%s.dtb", soc_compatible,
		 dev_compatible);
	env_set("fdtfile", buf);
}

static int exynos_blk_env_setup(void)
{
	const char *blk_ifname;
	int blk_dev = 0;
	struct blk_desc *blk_desc;
	struct disk_partition info = {0};
	unsigned long userdata_start = 0, userdata_size = 0;
	static char dfu_string[32];
	int i;

	blk_ifname = "mmc";
	blk_desc = blk_get_dev(blk_ifname, blk_dev);
	if (!blk_desc) {
#if IS_ENABLED(CONFIG_UFS)
		int ufs_ret = ufs_probe();
		printf("exynos_blk_env_setup: ufs_probe() = %d\n", ufs_ret);
#endif
#if IS_ENABLED(CONFIG_SCSI)
		int scsi_ret = scsi_scan(true);
		printf("exynos_blk_env_setup: scsi_scan() = %d\n", scsi_ret);
#endif
		blk_ifname = "scsi";
		blk_desc = blk_get_dev(blk_ifname, blk_dev);
		printf("exynos_blk_env_setup: blk_get_dev(scsi, 0) = %p\n", blk_desc);
	}
	if (!blk_desc) {
		log_warning("%s: storage device not available, skipping blkmap setup\n",
			    __func__);
		return 0;
	}

	for (i = 1; i < CONFIG_EFI_PARTITION_ENTRIES_NUMBERS; i++) {
		if (part_get_info(blk_desc, i, &info))
			continue;

		if (!update_info.dfu_string &&
		    (!strcasecmp(info.name, EXYNOS_BOOT_PARTITION) ||
		     !strcasecmp(info.name, "boot_a") ||
		     !strcasecmp(info.name, "boot_b"))) {
			snprintf(dfu_string, sizeof(dfu_string),
				 "%s %d=u-boot.bin part %d %d", blk_ifname,
				 blk_dev, blk_dev, i);
			update_info.dfu_string = dfu_string;
		}

		if (!strcasecmp(info.name, EXYNOS_USERDATA_PARTITION)) {
			userdata_start = info.start;
			userdata_size = info.size;
		}
	}

	if (userdata_size) {
		env_set("blkmap_blk_ifname", blk_ifname);
		env_set_ulong("blkmap_blk_dev", blk_dev);
		env_set_ulong("blkmap_blk_nr", userdata_start);
		env_set_hex("blkmap_size_r", userdata_size);
	} else {
		log_warning("%s: USERDATA partition not found, skipping blkmap\n",
			    __func__);
	}

	return 0;
}

static int exynos_fastboot_setup(void)
{
	struct blk_desc *blk_dev;
	struct disk_partition info = {0};
	phys_addr_t addr;
	bool boot_found = false, userdata_found = false;
	int i;

	/* Allocate and define buffer address for fastboot interface. */
	if (lmb_alloc(CONFIG_FASTBOOT_BUF_SIZE, &addr)) {
		log_warning("%s: failed to allocate fastboot buffer via LMB, using default 0x%lx\n",
			    __func__, (ulong)CONFIG_FASTBOOT_BUF_ADDR);
		addr = CONFIG_FASTBOOT_BUF_ADDR;
	}
	env_set_hex("fastboot_addr_r", addr);

#if IS_ENABLED(CONFIG_FASTBOOT_FLASH_MMC)
	blk_dev = blk_get_dev("mmc", CONFIG_FASTBOOT_FLASH_MMC_DEV);
#elif IS_ENABLED(CONFIG_FASTBOOT_FLASH_BLOCK)
	blk_dev = blk_get_dev(CONFIG_FASTBOOT_FLASH_BLOCK_INTERFACE_NAME,
			      CONFIG_FASTBOOT_FLASH_BLOCK_DEVICE_ID);
#else
	blk_dev = blk_get_dev("mmc", 0);
#endif
	if (!blk_dev)
		blk_dev = blk_get_dev("scsi", 0);
	if (!blk_dev) {
		log_warning("%s: storage device not available, fastboot flash/erase will have no storage backing\n",
			    __func__);
		return 0;
	}

	for (i = 1; i < CONFIG_EFI_PARTITION_ENTRIES_NUMBERS; i++) {
		if (part_get_info(blk_dev, i, &info))
			continue;

		if (!boot_found &&
		    (!strcasecmp(info.name, EXYNOS_BOOT_PARTITION) ||
		     !strcasecmp(info.name, "boot_a") ||
		     !strcasecmp(info.name, "boot_b"))) {
			env_set("fastboot_partition_alias_boot", info.name);
			boot_found = true;
		}

		if (!strcasecmp(info.name, EXYNOS_USERDATA_PARTITION)) {
			env_set("fastboot_partition_alias_userdata", info.name);
			userdata_found = true;
		}
	}

	if (!boot_found || !userdata_found) {
		log_warning("%s: required BOOT/USERDATA partition aliases missing (boot=%d, userdata=%d)\n",
			    __func__, boot_found, userdata_found);
	}

	/* Expose the fixed policy as a read-only fastboot getvar. */
	env_set("fastboot.partition-allowlist", "boot,userdata");

	return 0;
}

#define EXYNOS7870_DECON_BASE		0x14830000UL
#define EXYNOS7870_DECON_VIDCON0		0x0000
#define EXYNOS7870_DECON_VIDOUTCON0	0x0004
#define EXYNOS7870_DECON_WINCON0		0x0050
#define EXYNOS7870_DECON_VIDTCON4	0x0620
#define EXYNOS7870_DECON_VIDW_ADD0	0x0880
#define EXYNOS7870_DECON_TRIGCON		0x06b0
#define EXYNOS7870_DECON_UPDATE		0x0710
#define EXYNOS7870_VIDCON0_ENVID		BIT(1)
#define EXYNOS7870_VIDCON0_ENVID_F	BIT(0)
#define EXYNOS7870_VIDOUTCON0_I80IF	BIT(23)
#define EXYNOS7870_WINCON_ENWIN		BIT(0)
#define EXYNOS7870_TRIGCON_HW_UNMASK	BIT(4)
#define EXYNOS7870_UPDATE_STANDALONE	BIT(0)
#define EXYNOS7870_VIDTCON4_J7Y17LTE	0x077f0437

static int exynos7870_command_mode_video_handoff(void)
{
	void __iomem *decon = (void __iomem *)EXYNOS7870_DECON_BASE;
	struct video_uc_plat *plat;
	struct video_priv *priv;
	struct udevice *dev;
	u32 buf0, vidout0, wincon0;
	int ret, timeout;

	if (!of_machine_is_compatible("samsung,j7y17lte"))
		return 0;

	ret = uclass_get_device(UCLASS_VIDEO, 0, &dev);
	if (ret)
		return ret;

	plat = dev_get_uclass_plat(dev);
	priv = dev_get_uclass_priv(dev);
	if (plat->base != 0x67000000 || (ulong)priv->fb != plat->base ||
	    priv->xsize != 1080 || priv->ysize != 1920 ||
	    priv->line_length != 1080 * 4 || priv->bpix != VIDEO_BPP32)
		return -EINVAL;

	/*
	 * Discard the framebuffer contents inherited from diagnostic builds.
	 * Subsequent vidconsole output is rendered as white text on black.
	 */
	memset(priv->fb, 0, priv->line_length * priv->ysize);

	/* Flush the cleared console before asking DECON to transfer it. */
	ret = video_sync(dev, true);
	if (ret)
		return ret;

	vidout0 = readl(decon + EXYNOS7870_DECON_VIDOUTCON0);
	wincon0 = readl(decon + EXYNOS7870_DECON_WINCON0);
	buf0 = readl(decon + EXYNOS7870_DECON_VIDW_ADD0);

	if (!(vidout0 & EXYNOS7870_VIDOUTCON0_I80IF) ||
	    !(wincon0 & EXYNOS7870_WINCON_ENWIN) || buf0 != plat->base ||
	    readl(decon + EXYNOS7870_DECON_VIDTCON4) !=
		EXYNOS7870_VIDTCON4_J7Y17LTE)
		return -EINVAL;

	/*
	 * Match Samsung's Exynos7870 command-mode DECON start sequence:
	 * enable direct output, request a standalone shadow update, then
	 * unmask the hardware trigger. The previous bootloader owns panel
	 * initialization; this only transfers U-Boot's cache-flushed frame.
	 */
	setbits_le32(decon + EXYNOS7870_DECON_VIDCON0,
		     EXYNOS7870_VIDCON0_ENVID |
		     EXYNOS7870_VIDCON0_ENVID_F);
	setbits_le32(decon + EXYNOS7870_DECON_UPDATE,
		     EXYNOS7870_UPDATE_STANDALONE);
	setbits_le32(decon + EXYNOS7870_DECON_TRIGCON,
		     EXYNOS7870_TRIGCON_HW_UNMASK);

	for (timeout = 200; timeout > 0; timeout--) {
		if (!(readl(decon + EXYNOS7870_DECON_UPDATE) &
		      EXYNOS7870_UPDATE_STANDALONE))
			break;
		udelay(1000);
	}

	if (!timeout)
		return -ETIMEDOUT;

	env_set("fastboot.display-handoff", "command-mode-active");

	return 0;
}

int board_fastboot_mmc_flash_write_setup(const char *name)
{
	if (!name)
		return -EINVAL;

	if (!strcasecmp(name, EXYNOS_BOOT_PARTITION) ||
	    !strcasecmp(name, EXYNOS_USERDATA_PARTITION))
		return 0;

	return -EPERM;
}

int board_fastboot_mmc_erase_setup(const char *name)
{
	/* Deployment uses sparse/raw writes; destructive erase is unnecessary. */
	return -EPERM;
}

int board_fdt_blob_setup(void **fdtp)
{
	/* If internal FDT is not available, use the external FDT instead. */
	if (fdt_check_header(*fdtp))
		*fdtp = (void *)get_prev_bl_fdt_addr();

	return 0;
}

int timer_init(void)
{
	ofnode timer_node;

	/*
	 * In a lot of Exynos devices, the previous bootloader does not
	 * set CNTFRQ_EL0 properly. However, the timer node in
	 * devicetree has the correct frequency, use that instead.
	 */
	timer_node = ofnode_by_compatible(ofnode_null(), "arm,armv8-timer");
	gd->arch.timer_rate_hz = ofnode_read_u32_default(timer_node,
							 "clock-frequency", 0);

	return 0;
}

#include <video_font_ter32x64.h>

const char *exynos_current_initcall = "none";
int exynos_initcall_step = 0;

void exynos_draw_text(int x0, int y0, const char *str, u32 fg, u32 bg)
{
	volatile u32 *fb = (volatile u32 *)0xed000000;
	int cur_x = x0;
	int cur_y = y0;

	while (*str) {
		char c = *str++;
		if (c == '\n') {
			cur_y += 68;
			cur_x = x0;
			continue;
		}
		if ((unsigned char)c >= 256)
			c = '?';
		const unsigned char *glyph = &video_fontdata_32x64[(unsigned char)c * 256];
		for (int row = 0; row < 64; row++) {
			int fb_y = cur_y + row;
			if (fb_y < 0 || fb_y >= 2520)
				continue;
			for (int b = 0; b < 4; b++) {
				unsigned char byte = glyph[row * 4 + b];
				for (int bit = 0; bit < 8; bit++) {
					int fb_x = cur_x + b * 8 + bit;
					if (fb_x < 0 || fb_x >= 1080)
						continue;
					fb[fb_y * 1080 + fb_x] = (byte & (0x80 >> bit)) ? fg : bg;
				}
			}
		}
		cur_x += 32;
		if (cur_x + 32 > 1080) {
			cur_x = x0;
			cur_y += 68;
		}
	}
}

static void int_to_str(int v, char *out)
{
	int p = 0;
	if (v < 0) {
		out[p++] = '-';
		v = -v;
	}
	char tmp[16];
	int tp = 0;
	if (v == 0) {
		tmp[tp++] = '0';
	} else {
		while (v > 0 && tp < 15) {
			tmp[tp++] = '0' + (v % 10);
			v /= 10;
		}
	}
	while (tp > 0)
		out[p++] = tmp[--tp];
	out[p] = '\0';
}

void exynos_report_initcall_step(const char *name, int step)
{
	exynos_current_initcall = name;
	if (gd->flags & GD_FLG_RELOC) {
		char line[34];
		int i;
		for (i = 0; i < 32; i++)
			line[i] = ' ';
		line[32] = '\0';
		for (i = 0; i < 32 && name[i]; i++)
			line[i] = name[i];
		exynos_draw_text(30, 80, line, 0x00FFFFFF, 0x00000000);
	}
}

void exynos_report_initcall_fail(const char *name, int step, int ret)
{
	char ret_str[16];
	char step_str[16];
	char detail[64];

	int_to_str(ret, ret_str);
	int_to_str(step, step_str);

	int lp = 0;
	const char *s = "ret=";
	while (*s) detail[lp++] = *s++;
	s = ret_str;
	while (*s) detail[lp++] = *s++;
	s = " step=";
	while (*s) detail[lp++] = *s++;
	s = step_str;
	while (*s) detail[lp++] = *s++;
	detail[lp] = '\0';

	exynos_draw_text(30, 200, "INITCALL FAILED:", 0x00FF3333, 0x00000000);
	exynos_draw_text(30, 280, name, 0x00FFFF00, 0x00000000);
	exynos_draw_text(30, 360, detail, 0x0000FFFF, 0x00000000);
}

int board_early_init_f(void)
{
	exynos_parse_dram_banks(gd->fdt_blob);

	/* Visual milestone 1: RED stripe at top of framebuffer (0xed000000) */
	if (of_machine_is_compatible("samsung,exynos9610") ||
	    of_machine_is_compatible("motorola,troika")) {
		volatile u32 *fb = (volatile u32 *)0xed000000;
		for (int i = 0; i < 1080 * 40; i++)
			fb[i] = 0x00FF0000;
	}

	return 0;
}

int dram_init(void)
{
	unsigned int i;

	/* Visual milestone 2: YELLOW stripe */
	if (of_machine_is_compatible("samsung,exynos9610") ||
	    of_machine_is_compatible("motorola,troika")) {
		volatile u32 *fb = (volatile u32 *)0xed000000;
		for (int i = 1080 * 40; i < 1080 * 80; i++)
			fb[i] = 0x00FFFF00;
	}

	/* Select the largest RAM bank for U-Boot. */
	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		if (gd->ram_size < mem_map[i + 1].size) {
			gd->ram_base = mem_map[i + 1].phys;
			gd->ram_size = mem_map[i + 1].size;
		}
	}

	return 0;
}

int dram_init_banksize(void)
{
	unsigned int i;

	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		gd->bd->bi_dram[i].start = mem_map[i + 1].phys;
		gd->bd->bi_dram[i].size = mem_map[i + 1].size;
	}

	return 0;
}

int board_init(void)
{
	/* Visual milestone 3: GREEN stripe (indicates relocated C code running) */
	if (of_machine_is_compatible("samsung,exynos9610") ||
	    of_machine_is_compatible("motorola,troika")) {
		volatile u32 *fb = (volatile u32 *)0xed000000;
		for (int i = 1080 * 80; i < 1080 * 120; i++)
			fb[i] = 0x0000FF00;
	}

	return 0;
}

int misc_init_r(void)
{
	int ret;

	exynos_env_setup();

	ret = exynos_blk_env_setup();
	if (ret)
		return ret;

	ret = exynos_fastboot_setup();
	if (ret)
		return ret;

	ret = j7y17lte_panel_diagnostic();
	if (ret)
		log_warning("J7 panel calibration diagnostic failed: %d\n", ret);

	ret = exynos7870_command_mode_video_handoff();
	if (ret)
		log_warning("command-mode video handoff failed: %d\n", ret);

	if (of_machine_is_compatible("samsung,j7y17lte"))
		env_set("fastboot.efi-gop", "disabled-native-exynos-drm");

	return 0;
}
