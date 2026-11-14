# cpu-freq: CPU speedometer and speed limiter (GTK3)
#
#   make build          app, helper, translations, icons and (with libxfce4panel-2.0-dev)
#                       the XFCE panel plugin
#   make install        installs the app into $(DESTDIR)$(PREFIX)
#   make install-xfce   installs the panel plugin
#   make deb            build/cpu-freq_*.deb and, if built, build/cpu-freq-xfce_*.deb
#   make test           tests the root helper without root against fake sysfs trees
#   make pot            extracts the UI strings again into po/cpu-freq.pot
#   make clean
#
# Paths can be overridden, e.g. for Arch Linux:
#   make PREFIX=/usr LIBEXECDIR=/usr/lib PLUGDIR=/usr/lib/xfce4/panel/plugins \
#        LICENSEDIR=/usr/share/licenses/cpu-freq
PKG        := cpu-freq
VERSION    := 1.7.0
ARCH       := amd64
MAINT      := Marcel <WrkForMars@users.noreply.github.com>
IDIOMAS    := es pt fr de

PREFIX     ?= /usr
BINDIR     ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/libexec
DATADIR    ?= $(PREFIX)/share
LOCALEDIR  ?= $(DATADIR)/locale
PLUGDIR    ?= $(PREFIX)/lib/x86_64-linux-gnu/xfce4/panel/plugins
LICENSEDIR ?= $(DATADIR)/doc/cpu-freq
AYUDANTE   := $(LIBEXECDIR)/cpu-freq/ayudante
STRIP      ?= -s

CC       ?= gcc
CFLAGS   ?= -O2
WARN     := -std=gnu11 -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes -Werror
HARDEN   := -D_FORTIFY_SOURCE=2 -fstack-protector-strong
LDHARD   := -Wl,-z,relro,-z,now -Wl,--as-needed
GTK_CF   := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0 x11)
XFCE     := $(shell pkg-config --exists libxfce4panel-2.0 && echo yes)
MOS      := $(foreach l,$(IDIOMAS),build/locale/$(l)/LC_MESSAGES/cpu-freq.mo)
TAMANOS  := 16 24 32 48 64 128 256
ICONOS   := $(foreach t,$(TAMANOS),build/icons/$(t)x$(t)/cpu-freq.png)
POLICY   := io.github.wrkformars.cpufreq.policy

.PHONY: build install install-xfce deb clean test pot

build: build/cpu-freq build/ayudante build/$(POLICY) $(MOS) $(ICONOS) $(if $(XFCE),build/libcpu-freq-panel.so)
ifneq ($(XFCE),yes)
	@echo "(no libxfce4panel-2.0-dev: the panel plugin is not built)"
endif

build/cpu-freq: src/cpu-freq.c src/hw.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) -fPIE $(GTK_CF) -DAYUDANTE='"$(AYUDANTE)"' -DLOCALEDIR='"$(LOCALEDIR)"' \
	  $< -o $@ -pie $(LDHARD) $(LDFLAGS) $(GTK_LIBS) -lm

build/ayudante: src/ayudante.c src/hw.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) -fPIE $< -o $@ -pie $(LDHARD) $(LDFLAGS)

build/libcpu-freq-panel.so: src/panel.c src/hw.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) -fPIC -shared $$(pkg-config --cflags libxfce4panel-2.0) \
	  -DAYUDANTE='"$(AYUDANTE)"' -DLOCALEDIR='"$(LOCALEDIR)"' \
	  $< -o $@ $(LDHARD) $(LDFLAGS) $$(pkg-config --libs libxfce4panel-2.0) -lm

# The polkit action must point at the helper's real path
build/$(POLICY): data/$(POLICY).in
	@mkdir -p build
	sed 's|@AYUDANTE@|$(AYUDANTE)|' $< > $@

build/locale/%/LC_MESSAGES/cpu-freq.mo: po/%.po
	@mkdir -p $(dir $@)
	msgfmt -c -o $@ $<

# PNG, not SVG: GTK then never loads librsvg (~5 MB RSS) just for the window icon
build/icons/%/cpu-freq.png: data/cpu-freq.svg
	@mkdir -p $(dir $@)
	t=$*; rsvg-convert -w $${t%x*} -h $${t%x*} $< -o $@

