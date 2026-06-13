/*
 * fpga_axi_iic_common.h - Common FPGA AXI IIC Controller Definitions
 *
 * Shared definitions and functions for FPGA AXI IIC controllers
 * that can be used by both FBOSS and SONIC drivers.
 *
 * Based on Xilinx AXI IIC controller implementation.
 *
 * Copyright (C) 2025 Nexthop Systems Inc.
 * Copyright (C) 2023 Celestica Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _FPGA_AXI_IIC_COMMON_H_
#define _FPGA_AXI_IIC_COMMON_H_

#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/wait.h>
#include <linux/io.h>

#define XIIC_MSB_OFFSET 0
#define XIIC_REG_OFFSET (0x100 + XIIC_MSB_OFFSET)

/*
 * Register offsets in bytes from RegisterBase. Three is added to the
 * base offset to access LSB (IBM style) of the word
 */
#define XIIC_CR_REG_OFFSET (0x00 + XIIC_REG_OFFSET) /* Control Register   */
#define XIIC_SR_REG_OFFSET (0x04 + XIIC_REG_OFFSET) /* Status Register    */
#define XIIC_DTR_REG_OFFSET (0x08 + XIIC_REG_OFFSET) /* Data Tx Register   */
#define XIIC_DRR_REG_OFFSET (0x0C + XIIC_REG_OFFSET) /* Data Rx Register   */
#define XIIC_ADR_REG_OFFSET (0x10 + XIIC_REG_OFFSET) /* Address Register   */
#define XIIC_TFO_REG_OFFSET (0x14 + XIIC_REG_OFFSET) /* Tx FIFO Occupancy  */
#define XIIC_RFO_REG_OFFSET (0x18 + XIIC_REG_OFFSET) /* Rx FIFO Occupancy  */
#define XIIC_TBA_REG_OFFSET (0x1C + XIIC_REG_OFFSET) /* 10 Bit Address reg */
#define XIIC_RFD_REG_OFFSET (0x20 + XIIC_REG_OFFSET) /* Rx FIFO Depth reg  */
#define XIIC_GPO_REG_OFFSET (0x24 + XIIC_REG_OFFSET) /* Output Register    */

/* Control Register masks */
#define XIIC_CR_ENABLE_DEVICE_MASK 0x01 /* Device enable = 1      */
#define XIIC_CR_TX_FIFO_RESET_MASK 0x02 /* Transmit FIFO reset=1  */
#define XIIC_CR_MSMS_MASK 0x04 /* Master starts Txing=1  */
#define XIIC_CR_DIR_IS_TX_MASK 0x08 /* Dir of tx. Txing=1     */
#define XIIC_CR_NO_ACK_MASK 0x10 /* Tx Ack. NO ack = 1     */
#define XIIC_CR_REPEATED_START_MASK 0x20 /* Repeated start = 1     */
#define XIIC_CR_GENERAL_CALL_MASK 0x40 /* Gen Call enabled = 1   */

/* Status Register masks */
#define XIIC_SR_GEN_CALL_MASK 0x01 /* 1=a mstr issued a GC   */
#define XIIC_SR_ADDR_AS_SLAVE_MASK 0x02 /* 1=when addr as slave   */
#define XIIC_SR_BUS_BUSY_MASK 0x04 /* 1 = bus is busy        */
#define XIIC_SR_MSTR_RDING_SLAVE_MASK 0x08 /* 1=Dir: mstr <-- slave  */
#define XIIC_SR_TX_FIFO_FULL_MASK 0x10 /* 1 = Tx FIFO full       */
#define XIIC_SR_RX_FIFO_FULL_MASK 0x20 /* 1 = Rx FIFO full       */
#define XIIC_SR_RX_FIFO_EMPTY_MASK 0x40 /* 1 = Rx FIFO empty      */
#define XIIC_SR_TX_FIFO_EMPTY_MASK 0x80 /* 1 = Tx FIFO empty      */

