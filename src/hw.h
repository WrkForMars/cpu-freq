/*
 * hw.h: hardware detection shared by the app, the helper and the panel plugin.
 * Only uses libc, so the root helper does not depend on GLib.
 *
 *  - cpufreq policies and frequencies: a list (acpi-cpufreq) or a continuous range
 *    (amd-pstate, intel_pstate).
 *  - Turbo: cpufreq/boost (AMD and acpi) or intel_pstate/no_turbo (inverted).
 *  - Charger: same rule as auto-cpufreq, to pick [charger] or [battery].
 *  - auto-cpufreq: whether it is installed and where its configuration is.
 */
#ifndef CPUFREQ_HW_H
#define CPUFREQ_HW_H

#include <ctype.h>
#include <dirent.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define HW_MAX_FREC  32
#define HW_MAX_NIVEL 4

/* Root of every system path: empty for real; the tests point it at a fake tree */
#ifndef HW_RAIZ
#define HW_RAIZ ""
#endif
#define HW_CONF_ACF  HW_RAIZ "/etc/auto-cpufreq.conf"
#define HW_MARCA     "# cpu-freq:"

typedef struct {
    long min, max;                 /* cpuinfo_min_freq / cpuinfo_max_freq (kHz) */
    long nominal;                  /* top without turbo: base_frequency (Intel) or
                                      amd_pstate_nominal_freq (AMD); 0 if unknown */
    long disp[HW_MAX_FREC];        /* scaling_available_frequencies, ascending */
    int  ndisp;                    /* 0: continuous range */
    char driver[32];
} HwFrec;

static inline int hw_leer_linea(const char *ruta, char *buf, size_t n)
{
    FILE *f = fopen(ruta, "re");
    if (!f) return 0;
    int ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    if (ok) buf[strcspn(buf, "\n")] = '\0';
    return ok;
}

static inline int hw_leer_long(const char *ruta, long *v)
{
    char b[64], *fin;
    if (!hw_leer_linea(ruta, b, sizeof b)) return 0;
    *v = strtol(b, &fin, 10);
    return fin != b;
}

/* Policy directories (policyN, or cpuN/cpufreq on old kernels). */
static inline int hw_politicas(glob_t *g)
{
    if (glob(HW_RAIZ "/sys/devices/system/cpu/cpufreq/policy[0-9]*", GLOB_ONLYDIR, NULL, g) == 0 && g->gl_pathc)
        return 1;
    globfree(g);
    return glob(HW_RAIZ "/sys/devices/system/cpu/cpu[0-9]*/cpufreq", GLOB_ONLYDIR, NULL, g) == 0 && g->gl_pathc;
}

static int hw_cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

static inline int hw_frecuencias(HwFrec *h)
{
    memset(h, 0, sizeof *h);
    glob_t g;
    if (!hw_politicas(&g)) return 0;
    char ruta[PATH_MAX], b[512];
    const char *p0 = g.gl_pathv[0];

    snprintf(ruta, sizeof ruta, "%s/cpuinfo_min_freq", p0);
    hw_leer_long(ruta, &h->min);
    snprintf(ruta, sizeof ruta, "%s/cpuinfo_max_freq", p0);
    hw_leer_long(ruta, &h->max);
    snprintf(ruta, sizeof ruta, "%s/base_frequency", p0);
    if (!hw_leer_long(ruta, &h->nominal)) {
        snprintf(ruta, sizeof ruta, "%s/amd_pstate_nominal_freq", p0);
        hw_leer_long(ruta, &h->nominal);
    }
    snprintf(ruta, sizeof ruta, "%s/scaling_driver", p0);
    hw_leer_linea(ruta, h->driver, sizeof h->driver);

    snprintf(ruta, sizeof ruta, "%s/scaling_available_frequencies", p0);
    if (hw_leer_linea(ruta, b, sizeof b)) {
        char *s = b, *fin;
        while (h->ndisp < HW_MAX_FREC) {
            long v = strtol(s, &fin, 10);
            if (fin == s) break;
            if (v > 0) h->disp[h->ndisp++] = v;
            s = fin;
        }
        qsort(h->disp, (size_t)h->ndisp, sizeof h->disp[0], hw_cmp_long);
    }
    globfree(&g);
    return h->max > 0;
}

/* Can khz be used as the max limit? (from what the hardware supports) */
static inline int hw_valida_khz(const HwFrec *h, long khz)
{
    if (h->ndisp) {
        for (int i = 0; i < h->ndisp; i++) if (h->disp[i] == khz) return 1;
        return 0;
    }
    return khz >= h->min && khz <= h->max && khz % 1000 == 0;
}

/*
 * Limit levels offered by the app (ascending, not counting Auto or Turbo).
 * List: all of them if ≤ 4; otherwise min, two in between and max.
 * Range: min, midpoint rounded to 100 MHz and the top without turbo (nominal or max).
 */
static inline int hw_niveles(const HwFrec *h, long *out)
{
    int n = 0;
    if (h->ndisp && h->ndisp <= HW_MAX_NIVEL) {
        for (int i = 0; i < h->ndisp; i++) out[n++] = h->disp[i];
    } else if (h->ndisp) {
        int idx[4] = { 0, h->ndisp / 3, (2 * h->ndisp) / 3, h->ndisp - 1 };
        for (int i = 0; i < 4; i++)
            if (!n || out[n - 1] != h->disp[idx[i]]) out[n++] = h->disp[idx[i]];
    } else if (h->max > h->min) {
        long tope = (h->nominal > h->min && h->nominal < h->max) ? h->nominal : h->max;
        long medio = ((h->min + tope) / 2 + 50000) / 100000 * 100000;
        out[n++] = h->min;
        if (medio > h->min && medio < tope) out[n++] = medio;
        out[n++] = tope;
    }
    return n;
}

