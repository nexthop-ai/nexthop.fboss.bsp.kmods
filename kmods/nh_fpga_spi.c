// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 Nexthop Systems Inc.

#include <linux/module.h>
#include <linux/auxiliary_bus.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/sched.h>
#include <linux/spi/spi.h>
#include <linux/string.h>

#include "nh_fpga_fbiob.h"

#define DRIVER_NAME "nh_fpga_spi"
#define DRIVER_VERSION "1.0"

/* Xilinx AXI Quad SPI registers (AXI4-Lite, non-XIP) */
#define NH_FPGA_SPI_SRR 0x40
#define NH_FPGA_SPI_CR 0x60
#define NH_FPGA_SPI_SR 0x64
#define NH_FPGA_SPI_DTR 0x68
#define NH_FPGA_SPI_DRR 0x6c
#define NH_FPGA_SPI_SSR 0x70
#define NH_FPGA_SPI_RX_OCY 0x78
#define NH_FPGA_SPI_REG_SIZE 0x80

#define NH_FPGA_SPI_SRR_RESET 0x0a

#define NH_FPGA_SPI_CR_ENABLE BIT(1)
#define NH_FPGA_SPI_CR_MASTER BIT(2)
#define NH_FPGA_SPI_CR_CPOL BIT(3)
#define NH_FPGA_SPI_CR_CPHA BIT(4)
#define NH_FPGA_SPI_CR_TXFIFO_RESET BIT(5)
#define NH_FPGA_SPI_CR_RXFIFO_RESET BIT(6)
#define NH_FPGA_SPI_CR_MANUAL_SS BIT(7)
#define NH_FPGA_SPI_CR_INHIBIT BIT(8)
#define NH_FPGA_SPI_CR_BASE                                 \
	(NH_FPGA_SPI_CR_MANUAL_SS | NH_FPGA_SPI_CR_MASTER | \
	 NH_FPGA_SPI_CR_ENABLE)

#define NH_FPGA_SPI_SR_RX_EMPTY BIT(0)
#define NH_FPGA_SPI_SR_RX_FULL BIT(1)
#define NH_FPGA_SPI_SR_TX_EMPTY BIT(2)
#define NH_FPGA_SPI_SR_TX_FULL BIT(3)
#define NH_FPGA_SPI_SR_CMD_ERR BIT(10)

#define NH_FPGA_SPI_SS_NONE 0xffffffff
#define NH_FPGA_SPI_MAX_CS 32
#define NH_FPGA_SPI_MAX_FIFO_DEPTH 256
#define NH_FPGA_SPI_TIMEOUT_US 10000
/* Time for the last bytes to reach the RX FIFO once the TX FIFO is empty */
#define NH_FPGA_SPI_DRAIN_US 1000

#define SPINOR_OP_RDID 0x9f

struct nh_fpga_spi {
	struct device *dev;
	void __iomem *base;
	unsigned int fifo_depth;
	u32 cr;
};

static void nh_fpga_spi_soft_reset(struct nh_fpga_spi *spi)
{
	iowrite32(NH_FPGA_SPI_SRR_RESET, spi->base + NH_FPGA_SPI_SRR);
	/* Flush the posted reset write and let the core settle. */
	ioread32(spi->base + NH_FPGA_SPI_CR);
	usleep_range(1000, 2000);
}

static void nh_fpga_spi_reset_fifos(struct nh_fpga_spi *spi)
{
	iowrite32(spi->cr | NH_FPGA_SPI_CR_INHIBIT |
			  NH_FPGA_SPI_CR_TXFIFO_RESET |
			  NH_FPGA_SPI_CR_RXFIFO_RESET,
		  spi->base + NH_FPGA_SPI_CR);
	iowrite32(spi->cr | NH_FPGA_SPI_CR_INHIBIT, spi->base + NH_FPGA_SPI_CR);
}

