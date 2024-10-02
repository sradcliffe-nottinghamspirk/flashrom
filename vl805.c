/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2019, 2020 Carl-Daniel Hailfinger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

/* Driver for the VIA VL805 programmer hardware by VIA.
 * See http://www.via.com/ for more info.
 */

#include "programmer.h"
#include "spi.h"
#include "flash.h"
#include "hwaccess.h"

const struct dev_entry devs_vl805[] = {
	{0x1106, 0x3483, NT, "VIA", "VL805"},
	{0},
};

static struct pci_dev *dev = NULL;

static void vl805_setregval(int reg, uint32_t val)
{
	pci_write_long(dev, 0x78, reg);
	pci_write_long(dev, 0x7c, val);
}

static uint32_t vl805_getregval(int reg)
{
	pci_write_long(dev, 0x78, reg);

	return pci_read_long(dev, 0x7c);
}

/* Some of the registers have unknown purpose and are just used inside the init sequence replay. */
#define VL805_REG_0x30004		0x00030004
#define VL805_REG_STOP_POLLING		0x0004000c
#define VL805_REG_WB_EN			0x00040020
#define VL805_REG_SPI_OUTDATA		0x000400d0
#define VL805_REG_SPI_INDATA		0x000400e0
#define VL805_REG_SPI_TRANSACTION	0x000400f0
#define VL805_REG_CLK_DIV		0x000400f8
#define VL805_REG_SPI_CHIP_ENABLE_LEVEL	0x000400fc

/* Send a SPI command to the flash chip. */
static int vl805_spi_send_command(const struct flashctx *flash,
			unsigned int writecnt,
			unsigned int readcnt,
			const unsigned char *writearr,
			unsigned char *readarr)
{
	unsigned int i, j;
	uint32_t outdata;
	uint32_t indata = 0;
	unsigned int curwritecnt = 0;
	unsigned int curreadcnt = 0;
	unsigned int curtotalcnt = 0;
	unsigned int readpos = 0;

	unsigned int writes_left = writecnt;
	unsigned int reads_left = readcnt;
	unsigned int totalcnt = writecnt + readcnt;

	//msg_pdbg("vl805 write: 0x%08x cnt: %d Read: cnt %d, totalcnt %d\n", writearr[0], writecnt, readcnt, totalcnt);
	vl805_setregval(VL805_REG_SPI_CHIP_ENABLE_LEVEL, 0x00000000);

	for (j = 0; j < totalcnt; j += 4) {
		curtotalcnt = min(4, totalcnt - j);
		curwritecnt = min(4, writes_left);
		curreadcnt = min(4 - curwritecnt, reads_left);

		outdata = 0;
		for (i = 0; i < curtotalcnt; i++) {
			outdata <<= 8;
			if (i < curwritecnt) {
				outdata |= writearr[j + i];
				writes_left--;
			}
		}
		vl805_setregval(VL805_REG_SPI_OUTDATA, outdata);
		//msg_pdbg("VL805_REG_SPI_OUTDATA: 0x%08x curwritecnt=%d\n", outdata, curwritecnt);
		vl805_setregval(VL805_REG_SPI_TRANSACTION, 0x00000580 | (curtotalcnt << 3));
		//msg_pdbg("VL805_REG_SPI_TRANSACTION: 0x%08x curtotalcnt=%d\n", 0x00000580 | (curtotalcnt << 3), curtotalcnt);
		indata = vl805_getregval(VL805_REG_SPI_INDATA);
		//msg_pdbg("VL805_REG_SPI_INDATAN: 0x%08x curreadcnt=%d\n", indata, curreadcnt);
		for (i = curreadcnt; i > 0; i--) {
			readarr[readpos++] = (indata >> (8 * (i-1))) & 0xff;
			reads_left--;
		}
	}

	vl805_setregval(VL805_REG_SPI_CHIP_ENABLE_LEVEL, 0x00000001);

	return 0;
}

static const struct spi_master spi_master_vl805 = {
	.max_data_read	= 64 * 1024, /* Maximum data read size in one go (excluding opcode+address). */
	.max_data_write	= 256, /* Maximum data write size in one go (excluding opcode+address). */
	.command	= vl805_spi_send_command,
	.multicommand	= default_spi_send_multicommand,
	.read		= default_spi_read,
	.write_256	= default_spi_write_256,
	.write_aai	= default_spi_write_aai,
};

static void vl805_programmer_active(uint8_t val)
{
	pci_write_byte(dev, 0x43, val);
}

static int vl805_shutdown(void *data)
{
	/* Shutdown stuff. */
	vl805_programmer_active(0x0);

	return 0;
}

int vl805_init(void)
{
	uint32_t val;

	if (rget_io_perms())
		return 1;

	dev = pcidev_init(devs_vl805, PCI_BASE_ADDRESS_0); /* Actually no BAR setup needed at all. */
	if (!dev)
		return 1;

	vl805_programmer_active(0x1);
	val = pci_read_long(dev, 0x50);
	msg_pdbg("VL805 firmware version 0x%08x\n", val);
	vl805_programmer_active(0x0);

	/* Some sort of init sequence, just copied from the logs. */
	vl805_programmer_active(0x1);

	vl805_setregval(VL805_REG_SPI_CHIP_ENABLE_LEVEL, 0x00000001);
	// vl805_setregval(VL805_REG_0x30004, 0x00000200);
	val = vl805_getregval(VL805_REG_WB_EN);
	vl805_setregval(VL805_REG_WB_EN, (val & 0xffffff00) | 0x01);
	val = vl805_getregval(VL805_REG_STOP_POLLING);
	vl805_setregval(VL805_REG_STOP_POLLING, (val & 0xffffff00) | 0x01);

	/* We send 4 uninitialized(?) bytes to the flash chip here. */
	vl805_setregval(VL805_REG_SPI_TRANSACTION, 0x000005a0);
	vl805_setregval(VL805_REG_CLK_DIV, 0x0000000a);

	/* Some sort of cleanup sequence, just copied from the logs. */
	//vl805_setregval(VL805_REG_SPI_TRANSACTION, 0x00000000);
	vl805_programmer_active(0x0);

	register_shutdown(vl805_shutdown, NULL);
	vl805_programmer_active(0x1);

	register_spi_master(&spi_master_vl805);

	return 0;
}
