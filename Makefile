VERSION := 1.0.0
SRC     := src/mouse_finder.c
BINARY  := dist/mouse-finder
DEB     := dist/mouse-finder_$(VERSION)_amd64.deb

CC      := gcc
CFLAGS  := $(shell pkg-config --cflags gtk+-3.0 x11 xcursor cairo ayatana-appindicator3-0.1) -Wall -O2
LIBS    := $(shell pkg-config --libs   gtk+-3.0 x11 xcursor cairo ayatana-appindicator3-0.1) -lpthread -lm

.PHONY: all binary deb clean

all: binary deb

binary: $(BINARY)

$(BINARY): $(SRC)
	@mkdir -p dist
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	strip $@
	@echo "Built: $$(ls -lh $@ | awk '{print $$5}') binary"

deb: $(DEB)

$(DEB): $(BINARY) pkg/DEBIAN/control pkg/DEBIAN/postinst \
        pkg/usr/share/applications/mouse-finder.desktop \
        pkg/usr/lib/udev/rules.d/70-mouse-finder-uaccess.rules
	install -Dm755 $(BINARY) pkg/usr/bin/mouse-finder
	chmod 755 pkg/DEBIAN/postinst
	dpkg-deb --build --root-owner-group pkg $(DEB)
	@ls -lh $(DEB)

clean:
	rm -rf dist/ __pycache__/ *.spec