static unsigned int nh_fpga_spi_fifo_depth(struct nh_fpga_spi *spi)
{
	unsigned int depth = 0;

	/* The transmitter is inhibited out of reset, so this does not clock the bus. */
	nh_fpga_spi_soft_reset(spi);
	do {
		iowrite32(0, spi->base + NH_FPGA_SPI_DTR);
		depth++;
	} while (depth < NH_FPGA_SPI_MAX_FIFO_DEPTH &&
		 !(ioread32(spi->base + NH_FPGA_SPI_SR) &
		   NH_FPGA_SPI_SR_TX_FULL));

	return depth;
}

static int nh_fpga_spi_init_hw(struct nh_fpga_spi *spi)
{
	u32 cr;

	nh_fpga_spi_soft_reset(spi);
	spi->cr = NH_FPGA_SPI_CR_BASE;
	nh_fpga_spi_reset_fifos(spi);
	iowrite32(NH_FPGA_SPI_SS_NONE, spi->base + NH_FPGA_SPI_SSR);

	cr = ioread32(spi->base + NH_FPGA_SPI_CR);
	if (cr != (spi->cr | NH_FPGA_SPI_CR_INHIBIT)) {
		dev_err(spi->dev, "SPI master not responding (CR=0x%08x)\n",
			cr);
		return -ENODEV;
	}

	return 0;
}

static unsigned int nh_fpga_spi_rx_count(struct nh_fpga_spi *spi)
{
	/* The occupancy register reads one less than the number of entries. */
	u32 ocy = ioread32(spi->base + NH_FPGA_SPI_RX_OCY);

	if (!ocy &&
	    (ioread32(spi->base + NH_FPGA_SPI_SR) & NH_FPGA_SPI_SR_RX_EMPTY))
		return 0;
	return ocy + 1;
}

/* Wait until all n bytes of the released chunk are in the RX FIFO. */
static int nh_fpga_spi_wait_rx(struct nh_fpga_spi *spi, unsigned int n)
{
	unsigned int cnt;
	u32 sr;
	int ret;

	ret = read_poll_timeout(
		ioread32, sr,
		sr & (NH_FPGA_SPI_SR_TX_EMPTY | NH_FPGA_SPI_SR_CMD_ERR), 0,
		NH_FPGA_SPI_TIMEOUT_US, false, spi->base + NH_FPGA_SPI_SR);
	if (ret) {
		dev_err(spi->dev, "SPI transfer timed out (SR=0x%08x)\n", sr);
		return ret;
	}

	/*
	 * The core rejects opcodes outside its dual/quad-mode command table
	 * with Command Error, and cuts some others short.
	 */
	if (!(sr & NH_FPGA_SPI_SR_CMD_ERR)) {
		/* Skip the occupancy read when the chunk filled the RX FIFO. */
		if ((sr & NH_FPGA_SPI_SR_RX_FULL) && n == spi->fifo_depth)
			return 0;
		if (!read_poll_timeout(nh_fpga_spi_rx_count, cnt, cnt == n, 0,
				       NH_FPGA_SPI_DRAIN_US, false, spi))
			return 0;
	}

	dev_dbg(spi->dev, "SPI command not supported (SR=0x%08x)\n",
		ioread32(spi->base + NH_FPGA_SPI_SR));
	return -EOPNOTSUPP;
}

/*
 * Each FIFO-sized chunk is loaded with the transmitter inhibited and then
 * released; AXI Quad SPI in dual/quad mode requires the whole chunk up front.
 */
static int nh_fpga_spi_txrx(struct nh_fpga_spi *spi, const u8 *tx, u8 *rx,
			    unsigned int len)
{
	unsigned int done, n, i;
	u32 val;
	int ret;

	for (done = 0; done < len; done += n) {
		n = min(len - done, spi->fifo_depth);

		for (i = 0; i < n; i++)
			iowrite32(tx ? tx[done + i] : 0,
				  spi->base + NH_FPGA_SPI_DTR);

		iowrite32(spi->cr, spi->base + NH_FPGA_SPI_CR);

		ret = nh_fpga_spi_wait_rx(spi, n);
		if (ret) {
			nh_fpga_spi_soft_reset(spi);
			nh_fpga_spi_reset_fifos(spi);
			return ret;
		}

		for (i = 0; i < n; i++) {
			val = ioread32(spi->base + NH_FPGA_SPI_DRR);
			if (rx)
				rx[done + i] = val;
		}

		iowrite32(spi->cr | NH_FPGA_SPI_CR_INHIBIT,
			  spi->base + NH_FPGA_SPI_CR);
		/* The caller sets the transfer length, so yield between chunks. */
		cond_resched();
	}

	return 0;
}

