# Changelog

## 1.7.0

- Tray icon on KDE Plasma, GNOME (AppIndicator extension) and Wayland through
  StatusNotifierItem. `libayatana-appindicator3` is loaded only when needed; XFCE, MATE and
  Cinnamon keep the XEmbed icon with left-click toggle. `CPU_FREQ_BANDEJA=xembed|sni` forces one.
- `make install` / `make install-xfce` with `PREFIX`, `LIBEXECDIR`, `PLUGDIR` and `LICENSEDIR`;
  the polkit action is generated with the real helper path. The `.deb` build uses them.
- Arch Linux PKGBUILD in `packaging/arch/`.
- Code comments in English.

## 1.6.0

- Right-click menu on the tray icon and the XFCE panel plugin to pick Auto, a frequency level
  or Turbo without opening the window.
- Two-column layout (about 860×510) when the screen's usable height is under 980 px.
- The graph legend shows the 5-minute average frequency and average/peak temperature, with a
  dashed line at the average frequency.
- `make test`: 67 checks of the root helper against fake sysfs trees, run by the GitHub Action.

## 1.5.0

- Graph of the last 5 minutes of frequency and temperature.
- Clicking the tray icon or panel plugin again hides or closes the window.

## 1.4.0

- English is the source language; Spanish, Portuguese, French and German translations.
- Settings: system/light/dark/AMOLED theme, language and bottom bar.
- Optional tray icon that keeps the app running and starts with the session.
- XFCE panel plugin (`cpu-freq-xfce` package).

## 1.2.0

- Works on any cpufreq driver: limit buttons are built from the processor's frequencies
  (list or continuous range); Intel `no_turbo` support.
- With auto-cpufreq installed, the choice is saved in its configuration for the current
  power source, so it is kept instead of reverted.

## 1.0.0

- First version: speedometer, four speed limits for an AMD Ryzen 3 3250U, system status and a
  polkit helper.