install: build
	install -Dm755 $(STRIP) build/cpu-freq $(DESTDIR)$(BINDIR)/cpu-freq
	install -Dm755 $(STRIP) build/ayudante $(DESTDIR)$(AYUDANTE)
	install -Dm644 build/$(POLICY) $(DESTDIR)$(DATADIR)/polkit-1/actions/$(POLICY)
	install -Dm644 data/cpu-freq.desktop $(DESTDIR)$(DATADIR)/applications/cpu-freq.desktop
	for l in $(IDIOMAS); do \
	  install -Dm644 build/locale/$$l/LC_MESSAGES/cpu-freq.mo $(DESTDIR)$(LOCALEDIR)/$$l/LC_MESSAGES/cpu-freq.mo; \
	done
	for t in $(TAMANOS); do \
	  install -Dm644 build/icons/$${t}x$${t}/cpu-freq.png $(DESTDIR)$(DATADIR)/icons/hicolor/$${t}x$${t}/apps/cpu-freq.png; \
	done
	install -Dm644 LICENSE $(DESTDIR)$(LICENSEDIR)/copyright

install-xfce: build/libcpu-freq-panel.so
	install -Dm644 $(STRIP) build/libcpu-freq-panel.so $(DESTDIR)$(PLUGDIR)/libcpu-freq-panel.so
	install -Dm644 data/cpu-freq-panel.desktop $(DESTDIR)$(DATADIR)/xfce4/panel/plugins/cpu-freq-panel.desktop

pot:
	xgettext --from-code=UTF-8 -k_ -kN_ -L C --package-name=cpu-freq --package-version=$(VERSION) \
	  -o po/cpu-freq.pot src/cpu-freq.c src/panel.c

# CONTROL <template> <package> <stage>: fills data/<template> with version, size and dependencies
define CONTROL
	sed -e 's|@VERSION@|$(VERSION)|g' -e 's|@ARCH@|$(ARCH)|' -e 's|@MAINT@|$(MAINT)|' \
	    -e "s|@SIZE@|$$(du -sk --exclude=DEBIAN $(3) | cut -f1)|" \
	    -e "s|@SHLIBS@|$$(cat build/shlibs-$(2).txt)|" data/$(1) > $(3)/DEBIAN/control
endef

deb: build
	rm -rf build/stage build/stage-xfce
	$(MAKE) --no-print-directory install DESTDIR=$(CURDIR)/build/stage PREFIX=/usr
	mkdir -p build/stage/DEBIAN build/debian
	printf 'Source: $(PKG)\n\nPackage: $(PKG)\nArchitecture: $(ARCH)\n' > build/debian/control
	cd build && dpkg-shlibdeps -O stage/usr/bin/cpu-freq stage$(AYUDANTE) \
	  | sed 's/^shlibs:Depends=//' > shlibs-cpu-freq.txt
	test -s build/shlibs-cpu-freq.txt
	$(call CONTROL,control.in,cpu-freq,build/stage)
	cd build/stage && find usr -type f -exec md5sum {} + > DEBIAN/md5sums
	dpkg-deb --root-owner-group -Zxz --build build/stage build/$(PKG)_$(VERSION)_$(ARCH).deb
ifeq ($(XFCE),yes)
	$(MAKE) --no-print-directory install-xfce DESTDIR=$(CURDIR)/build/stage-xfce PREFIX=/usr
	mkdir -p build/stage-xfce/DEBIAN
	cd build && dpkg-shlibdeps -O stage-xfce$(PLUGDIR)/libcpu-freq-panel.so \
	  | sed 's/^shlibs:Depends=//' > shlibs-cpu-freq-xfce.txt
	$(call CONTROL,control-xfce.in,cpu-freq-xfce,build/stage-xfce)
	cd build/stage-xfce && find usr -type f -exec md5sum {} + > DEBIAN/md5sums
	dpkg-deb --root-owner-group -Zxz --build build/stage-xfce build/$(PKG)-xfce_$(VERSION)_$(ARCH).deb
endif

# Test-only build of the helper: no root check, every system path under build/prueba/raiz.
# It is never installed or packaged.
build/prueba/ayudante: src/ayudante.c src/hw.h
	@mkdir -p build/prueba
	$(CC) $(CFLAGS) $(WARN) -DPRUEBA -DHW_RAIZ='"$(CURDIR)/build/prueba/raiz"' $< -o $@

test: build/prueba/ayudante
	bash tests/ayudante.sh build/prueba/ayudante $(CURDIR)/build/prueba/raiz

clean:
	rm -rf build
