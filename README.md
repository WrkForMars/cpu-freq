# cpu-freq

A CPU speedometer for Linux (C + GTK3) that also lets you **cap the processor speed** in one
click, and **works with auto-cpufreq** instead of fighting it.

| Light | Dark | AMOLED (French) |
| --- | --- | --- |
| ![Light theme](docs/captura-claro.png) | ![Dark theme](docs/captura-oscuro.png) | ![AMOLED in French](docs/captura-amoled.png) |

- Current frequency (average of all cores) and CPU governor.
- Limit buttons built from your processor: **Auto**, its frequency levels and **Turbo**.
- Temperature (max of CPU/GPU), battery, charger, free RAM and used swap.
- A line under the buttons always shows the **real state** and warns you if the system is not
  keeping your choice.
- **Settings**: theme (system, light, dark, AMOLED), language (English, Español, Português,
  Français, Deutsch) and bottom bar.

## Bottom bar

| Option | What it does | Memory |
| --- | --- | --- |
| **None** (default) | The app only runs while its window is open | 0 once closed |
| **Tray icon** | Mini speedometer in the tray (color follows temperature). Closing the window hides it; starts with your session | ~30 MB, always |
| **Panel plugin (XFCE)** | Mini speedometer and frequency inside the XFCE panel; click to open the app | ~30 kB inside the panel |

## Install

Download the `.deb` files from [Releases](../../releases) and run:

```bash
sudo apt install ./cpu-freq_*_amd64.deb
sudo apt install ./cpu-freq-xfce_*_amd64.deb   # optional: XFCE panel plugin
```

## How it works

| You press | With auto-cpufreq | Without auto-cpufreq |
| --- | --- | --- |
| **Auto** | Restores your original values for that section | Removes the limit and turns turbo on |
| **1.4 GHz** (etc.) | Saves `turbo = never` and `scaling_max_freq` | Writes the limit and turns turbo off |
| **Turbo** | Saves `turbo = always`, no limit | Removes the limit and turns turbo on |

- With **auto-cpufreq**, the choice is saved in `/etc/auto-cpufreq.conf`, only in the section
  for the current power source (`[charger]` or `[battery]`), next to a `# cpu-freq: …` marker.
  auto-cpufreq watches that file and reloads it on its own; the service is never restarted.
  Before the first change, a copy is saved to `/etc/auto-cpufreq.conf.cpu-freq-orig`.
- The app decides whether you are on the charger with the same rule auto-cpufreq uses.
- Writing to the system needs root. A **minimal helper** (`/usr/libexec/cpu-freq/ayudante`)
  does it through `pkexec`. The polkit action needs no password for the active local session
  and is denied to remote or inactive sessions.
- The helper only accepts `auto`, `turbo` or a frequency in kHz that the processor supports
  (`scaling_available_frequencies`, or the `cpuinfo_min_freq`–`cpuinfo_max_freq` range).
  It never takes a path.

## Compatibility

| Driver / case | Status |
| --- | --- |
| `acpi-cpufreq` (AMD Ryzen 3 3250U), Linux Mint 22.3 XFCE, auto-cpufreq 3.2.0 | Tested |
| XFCE 4.18 tray and panel plugin | Tested |
| `amd-pstate` / `amd-pstate-epp` (continuous range, `cpufreq/boost`) | Not tested |
| `intel_pstate` (continuous range, `intel_pstate/no_turbo`) | Not tested |
| `[battery]` section (laptop unplugged) | Not tested |
| auto-cpufreq installed as a snap | Not tested |
| Tray icon on KDE Plasma / GNOME (no XEmbed tray) | Not supported |
| auto-cpufreq config in root's `~/.config` | Not supported: `/etc/auto-cpufreq.conf` is used |

If you try it on other hardware, please open an issue with the result.

## Color thresholds

Defaults: high temperature 85 °C, critical 95 °C, low battery 25 %, minimum RAM 250 MB.
Change them in `~/.config/cpu-freq.conf`, where the app also stores its settings:

```ini
TEMP_AVISO=85
TEMP_CRITICA=95
BATERIA_BAJA=25
RAM_MINIMA=250
```

## Build

```bash
sudo apt install build-essential pkg-config libgtk-3-dev libxfce4panel-2.0-dev librsvg2-bin gettext dpkg-dev
make build     # app, helper, translations and panel plugin in build/
make deb       # cpu-freq and cpu-freq-xfce packages in build/
make prueba    # tests the helper without root on a copy of /etc/auto-cpufreq.conf
make clean
```

Without `libxfce4panel-2.0-dev`, everything except the panel plugin is built.

## Translations

UI strings are in English in the source code. Translations live in `po/<lang>.po`. To add a
language, copy `po/cpu-freq.pot` to `po/<code>.po`, translate it and add the code to
`IDIOMAS` in the `Makefile`. `make pot` extracts the strings again when they change.

## Memory

About 33 MB RSS with the window open (the same in the background if you pick the tray icon).
An empty GTK3 window already takes ~30 MB on the same machine, mostly libraries shared with
the rest of the desktop (PSS ≈ 9 MB).

## License

[MIT](LICENSE). It contains no auto-cpufreq code: it only edits its configuration file.
