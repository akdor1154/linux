# normal makefile
KDIR := /lib/modules/$$(uname -r)/build
build = $(MAKE) -C $(KDIR) M=$$PWD $1

.PHONY: clean help modules modules_install

modules: hid-logitech-hidpp.c hid-logitech-dj.c
	$(MAKE) -C ($KDIR)

help clean modules:
	$(call build,$@)

modules_install: default
	$(call build,$@)
