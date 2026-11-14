# cpu-freq: CPU speedometer and speed limiter (GTK3)
#
#   make build    app, helper, translations and (with libxfce4panel-2.0-dev) the panel plugin
#   make deb      build/cpu-freq_*.deb and, if built, build/cpu-freq-xfce_*.deb
#   make pot      extracts the UI strings again into po/cpu-freq.pot
#   make test     tests the root helper without root against fake sysfs/auto-cpufreq trees
#   make clean
PKG      := cpu-freq
VERSION  := 1.6.0
ARCH     := amd64
AYUDANTE := /usr/libexec/cpu-freq/ayudante
LOCALEDIR:= /usr/share/locale
IDIOMAS  := es pt fr de
MAINT    := Marcel <WrkForMars@users.noreply.github.com>
PLUGDIR  := /usr/lib/x86_64-linux-gnu/xfce4/panel/plugins

CC       ?= gcc
CFLAGS   ?= -O2
WARN     := -std=gnu11 -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes -Werror
HARDEN   := -D_FORTIFY_SOURCE=2 -fstack-protector-strong
LDHARD   := -Wl,-z,relro,-z,now -Wl,--as-needed
GTK_CF   := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)
XFCE     := $(shell pkg-config --exists libxfce4panel-2.0 && echo si)
MOS      := $(foreach l,$(IDIOMAS),build/locale/$(l)/LC_MESSAGES/cpu-freq.mo)

.PHONY: build deb clean test pot

build: build/cpu-freq build/ayudante $(MOS) $(if $(XFCE),build/libcpu-freq-panel.so)
ifneq ($(XFCE),si)
	@echo "(sin libxfce4panel-2.0-dev: no se compila el complemento del panel)"
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
	  -DLOCALEDIR='"$(LOCALEDIR)"' $< -o $@ $(LDHARD) $(LDFLAGS) $$(pkg-config --libs libxfce4panel-2.0) -lm

build/locale/%/LC_MESSAGES/cpu-freq.mo: po/%.po
	@mkdir -p $(dir $@)
	msgfmt -c -o $@ $<

pot:
	xgettext --from-code=UTF-8 -k_ -kN_ -L C --package-name=cpu-freq --package-version=$(VERSION) \
	  -o po/cpu-freq.pot src/cpu-freq.c src/panel.c

# control <plantilla> <paquete> <stage>: rellena data/<plantilla> con versión, tamaño y dependencias
define CONTROL
	sed -e 's|@VERSION@|$(VERSION)|g' -e 's|@ARCH@|$(ARCH)|' -e 's|@MAINT@|$(MAINT)|' \
	    -e "s|@SIZE@|$$(du -sk --exclude=DEBIAN $(3) | cut -f1)|" \
	    -e "s|@SHLIBS@|$$(cat build/shlibs-$(2).txt)|" data/$(1) > $(3)/DEBIAN/control
endef

deb: build
	rm -rf build/stage build/stage-xfce
	install -Dm755 -s build/cpu-freq build/stage/usr/bin/cpu-freq
	install -Dm755 -s build/ayudante build/stage$(AYUDANTE)
	install -Dm644 data/io.github.wrkformars.cpufreq.policy build/stage/usr/share/polkit-1/actions/io.github.wrkformars.cpufreq.policy
	install -Dm644 data/cpu-freq.desktop build/stage/usr/share/applications/cpu-freq.desktop
	for l in $(IDIOMAS); do \
	  install -Dm644 build/locale/$$l/LC_MESSAGES/cpu-freq.mo build/stage$(LOCALEDIR)/$$l/LC_MESSAGES/cpu-freq.mo; \
	done
	# PNG y no SVG: así GTK no carga librsvg (~5 MB de RSS) para el icono de la ventana
	for t in 16 24 32 48 64 128 256; do \
	  mkdir -p build/stage/usr/share/icons/hicolor/$${t}x$${t}/apps; \
	  rsvg-convert -w $$t -h $$t data/cpu-freq.svg -o build/stage/usr/share/icons/hicolor/$${t}x$${t}/apps/cpu-freq.png; \
	done
	mkdir -p build/stage/DEBIAN build/debian
	printf 'Source: $(PKG)\n\nPackage: $(PKG)\nArchitecture: $(ARCH)\n' > build/debian/control
	cd build && dpkg-shlibdeps -O stage/usr/bin/cpu-freq stage$(AYUDANTE) \
	  | sed 's/^shlibs:Depends=//' > shlibs-cpu-freq.txt
	test -s build/shlibs-cpu-freq.txt
	$(call CONTROL,control.in,cpu-freq,build/stage)
	cd build/stage && find usr -type f -exec md5sum {} + > DEBIAN/md5sums
	dpkg-deb --root-owner-group -Zxz --build build/stage build/$(PKG)_$(VERSION)_$(ARCH).deb
ifeq ($(XFCE),si)
	install -Dm644 -s build/libcpu-freq-panel.so build/stage-xfce$(PLUGDIR)/libcpu-freq-panel.so
	install -Dm644 data/cpu-freq-panel.desktop build/stage-xfce/usr/share/xfce4/panel/plugins/cpu-freq-panel.desktop
	mkdir -p build/stage-xfce/DEBIAN
	cd build && dpkg-shlibdeps -O stage-xfce$(PLUGDIR)/libcpu-freq-panel.so \
	  | sed 's/^shlibs:Depends=//' > shlibs-cpu-freq-xfce.txt
	$(call CONTROL,control-xfce.in,cpu-freq-xfce,build/stage-xfce)
	cd build/stage-xfce && find usr -type f -exec md5sum {} + > DEBIAN/md5sums
	dpkg-deb --root-owner-group -Zxz --build build/stage-xfce build/$(PKG)-xfce_$(VERSION)_$(ARCH).deb
endif

# Test build of the helper: no root check, every system path under build/prueba/raiz.
# It is never packaged.
build/prueba/ayudante: src/ayudante.c src/hw.h
	@mkdir -p build/prueba
	$(CC) $(CFLAGS) $(WARN) -DPRUEBA -DHW_RAIZ='"$(CURDIR)/build/prueba/raiz"' $< -o $@

test: build/prueba/ayudante
	bash tests/ayudante.sh build/prueba/ayudante $(CURDIR)/build/prueba/raiz

clean:
	rm -rf build
