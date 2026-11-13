// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 Nexthop Systems Inc.
#include <linux/module.h>
#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include "nh_fpga_fbiob.h"
#include "platform/nh_platform.h"

#define DRIVER_NAME "nh_fpga_flash_sel"
#define DRIVER_VERSION "1.0"

#define NH_SH0_CHAIN_TYPE GENMASK(3, 0)

enum {
	NH_FLASH_SEL_SPI_ENABLE,
	NH_FLASH_SEL_GRAB,
	NH_FLASH_SEL_SEL,
	NH_FLASH_SEL_NUM_LINES,
};

struct nh_flash_sel_line {
	u32 reg;
	u32 bit;
};

struct nh_flash_sel {
	struct gpio_chip gc;
	void __iomem *base;
	struct mutex lock; /* serializes read-modify-write of the registers */
	unsigned long driven;
	struct nh_flash_sel_line lines[NH_FLASH_SEL_NUM_LINES];
	const char *names[NH_FLASH_SEL_NUM_LINES];
	u32 sh0_reg;
	u32 reload_mask;
};

static int nh_flash_sel_get_direction(struct gpio_chip *gc, unsigned int off)
{
	struct nh_flash_sel *sel = gpiochip_get_data(gc);

	return test_bit(off, &sel->driven) ? GPIO_LINE_DIRECTION_OUT :
					     GPIO_LINE_DIRECTION_IN;
}

static int nh_flash_sel_get(struct gpio_chip *gc, unsigned int off)
{
	struct nh_flash_sel *sel = gpiochip_get_data(gc);
	const struct nh_flash_sel_line *l = &sel->lines[off];

	return !!(ioread32(sel->base + l->reg) & BIT(l->bit));
}

static void nh_flash_sel_write_locked(struct nh_flash_sel *sel,
				      unsigned int off, int val)
{
	const struct nh_flash_sel_line *l = &sel->lines[off];
	u32 v;

	v = ioread32(sel->base + l->reg);
	/* Writing back a live reload pulse would fire another reload. */
	if (l->reg == sel->sh0_reg)
		v &= ~sel->reload_mask;
	if (val)
		v |= BIT(l->bit);
	else
		v &= ~BIT(l->bit);
	iowrite32(v, sel->base + l->reg);
}

static void nh_flash_sel_set(struct gpio_chip *gc, unsigned int off, int val)
{
	struct nh_flash_sel *sel = gpiochip_get_data(gc);

	mutex_lock(&sel->lock);
	nh_flash_sel_write_locked(sel, off, val);
	mutex_unlock(&sel->lock);
}

static int nh_flash_sel_direction_output(struct gpio_chip *gc, unsigned int off,
					 int val)
{
	struct nh_flash_sel *sel = gpiochip_get_data(gc);

	mutex_lock(&sel->lock);
	nh_flash_sel_write_locked(sel, off, val);
	set_bit(off, &sel->driven);
	mutex_unlock(&sel->lock);
	return 0;
}

/* fw_util releases the flash with gpioget, which requests the line as input. */
static int nh_flash_sel_direction_input(struct gpio_chip *gc, unsigned int off)
{
	struct nh_flash_sel *sel = gpiochip_get_data(gc);

	mutex_lock(&sel->lock);
	nh_flash_sel_write_locked(sel, off, 0);
	clear_bit(off, &sel->driven);
	mutex_unlock(&sel->lock);
	return 0;
}

static const struct nh_flash_sel_layout *
nh_flash_sel_find_layout(const struct nh_flash_sel_cfg *cfg, u32 chain)
{
	u32 i;

	for (i = 0; i < cfg->num_layouts; i++)
		if (cfg->layouts[i].chain_type == chain)
			return &cfg->layouts[i];
	return NULL;
}

