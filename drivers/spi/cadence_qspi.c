// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2012
 * Altera Corporation <www.altera.com>
 */

#include <common.h>
#include <asm-generic/io.h>
#include <clk.h>
#include <dm.h>
#include <fdtdec.h>
#include <malloc.h>
#include <spi.h>
#include <spi-mem.h>
#include <linux/errno.h>
#include "cadence_qspi.h"

#define CQSPI_STIG_READ			0
#define CQSPI_STIG_WRITE		1
#define CQSPI_READ			2
#define CQSPI_WRITE			3

#ifndef CONFIG_CQSPI_REF_CLK
#define CONFIG_CQSPI_REF_CLK  0
#endif

DECLARE_GLOBAL_DATA_PTR;

static int cadence_spi_write_speed(struct udevice *bus, uint hz)
{
	struct cadence_spi_platdata *plat = bus->platdata;
	struct cadence_spi_priv *priv = dev_get_priv(bus);

	cadence_qspi_apb_config_baudrate_div(priv->regbase,
					     priv->ref_clk_hz, hz);

	/* Reconfigure delay timing if speed is changed. */
	cadence_qspi_apb_delay(priv->regbase, priv->ref_clk_hz, hz,
			       plat->tshsl_ns, plat->tsd2d_ns,
			       plat->tchsh_ns, plat->tslch_ns);

	return 0;
}

static int cadence_spi_read_id(void *reg_base, u8 len, u8 *idcode)
{
	struct spi_mem_op op = SPI_MEM_OP(SPI_MEM_OP_CMD(0x9F, 1),
					  SPI_MEM_OP_NO_ADDR,
					  SPI_MEM_OP_NO_DUMMY,
					  SPI_MEM_OP_DATA_IN(len, idcode, 1));

	return cadence_qspi_apb_command_read(reg_base, &op);
}

/* Calibration sequence to determine the read data capture delay register */
static int spi_calibration(struct udevice *bus, uint hz)
{
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	void *base = priv->regbase;
	unsigned int idcode = 0, temp = 0;
	int err = 0, i, range_lo = -1, range_hi = -1;

	/* start with slowest clock (1 MHz) */
	cadence_spi_write_speed(bus, 1000000);

	/* configure the read data capture delay register to 0 */
	cadence_qspi_apb_readdata_capture(base, 1, 0);

	/* Enable QSPI */
	cadence_qspi_apb_controller_enable(base);

	/* read the ID which will be our golden value */
	err = cadence_spi_read_id(base, 3, (u8 *)&idcode);
	if (err) {
		puts("SF: Calibration failed (read)\n");
		return err;
	}

	/* use back the intended clock and find low range */
	cadence_spi_write_speed(bus, hz);
	for (i = 0; i < CQSPI_READ_CAPTURE_MAX_DELAY; i++) {
		/* Disable QSPI */
		cadence_qspi_apb_controller_disable(base);

		/* reconfigure the read data capture delay register */
		cadence_qspi_apb_readdata_capture(base, 1, i);

		/* Enable back QSPI */
		cadence_qspi_apb_controller_enable(base);

		/* issue a RDID to get the ID value */
		err = cadence_spi_read_id(base, 3, (u8 *)&temp);
		if (err) {
			puts("SF: Calibration failed (read)\n");
			return err;
		}

		/* search for range lo */
		if (range_lo == -1 && temp == idcode) {
			range_lo = i;
			continue;
		}

		/* search for range hi */
		if (range_lo != -1 && temp != idcode) {
			range_hi = i - 1;
			break;
		}
		range_hi = i;
	}

	if (range_lo == -1) {
		puts("SF: Calibration failed (low range)\n");
		return err;
	}

	/* Disable QSPI for subsequent initialization */
	cadence_qspi_apb_controller_disable(base);

	/* configure the final value for read data capture delay register */
	cadence_qspi_apb_readdata_capture(base, 1, (range_hi + range_lo) / 2);
	debug("SF: Read data capture delay calibrated to %i (%i - %i)\n",
	      (range_hi + range_lo) / 2, range_lo, range_hi);

	/* just to ensure we do once only when speed or chip select change */
	priv->qspi_calibrated_hz = hz;
	priv->qspi_calibrated_cs = spi_chip_select(bus);

	return 0;
}