/*
 * The first SCK edges after FPGA configuration are lost in STARTUPE2, so clock
 * one byte with no slave selected. RDID is used because dual/quad-mode cores
 * reject unsupported opcodes.
 */
static void nh_fpga_spi_prime(struct nh_fpga_spi *spi)
{
	u8 tx = SPINOR_OP_RDID, rx;

	if (nh_fpga_spi_txrx(spi, &tx, &rx, 1))
		dev_warn(spi->dev, "SPI prime transfer failed\n");
}

static void nh_fpga_spi_set_cs(struct spi_device *sdev, bool level)
{
	struct nh_fpga_spi *spi = spi_controller_get_devdata(sdev->controller);

	if (level) {
		iowrite32(NH_FPGA_SPI_SS_NONE, spi->base + NH_FPGA_SPI_SSR);
		return;
	}

	spi->cr = NH_FPGA_SPI_CR_BASE;
	if (sdev->mode & SPI_CPOL)
		spi->cr |= NH_FPGA_SPI_CR_CPOL;
	if (sdev->mode & SPI_CPHA)
		spi->cr |= NH_FPGA_SPI_CR_CPHA;
	iowrite32(spi->cr | NH_FPGA_SPI_CR_INHIBIT, spi->base + NH_FPGA_SPI_CR);
	iowrite32(NH_FPGA_SPI_SS_NONE & ~BIT(spi_get_chipselect(sdev, 0)),
		  spi->base + NH_FPGA_SPI_SSR);
}

/*
 * Return errors in msg->status so the SPI core does not log the expected
 * failures of flashrom's probes for other flash chips.
 */
static int nh_fpga_spi_transfer_one_message(struct spi_controller *ctlr,
					    struct spi_message *msg)
{
	struct nh_fpga_spi *spi = spi_controller_get_devdata(ctlr);
	struct spi_transfer *xfer;
	int ret = 0;

	nh_fpga_spi_set_cs(msg->spi, false);
	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		ret = nh_fpga_spi_txrx(spi, xfer->tx_buf, xfer->rx_buf,
				       xfer->len);
		if (ret)
			break;
		msg->actual_length += xfer->len;
		spi_transfer_delay_exec(xfer);
		if (xfer->cs_change &&
		    !list_is_last(&xfer->transfer_list, &msg->transfers)) {
			nh_fpga_spi_set_cs(msg->spi, true);
			spi_delay_exec(&xfer->cs_change_delay, xfer);
			nh_fpga_spi_set_cs(msg->spi, false);
		}
	}
	nh_fpga_spi_set_cs(msg->spi, true);

	msg->status = ret;
	spi_finalize_current_message(ctlr);
	return 0;
}

static int nh_fpga_spi_add_device(struct spi_controller *ctlr,
				  const struct spi_dev_info *info)
{
	struct spi_board_info board = {
		.max_speed_hz = info->max_speed_hz,
		.bus_num = ctlr->bus_num,
		.chip_select = info->chip_select,
	};

	if (strscpy(board.modalias, info->modalias, sizeof(board.modalias)) < 0)
		return -EINVAL;

	if (!spi_new_device(ctlr, &board))
		return -ENXIO;

	return 0;
}

