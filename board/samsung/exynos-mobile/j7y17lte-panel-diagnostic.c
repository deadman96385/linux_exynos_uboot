// SPDX-License-Identifier: GPL-2.0
/*
 * Read-only AMS549KU15/S6E3FA3 calibration diagnostic for j7y17lte.
 *
 * S-Boot leaves the Exynos7870 DSIM and panel active. This code deliberately
 * does not reset either block or program gamma, AOR, ELVSS, ACL, or HBM. It
 * performs polled DCS transfers against that live link, reads the exact panel
 * calibration payloads, and always closes both manufacturer keys.
 */

#include <asm/io.h>
#include <dm.h>
#include <dm/ofnode.h>
#include <env.h>
#include <errno.h>
#include <hexdump.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/types.h>

#include "j7y17lte-panel-diagnostic.h"

#define J7_DSIM_BASE			0x14800000UL
#define J7_DSIM_ESCMODE			0x1c
#define J7_DSIM_INTSRC			0x34
#define J7_DSIM_PKTHDR			0x3c
#define J7_DSIM_PAYLOAD			0x40
#define J7_DSIM_RXFIFO			0x44
#define J7_DSIM_FIFOCTRL			0x4c

#define J7_DSIM_INT_RX_TIMEOUT		BIT(21)
#define J7_DSIM_INT_BTA_TIMEOUT		BIT(20)
#define J7_DSIM_INT_SFR_FIFO_EMPTY	BIT(29)
#define J7_DSIM_INT_SFR_HDR_FIFO_EMPTY	BIT(28)
#define J7_DSIM_INT_RX_DONE		BIT(18)
#define J7_DSIM_INT_RX_ECC_ERR		BIT(15)
#define J7_DSIM_INT_RX_CRC_ERR		BIT(14)

#define J7_DSIM_CMD_LPDT_LP		BIT(7)
#define J7_DSIM_FIFO_EMPTY_RX		BIT(12)
#define J7_DSIM_FIFO_INIT_RX		BIT(2)

#define J7_DSIM_RX_FIFO_WORDS		64
#define J7_DSIM_TIMEOUT_US		100000

#define MIPI_DSI_DCS_SHORT_WRITE		0x05
#define MIPI_DSI_DCS_SHORT_WRITE_PARAM	0x15
#define MIPI_DSI_DCS_LONG_WRITE		0x39
#define MIPI_DSI_DCS_READ		0x06
#define MIPI_DSI_SET_MAX_RETURN		0x37

#define MIPI_DSI_RX_ACK_ERROR		0x02
#define MIPI_DSI_RX_GENERIC_SHORT_1	0x11
#define MIPI_DSI_RX_GENERIC_SHORT_2	0x12
#define MIPI_DSI_RX_GENERIC_LONG		0x1a
#define MIPI_DSI_RX_DCS_LONG		0x1c
#define MIPI_DSI_RX_DCS_SHORT_1		0x21
#define MIPI_DSI_RX_DCS_SHORT_2		0x22

#define S6E3FA3_ID_LEN			3
#define S6E3FA3_MTP_LEN			35
#define S6E3FA3_MTP_DATE_LEN		47
#define S6E3FA3_DATE_OFFSET		40
#define S6E3FA3_DATE_LEN			7
#define S6E3FA3_ELVSS_LEN		23
#define S6E3FA3_HBM_LEN			28
#define S6E3FA3_EXPORT_LEN		28

static void __iomem * const j7_dsim = (void __iomem *)J7_DSIM_BASE;

static u32 j7_dsim_read(u32 offset)
{
	return readl(j7_dsim + offset);
}

static void j7_dsim_write(u32 offset, u32 value)
{
	writel(value, j7_dsim + offset);
}

static void j7_dsim_clear_interrupts(void)
{
	u32 status = j7_dsim_read(J7_DSIM_INTSRC);

	if (status)
		j7_dsim_write(J7_DSIM_INTSRC, status);
}