/* Interrupt Status Register masks    Interrupt occurs when...       */
#define XIIC_INTR_ARB_LOST_MASK 0x01 /* 1 = arbitration lost   */
#define XIIC_INTR_TX_ERROR_MASK 0x02 /* 1=Tx error/msg complete */
#define XIIC_INTR_TX_EMPTY_MASK 0x04 /* 1 = Tx FIFO/reg empty  */
#define XIIC_INTR_RX_FULL_MASK 0x08 /* 1=Rx FIFO/reg=OCY level */
#define XIIC_INTR_BNB_MASK 0x10 /* 1 = Bus not busy       */
#define XIIC_INTR_AAS_MASK 0x20 /* 1 = when addr as slave */
#define XIIC_INTR_NAAS_MASK 0x40 /* 1 = not addr as slave  */
#define XIIC_INTR_TX_HALF_MASK 0x80 /* 1 = TX FIFO half empty */

/* The following constants specify the depth of the FIFOs */
#define IIC_RX_FIFO_DEPTH 16 /* Rx fifo capacity       */
#define IIC_TX_FIFO_DEPTH 16 /* Tx fifo capacity       */

/*
 * Tx Fifo upper bit masks.
 */
#define XIIC_TX_DYN_START_MASK 0x0100 /* 1 = Set dynamic start */
#define XIIC_TX_DYN_STOP_MASK 0x0200 /* 1 = Set dynamic stop */

/*
 * The following constants define the register offsets for the Interrupt
 * registers. There are some holes in the memory map for reserved addresses
 * to allow other registers to be added and still match the memory map of the
 * interrupt controller registers
 */
#define XIIC_IISR_OFFSET 0x20 /* Interrupt Status Register */
#define XIIC_RESETR_OFFSET 0x40 /* Reset Register */

#define XIIC_RESET_MASK 0xAUL

#define XIIC_PM_TIMEOUT 1000 /* ms */
/* timeout waiting for the controller to respond */
#define XIIC_I2C_TIMEOUT (msecs_to_jiffies(1000))

/* I2C state machine states */
enum axi_iic_state {
	STATE_DONE = 0,
	STATE_INIT,
	STATE_ADDR,
	STATE_ADDR10,
	STATE_START,
	STATE_WRITE,
	STATE_READ,
	STATE_STOP,
	STATE_ERROR,
};

/* Forward declaration */
struct fpga_axi_iic;

/* Logging function pointer type */
typedef void (*axi_iic_log_func_t)(struct fpga_axi_iic *i2c, int level,
				   const char *fmt, ...);

/* Common AXI IIC controller structure */
struct fpga_axi_iic {
	void __iomem *base;
	struct i2c_msg *msg;
	int pos;
	int nmsgs;
	int state;
	struct mutex lock;
	wait_queue_head_t wait;
	u32 timeout;
	int ip_clock_khz;
	int bus_clock_khz;

	/* Logging function - set by driver */
	axi_iic_log_func_t log_func;
	void *log_data; /* Driver-specific data for logging */
};

/* Function prototypes */
int fpga_axi_iic_init(struct fpga_axi_iic *i2c);
int fpga_axi_iic_xfer(struct fpga_axi_iic *i2c, struct i2c_msg *msgs, int num);
u32 fpga_axi_iic_func(struct i2c_adapter *adap);

/* Register access functions */
static inline void xiic_setreg32(struct fpga_axi_iic *i2c, int reg, u32 value)
{
	iowrite32(value, i2c->base + reg);
}

static inline u32 xiic_getreg32(struct fpga_axi_iic *i2c, int reg)
{
	return ioread32(i2c->base + reg);
}

/* Logging helper macros */
#define AXI_IIC_LOG_ERR 0
#define AXI_IIC_LOG_WARN 1
#define AXI_IIC_LOG_INFO 2
#define AXI_IIC_LOG_DBG 3

#define axi_iic_log(i2c, level, fmt, ...)                                    \
	do {                                                                 \
		if ((i2c)->log_func)                                         \
			(i2c)->log_func((i2c), (level), fmt, ##__VA_ARGS__); \
	} while (0)

#endif /* _FPGA_AXI_IIC_COMMON_H_ */
