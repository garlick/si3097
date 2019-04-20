KERNEL_VERSION ?= $(shell uname -r)
KERNEL_PATH ?= /lib/modules/$(KERNEL_VERSION)/build
CONFIG_MODULE_SIG=n

obj-m += si3097.o

si3097-y = module.o irup.o uart.o mmap.o ioctl.o

all: modules

modules clean:
	make -C $(KERNEL_PATH) M=$(shell pwd) $@
