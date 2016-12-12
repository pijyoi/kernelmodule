ifneq ($(KERNELRELEASE),)
obj-m := mymiscdev.o
else
KDIR := /lib/modules/$$(uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$$PWD

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean
endif

