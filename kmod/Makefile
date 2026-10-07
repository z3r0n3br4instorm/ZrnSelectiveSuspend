# SPDX-License-Identifier: GPL-2.0-only
# Out-of-tree build of zss.ko. KDIR may point at another kernel's build directory.
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
