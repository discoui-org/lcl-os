.PHONY: default qemu build iso flash fonts test clean help

default: qemu

qemu:
	@python3 main.py qemu $(if $(filter 1 yes true on,$(GPU)),--gpu,) $(if $(filter 1 yes true on,$(NATIVE)),--native,) $(if $(filter 1 yes true on,$(RETINA)),--retina,) $(if $(ARCH),--arch $(ARCH),) $(if $(SCALE),--scale $(SCALE),) $(if $(WIDTH),--width $(WIDTH),) $(if $(HEIGHT),--height $(HEIGHT),) $(if $(USB),--usb $(USB),)

build:
	@python3 main.py build $(if $(ARCH),--arch $(ARCH),)

iso:
	@python3 main.py iso $(if $(ARCH),--arch $(ARCH),)

flash:
	@python3 main.py flash $(if $(USB_DEV),--dev $(USB_DEV),) $(if $(ARCH),--arch $(ARCH),)

fonts:
	@python3 main.py fonts

test:
	@python3 main.py test

clean:
	@python3 main.py clean

help:
	@python3 main.py --help
