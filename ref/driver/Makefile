MOD_CON_NAME := memcon
MOD_DISK_NAME := memdisk

KERNEL_SOURCE := /lib/modules/`uname -r`/build/

$(MOD_CON_NAME)-objs := pblaze_hal.o pblaze_pcie.o pblaze_raid.o pblaze_con.o
obj-m +=$(MOD_CON_NAME).o

$(MOD_DISK_NAME)-objs := pblaze_hal.o pblaze_disk.o
obj-m +=$(MOD_DISK_NAME).o

default:
	$(MAKE) -C $(KERNEL_SOURCE) M=`pwd` modules
clean:
	$(MAKE) -C $(KERNEL_SOURCE) M=`pwd` clean
	$(RM) Module.markers modules.order

# To autoload driver at startup time, we should edit these system config file:
# gentoo with /etc/conf.d/modules
# ubuntu with /etc/modules
# suse with /etc/sysconfig/kernel
# redhat, centos with /etc/sysconfig/modules/memdisk.modules
install:
	mkdir -p /lib/modules/`uname -r`/
	if [ -e /lib/modules/`uname -r`/memdisk.ko ]; then rm -f /lib/modules/`uname -r`/memdisk.ko; fi
	if [ -e /lib/modules/`uname -r`/memcon.ko ]; then rm -f /lib/modules/`uname -r`/memcon.ko; fi
	if [ -e /sys/module/memdisk ]; then /sbin/rmmod memdisk; fi
	if [ -e /sys/module/memcon ]; then /sbin/rmmod memcon; fi
	if [ -e /etc/modules ]; then \
		sed -i '/memdisk/'d /etc/modules; \
		sed -i '/memcon/'d /etc/modules; \
	elif [ -e /etc/sysconfig/modules ]; then \
		rm -f /etc/sysconfig/modules/memdisk.modules; \
	elif [ -e /etc/conf.d/modules ]; then	\
		sed -i '/memdisk/d' /etc/conf.d/modules;	\
		sed -i '/memcon/d' /etc/conf.d/modules;	\
	elif [ -e /etc/sysconfig/kernel ]; then	\
		sed -i 's/\(MODULES_LOADED_ON_BOOT="\)\(memcon memdisk \)\(.*\)"/\1\3"/' /etc/sysconfig/kernel; \
	fi
	cp -f ./memcon.ko /lib/modules/`uname -r`/
	cp -f ./memdisk.ko /lib/modules/`uname -r`/ && depmod
	modprobe memcon
	modprobe memdisk
	if [ -e /etc/modules ]; then \
		mv /etc/modules /etc/modules~; \
		sed '/^[^#]*$$/,$$ d' </etc/modules~ >>/etc/modules; \
		echo memcon>>/etc/modules; \
		echo memdisk>>/etc/modules; \
		sed -n '/^[^#]*$$/,$$ p' </etc/modules~ >>/etc/modules; \
		chmod +x /etc/modules; \
		rm -f /etc/modules~; \
	elif [ -e /etc/sysconfig/modules ]; then \
		touch /etc/sysconfig/modules/memdisk.modules; \
		echo "#!/bin/bash" >>/etc/sysconfig/modules/memdisk.modules; \
		echo modprobe memdisk >>/etc/sysconfig/modules/memdisk.modules; \
		chmod +x /etc/sysconfig/modules/memdisk.modules; \
	elif [ -e /etc/conf.d/modules ]; then	\
		mv /etc/conf.d/modules /etc/conf.d/modules~ ;	\
		sed '/^[^#]*$$/,$$ d' </etc/conf.d/modules~ >>/etc/conf.d/modules; \
		echo 'modules_'`uname -r | tr .- _`'="memdisk"' >> /etc/conf.d/modules; \
		sed -n '/^[^#]*$$/,$$ p' </etc/conf.d/modules~ >>/etc/conf.d/modules; \
		rm -f /etc/conf.d/modules~; \
	elif [ -e /etc/sysconfig/kernel ]; then	\
		sed -i 's/\(MODULES_LOADED_ON_BOOT="\)\(.*\)"/\1memcon memdisk \2"/' /etc/sysconfig/kernel; \
	fi

uninstall:
	if [ -e /lib/modules/`uname -r`/memdisk.ko ]; then rm -f /lib/modules/`uname -r`/memdisk.ko; fi
	if [ -e /lib/modules/`uname -r`/memcon.ko ]; then rm -f /lib/modules/`uname -r`/memcon.ko; fi
	if [ -e /sys/module/memdisk ]; then /sbin/rmmod memdisk; fi
	if [ -e /sys/module/memcon ]; then /sbin/rmmod memcon; fi
	if [ -e /etc/modules ]; then \
		sed -i '/memcon/'d /etc/modules; \
		sed -i '/memdisk/'d /etc/modules; \
	elif [ -e /etc/sysconfig/modules ]; then \
		rm -f /etc/sysconfig/modules/memdisk.modules; \
	elif [ -e /etc/conf.d/modules ]; then	\
		sed -i '/memdisk/d' /etc/conf.d/modules;	\
		sed -i '/memcon/d' /etc/conf.d/modules;	\
	elif [ -e /etc/sysconfig/kernel ]; then	\
		sed -i 's/\(MODULES_LOADED_ON_BOOT="\)\(memcon memdisk \)\(.*\)"/\1\3"/' /etc/sysconfig/kernel; \
	fi
