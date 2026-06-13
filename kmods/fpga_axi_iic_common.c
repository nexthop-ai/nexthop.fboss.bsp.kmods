/*
 * fpga_axi_iic_common.c - Common FPGA AXI IIC Controller Implementation
 *
 * Shared implementation of FPGA AXI IIC controller functions
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

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include "fpga_axi_iic_common.h"

/* Forward declarations */
static int xiic_clear_rx_fifo(struct fpga_axi_iic *i2c);
static void xiic_irq_clr(struct fpga_axi_iic *i2c, u32 mask);
static int xiic_poll_wait(struct fpga_axi_iic *i2c);
static void xiic_process(struct fpga_axi_iic *i2c);
static int fpga_axi_iic_poll(struct fpga_axi_iic *i2c, struct i2c_msg *msgs,
			     int num);
static int xiic_reinit(struct fpga_axi_iic *i2c);

/**
 * xiic_irq_clr - Clear interrupt flags
 * @i2c: AXI IIC device instance
 * @mask: interrupt mask to clear
 */
static void xiic_irq_clr(struct fpga_axi_iic *i2c, u32 mask)
{
	u32 isr = xiic_getreg32(i2c, XIIC_IISR_OFFSET);
	xiic_setreg32(i2c, XIIC_IISR_OFFSET, isr & mask);
}

/**
 * xiic_clear_rx_fifo - Clear RX FIFO
 * @i2c: AXI IIC device instance
 *
 * Return: 0 on success, -ETIMEDOUT on timeout
 */
static int xiic_clear_rx_fifo(struct fpga_axi_iic *i2c)
{
	u8 sr;
	unsigned long timeout;

	timeout = jiffies + XIIC_I2C_TIMEOUT;
	for (sr = xiic_getreg32(i2c, XIIC_SR_REG_OFFSET);
	     !(sr & XIIC_SR_RX_FIFO_EMPTY_MASK);
	     sr = xiic_getreg32(i2c, XIIC_SR_REG_OFFSET)) {
		xiic_getreg32(i2c, XIIC_DRR_REG_OFFSET);
		if (time_after(jiffies, timeout)) {
			axi_iic_log(i2c, AXI_IIC_LOG_ERR,
				    "Failed to clear rx fifo\n");
			return -ETIMEDOUT;
		}
	}

	return 0;
}

/**
 * xiic_poll_wait_reg - Wait until something changes in a register
 * @i2c: AXI IIC device instance
 * @reg: register to query
 * @mask: bitmask to apply on register value
 * @val: expected result
 * @timeout: timeout in jiffies
 *
 * Return: 0 on success, -ETIMEDOUT on timeout
 */
static int xiic_poll_wait_reg(struct fpga_axi_iic *i2c, int reg, u8 mask,
			      u8 val, const unsigned long timeout)
{
	unsigned long j;
	u8 status = 0;

	j = jiffies + timeout;
	while (1) {
		mutex_lock(&i2c->lock);
		status = xiic_getreg32(i2c, reg);
		mutex_unlock(&i2c->lock);
		if ((status & mask) == val)
			break;
		if (time_after(jiffies, j))
			return -ETIMEDOUT;
		cpu_relax();
		cond_resched();
	}
	return 0;
}

/**
 * xiic_poll_wait - Wait until it's possible to process some data
 * @i2c: AXI IIC device instance
 *
 * Used when the device is in polling mode (interrupts disabled).
 *
 * Return: 0 on success, -ETIMEDOUT on timeout
 */
