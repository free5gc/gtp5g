
RHEL8 := $(shell cat /etc/redhat-release 2>/dev/null | grep -c " 8." )
ifneq (,$(findstring 1, $(RHEL8)))
	RHEL8FLAG := -DRHEL8
endif

PWD := $(shell pwd) 

CONFIG_MODULE_SIG=n
MODULE_NAME = gtp5g
MOD_KERNEL_PATH := kernel/drivers/net

ifeq ($(KVER),)
	KVER := $(shell uname -r)
endif

ifeq ($(KDIR),)
	KDIR := /lib/modules/$(KVER)/build
endif

ifneq ($(RHEL8FLAG),)
	INSTALL := $(MAKE) -C $(KDIR) M=$$PWD INSTALL_MOD_PATH=$(DESTDIR) INSTALL_MOD_DIR=$(MOD_KERNEL_PATH) modules_install
else
	INSTALL := cp $(MODULE_NAME).ko $(DESTDIR)/lib/modules/$(KVER)/$(MOD_KERNEL_PATH)
	RUN_DEPMOD := true
endif

ifneq ($(RUN_DEPMOD),)
	DEPMOD := /sbin/depmod -a
else
	DEPMOD := true
endif

MY_CFLAGS += -g -DDEBUG $(RHEL8FLAG)
# MY_CFLAGS += -DMATCH_IP # match IP address(in F-TEID) or not
EXTRA_CFLAGS += -Wno-misleading-indentation -Wuninitialized
CC += ${MY_CFLAGS}

obj-m := $(MODULE_NAME).o
gtp5g-y += main.o 
gtp5g-y += api_version.o
gtp5g-y += log.o util.o
gtp5g-y += dev.o genl.o net.o link.o proc.o
gtp5g-y += gtp.o pktinfo.o hash.o seid.o encap.o
gtp5g-y += pdr.o far.o qer.o bar.o urr.o genl.o genl_pdr.o genl_far.o genl_qer.o genl_bar.o genl_urr.o genl_version.o

default: module

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
 
install:
	$(INSTALL)
	modprobe udp_tunnel
	$(DEPMOD)
	modprobe $(MODULE_NAME)
	echo "gtp5g" >> /etc/modules

uninstall:
	rm -f $(DESTDIR)/lib/modules/$(KVER)/$(MOD_KERNEL_PATH)/$(MODULE_NAME).ko
	$(DEPMOD)
	sed -zi "s/gtp5g\n//g" /etc/modules
	rmmod -f  $(MODULE_NAME)
