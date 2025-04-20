PACKAGE := new-hidpp
PACKAGE_VERSION := $(shell git describe --long).$(shell date -u -d "$$(git show -s --format=%ci HEAD)" +%Y%m%d)

DKMS_ROOT_PATH := /usr/src/$(PACKAGE)-$(PACKAGE_VERSION)

ifndef TARGET
TARGET = $(shell uname -r)
endif

.PHONY: dkms dkms_clean

dkms:
	mkdir -p $(DKMS_ROOT_PATH)
	rsync --recursive --verbose --copy-links \
		--include '/*/' --include '*.c' --include '*.h' \
		--include 'Makefile' --include 'Kbuild' \
		--exclude '*' module/ $(DKMS_ROOT_PATH)/
	echo "$(PACKAGE_VERSION)" >$(DKMS_ROOT_PATH)/VERSION
	sed -e '/^PACKAGE_VERSION=/ s/=.*/=\"$(PACKAGE_VERSION)\"/' <dkms.conf >$(DKMS_ROOT_PATH)/dkms.conf
	dkms add -m $(PACKAGE) -v $(PACKAGE_VERSION)
	dkms build -m $(PACKAGE) -v $(PACKAGE_VERSION) -k $(TARGET)
	dkms install --force -m $(PACKAGE) -v $(PACKAGE_VERSION) -k $(TARGET)

dkms_clean:
	dkms remove -m $(PACKAGE) -v $(PACKAGE_VERSION) --all
	rm -rf $(DKMS_ROOT_PATH)