static int nh_fpga_spi_probe(struct auxiliary_device *aux_dev,
			     const struct auxiliary_device_id *id)
{
	struct nh_fpga_aux_dev *fpga_aux_dev =
		container_of(aux_dev, struct nh_fpga_aux_dev, aux_dev);
	struct fbiob_aux_data *aux_data = &fpga_aux_dev->dev_info;
	struct spi_controller *ctlr;
	struct nh_fpga_spi *spi;
	unsigned int i;
	int ret;

	if ((resource_size_t)aux_data->csr_offset + NH_FPGA_SPI_REG_SIZE >
	    fpga_aux_dev->parent->mmio_len) {
		dev_err(&aux_dev->dev, "CSR offset 0x%x exceeds the BAR\n",
			aux_data->csr_offset);
		return -EINVAL;
	}

	if (!aux_data->spi_data.num_spidevs ||
	    aux_data->spi_data.num_spidevs > FBIOB_SPIDEV_MAX) {
		dev_err(&aux_dev->dev, "Invalid number of SPI devices: %u\n",
			aux_data->spi_data.num_spidevs);
		return -EINVAL;
	}

	ctlr = devm_spi_alloc_host(&aux_dev->dev, sizeof(*spi));
	if (!ctlr)
		return -ENOMEM;

	spi = spi_controller_get_devdata(ctlr);
	spi->dev = &aux_dev->dev;
	spi->base = fpga_aux_dev->csr_base;

	spi->fifo_depth = nh_fpga_spi_fifo_depth(spi);
	ret = nh_fpga_spi_init_hw(spi);
	if (ret)
		return ret;
	nh_fpga_spi_prime(spi);

	ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->num_chipselect = NH_FPGA_SPI_MAX_CS;
	ctlr->transfer_one_message = nh_fpga_spi_transfer_one_message;

	ret = devm_spi_register_controller(&aux_dev->dev, ctlr);
	if (ret) {
		dev_err(&aux_dev->dev,
			"Failed to register SPI controller: %d\n", ret);
		return ret;
	}

	for (i = 0; i < aux_data->spi_data.num_spidevs; i++) {
		const struct spi_dev_info *info =
			&aux_data->spi_data.spidevs[i];

		ret = nh_fpga_spi_add_device(ctlr, info);
		if (ret) {
			dev_err(&aux_dev->dev,
				"Failed to add SPI device on CS %u: %d\n",
				info->chip_select, ret);
			return ret;
		}
	}

	dev_info(&aux_dev->dev,
		 "Registered SPI bus %d at offset 0x%x (FIFO depth %u)\n",
		 ctlr->bus_num, aux_data->csr_offset, spi->fifo_depth);

	return 0;
}

static const struct auxiliary_device_id nh_fpga_spi_aux_id_table[] = {
	{
		.name = "fbiob_pci.spi_master",
	},
	{},
};
MODULE_DEVICE_TABLE(auxiliary, nh_fpga_spi_aux_id_table);

static struct auxiliary_driver nh_fpga_spi_aux_driver = {
	.name = DRIVER_NAME,
	.probe = nh_fpga_spi_probe,
	.id_table = nh_fpga_spi_aux_id_table,
};

static int __init nh_fpga_spi_init_module(void)
{
	int ret;

	pr_info("NH FPGA SPI driver v%s loading\n", DRIVER_VERSION);

	ret = auxiliary_driver_register(&nh_fpga_spi_aux_driver);
	if (ret) {
		pr_err("Failed to register auxiliary driver: %d\n", ret);
		return ret;
	}

	pr_info("NH FPGA SPI driver loaded successfully\n");
	return 0;
}

static void __exit nh_fpga_spi_exit_module(void)
{
	pr_info("NH FPGA SPI driver unloading\n");
	auxiliary_driver_unregister(&nh_fpga_spi_aux_driver);
	pr_info("NH FPGA SPI driver unloaded\n");
}

module_init(nh_fpga_spi_init_module);
module_exit(nh_fpga_spi_exit_module);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Arif Mohammad <marif@nexthop.ai>");
MODULE_DESCRIPTION("Nexthop FPGA SPI Controller Driver");
MODULE_VERSION(DRIVER_VERSION);