static int j7_dsim_wait_interrupt(u32 completion)
{
	unsigned int timeout;
	u32 status;

	for (timeout = 0; timeout < J7_DSIM_TIMEOUT_US; timeout += 10) {
		status = j7_dsim_read(J7_DSIM_INTSRC);
		if (status & (J7_DSIM_INT_RX_TIMEOUT |
			      J7_DSIM_INT_BTA_TIMEOUT |
			      J7_DSIM_INT_RX_ECC_ERR |
			      J7_DSIM_INT_RX_CRC_ERR))
			return -EIO;
		if (status & completion)
			return 0;
		udelay(10);
	}

	return -ETIMEDOUT;
}

static void j7_dsim_drain_rx(void)
{
	unsigned int words;

	for (words = 0; words < J7_DSIM_RX_FIFO_WORDS; words++) {
		if (j7_dsim_read(J7_DSIM_FIFOCTRL) &
		    J7_DSIM_FIFO_EMPTY_RX)
			break;
		j7_dsim_read(J7_DSIM_RXFIFO);
	}
}

static void j7_dsim_reset_rx(void)
{
	u32 fifoctrl = j7_dsim_read(J7_DSIM_FIFOCTRL);

	/* Samsung's Exynos7870 driver pulses INIT_RX low, then high. */
	j7_dsim_write(J7_DSIM_FIFOCTRL,
		      fifoctrl & ~J7_DSIM_FIFO_INIT_RX);
	j7_dsim_write(J7_DSIM_FIFOCTRL,
		      fifoctrl | J7_DSIM_FIFO_INIT_RX);
}

static int j7_dsim_write_header(u8 type, u8 data0, u8 data1)
{
	u32 header = type | ((u32)data0 << 8) | ((u32)data1 << 16);

	j7_dsim_clear_interrupts();
	j7_dsim_write(J7_DSIM_PKTHDR, header);

	return j7_dsim_wait_interrupt(J7_DSIM_INT_SFR_HDR_FIFO_EMPTY);
}

static int j7_dsim_dcs_write(const u8 *data, size_t length)
{
	size_t offset;
	u32 payload;
	u8 type;
	int ret;

	if (!data || !length || length > 0xffff)
		return -EINVAL;

	if (length <= 2) {
		type = length == 1 ? MIPI_DSI_DCS_SHORT_WRITE :
				    MIPI_DSI_DCS_SHORT_WRITE_PARAM;
		return j7_dsim_write_header(type, data[0],
					    length == 2 ? data[1] : 0);
	}

	j7_dsim_clear_interrupts();
	for (offset = 0; offset < length; offset += 4) {
		payload = data[offset];
		if (offset + 1 < length)
			payload |= (u32)data[offset + 1] << 8;
		if (offset + 2 < length)
			payload |= (u32)data[offset + 2] << 16;
		if (offset + 3 < length)
			payload |= (u32)data[offset + 3] << 24;
		j7_dsim_write(J7_DSIM_PAYLOAD, payload);
	}

	j7_dsim_write(J7_DSIM_PKTHDR,
		      MIPI_DSI_DCS_LONG_WRITE | ((u32)length << 8));

	ret = j7_dsim_wait_interrupt(J7_DSIM_INT_SFR_FIFO_EMPTY);
	if (ret)
		return ret;

	return j7_dsim_wait_interrupt(J7_DSIM_INT_SFR_HDR_FIFO_EMPTY);
}

static int j7_dsim_wait_rx(void)
{
	unsigned int timeout;
	u32 status;

	for (timeout = 0; timeout < J7_DSIM_TIMEOUT_US; timeout += 10) {
		status = j7_dsim_read(J7_DSIM_INTSRC);
		if (status & J7_DSIM_INT_RX_DONE)
			return 0;
		udelay(10);
	}

	return -ETIMEDOUT;
}

static int j7_dsim_read_long(u32 response, u8 *data, size_t length)
{
	size_t copied = 0;
	size_t response_length = (response >> 8) & 0xffff;
	u32 payload;

	if (response_length != length)
		return -EMSGSIZE;

	while (copied < length) {
		if (j7_dsim_read(J7_DSIM_FIFOCTRL) &
		    J7_DSIM_FIFO_EMPTY_RX)
			return -EIO;
		payload = j7_dsim_read(J7_DSIM_RXFIFO);

		data[copied++] = payload;
		if (copied < length)
			data[copied++] = payload >> 8;
		if (copied < length)
			data[copied++] = payload >> 16;
		if (copied < length)
			data[copied++] = payload >> 24;
	}

	return copied;
}