static int cadence_spi_set_speed(struct udevice *bus, uint hz)
{
	struct cadence_spi_platdata *plat = bus->platdata;
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	int err;

	if (hz > plat->max_hz)
		hz = plat->max_hz;

	/* Disable QSPI */
	cadence_qspi_apb_controller_disable(priv->regbase);

	/*
	 * Calibration required for different current SCLK speed, requested
	 * SCLK speed or chip select
	 */
	if (priv->previous_hz != hz ||
	    priv->qspi_calibrated_hz != hz ||
	    priv->qspi_calibrated_cs != spi_chip_select(bus)) {
		err = spi_calibration(bus, hz);
		if (err)
			return err;

		/* prevent calibration run when same as previous request */
		priv->previous_hz = hz;
	}

	/* Enable QSPI */
	cadence_qspi_apb_controller_enable(priv->regbase);

	debug("%s: speed=%d\n", __func__, hz);

	return 0;
}

static int cadence_spi_probe(struct udevice *bus)
{
	struct cadence_spi_platdata *plat = bus->platdata;
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	long int clk_rate = 0;

	priv->regbase = plat->regbase;
	priv->ahbbase = plat->ahbbase;

	if (plat->clk.dev)
		clk_rate = clk_get_rate(&plat->clk);

	if (clk_rate <= 0) {
		clk_rate = dev_read_u32_default(bus, "clock-frequency",
						CONFIG_CQSPI_REF_CLK);
		if (!clk_rate) {
			printf("cqspi: refclk frequency unavailable\n");
			return -EINVAL;
		}
	}
	priv->ref_clk_hz = clk_rate;

	if (!priv->qspi_is_init) {
		cadence_qspi_apb_controller_init(plat);
		priv->qspi_is_init = 1;
	}

	return 0;
}

static int cadence_spi_set_mode(struct udevice *bus, uint mode)
{
	struct cadence_spi_platdata *plat = bus->platdata;
	struct cadence_spi_priv *priv = dev_get_priv(bus);

	/* Disable QSPI */
	cadence_qspi_apb_controller_disable(priv->regbase);

	/* Set SPI mode */
	cadence_qspi_apb_set_clk_mode(priv->regbase, mode);

	/* Enable Direct Access Controller */
	if (plat->use_dac_mode)
		cadence_qspi_apb_dac_mode_enable(priv->regbase);

	/* Enable QSPI */
	cadence_qspi_apb_controller_enable(priv->regbase);

	return 0;
}

static int cadence_spi_mem_exec_op(struct spi_slave *spi,
				   const struct spi_mem_op *op)
{
	struct udevice *bus = spi->dev->parent;
	struct cadence_spi_platdata *plat = bus->platdata;
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	void *base = priv->regbase;
	int err = 0;
	u32 mode = CQSPI_STIG_WRITE;

	/* Set Chip select */
	cadence_qspi_apb_chipselect(base, spi_chip_select(spi->dev),
				    plat->is_decoded_cs);

		if (op->data.dir == SPI_MEM_DATA_IN) {
			/* read */
			/* Use STIG if no address. */
			if (!op->addr.nbytes)
				mode = CQSPI_STIG_READ;
			else
				mode = CQSPI_READ;
		} else {
			/* write */
			if (!op->addr.nbytes)
				mode = CQSPI_STIG_WRITE;
			else
				mode = CQSPI_WRITE;
		}

		switch (mode) {
		case CQSPI_STIG_READ:
			err = cadence_qspi_apb_command_read(
				base, op);

		break;
		case CQSPI_STIG_WRITE:
			err = cadence_qspi_apb_command_write(base, op);
		break;
		case CQSPI_READ:
			err = cadence_qspi_apb_read_setup(plat, op);
			if (!err) {
				err = cadence_qspi_apb_read_execute
				(plat, op);
			}
		break;
		case CQSPI_WRITE:
			err = cadence_qspi_apb_write_setup
				(plat, op);
			if (!err) {
				err = cadence_qspi_apb_write_execute
				(plat, op);
			}
		break;
		default:
			err = -1;
			break;
		}

	return err;
}

