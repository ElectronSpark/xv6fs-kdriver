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

all: modules mkfs.xv6fs

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Userspace mkfs tool
mkfs.xv6fs: mkfs.xv6fs.c
	$(CC) -Wall -Wextra -O2 -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f mkfs.xv6fs

# Install mkfs.xv6fs so that `mkfs -t xv6fs` works
install-mkfs: mkfs.xv6fs
	sudo install -m 755 mkfs.xv6fs /sbin/mkfs.xv6fs

uninstall-mkfs:
	sudo rm -f /sbin/mkfs.xv6fs

# Convenience targets for loading/unloading the module
load:
	sudo insmod xv6fs.ko

unload:
	sudo rmmod xv6fs

.PHONY: all modules clean load unload install-mkfs uninstall-mkfs compile_commands.json

# Generate compile_commands.json using VM guest headers for IntelliSense.
# Requires: vm/kheaders/ (created by vm/setup.sh)
#           gen_compile_commands.py from a kernel source tree
GEN_COMPDB ?= $(firstword $(wildcard /home/*/reps/WSL2-Linux-Kernel/scripts/clang-tools/gen_compile_commands.py) \
              $(wildcard $(KDIR)/scripts/clang-tools/gen_compile_commands.py))

compile_commands.json: vm/kheaders/amd64/Makefile
	-$(MAKE) -C vm/kheaders/amd64 M=$(PWD) modules
	python3 $(GEN_COMPDB) -d . -o $@
	sed -i -e 's|"directory": ".*"|"directory": "$(PWD)"|' \
	       -e 's|-I\./|-I$(PWD)/vm/kheaders/amd64/|g' $@
	@echo "Generated $@"