static int j7_dsim_dcs_read(u8 command, u8 *data, size_t length)
{
	u32 response;
	u8 type;
	int ret;

	if (!data || !length || length > 0xffff)
		return -EINVAL;

	j7_dsim_reset_rx();
	j7_dsim_write(J7_DSIM_INTSRC, J7_DSIM_INT_RX_DONE);

	ret = j7_dsim_write_header(MIPI_DSI_SET_MAX_RETURN,
				   length & 0xff, length >> 8);
	if (ret)
		return ret;

	ret = j7_dsim_write_header(MIPI_DSI_DCS_READ, command, 0);
	if (ret)
		goto out;

	ret = j7_dsim_wait_rx();
	if (ret) {
		printf("J7Y17LTE-PANEL:read-failed cmd=%02x error=%d ints=%08x fifo=%08x\n",
		       command, ret, j7_dsim_read(J7_DSIM_INTSRC),
		       j7_dsim_read(J7_DSIM_FIFOCTRL));
		goto out;
	}

	response = j7_dsim_read(J7_DSIM_RXFIFO);
	type = response & 0xff;
	switch (type) {
	case MIPI_DSI_RX_GENERIC_SHORT_1:
	case MIPI_DSI_RX_DCS_SHORT_1:
		if (length != 1) {
			ret = -EMSGSIZE;
			break;
		}
		data[0] = response >> 8;
		ret = 1;
		break;
	case MIPI_DSI_RX_GENERIC_SHORT_2:
	case MIPI_DSI_RX_DCS_SHORT_2:
		if (length != 2) {
			ret = -EMSGSIZE;
			break;
		}
		data[0] = response >> 8;
		data[1] = response >> 16;
		ret = 2;
		break;
	case MIPI_DSI_RX_GENERIC_LONG:
	case MIPI_DSI_RX_DCS_LONG:
		ret = j7_dsim_read_long(response, data, length);
		break;
	case MIPI_DSI_RX_ACK_ERROR:
		printf("J7Y17LTE-PANEL:dsi-error-report=%04x\n",
		       (response >> 8) & 0xffff);
		ret = -EIO;
		break;
	default:
		printf("J7Y17LTE-PANEL:unexpected-response=%08x\n", response);
		ret = -EPROTO;
		break;
	}

out:
	j7_dsim_drain_rx();
	j7_dsim_clear_interrupts();

	return ret;
}

static int j7_dsim_dcs_read_retry(u8 command, u8 *data, size_t length)
{
	unsigned int attempt;
	int ret = -EIO;

	for (attempt = 1; attempt <= 2; attempt++) {
		ret = j7_dsim_dcs_read(command, data, length);
		if (ret == length)
			return ret;
		printf("J7Y17LTE-PANEL:read-retry cmd=%02x attempt=%u ret=%d\n",
		       command, attempt, ret);
	}

	return ret;
}

static void j7_panel_print_bytes(const char *name, const u8 *data, size_t length)
{
	size_t i;

	printf("J7Y17LTE-PANEL:%s=", name);
	for (i = 0; i < length; i++)
		printf("%02x", data[i]);
	putc('\n');
}

static void j7_panel_export_bytes(const char *name, const u8 *data,
				  size_t length)
{
	char value[S6E3FA3_EXPORT_LEN * 2 + 1];
	char *end;

	if (length > S6E3FA3_EXPORT_LEN)
		return;

	end = bin2hex(value, data, length);
	*end = '\0';
	env_set(name, value);
}

