/*
 * hw.h: detección del hardware compartida por la app y el ayudante.
 * Solo usa libc, para que el ayudante (root) no dependa de GLib.
 *
 *  - Políticas de cpufreq y frecuencias: lista (acpi-cpufreq) o rango continuo
 *    (amd-pstate, intel_pstate).
 *  - Turbo: cpufreq/boost (AMD y acpi) o intel_pstate/no_turbo (invertido).
 *  - Cargador: misma regla que auto-cpufreq, para elegir [charger] o [battery].
 *  - auto-cpufreq: si está instalado y dónde está su configuración.
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

#define HW_CONF_ACF  "/etc/auto-cpufreq.conf"
#define HW_MARCA     "# cpu-freq:"

typedef struct {
    long min, max;                 /* cpuinfo_min_freq / cpuinfo_max_freq (kHz) */
    long nominal;                  /* tope sin turbo: base_frequency (Intel) o
                                      amd_pstate_nominal_freq (AMD); 0 si no se sabe */
    long disp[HW_MAX_FREC];        /* scaling_available_frequencies, ascendente */
    int  ndisp;                    /* 0: rango continuo */
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

/* Directorios de política (policyN, o cpuN/cpufreq en kernels viejos). */
static inline int hw_politicas(glob_t *g)
{
    if (glob("/sys/devices/system/cpu/cpufreq/policy[0-9]*", GLOB_ONLYDIR, NULL, g) == 0 && g->gl_pathc)
        return 1;
    globfree(g);
    return glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq", GLOB_ONLYDIR, NULL, g) == 0 && g->gl_pathc;
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

/* ¿Puede usarse khz como límite máximo? (lista de la que admite el hardware) */
static inline int hw_valida_khz(const HwFrec *h, long khz)
{
    if (h->ndisp) {
        for (int i = 0; i < h->ndisp; i++) if (h->disp[i] == khz) return 1;
        return 0;
    }
    return khz >= h->min && khz <= h->max && khz % 1000 == 0;
}

/*
 * Niveles de límite que ofrece la app (ascendentes, sin contar Auto ni Turbo).
 * Lista: todos si son ≤ 4; si son más, mínimo, dos intermedios y máximo.
 * Rango: mínimo, punto medio redondeado a 100 MHz y tope sin turbo (nominal o máximo).
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

/* Archivo de turbo: devuelve la ruta (o NULL) y si el valor va invertido. */
static inline const char *hw_ruta_turbo(int *inverso)
{
    if (access("/sys/devices/system/cpu/intel_pstate/no_turbo", F_OK) == 0) {
        *inverso = 1;
        return "/sys/devices/system/cpu/intel_pstate/no_turbo";
    }
    *inverso = 0;
    if (access("/sys/devices/system/cpu/cpufreq/boost", F_OK) == 0)
        return "/sys/devices/system/cpu/cpufreq/boost";
    return NULL;
}

/* 1 encendido, 0 apagado, -1 no hay control de turbo. */
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
 * Misma regla que auto-cpufreq (SystemInfo.external_power_state):
 * sin batería del sistema → cargador; si hay fuentes externas con online,
 * cargador si alguna está en línea; si no, según el estado de la batería.
 * Devuelve también si hay batería y si hay alguna fuente externa.
 */
static inline int hw_cargador(int *hay_bateria, int *hay_externa)
{
    DIR *d = opendir("/sys/class/power_supply");
    int bateria = 0, externas = 0, en_linea = 0;
    char estado[32] = "";
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' || strstr(e->d_name, "hidpp_battery")) continue;
            char ruta[PATH_MAX], tipo[32] = "", alcance[32] = "", v[8];
            snprintf(ruta, sizeof ruta, "/sys/class/power_supply/%s/type", e->d_name);
            if (!hw_leer_linea(ruta, tipo, sizeof tipo)) continue;
            snprintf(ruta, sizeof ruta, "/sys/class/power_supply/%s/scope", e->d_name);
            hw_leer_linea(ruta, alcance, sizeof alcance);
            if (!strcasecmp(alcance, "device")) continue;
            if (!strcasecmp(tipo, "battery")) {
                if (!bateria) {
                    snprintf(ruta, sizeof ruta, "/sys/class/power_supply/%s/status", e->d_name);
                    hw_leer_linea(ruta, estado, sizeof estado);
                }
                bateria = 1;
                continue;
            }
            snprintf(ruta, sizeof ruta, "/sys/class/power_supply/%s/online", e->d_name);
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

/* ¿Está auto-cpufreq instalado? (paquete, script oficial o snap) */
static inline int hw_hay_auto_cpufreq(void)
{
    static const char *rutas[] = {
        HW_CONF_ACF, "/opt/auto-cpufreq", "/usr/local/bin/auto-cpufreq",
        "/usr/bin/auto-cpufreq", "/snap/bin/auto-cpufreq",
    };
    for (size_t i = 0; i < sizeof rutas / sizeof rutas[0]; i++)
        if (access(rutas[i], F_OK) == 0) return 1;
    return 0;
}

#endif
