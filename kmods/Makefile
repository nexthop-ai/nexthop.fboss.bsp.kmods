# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2025 Nexthop Systems Inc.
#
# NH FPGA FBIOB PCIe Driver Makefile

ifneq ($(KERNELRELEASE),)
    # This executes when kernel's build system calls this Makefile
    obj-m := fbiob_pci.o nh_fpga_i2c.o nh_fpga_mux.o nh_fpga_info.o nh_fpga_asic_temp.o nh_fpga_fan.o nh_xcvr_ctrl.o nh_fpga_led.o nh_fpga_port_led.o nh_fpga_psu.o

    # Shared platform objects linked into each module that uses per-platform data.
    # Per-SKU objects (platform/<sku>.o) are appended by each platform integration.
    nh_platform_objs := platform/nh_platform.o \
                        platform/cf2.o \
                        platform/m4062nhp_fpga0.o \
                        platform/m4062nhp_fpga1.o

    # Object files for fbiob_pci.ko
    fbiob_pci-objs := nh_fpga_fbiob_main.o nh_fpga_aux.o

    # Object files for nh_fpga_i2c.ko
    nh_fpga_i2c-objs := nh_fpga_i2c_main.o fpga_axi_iic_common.o \
                        $(nh_platform_objs)

    # nh_fpga_mux.ko is a single-file module, no -objs needed

    # Object files for nh_fpga_asic_temp.ko
    nh_fpga_asic_temp-objs := nh_fpga_asic_temp_main.o $(nh_platform_objs)

    # Object files for nh_fpga_psu.ko (PSU presence)
    nh_fpga_psu-objs := nh_fpga_psu_main.o $(nh_platform_objs)

    # Object files for nh_fpga_fan.ko
    nh_fpga_fan-objs := nh_fpga_fan_main.o $(nh_platform_objs)

    # Object files for nh_fpga_led.ko (core LED driver for all LED types)
    nh_fpga_led-objs := nh_fpga_led_core_main.o $(nh_platform_objs)

    # Object files for nh_xcvr_ctrl.ko
    nh_xcvr_ctrl-objs := nh_xcvr_ctrl_main.o $(nh_platform_objs)

    # Object files for nh_fpga_port_led.ko (port LED control + trigger support)
    nh_fpga_port_led-objs := nh_fpga_port_led_main.o nh_fpga_led_trigger.o $(nh_platform_objs)
else
    # This executes when we run 'make' from the command line
    KERNELDIR ?= /lib/modules/$(shell ls -1 --ignore '[^1-9]*' /lib/modules | sort -r | head -n 1)/build
    PWD := $(shell pwd)

    # Default to the FBOSS uapi headers bundled in this repository.
    # Override FBOSS_HEADERS to build against headers from a different location.
    FBOSS_HEADERS ?= $(PWD)/../fboss_headers
    KERNEL_CFLAGS = ccflags-y="-I$(FBOSS_HEADERS)"

default:
	$(MAKE) -j $(shell nproc) -C $(KERNELDIR) M=$(PWD) $(KERNEL_CFLAGS) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

.PHONY: default clean install

endif