int j7y17lte_panel_diagnostic(void)
{
	static const u8 key_f0_on[] = { 0xf0, 0x5a, 0x5a };
	static const u8 key_f0_off[] = { 0xf0, 0xa5, 0xa5 };
	u8 mtp_date[S6E3FA3_MTP_DATE_LEN];
	u8 elvss[S6E3FA3_ELVSS_LEN];
	u8 hbm[S6E3FA3_HBM_LEN];
	u8 id[S6E3FA3_ID_LEN];
	u32 escmode;
	int first_error = 0;
	int ret;

	if (!of_machine_is_compatible("samsung,j7y17lte"))
		return 0;

	escmode = j7_dsim_read(J7_DSIM_ESCMODE);
	j7_dsim_write(J7_DSIM_ESCMODE, escmode | J7_DSIM_CMD_LPDT_LP);
	printf("J7Y17LTE-PANEL:dsim ints=%08x fifo=%08x escmode=%08x->%08x\n",
	       j7_dsim_read(J7_DSIM_INTSRC),
	       j7_dsim_read(J7_DSIM_FIFOCTRL), escmode,
	       j7_dsim_read(J7_DSIM_ESCMODE));

	ret = j7_dsim_dcs_read_retry(0x04, id, sizeof(id));
	if (ret != sizeof(id)) {
		first_error = ret < 0 ? ret : -EIO;
		printf("J7Y17LTE-PANEL:id-read-failed=%d\n", first_error);
		goto out_status;
	}
	j7_panel_print_bytes("id", id, sizeof(id));
	j7_panel_export_bytes("fastboot.panel-id", id, sizeof(id));

	ret = j7_dsim_dcs_write(key_f0_on, sizeof(key_f0_on));
	if (ret) {
		first_error = ret;
		goto keys_off;
	}
	ret = j7_dsim_dcs_read_retry(0xc8, mtp_date, sizeof(mtp_date));
	if (ret != sizeof(mtp_date)) {
		first_error = ret < 0 ? ret : -EIO;
		printf("J7Y17LTE-PANEL:mtp-read-failed=%d\n", first_error);
		goto keys_off;
	}
	j7_panel_print_bytes("c8-raw", mtp_date, sizeof(mtp_date));
	j7_panel_print_bytes("mtp-c8", mtp_date, S6E3FA3_MTP_LEN);
	j7_panel_print_bytes("date-c8", mtp_date + S6E3FA3_DATE_OFFSET,
			     S6E3FA3_DATE_LEN);
	j7_panel_export_bytes("fastboot.panel-mtp-0", mtp_date,
			      S6E3FA3_EXPORT_LEN);
	j7_panel_export_bytes("fastboot.panel-mtp-1",
			      mtp_date + S6E3FA3_EXPORT_LEN,
			      S6E3FA3_MTP_LEN - S6E3FA3_EXPORT_LEN);
	j7_panel_export_bytes("fastboot.panel-date",
			      mtp_date + S6E3FA3_DATE_OFFSET,
			      S6E3FA3_DATE_LEN);

	ret = j7_dsim_dcs_read_retry(0xb6, elvss, sizeof(elvss));
	if (ret != sizeof(elvss)) {
		first_error = ret < 0 ? ret : -EIO;
		printf("J7Y17LTE-PANEL:elvss-read-failed=%d\n", first_error);
		goto keys_off;
	}
	j7_panel_print_bytes("elvss-b6", elvss, sizeof(elvss));
	j7_panel_export_bytes("fastboot.panel-elvss", elvss, sizeof(elvss));

	ret = j7_dsim_dcs_read_retry(0xb4, hbm, sizeof(hbm));
	if (ret != sizeof(hbm)) {
		first_error = ret < 0 ? ret : -EIO;
		printf("J7Y17LTE-PANEL:hbm-read-failed=%d\n", first_error);
		goto keys_off;
	}
	j7_panel_print_bytes("hbm-b4", hbm, sizeof(hbm));
	j7_panel_export_bytes("fastboot.panel-hbm", hbm, sizeof(hbm));

keys_off:
	ret = j7_dsim_dcs_write(key_f0_off, sizeof(key_f0_off));
	if (!first_error)
		first_error = ret;

out_status:
	j7_dsim_write(J7_DSIM_ESCMODE, escmode);
	if (first_error) {
		env_set("fastboot.panel-diagnostic", "calibration-read-failed");
		printf("J7Y17LTE-PANEL:result=failed error=%d\n", first_error);
	} else {
		env_set("fastboot.panel-diagnostic", "calibration-readable");
		printf("J7Y17LTE-PANEL:result=calibration-readable\n");
	}

	return first_error;
}
