# SPDX-License-Identifier: GPL-2.0

obj-m += pblaze_probe.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean load unload

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

load: all
	sudo insmod pblaze_probe.ko

unload:
	sudo rmmod pblaze_probe
