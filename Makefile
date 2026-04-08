# SPDX-License-Identifier: GPL-2.0
#
# Makefile for xv6fs kernel module
#
# Usage:
#   make              - build against the running kernel
#   make KDIR=<path>  - build against a specific kernel source tree
#   make clean        - remove build artifacts

obj-m += xv6fs.o

xv6fs-objs := xv6fs_main.o super.o inode.o dir.o file.o

# Default: build against the currently running kernel
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Convenience targets for loading/unloading the module
load:
	sudo insmod xv6fs.ko

unload:
	sudo rmmod xv6fs

.PHONY: all clean load unload