static int xiic_poll_wait(struct fpga_axi_iic *i2c)
{
	u8 mask = 0, status = 0;
	int err = 0;
	int val = 0;
	int tmp = 0;

	mutex_lock(&i2c->lock);
	if (i2c->state == STATE_DONE) {
		/* transfer is over */
		mask = XIIC_SR_BUS_BUSY_MASK;
	} else if (i2c->state == STATE_WRITE || i2c->state == STATE_START) {
		/* ongoing transfer */
		if (0 == i2c->msg->len) {
			mask = XIIC_INTR_TX_ERROR_MASK;
		} else {
			mask = XIIC_SR_TX_FIFO_FULL_MASK;
		}
	} else if (i2c->state == STATE_READ) {
		/* ongoing receive */
		mask = XIIC_SR_TX_FIFO_EMPTY_MASK | XIIC_SR_RX_FIFO_EMPTY_MASK;
	}
	mutex_unlock(&i2c->lock);

	/*
	 * Once we are here we expect to get the expected result immediately
	 * so if after 50ms we timeout then something is broken.
	 */

	if (1 == i2c->nmsgs && 0 == i2c->msg->len &&
	    i2c->state == STATE_START && !(i2c->msg->flags & I2C_M_RD)) {
		/* for i2cdetect I2C_SMBUS_QUICK mode */
		err = xiic_poll_wait_reg(i2c, XIIC_IISR_OFFSET, mask, mask,
					 msecs_to_jiffies(50));
		mutex_lock(&i2c->lock);
		status = xiic_getreg32(i2c, XIIC_SR_REG_OFFSET);
		mutex_unlock(&i2c->lock);
		if (0 != err) {
			/* AXI IIC as a transceiver, if ever an XIIC_INTR_TX_ERROR_MASK
			 * interrupt happens, means no such i2c device */
			err = 0;
		} else {
			err = -ETIMEDOUT;
		}
	} else {
		if (mask & XIIC_SR_TX_FIFO_EMPTY_MASK) {
			err = xiic_poll_wait_reg(i2c, XIIC_SR_REG_OFFSET, mask,
						 XIIC_SR_TX_FIFO_EMPTY_MASK,
						 msecs_to_jiffies(50));
			mask &= ~XIIC_SR_TX_FIFO_EMPTY_MASK;
		}
		if (0 == err) {
			err = xiic_poll_wait_reg(i2c, XIIC_SR_REG_OFFSET, mask,
						 0, msecs_to_jiffies(50));
		}
		mutex_lock(&i2c->lock);
		status = xiic_getreg32(i2c, XIIC_IISR_OFFSET);

		if ((status & XIIC_INTR_ARB_LOST_MASK) ||
		    ((status & XIIC_INTR_TX_ERROR_MASK) &&
		     !(status & XIIC_INTR_RX_FULL_MASK) &&
		     !(i2c->msg->flags & I2C_M_RD))) {
			/* AXI IIC as a transceiver, if ever an XIIC_INTR_TX_ERROR_MASK
			 * interrupt happens, return */
			err = -ETIMEDOUT;

			if (status & XIIC_INTR_ARB_LOST_MASK) {
				val = xiic_getreg32(i2c, XIIC_CR_REG_OFFSET);
				tmp = XIIC_CR_MSMS_MASK;
				val &= (~tmp);
				xiic_setreg32(i2c, XIIC_CR_REG_OFFSET, val);
				xiic_setreg32(i2c, XIIC_IISR_OFFSET,
					      XIIC_INTR_ARB_LOST_MASK);
				axi_iic_log(
					i2c, AXI_IIC_LOG_ERR,
					"TRANSFER STATUS ERROR, ISR: bit 0x%x happens\n",
					XIIC_INTR_ARB_LOST_MASK);
			}
			if (status & XIIC_INTR_TX_ERROR_MASK) {
				int sta = 0;
				int cr = 0;
				sta = xiic_getreg32(i2c, XIIC_SR_REG_OFFSET);
				cr = xiic_getreg32(i2c, XIIC_CR_REG_OFFSET);
				xiic_setreg32(i2c, XIIC_IISR_OFFSET,
					      XIIC_INTR_TX_ERROR_MASK);
				axi_iic_log(
					i2c, AXI_IIC_LOG_DBG,
					"TRANSFER STATUS ERROR, ISR: bit 0x%x happens; SR: bit 0x%x; CR: bit 0x%x\n",
					status, sta, cr);
			}
			/* Soft reset IIC controller. */
			xiic_setreg32(i2c, XIIC_RESETR_OFFSET, XIIC_RESET_MASK);
			(void)xiic_reinit(i2c);
			mutex_unlock(&i2c->lock);
			return err;
		}
		mutex_unlock(&i2c->lock);
	}

	if (err)
		axi_iic_log(i2c, AXI_IIC_LOG_DBG,
			    "STATUS timeout, bit 0x%x did not clear in 50ms\n",
			    status);

	return err;
}

/**
 * xiic_process - Process I2C state machine
 * @i2c: AXI IIC device instance
 */