/* Turbo file: returns its path (or NULL) and whether the value is inverted. */
static inline const char *hw_ruta_turbo(int *inverso)
{
    if (access(HW_RAIZ "/sys/devices/system/cpu/intel_pstate/no_turbo", F_OK) == 0) {
        *inverso = 1;
        return HW_RAIZ "/sys/devices/system/cpu/intel_pstate/no_turbo";
    }
    *inverso = 0;
    if (access(HW_RAIZ "/sys/devices/system/cpu/cpufreq/boost", F_OK) == 0)
        return HW_RAIZ "/sys/devices/system/cpu/cpufreq/boost";
    return NULL;
}

/* 1 on, 0 off, -1 no turbo control. */
static inline int hw_turbo(void)
{
    int inv;
    const char *r = hw_ruta_turbo(&inv);
    long v;
    if (!r || !hw_leer_long(r, &v)) return -1;
    return (v != 0) ^ inv;
}

static inline int hw_valor_turbo(int encendido, char *b, size_t n)
{
    int inv;
    if (!hw_ruta_turbo(&inv)) return 0;
    snprintf(b, n, "%d", encendido ^ inv);
    return 1;
}

/*
 * Same rule as auto-cpufreq (SystemInfo.external_power_state):
 * no system battery → charger; if there are external supplies with "online",
 * charger if any is online; otherwise, from the battery status.
 * Also reports whether there is a battery and any external supply.
 */
static inline int hw_cargador(int *hay_bateria, int *hay_externa)
{
    DIR *d = opendir(HW_RAIZ "/sys/class/power_supply");
    int bateria = 0, externas = 0, en_linea = 0;
    char estado[32] = "";
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' || strstr(e->d_name, "hidpp_battery")) continue;
            char ruta[PATH_MAX], tipo[32] = "", alcance[32] = "", v[8];
            snprintf(ruta, sizeof ruta, HW_RAIZ "/sys/class/power_supply/%s/type", e->d_name);
            if (!hw_leer_linea(ruta, tipo, sizeof tipo)) continue;
            snprintf(ruta, sizeof ruta, HW_RAIZ "/sys/class/power_supply/%s/scope", e->d_name);
            hw_leer_linea(ruta, alcance, sizeof alcance);
            if (!strcasecmp(alcance, "device")) continue;
            if (!strcasecmp(tipo, "battery")) {
                if (!bateria) {
                    snprintf(ruta, sizeof ruta, HW_RAIZ "/sys/class/power_supply/%s/status", e->d_name);
                    hw_leer_linea(ruta, estado, sizeof estado);
                }
                bateria = 1;
                continue;
            }
            snprintf(ruta, sizeof ruta, HW_RAIZ "/sys/class/power_supply/%s/online", e->d_name);
            if (hw_leer_linea(ruta, v, sizeof v) && (v[0] == '0' || v[0] == '1' || v[0] == '2')) {
                externas++;
                if (v[0] != '0') en_linea = 1;
            }
        }
        closedir(d);
    }
    if (hay_bateria) *hay_bateria = bateria;
    if (hay_externa) *hay_externa = externas > 0;
    if (!bateria) return 1;
    if (externas) return en_linea;
    return strcasecmp(estado, "discharging") != 0;
}

/* Is auto-cpufreq installed? (package, official installer or snap) */
static inline int hw_hay_auto_cpufreq(void)
{
    static const char *rutas[] = {
        HW_CONF_ACF, HW_RAIZ "/opt/auto-cpufreq", HW_RAIZ "/usr/local/bin/auto-cpufreq",
        HW_RAIZ "/usr/bin/auto-cpufreq", HW_RAIZ "/snap/bin/auto-cpufreq",
    };
    for (size_t i = 0; i < sizeof rutas / sizeof rutas[0]; i++)
        if (access(rutas[i], F_OK) == 0) return 1;
    return 0;
}

/*
 * "# cpu-freq: X" marker in auto-cpufreq's [charger] or [battery] section.
 * Returns 1 and copies X (e.g. "1400000" or "turbo") if present; 0 otherwise (= Auto).
 */
static inline int hw_marca(int cargador, char *out, size_t n)
{
    FILE *f = fopen(HW_CONF_ACF, "re");
    if (!f) return 0;
    const char *buscada = cargador ? "charger" : "battery";
    char linea[256], sec[64] = "";
    int hay = 0;
    while (fgets(linea, sizeof linea, f)) {
        char *p = linea;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '[') {
            char *c = strchr(p, ']');
            if (c) { *c = '\0'; snprintf(sec, sizeof sec, "%s", p + 1); }
        } else if (!strcmp(sec, buscada) && !strncmp(p, HW_MARCA, strlen(HW_MARCA))) {
            p += strlen(HW_MARCA);
            while (isspace((unsigned char)*p)) p++;
            p[strcspn(p, " \t\r\n")] = '\0';
            snprintf(out, n, "%s", p);
            hay = 1;
        }
    }
    fclose(f);
    return hay;
}

#endif
