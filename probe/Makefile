obj-m := apple_xdr_probe.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all: layout.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Offsets of the running driver; regenerate whenever the kernel changes.
layout.h: gen-layout.sh /sys/kernel/btf/appledrm
	./gen-layout.sh > $@.tmp && mv $@.tmp $@

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f layout.h layout.h.tmp

.PHONY: all clean
