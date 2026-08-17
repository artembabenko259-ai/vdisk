obj-m += vdisk_drv.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
CC   ?= cc

.PHONY: all module cli clean load unload

all: module cli

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

cli: vdisk
vdisk: vdisk-cli.c
	$(CC) -Wall -Wextra -O2 -std=c11 -o vdisk vdisk-cli.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f vdisk

load:
	sudo insmod vdisk_drv.ko

unload:
	sudo rmmod vdisk_drv
