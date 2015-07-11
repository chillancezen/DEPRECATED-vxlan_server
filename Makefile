
obj-m := vxlan_server.o
vxlan_server-objs=vxlan_main.o vxlan_util.o
KVERS=$(shell uname -r)

CURDIR=$(shell pwd)

kernel_modules:
	make -C /lib/modules/$(KVERS)/build M=$(CURDIR) modules
install:
	insmod ./vxlan_server.ko
uninstall:
	rmmod vxlan_server
clean:
	make  -C /lib/modules/$(KVERS)/build M=$(CURDIR) clean
reset:
	-make uninstall -C .
	make install -C .