static int cadence_spi_ofdata_to_platdata(struct udevice *bus)
{
	struct cadence_spi_platdata *plat = bus->platdata;
	const void *blob = gd->fdt_blob;
	int node = dev_of_offset(bus);
	fdt_addr_t mmap_addr, mmap_size;
	int subnode;
	int ret;

	plat->regbase = (void *)devfdt_get_addr_index(bus, 0);
	plat->ahbbase = (void *)devfdt_get_addr_index(bus, 1);
	mmap_addr = devfdt_get_addr_size_index(bus, 1, &mmap_size);
	plat->ahbbase = map_physmem(mmap_addr, mmap_size, MAP_NOCACHE);
	plat->ahbsize = mmap_size;
	if (plat->ahbsize >= SZ_8M)
		plat->use_dac_mode = true;

	plat->is_decoded_cs = fdtdec_get_bool(blob, node, "cdns,is-decoded-cs");
	plat->fifo_depth = fdtdec_get_uint(blob, node, "cdns,fifo-depth", 128);
	plat->fifo_width = fdtdec_get_uint(blob, node, "cdns,fifo-width", 4);
	plat->trigger_address = fdtdec_get_uint(blob, node,
						"cdns,trigger-address", 0);

	ret = clk_get_by_index(bus, 0, &plat->clk);
	if (ret && ret != -ENOENT && ret != -ENODEV && ret != -ENOSYS) {
		debug("cqspi: failed to get clock\n");
		return ret;
	}

	/* All other paramters are embedded in the child node */
	subnode = fdt_first_subnode(blob, node);
	if (subnode < 0) {
		printf("Error: subnode with SPI flash config missing!\n");
		return -ENODEV;
	}

	/* Use 500 KHz as a suitable default */
	plat->max_hz = fdtdec_get_uint(blob, subnode, "spi-max-frequency",
				       500000);

	/* Read other parameters from DT */
	plat->page_size = fdtdec_get_uint(blob, subnode, "page-size", 256);
	plat->block_size = fdtdec_get_uint(blob, subnode, "block-size", 16);
	plat->tshsl_ns = fdtdec_get_uint(blob, subnode, "cdns,tshsl-ns", 200);
	plat->tsd2d_ns = fdtdec_get_uint(blob, subnode, "cdns,tsd2d-ns", 255);
	plat->tchsh_ns = fdtdec_get_uint(blob, subnode, "cdns,tchsh-ns", 20);
	plat->tslch_ns = fdtdec_get_uint(blob, subnode, "cdns,tslch-ns", 20);

	debug("%s: regbase=%p ahbbase=%p max-frequency=%d page-size=%d\n",
	      __func__, plat->regbase, plat->ahbbase, plat->max_hz,
	      plat->page_size);

	return 0;
}

static const struct spi_controller_mem_ops cadence_spi_mem_ops = {
	.exec_op = cadence_spi_mem_exec_op,
};

static const struct dm_spi_ops cadence_spi_ops = {
	.set_speed	= cadence_spi_set_speed,
	.set_mode	= cadence_spi_set_mode,
	.mem_ops	= &cadence_spi_mem_ops,
	/*
	 * cs_info is not needed, since we require all chip selects to be
	 * in the device tree explicitly
	 */
};

static const struct udevice_id cadence_spi_ids[] = {
	{ .compatible = "cdns,qspi-nor" },
	{ }
};

U_BOOT_DRIVER(cadence_spi) = {
	.name = "cadence_spi",
	.id = UCLASS_SPI,
	.of_match = cadence_spi_ids,
	.ops = &cadence_spi_ops,
	.ofdata_to_platdata = cadence_spi_ofdata_to_platdata,
	.platdata_auto_alloc_size = sizeof(struct cadence_spi_platdata),
	.priv_auto_alloc_size = sizeof(struct cadence_spi_priv),
	.probe = cadence_spi_probe,
};