static void xiic_process(struct fpga_axi_iic *i2c)
{
	struct i2c_msg *msg = i2c->msg;
	u16 val;

	/*
	 * If we spin here because we are in timeout, so we are going
	 * to be in STATE_ERROR.
	 */
	mutex_lock(&i2c->lock);

	if (i2c->state == STATE_START) {
		i2c->state = (msg->flags & I2C_M_RD) ? STATE_READ : STATE_WRITE;
		/* if it's the time sequence is 'start bit + address + read bit + stop bit' */
		if (i2c->state == STATE_READ) {
			/* it's the last message so we include dynamic stop bit with length */
			val = msg->len | XIIC_TX_DYN_STOP_MASK;
			xiic_setreg32(i2c, XIIC_DTR_REG_OFFSET, val);
			goto out;
		}
	}
	if (i2c->state == STATE_READ) {
		/* suit for I2C_FUNC_SMBUS_BLOCK_DATA */
		if (msg->flags & I2C_M_RECV_LEN) {
			msg->len = xiic_getreg32(i2c, XIIC_DRR_REG_OFFSET);
			msg->flags &= ~I2C_M_RECV_LEN;
			msg->buf[i2c->pos++] = msg->len;
		} else {
			msg->buf[i2c->pos++] =
				xiic_getreg32(i2c, XIIC_DRR_REG_OFFSET);
		}
	} else if (i2c->state == STATE_WRITE) {
		/* if it reaches the last byte data to be sent */
		if ((i2c->pos == msg->len - 1) && (i2c->nmsgs == 1)) {
			val = msg->buf[i2c->pos++] | XIIC_TX_DYN_STOP_MASK;
			xiic_setreg32(i2c, XIIC_DTR_REG_OFFSET, val);
			i2c->state = STATE_DONE;
			goto out;
			/* if it is not the last byte data to be sent */
		} else if (i2c->pos < msg->len) {
			xiic_setreg32(i2c, XIIC_DTR_REG_OFFSET,
				      msg->buf[i2c->pos++]);
			goto out;
		}
	}

	/* end of msg? */
	if (i2c->pos == msg->len) {
		i2c->nmsgs--;
		i2c->pos = 0;
		if (i2c->nmsgs) {
			i2c->msg++;
			msg = i2c->msg;
			if (!(msg->flags & I2C_M_NOSTART)) { /* send start? */
				i2c->state = STATE_START;
				xiic_setreg32(i2c, XIIC_DTR_REG_OFFSET,
					      i2c_8bit_addr_from_msg(msg) |
						      XIIC_TX_DYN_START_MASK);
				goto out;
			}
		} else { /* end? */
			i2c->state = STATE_DONE;
			goto out;
		}
	}

out:
	mutex_unlock(&i2c->lock);
	return;
}

/**
 * fpga_axi_iic_poll - Perform I2C transfer in polling mode
 * @i2c: AXI IIC device instance
 * @msgs: I2C messages to transfer
 * @num: number of messages
 *
 * Return: number of messages transferred on success, negative error code on failure
 */
static int fpga_axi_iic_poll(struct fpga_axi_iic *i2c, struct i2c_msg *msgs,
			     int num)
{
	int ret = 0;

	mutex_lock(&i2c->lock);
	/* Soft reset IIC controller. */
	xiic_setreg32(i2c, XIIC_RESETR_OFFSET, XIIC_RESET_MASK);
	/* Set receive Fifo depth to maximum (zero based). */
	xiic_setreg32(i2c, XIIC_RFD_REG_OFFSET, IIC_RX_FIFO_DEPTH - 1);

	/* Reset Tx Fifo. */
	xiic_setreg32(i2c, XIIC_CR_REG_OFFSET, XIIC_CR_TX_FIFO_RESET_MASK);

	/* Enable IIC Device, remove Tx Fifo reset & disable general call. */
	xiic_setreg32(i2c, XIIC_CR_REG_OFFSET, XIIC_CR_ENABLE_DEVICE_MASK);

	/* make sure RX fifo is empty */
	ret = xiic_clear_rx_fifo(i2c);
	if (ret) {
		mutex_unlock(&i2c->lock);
		return ret;
	}

	i2c->msg = msgs;
	i2c->pos = 0;
	i2c->nmsgs = num;
	i2c->state = STATE_START;

	if (msgs->len == 0 && num == 1) { /* suit for i2cdetect time sequence */
		u8 status = xiic_getreg32(i2c, XIIC_IISR_OFFSET);
		xiic_irq_clr(i2c, status);
		/* send out the 1st byte data and stop bit */
		xiic_setreg32(i2c, XIIC_DTR_REG_OFFSET,
			      i2c_8bit_addr_from_msg(msgs) |
				      XIIC_TX_DYN_START_MASK |
				      XIIC_TX_DYN_STOP_MASK);
	} else {
		/* send out the 1st byte data */
		xiic_setreg32(i2c, XIIC_DTR_REG_OFFSET,
			      i2c_8bit_addr_from_msg(msgs) |
				      XIIC_TX_DYN_START_MASK);
	}
	mutex_unlock(&i2c->lock);

	while (1) {
		int err;

		err = xiic_poll_wait(i2c);
		if (err) {
			i2c->state = STATE_ERROR;
			break;
		} else if (i2c->state == STATE_DONE) {
			break;
		}
		xiic_process(i2c);
	}

	return (i2c->state == STATE_DONE) ? num : -EIO;
}

