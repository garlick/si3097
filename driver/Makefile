KERNEL_VERSION ?= $(shell uname -r)
KERNEL_PATH ?= /lib/modules/$(KERNEL_VERSION)/build
CONFIG_MODULE_SIG=n

EXTRA_CFLAGS = -DDEBUG

obj-m += si3097.o

si3097-y = module.o irup.o uart.o mmap.o ioctl.o

all: modules

modules clean:
	make -C $(KERNEL_PATH) M=$(shell pwd) $@

check:
	scripts/checkpatch.pl --no-tree -f --ignore=LINUX_VERSION_CODE \
		ioctl.c irup.c mmap.c module.c si3097.h si3097_module.h uart.c