static int nh_flash_sel_probe(struct auxiliary_device *aux_dev,
			      const struct auxiliary_device_id *id)
{
	struct nh_fpga_aux_dev *fpga_aux_dev =
		container_of(aux_dev, struct nh_fpga_aux_dev, aux_dev);
	const struct nh_platform_cfg *platform_cfg;
	const struct nh_flash_sel_layout *layout;
	const struct nh_flash_sel_cfg *cfg;
	u16 device_id = fpga_aux_dev->parent->pdev->device;
	struct nh_flash_sel *sel;
	unsigned int i;
	u32 chain;

	platform_cfg = nh_get_platform(device_id);
	cfg = platform_cfg ? platform_cfg->flash_sel_cfg : NULL;
	if (!cfg) {
		dev_err(&aux_dev->dev,
			"No flash select config for device ID: 0x%04x\n",
			device_id);
		return -ENODEV;
	}

	chain = FIELD_GET(NH_SH0_CHAIN_TYPE,
			  ioread32(fpga_aux_dev->csr_base + cfg->sh0_reg));
	layout = nh_flash_sel_find_layout(cfg, chain);
	if (!layout) {
		dev_err(&aux_dev->dev,
			"unsupported switch card shift chain type %u\n", chain);
		return -ENODEV;
	}

	sel = devm_kzalloc(&aux_dev->dev, sizeof(*sel), GFP_KERNEL);
	if (!sel)
		return -ENOMEM;

	sel->base = fpga_aux_dev->csr_base;
	sel->sh0_reg = cfg->sh0_reg;
	sel->reload_mask = BIT(layout->reload_bit);
	sel->lines[NH_FLASH_SEL_SPI_ENABLE] =
		(struct nh_flash_sel_line){ cfg->spi_enable_reg,
					    cfg->spi_enable_bit };
	sel->lines[NH_FLASH_SEL_GRAB] =
		(struct nh_flash_sel_line){ cfg->sh0_reg, layout->grab_bit };
	sel->lines[NH_FLASH_SEL_SEL] =
		(struct nh_flash_sel_line){ cfg->sh0_reg, layout->sel_bit };
	sel->names[NH_FLASH_SEL_SPI_ENABLE] = "spi_enable";
	sel->names[NH_FLASH_SEL_GRAB] = "flash_grab";
	sel->names[NH_FLASH_SEL_SEL] = layout->sel_name;
	mutex_init(&sel->lock);

	/* Let gpioget release lines an interrupted upgrade left at 1. */
	for (i = 0; i < NH_FLASH_SEL_NUM_LINES; i++) {
		const struct nh_flash_sel_line *l = &sel->lines[i];

		if (ioread32(sel->base + l->reg) & BIT(l->bit))
			set_bit(i, &sel->driven);
	}

	sel->gc.label = dev_name(&aux_dev->dev);
	sel->gc.parent = &aux_dev->dev;
	sel->gc.owner = THIS_MODULE;
	sel->gc.base = -1;
	sel->gc.ngpio = NH_FLASH_SEL_NUM_LINES;
	sel->gc.names = sel->names;
	sel->gc.can_sleep = true;
	sel->gc.get_direction = nh_flash_sel_get_direction;
	sel->gc.get = nh_flash_sel_get;
	sel->gc.set = nh_flash_sel_set;
	sel->gc.direction_output = nh_flash_sel_direction_output;
	sel->gc.direction_input = nh_flash_sel_direction_input;

	return devm_gpiochip_add_data(&aux_dev->dev, &sel->gc, sel);
}

static const struct auxiliary_device_id nh_flash_sel_aux_id_table[] = {
	{ .name = "fbiob_pci.gpiochip_flash_sel" },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, nh_flash_sel_aux_id_table);

static struct auxiliary_driver nh_flash_sel_aux_driver = {
	.name = DRIVER_NAME,
	.probe = nh_flash_sel_probe,
	.id_table = nh_flash_sel_aux_id_table,
};

static int __init nh_flash_sel_init_module(void)
{
	return auxiliary_driver_register(&nh_flash_sel_aux_driver);
}

static void __exit nh_flash_sel_exit_module(void)
{
	auxiliary_driver_unregister(&nh_flash_sel_aux_driver);
}

module_init(nh_flash_sel_init_module);
module_exit(nh_flash_sel_exit_module);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Maisey Perelonia <maisey@nexthop.ai>");
MODULE_DESCRIPTION("Nexthop FPGA Switch Card Flash Select GPIO Driver");
MODULE_VERSION(DRIVER_VERSION);
