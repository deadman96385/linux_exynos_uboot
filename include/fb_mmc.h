/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2014 Broadcom Corporation.
 */

#ifndef _FB_MMC_H_
#define _FB_MMC_H_

struct blk_desc;
struct disk_partition;

/**
 * board_fastboot_mmc_flash_write_setup() - authorize an MMC flash target
 *
 * Boards may override this hook to enforce a partition allowlist. It runs
 * before raw-device, GPT, MBR, boot-hardware-partition, and named-partition
 * handling.
 *
 * Return: 0 to allow the write, or a negative error to reject it.
 */
int board_fastboot_mmc_flash_write_setup(const char *name);

/**
 * board_fastboot_mmc_erase_setup() - authorize an MMC erase target
 *
 * Return: 0 to allow the erase, or a negative error to reject it.
 */
int board_fastboot_mmc_erase_setup(const char *name);

/**
 * fastboot_mmc_get_part_info() - Lookup eMMC partion by name
 *
 * @part_name: Named partition to lookup
 * @dev_desc: Pointer to returned blk_desc pointer
 * @part_info: Pointer to returned struct disk_partition
 * @response: Pointer to fastboot response buffer
 */
int fastboot_mmc_get_part_info(const char *part_name,
			       struct blk_desc **dev_desc,
			       struct disk_partition *part_info,
			       char *response);

/**
 * fastboot_mmc_flash_write() - Write image to eMMC for fastboot
 *
 * @cmd: Named partition to write image to
 * @download_buffer: Pointer to image data
 * @download_bytes: Size of image data
 * @response: Pointer to fastboot response buffer
 */
void fastboot_mmc_flash_write(const char *cmd, void *download_buffer,
			      u32 download_bytes, char *response);
/**
 * fastboot_mmc_flash_erase() - Erase eMMC for fastboot
 *
 * @cmd: Named partition to erase
 * @response: Pointer to fastboot response buffer
 */
void fastboot_mmc_erase(const char *cmd, char *response);
#endif