/**
 * fpga_axi_iic_xfer - I2C master transfer function with retry logic
 * @i2c: AXI IIC device instance
 * @msgs: I2C messages to transfer
 * @num: number of messages
 *
 * Return: number of messages transferred on success, negative error code on failure
 */
int fpga_axi_iic_xfer(struct fpga_axi_iic *i2c, struct i2c_msg *msgs, int num)
{
	int err = -EIO;
	u8 retry = 0, max_retry = 0;

	if (((1 == msgs->len && (msgs->flags & I2C_M_RD)) ||
	     (0 == msgs->len && !(msgs->flags & I2C_M_RD))) &&
	    num == 1) {
		/* I2C_SMBUS_QUICK or I2C_SMBUS_BYTE */
		max_retry = 1;
	} else {
		max_retry =
			5; /* retry 5 times if receive a NACK or other errors */
	}

	while ((-EIO == err) && (retry < max_retry)) {
		err = fpga_axi_iic_poll(i2c, msgs, num);
		retry++;
	}

	return err;
}
EXPORT_SYMBOL_GPL(fpga_axi_iic_xfer);

/**
 * fpga_axi_iic_func - Report I2C functionality
 * @adap: I2C adapter (unused)
 *
 * Return: supported I2C functionality flags
 */
u32 fpga_axi_iic_func(struct i2c_adapter *adap)
{
	/* a typical full-I2C adapter would use the following */
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}
EXPORT_SYMBOL_GPL(fpga_axi_iic_func);

/**
 * xiic_reinit - Reinitialize the I2C controller
 * @i2c: AXI IIC device instance
 *
 * Return: 0 on success, negative error code on failure
 */
static int xiic_reinit(struct fpga_axi_iic *i2c)
{
	int ret;
	int val = 0;

	/* Soft reset IIC controller. */
	xiic_setreg32(i2c, XIIC_RESETR_OFFSET, XIIC_RESET_MASK);

	/* Set receive Fifo depth to maximum (zero based). */
	xiic_setreg32(i2c, XIIC_RFD_REG_OFFSET, IIC_RX_FIFO_DEPTH - 1);

	/* Reset Tx Fifo. */
	xiic_setreg32(i2c, XIIC_CR_REG_OFFSET, XIIC_CR_TX_FIFO_RESET_MASK);

	/* Enable IIC Device, remove Tx Fifo reset & disable general call. */
	val |= XIIC_CR_ENABLE_DEVICE_MASK;
	val |= XIIC_CR_DIR_IS_TX_MASK;
	xiic_setreg32(i2c, XIIC_CR_REG_OFFSET, val);

	/* make sure RX fifo is empty */
	ret = xiic_clear_rx_fifo(i2c);
	if (ret)
		return ret;

	return 0;
}

/**
 * fpga_axi_iic_init - Initialize I2C controller
 * @i2c: AXI IIC device instance
 *
 * Return: 0 on success, negative error code on failure
 */
int fpga_axi_iic_init(struct fpga_axi_iic *i2c)
{
	int ret;

	if (!i2c || !i2c->base) {
		return -EINVAL;
	}

	/* Initialize mutex and wait queue */
	mutex_init(&i2c->lock);
	init_waitqueue_head(&i2c->wait);

	/* Set default timeout */
	i2c->timeout = msecs_to_jiffies(50);

	ret = xiic_reinit(i2c);
	if (ret < 0) {
		axi_iic_log(i2c, AXI_IIC_LOG_ERR,
			    "Cannot reinitialize controller\n");
		mutex_destroy(&i2c->lock);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(fpga_axi_iic_init);

/* This file is part of the nh_fpga_i2c module */
