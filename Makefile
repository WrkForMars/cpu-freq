# cpu-freq: velocímetro y límite de velocidad del CPU (GTK3)
PKG      := cpu-freq
VERSION  := 1.2.0
ARCH     := amd64
DEB      := build/$(PKG)_$(VERSION)_$(ARCH).deb
STAGE    := build/stage
AYUDANTE := /usr/libexec/cpu-freq/ayudante

CC       ?= gcc
CFLAGS   ?= -O2
WARN     := -std=gnu11 -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes -Werror
HARDEN   := -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
LDFLAGS  += -pie -Wl,-z,relro,-z,now -Wl,--as-needed
GTK_CF   := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)

.PHONY: build deb clean prueba

build: build/cpu-freq build/ayudante

build/cpu-freq: src/cpu-freq.c src/hw.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) $(GTK_CF) -DAYUDANTE='"$(AYUDANTE)"' $< -o $@ $(LDFLAGS) $(GTK_LIBS) -lm

build/ayudante: src/ayudante.c src/hw.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) $< -o $@ $(LDFLAGS)

deb: build
	rm -rf build/stage
	install -Dm755 -s build/cpu-freq $(STAGE)/usr/bin/cpu-freq
	install -Dm755 -s build/ayudante $(STAGE)$(AYUDANTE)
	install -Dm644 data/io.github.wrkformars.cpufreq.policy $(STAGE)/usr/share/polkit-1/actions/io.github.wrkformars.cpufreq.policy
	install -Dm644 data/cpu-freq.desktop $(STAGE)/usr/share/applications/cpu-freq.desktop
	# PNG y no SVG: así GTK no carga librsvg (~5 MB de RSS) para el icono de la ventana
	for t in 16 24 32 48 64 128 256; do \
	  mkdir -p $(STAGE)/usr/share/icons/hicolor/$${t}x$${t}/apps; \
	  rsvg-convert -w $$t -h $$t data/cpu-freq.svg -o $(STAGE)/usr/share/icons/hicolor/$${t}x$${t}/apps/cpu-freq.png; \
	done
	mkdir -p $(STAGE)/DEBIAN build/debian
	printf 'Source: $(PKG)\n\nPackage: $(PKG)\nArchitecture: $(ARCH)\n' > build/debian/control
	cd build && dpkg-shlibdeps -O stage/usr/bin/cpu-freq stage$(AYUDANTE) \
	  | sed 's/^shlibs:Depends=//' > shlibs.txt
	test -s build/shlibs.txt
	{ echo 'Package: $(PKG)'; \
	  echo 'Version: $(VERSION)'; \
	  echo 'Architecture: $(ARCH)'; \
	  echo 'Maintainer: Marcel <WrkForMars@users.noreply.github.com>'; \
	  echo "Installed-Size: $$(du -sk --exclude=DEBIAN $(STAGE) | cut -f1)"; \
	  echo "Depends: $$(cat build/shlibs.txt), pkexec | policykit-1"; \
	  echo 'Recommends: auto-cpufreq'; \
	  echo 'Section: utils'; \
	  echo 'Priority: optional'; \
	  echo 'Description: Velocímetro y límite de velocidad del CPU'; \
	  echo ' Muestra la frecuencia actual, el modo del procesador, temperatura,'; \
	  echo ' batería, RAM y swap, y permite limitar la velocidad a las'; \
	  echo ' frecuencias que admite el procesador, con o sin turbo, mediante un'; \
	  echo ' ayudante autorizado por polkit.'; \
	  echo ' Si auto-cpufreq está instalado, la elección se guarda en su'; \
	  echo ' configuración para la fuente de energía actual.'; \
	} > $(STAGE)/DEBIAN/control
	cd $(STAGE) && find usr -type f -exec md5sum {} + > DEBIAN/md5sums
	dpkg-deb --root-owner-group -Zxz --build $(STAGE) $(DEB)

# Prueba del ayudante sin root sobre una copia de /etc/auto-cpufreq.conf
prueba:
	@mkdir -p build/prueba
	cp /etc/auto-cpufreq.conf build/prueba/
	$(CC) $(CFLAGS) $(WARN) -DPRUEBA -DPRUEBA_DIR='"build/prueba"' src/ayudante.c -o build/prueba/ayudante
	for p in 1700000 turbo auto; do build/prueba/ayudante $$p && echo "== $$p" && cat build/prueba/auto-cpufreq.conf; done

clean:
	rm -rf build
