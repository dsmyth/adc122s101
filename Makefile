# Makefile for TI ADC122S101 kernel module

PROGS := adc122s101.ko rwadc

ifneq ($(KERNELRELEASE),)
    obj-m :=  adc122s101.o
else
    PWD :=  $(shell pwd)

all: $(PROGS)

install: all
	sudo install $(PROGS) $(DESTDIR)

adc122s101.ko:
ifeq ($(strip $(KERNELDIR)),)
	$(error "KERNELDIR is undefined!")
else
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules 
endif

rwadc: rwadc.o

clean:
	rm -rf rwadc *~ *.ko *.o *.mod.c modules.order Module.symvers .adc* .tmp_versions

endif

