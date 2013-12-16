# Makefile for TI ADC122S101 kernel module

PROGS := adc122s101.ko

ifneq ($(KERNELRELEASE),)
    obj-m :=  adc122s101.o
else
    PWD :=  $(shell pwd)

all: $(PROGS)

install: all
ifeq ($(strip $(KERNELDIR)),)
	$(error "KERNELDIR is undefined!")
else
	$(MAKE) -C $(KERNELDIR) M=$(PWD) INSTALL_MOD_PATH=$(DESTDIR) modules_install
endif

adc122s101.ko:	adc122s101.c
ifeq ($(strip $(KERNELDIR)),)
	$(error "KERNELDIR is undefined!")
else
	$(MAKE) -C $(KERNELDIR) M=$(PWD) INSTALL_MOD_PATH=$(DESTDIR) modules 
endif

clean:
	rm -rf *~ *.ko *.o *.mod.c modules.order Module.symvers .adc* .tmp_versions

endif

