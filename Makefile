# Makefile for TI ADC122S101 kernel module

PROGS := adc122s101.ko

MODDIR := $(DESTDIR)/lib/modules

ifneq ($(KERNELRELEASE),)
    obj-m :=  adc122s101.o
else
    PWD :=  $(shell pwd)

all: $(PROGS)

install: all
	install -v -D adc122s101.ko $(MODDIR)

adc122s101.ko:	adc122s101.c
ifeq ($(strip $(KERNELDIR)),)
	$(error "KERNELDIR is undefined!")
else
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules 
endif

clean:
	rm -rf *~ *.ko *.o *.mod.c modules.order Module.symvers .adc* .tmp_versions

endif

