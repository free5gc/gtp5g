
RHEL8 := $(shell cat /etc/redhat-release 2>/dev/null | grep -c " 8." )
ifneq (,$(findstring 1, $(RHEL8)))
	RHEL8FLAG := -DRHEL8
endif

PWD := $(shell pwd) 
KVERSION := $(shell uname -r)

ifeq ($(KVER),)
		KVER := $(shell uname -r)
	endif

ifneq ($(RUN_DEPMOD),)
		DEPMOD := /sbin/depmod -a
	else
		DEPMOD := true
	endif

ifeq ($(KDIR),)
		KDIR := /lib/modules/$(KVER)/build
	endif


CONFIG_MODULE_SIG=n
MODULE_NAME = gtp5g

MY_CFLAGS += -g -DDEBUG $(RHEL8FLAG)
EXTRA_CFLAGS += -Wno-misleading-indentation -Wuninitialized
CC += ${MY_CFLAGS}

obj-m := $(MODULE_NAME).o

all:
	make -C $(KDIR) M=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean
 
install:
	modprobe udp_tunnel
	cp $(MODULE_NAME).ko /lib/modules/`uname -r`/kernel/drivers/net
	$(DEPMOD)
	modprobe $(MODULE_NAME)
	echo "gtp5g" >> /etc/modules

uninstall:
	rmmod $(MODULE_NAME)
	rm -f /lib/modules/`uname -r`/kernel/drivers/net/$(MODULE_NAME).ko
	$(DEPMOD)
	sed -zi "s/gtp5g\n//g" /etc/modules
