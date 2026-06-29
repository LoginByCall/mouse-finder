VERSION := 1.0.0
BINARY  := dist/mouse-finder
DEB     := dist/mouse-finder_$(VERSION)_all.deb

.PHONY: all binary deb clean

all: binary deb

binary: $(BINARY)

$(BINARY): mouse_finder.py
	pip3 install --quiet pyinstaller --break-system-packages 2>/dev/null || true
	pyinstaller --onefile --name mouse-finder --clean mouse_finder.py

deb: $(DEB)

$(DEB): mouse_finder.py pkg/DEBIAN/control
	cp mouse_finder.py pkg/usr/lib/mouse-finder/mouse_finder.py
	dpkg-deb --build --root-owner-group pkg $(DEB)
	@echo "Package: $(DEB)"
	@ls -lh $(DEB)

clean:
	rm -rf dist/ build/ __pycache__/ *.spec
