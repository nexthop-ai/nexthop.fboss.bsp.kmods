# Nexthop FBOSS BSP kernel modules

ifneq ($(KERNELRELEASE),)
    # This executes when the kernel's build system calls this Makefile.
    # Add new modules to obj-m (and their -objs lists) as drivers are migrated
    # into this directory.
    obj-m := nh_bsp_stub.o
else
    # This executes when we run 'make' from the command line.
    KERNELDIR ?= /lib/modules/$(shell ls -1 --ignore '[^1-9]*' /lib/modules | sort -rV | head -n 1)/build
    PWD := $(shell pwd)

default:
	$(MAKE) -j $(shell nproc) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

.PHONY: default clean install

endif
