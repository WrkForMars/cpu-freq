/*
 * cpu-freq: CPU speedometer and speed limiter.
 *
 * By default a single window that reads sysfs/proc only while it is open and exits when
 * closed. Settings offer:
 *   - Theme: system, light, dark or AMOLED (pure black).
 *   - Language: system, Spanish, English, Portuguese, French or German (gettext).
 *   - Bottom bar: none, a tray icon (the app keeps running after the window is closed and
 *     starts with the session) or the XFCE panel plugin (cpu-freq-xfce package, runs
 *     inside the panel).
 * Settings are stored in ~/.config/cpu-freq.conf.
 *
 * The positions (Auto, frequency levels and Turbo) come from the hardware (hw.h).
 * To write, it runs AYUDANTE through pkexec; if auto-cpufreq is installed, the helper
 * saves the choice in its configuration.
 *
 * Identifiers are in Spanish (the project started that way); comments and UI strings are
 * in English.
 */
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <dlfcn.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#include <ctype.h>
#include <dirent.h>
#include <glob.h>
#include <libintl.h>
#include <locale.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "hw.h"

#ifndef AYUDANTE
#define AYUDANTE "/usr/libexec/cpu-freq/ayudante"
#endif
#ifndef LOCALEDIR
#define LOCALEDIR "/usr/share/locale"
#endif
#define DOMINIO        "cpu-freq"
#define APP_ID         "io.github.wrkformars.CpuFreq"
#define PLUGIN_PANEL   "/usr/share/xfce4/panel/plugins/cpu-freq-panel.desktop"

#define _(s)  gettext(s)
#define N_(s) (s)

#define REFRESCO_MS   1500
#define ANIM_US       180000
#define MSG_ERROR_S   6

extern int _nl_msg_cat_cntr;   /* glibc: bumping it invalidates gettext's cache */

/* ---------------------------------------------------------------- Palette */

typedef struct {
    const char *fondo, *superficie, *linea, *borde, *tinta, *tinta_suave;
    const char *acento, *sobre_acento, *acento_texto, *acento_suave;
    const char *ok, *ok_suave, *ok_tinta;
    const char *adv, *adv_suave, *adv_tinta;
    const char *err, *err_suave, *err_tinta;
} Paleta;

/* A single blue accent (#2563EB). Contrast checked in all three themes:
 * ink ≥17:1, soft ink ≥7.6:1, accent/background ≥3.7:1, text on accent 5.2:1,
 * control borders ≥3.5:1, chip text ≥8:1. */
static const Paleta CLARO = {
    "#F7F8FA", "#FFFFFF", "#DDE1E7", "#808A99", "#0F1720", "#4A5565",
    "#2563EB", "#FFFFFF", "#1D4ED8", "#E3ECFD",
    "#15803D", "#E4F4E8", "#14532D",
    "#B45309", "#FDF0DF", "#7C2D12",
    "#B91C1C", "#FBE7E7", "#7F1D1D",
};
static const Paleta OSCURO = {
    "#0E1116", "#161B22", "#2A313C", "#6E7887", "#F2F4F7", "#A9B2BF",
    "#2563EB", "#FFFFFF", "#7AA7FF", "#1B2A45",
    "#4ADE80", "#12291B", "#A7F3C0",
    "#FBBF24", "#2E2410", "#FDE68A",
    "#F87171", "#321616", "#FECACA",
};
/* AMOLED: pure black background; surfaces are separated by borders, not gray. */
static const Paleta AMOLED = {
    "#000000", "#000000", "#262626", "#737373", "#F5F5F5", "#A3A3A3",
    "#2563EB", "#FFFFFF", "#7AA7FF", "#0F1A33",
    "#4ADE80", "#0B1F12", "#A7F3C0",
    "#FBBF24", "#241B08", "#FDE68A",
    "#F87171", "#2A0F0F", "#FECACA",
};

/* --------------------------------------------------------------- Settings */

typedef struct { const char *id, *nombre; } Opcion;

static const Opcion TEMAS[] = {
    { "sistema", N_("System") }, { "claro", N_("Light") },
    { "oscuro", N_("Dark") },   { "amoled", N_("AMOLED") },
};
/* Language names are written in their own language: not translated. */
static const Opcion IDIOMAS[] = {
    { "sistema", N_("System") }, { "es", "Español" }, { "en", "English" },
    { "pt", "Português" }, { "fr", "Français" }, { "de", "Deutsch" },
};
static const Opcion BARRAS[] = {
    { "ninguna", N_("None") }, { "bandeja", N_("Tray icon") },
    { "panel", N_("Panel plugin (XFCE)") },
};

static struct { char tema[12], idioma[12], barra[12]; } aj = { "sistema", "sistema", "ninguna" };
static gchar *language_original;   /* LANGUAGE at startup, to go back to "System" */

/* -------------------------------------------------------------- Positions */

#define MAX_POS (HW_MAX_NIVEL + 2)
typedef struct { char arg[16], etiqueta[16], nombre[40], ayuda[96]; long khz; int boost; } Posicion;
static Posicion POS[MAX_POS];     /* 0 = Auto; then the levels; Turbo last if available */
static int NPOS;
static HwFrec hw;
static long tope_sin_turbo;       /* highest level without turbo (kHz) */
static int turbo_pos = -1;        /* Turbo's index, -1 if the machine has none */

/* ------------------------------------------------------------------ State */

typedef struct {
    double ghz;            /* average of scaling_cur_freq */
    long   max_khz;        /* scaling_max_freq if all policies agree, -1 otherwise */
    int    boost;          /* 0/1, -1 if there is no turbo */
    char   governor[32];
    int    temp;           /* °C, -1 if none */
    int    bat;            /* %, -1 if there is no battery */
    char   bat_estado[32];
    int    cargador;       /* 1/0, same rule as auto-cpufreq */
    int    hay_bateria, hay_externa;
    long   ram_mb, swap_mb;
    int    remoto, vigilante;
} Lectura;

static struct {
    int temp_aviso, temp_critica, bateria_baja, ram_minima;
} umbral = { 85, 95, 25, 250 };

enum { F_TEMP, F_BAT, F_CARG, F_RAM, F_SWAP, F_REMOTO, F_VIG, N_FILAS };

static struct {
    GtkApplication *gapp;
    GtkWidget *ventana, *contenido, *gauge, *modo, *botones[MAX_POS], *aviso, *titulo_lim;
    GtkWidget *valor[N_FILAS], *chip[N_FILAS], *fila[N_FILAS];
    GtkWidget *nota_ajustes, *grafica, *ley_ghz, *ley_temp;
    gboolean fila_fija[N_FILAS];  /* FALSE: the row is hidden (no personal scripts) */
    double escala;                /* GHz at the end of the dial */
    GtkCssProvider *css;
    const Paleta *pal;
    Lectura l;
    int elegido;           /* chosen position: auto-cpufreq marker or, without it, this session's */
    gboolean con_acf;      /* auto-cpufreq instalado */
    int discrepa;          /* consecutive refreshes in which the system does not match the choice */
    int real;              /* position matching the system, -1 none */
    gboolean actualizando, aplicando;
    char error[160];
    gint64 error_hasta;
    double mostrado, desde, hacia;
    gint64 anim_inicio;
    guint anim_id, timer;
    gboolean primera, oculta_al_inicio;
    GtkStatusIcon *bandeja;
    char bandeja_firma[32];       /* avoids redrawing the icon when nothing changed */
} app;

/* --------------------------------------------------------------- Readings */

static gboolean leer_linea(const char *ruta, char *buf, size_t n)
{
    FILE *f = fopen(ruta, "re");
    if (!f) return FALSE;
    gboolean ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    if (ok) buf[strcspn(buf, "\n")] = '\0';
    return ok;
}

static gboolean leer_long(const char *ruta, long *v)
{
    char b[64];
    if (!leer_linea(ruta, b, sizeof b)) return FALSE;
    char *fin;
    *v = strtol(b, &fin, 10);
    return fin != b;
}

static gchar *ruta_conf(void) { return g_build_filename(g_get_user_config_dir(), "cpu-freq.conf", NULL); }

/*
 * Thresholds: ~/.config/modo-remoto.conf, then ~/.config/cpu-freq.conf (the last one wins).
 * Only these keys are read; everything else in those files (e.g. an ntfy topic) is ignored.
 */
static void leer_conf_de(const char *nombre, gboolean con_ajustes)
{
    char *ruta = g_build_filename(g_get_user_config_dir(), nombre, NULL);
    FILE *f = fopen(ruta, "re");
    g_free(ruta);
    if (!f) return;
    char linea[256];
    static const struct { const char *k; int *v; } numeros[] = {
        { "TEMP_AVISO=",   &umbral.temp_aviso },
        { "TEMP_CRITICA=", &umbral.temp_critica },
        { "BATERIA_BAJA=", &umbral.bateria_baja },
        { "RAM_MINIMA=",   &umbral.ram_minima },
    };
    const struct { const char *k; char *v; size_t n; const Opcion *ops; size_t nops; } textos[] = {
        { "TEMA=",   aj.tema,   sizeof aj.tema,   TEMAS,   G_N_ELEMENTS(TEMAS) },
        { "IDIOMA=", aj.idioma, sizeof aj.idioma, IDIOMAS, G_N_ELEMENTS(IDIOMAS) },
        { "BARRA=",  aj.barra,  sizeof aj.barra,  BARRAS,  G_N_ELEMENTS(BARRAS) },
    };
    while (fgets(linea, sizeof linea, f)) {
        char *p = linea;
        while (isspace((unsigned char)*p)) p++;
        for (size_t i = 0; i < G_N_ELEMENTS(numeros); i++) {
            size_t n = strlen(numeros[i].k);
            if (strncmp(p, numeros[i].k, n) == 0) {
                char *fin;
                long v = strtol(p + n, &fin, 10);
                if (fin != p + n && v > 0 && v < 100000) *numeros[i].v = (int)v;
            }
        }
        for (size_t i = 0; con_ajustes && i < G_N_ELEMENTS(textos); i++) {
            size_t n = strlen(textos[i].k);
            if (strncmp(p, textos[i].k, n) != 0) continue;
            char *v = g_strstrip(p + n);
            for (size_t j = 0; j < textos[i].nops; j++)        /* known values only */
                if (!strcmp(v, textos[i].ops[j].id)) g_strlcpy(textos[i].v, v, textos[i].n);
        }
    }
    fclose(f);
}

static void leer_conf(void)
{
    leer_conf_de("modo-remoto.conf", FALSE);
    leer_conf_de("cpu-freq.conf", TRUE);
}

/* Rewrites TEMA/IDIOMA/BARRA in ~/.config/cpu-freq.conf, keeping everything else. */
static void guardar_ajustes(void)
{
    gchar *ruta = ruta_conf(), *viejo = NULL;
    GString *s = g_string_new(NULL);
    if (g_file_get_contents(ruta, &viejo, NULL, NULL)) {
        gchar **lineas = g_strsplit(viejo, "\n", -1);
        for (gchar **l = lineas; *l; l++) {
            const char *p = *l;
            while (isspace((unsigned char)*p)) p++;
            if (g_str_has_prefix(p, "TEMA=") || g_str_has_prefix(p, "IDIOMA=") ||
                g_str_has_prefix(p, "BARRA=") || (!**l && !l[1]))
                continue;
            g_string_append_printf(s, "%s\n", *l);
        }
        g_strfreev(lineas);
        g_free(viejo);
    } else {
        g_string_append(s, "# Ajustes de cpu-freq\n");
    }
    g_string_append_printf(s, "TEMA=%s\nIDIOMA=%s\nBARRA=%s\n", aj.tema, aj.idioma, aj.barra);
    gchar *dir = g_path_get_dirname(ruta);
    g_mkdir_with_parents(dir, 0700);
    g_file_set_contents(ruta, s->str, -1, NULL);
    g_free(dir);
    g_string_free(s, TRUE);
    g_free(ruta);
}

static void leer_cpu(Lectura *l)
{
    glob_t g;
    l->ghz = 0; l->max_khz = -1; strcpy(l->governor, "?");
    if (!hw_politicas(&g)) return;
    long suma = 0, n = 0, max = 0;
    gboolean max_igual = TRUE, gov_igual = TRUE;
    char gov[32] = "";
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char ruta[PATH_MAX], b[32];
        long v;
        snprintf(ruta, sizeof ruta, "%s/scaling_cur_freq", g.gl_pathv[i]);
        if (leer_long(ruta, &v)) { suma += v; n++; }
        snprintf(ruta, sizeof ruta, "%s/scaling_max_freq", g.gl_pathv[i]);
        if (leer_long(ruta, &v)) {
            if (max && v != max) max_igual = FALSE;
            max = v;
        }
        snprintf(ruta, sizeof ruta, "%s/scaling_governor", g.gl_pathv[i]);
        if (leer_linea(ruta, b, sizeof b)) {
            if (gov[0] && strcmp(gov, b) != 0) gov_igual = FALSE;
            g_strlcpy(gov, b, sizeof gov);
        }
    }
    globfree(&g);
    if (n) l->ghz = (double)suma / n / 1e6;
    if (max && max_igual) l->max_khz = max;
    if (gov[0]) g_strlcpy(l->governor, gov_igual ? gov : "", sizeof l->governor);
    l->boost = hw_turbo();
}

/* Same as vigilante-laptop: max of temp*_input over CPU/GPU sensors. */
static void leer_temp(Lectura *l)
{
    glob_t g;
    l->temp = -1;
    if (glob("/sys/class/hwmon/hwmon*", 0, NULL, &g) != 0) return;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char ruta[PATH_MAX], nombre[32];
        snprintf(ruta, sizeof ruta, "%s/name", g.gl_pathv[i]);
        if (!leer_linea(ruta, nombre, sizeof nombre)) continue;
        if (strcmp(nombre, "k10temp") && strcmp(nombre, "coretemp") &&
            strcmp(nombre, "amdgpu") && strcmp(nombre, "acpitz") &&
            strcmp(nombre, "zenpower")) continue;
        glob_t t;
        snprintf(ruta, sizeof ruta, "%s/temp*_input", g.gl_pathv[i]);
        if (glob(ruta, 0, NULL, &t) == 0) {
            for (size_t j = 0; j < t.gl_pathc; j++) {
                long v;
                if (leer_long(t.gl_pathv[j], &v) && v / 1000 > l->temp) l->temp = (int)(v / 1000);
            }
            globfree(&t);
        }
    }
    globfree(&g);
}

static void leer_energia(Lectura *l)
{
    glob_t g;
    l->bat = -1; l->bat_estado[0] = '\0';
    l->cargador = hw_cargador(&l->hay_bateria, &l->hay_externa);
    if (glob("/sys/class/power_supply/BAT*", 0, NULL, &g) == 0) {
        char ruta[PATH_MAX];
        long v;
        snprintf(ruta, sizeof ruta, "%s/capacity", g.gl_pathv[0]);
        if (leer_long(ruta, &v)) l->bat = (int)v;
        snprintf(ruta, sizeof ruta, "%s/status", g.gl_pathv[0]);
        leer_linea(ruta, l->bat_estado, sizeof l->bat_estado);
        globfree(&g);
    }
}

static void leer_memoria(Lectura *l)
{
    FILE *f = fopen("/proc/meminfo", "re");
    l->ram_mb = l->swap_mb = -1;
    if (!f) return;
    char linea[128];
    long disp = -1, st = -1, sf = -1, v;
    while (fgets(linea, sizeof linea, f)) {
        if (sscanf(linea, "MemAvailable: %ld", &v) == 1) disp = v;
        else if (sscanf(linea, "SwapTotal: %ld", &v) == 1) st = v;
        else if (sscanf(linea, "SwapFree: %ld", &v) == 1) sf = v;
    }
    fclose(f);
    if (disp >= 0) l->ram_mb = disp / 1024;
    if (st >= 0 && sf >= 0) l->swap_mb = (st - sf) / 1024;
}

/*
 * Same signals as `modo-remoto estado`, without running it (its output includes the ntfy
 * topic):
 *  - remote: a systemd-inhibit --what=sleep:idle:handle-lid-switch process exists
 *  - watchdog: the vigilante-laptop.service user unit has processes
 */
static void leer_remoto(Lectura *l)
{
    l->remoto = 0; l->vigilante = 0;
    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *e;
        while (!l->remoto && (e = readdir(d))) {
            if (!isdigit((unsigned char)e->d_name[0])) continue;
            char ruta[300], buf[512];
            snprintf(ruta, sizeof ruta, "/proc/%s/cmdline", e->d_name);
            FILE *f = fopen(ruta, "re");
            if (!f) continue;
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            if (n == 0) continue;
            buf[n] = '\0';
            const char *base = strrchr(buf, '/');
            base = base ? base + 1 : buf;
            if (strcmp(base, "systemd-inhibit") != 0) continue;
            for (size_t i = 0; i < n; i++) if (buf[i] == '\0') buf[i] = ' ';
            if (strstr(buf, "--what=sleep:idle:handle-lid-switch")) l->remoto = 1;
        }
        closedir(d);
    }

    char patron[256];
    unsigned uid = (unsigned)getuid();
    snprintf(patron, sizeof patron,
             "/sys/fs/cgroup/user.slice/user-%u.slice/user@%u.service{,/*}/vigilante-laptop.service/cgroup.procs",
             uid, uid);
    glob_t g;
    if (glob(patron, GLOB_BRACE, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && !l->vigilante; i++) {
            char b[16];
            if (leer_linea(g.gl_pathv[i], b, sizeof b) && b[0]) l->vigilante = 1;
        }
        globfree(&g);
    }
}

/* The choice lives in auto-cpufreq: "# cpu-freq: X" marker in [charger] or [battery]. */
static void leer_eleccion(const Lectura *l)
{
    if (!app.con_acf) return;
    app.elegido = 0;
    FILE *f = fopen(HW_CONF_ACF, "re");
    if (!f) return;
    const char *buscada = l->cargador ? "charger" : "battery";
    char linea[256], sec[64] = "";
    while (fgets(linea, sizeof linea, f)) {
        char *p = linea;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '[') {
            char *c = strchr(p, ']');
            if (c) { *c = '\0'; g_strlcpy(sec, p + 1, sizeof sec); }
        } else if (!strcmp(sec, buscada) && !strncmp(p, HW_MARCA, strlen(HW_MARCA))) {
            p = g_strstrip(p + strlen(HW_MARCA));
            char khz[16];
            if (strchr(p, '.')) {   /* marker from version 1.1: "1.4" in GHz */
                snprintf(khz, sizeof khz, "%ld", lround(g_ascii_strtod(p, NULL) * 1e6));
                p = khz;
            }
            for (int i = 1; i < NPOS; i++) if (!strcmp(p, POS[i].arg)) app.elegido = i;
        }
    }
    fclose(f);
}

/* Does position i match what the system has? */
static gboolean coincide(int i, const Lectura *l)
{
    if (i == turbo_pos) return l->boost == 1 && l->max_khz >= tope_sin_turbo;
    return l->max_khz == POS[i].khz && (POS[i].boost < 0 || l->boost == POS[i].boost);
}

static void crear_posiciones(void)
{
    long niveles[HW_MAX_NIVEL];
    int n = hw_frecuencias(&hw) ? hw_niveles(&hw, niveles) : 0;
    gboolean turbo = hw_turbo() >= 0;

    Posicion *a = &POS[0];
    g_strlcpy(a->arg, "auto", sizeof a->arg);
    g_strlcpy(a->etiqueta, _("Auto"), sizeof a->etiqueta);
    g_strlcpy(a->nombre, _("Automatic"), sizeof a->nombre);
    g_strlcpy(a->ayuda, app.con_acf ? _("auto-cpufreq decides the limit and turbo")
                                    : _("No limit: the processor uses its full range"), sizeof a->ayuda);
    NPOS = 1;
    turbo_pos = -1;
    for (int i = 0; i < n; i++) {
        Posicion *p = &POS[NPOS++];
        snprintf(p->arg, sizeof p->arg, "%ld", niveles[i]);
        snprintf(p->etiqueta, sizeof p->etiqueta, "%.1f GHz", niveles[i] / 1e6);
        g_strlcpy(p->nombre, p->etiqueta, sizeof p->nombre);
        snprintf(p->ayuda, sizeof p->ayuda, turbo ? _("Limit %s, turbo off") : _("Limit %s"),
                 p->etiqueta);
        p->khz = niveles[i];
        p->boost = turbo ? 0 : -1;
    }
    tope_sin_turbo = n ? niveles[n - 1] : hw.max;
    if (turbo && n) {
        Posicion *p = &POS[turbo_pos = NPOS++];
        char tope[16];
        g_strlcpy(tope, POS[n].etiqueta, sizeof tope);
        g_strlcpy(p->arg, "turbo", sizeof p->arg);
        g_strlcpy(p->etiqueta, _("Turbo"), sizeof p->etiqueta);
        snprintf(p->nombre, sizeof p->nombre, _("%s + turbo"), tope);
        g_strlcpy(p->ayuda, _("No limit, turbo always on"), sizeof p->ayuda);
        p->khz = 0;
        p->boost = 1;
    }
    /* Scale: next integer above the max, +1 so the turbo zone shows */
    if (app.escala <= 0)
        app.escala = MAX(2.0, ceil(MAX(hw.max, tope_sin_turbo) / 1e6) + (turbo ? 1 : 0));
}

/* --------------------------------------------------------- Language/theme */

static void aplicar_idioma(void)
{
    if (!strcmp(aj.idioma, "sistema")) {
        if (language_original) g_setenv("LANGUAGE", language_original, TRUE);
        else g_unsetenv("LANGUAGE");
    } else {
        g_setenv("LANGUAGE", aj.idioma, TRUE);
    }
    ++_nl_msg_cat_cntr;
}

static gboolean es_oscuro_sistema(void)
{
    GtkSettings *s = gtk_settings_get_default();
    gboolean prefiere = FALSE;
    gchar *tema = NULL;
    g_object_get(s, "gtk-application-prefer-dark-theme", &prefiere, "gtk-theme-name", &tema, NULL);
    const char *env = g_getenv("GTK_THEME");
    gboolean r = prefiere || (tema && strcasestr(tema, "dark")) || (env && strcasestr(env, "dark"));
    g_free(tema);
    return r;
}

static void aplicar_css(void)
{
    const Paleta *p = app.pal = !strcmp(aj.tema, "claro")  ? &CLARO
                              : !strcmp(aj.tema, "oscuro") ? &OSCURO
                              : !strcmp(aj.tema, "amoled") ? &AMOLED
                              : es_oscuro_sistema() ? &OSCURO : &CLARO;
    gchar *css = g_strdup_printf(
        "window.cpufreq { background-color: %s; color: %s; font-family: \"Noto Sans\", \"DejaVu Sans\", sans-serif; }\n"
        ".cpufreq label { font-size: 16px; }\n"
        ".cpufreq .etiqueta { font-size: 12px; font-weight: 600; color: %s; }\n"
        ".cpufreq .suave { color: %s; }\n"
        ".cpufreq .posiciones button {"
        "  min-height: 44px; padding: 0 12px; margin: 0;"
        "  background-image: none; background-color: %s; color: %s;"
        "  border: 1px solid %s; border-radius: 0; box-shadow: none; text-shadow: none;"
        "  transition: background-color 150ms ease-out, color 150ms ease-out; }\n"
        ".cpufreq .posiciones button label { font-size: 16px; font-weight: 500; }\n"
        ".cpufreq .posiciones button:not(:first-child) { border-left-width: 0; }\n"
        ".cpufreq .posiciones button:first-child { border-radius: 8px 0 0 8px; }\n"
        ".cpufreq .posiciones button:last-child { border-radius: 0 8px 8px 0; }\n"
        ".cpufreq .posiciones button:hover { background-color: %s; }\n"
        ".cpufreq .posiciones button:checked { background-color: %s; color: %s; border-color: %s; }\n"
        ".cpufreq button:focus { outline-color: %s; outline-style: solid;"
        "  outline-width: 2px; outline-offset: -5px; -gtk-outline-radius: 4px; }\n"
        ".cpufreq .posiciones button:checked:focus { outline-color: %s; }\n"
        ".cpufreq .posiciones button:disabled { color: %s; }\n"
        ".cpufreq .fantasma { min-height: 32px; padding: 0 8px; background: none; border: none;"
        "  box-shadow: none; color: %s; border-radius: 8px; }\n"
        ".cpufreq .fantasma label { font-size: 14px; font-weight: 600; }\n"
        ".cpufreq .fantasma:hover { background-color: %s; }\n"
        ".cpufreq popover, .cpufreq popover.background { background-color: %s; color: %s;"
        "  border: 1px solid %s; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.18); }\n"
        ".cpufreq popover label { font-size: 14px; }\n"
        ".cpufreq popover combobox button { min-height: 36px; background-image: none;"
        "  background-color: %s; color: %s; border: 1px solid %s; border-radius: 8px; box-shadow: none; }\n"
        ".cpufreq .tarjeta { background-color: %s; border: 1px solid %s; border-radius: 12px;"
        "  padding: 4px 16px; }\n"
        ".cpufreq .fila { padding: 8px 0; border-bottom: 1px solid %s; }\n"
        ".cpufreq .fila.ultima { border-bottom-width: 0; }\n"
        ".cpufreq .chip { font-size: 12px; font-weight: 600; padding: 2px 8px; border-radius: 4px; }\n"
        ".cpufreq .chip.ok { background-color: %s; color: %s; }\n"
        ".cpufreq .chip.adv { background-color: %s; color: %s; }\n"
        ".cpufreq .chip.err { background-color: %s; color: %s; }\n"
        ".cpufreq .aviso { font-size: 12px; color: %s; }\n"
        ".cpufreq .aviso.err { color: %s; }\n"
        ".cpufreq .aviso.adv { color: %s; }\n"
        ".cpufreq .serie-ghz { color: %s; font-size: 12px; font-weight: 600; }\n"
        ".cpufreq .serie-temp { color: %s; font-size: 12px; font-weight: 600; }\n"
        ".cpufreq .tarjeta.historial { padding: 12px 16px; }\n",
        p->fondo, p->tinta,
        p->tinta_suave,
        p->tinta_suave,
        p->superficie, p->tinta, p->borde,
        p->acento_suave,
        p->acento, p->sobre_acento, p->acento,
        p->acento_texto,
        p->sobre_acento,
        p->tinta_suave,
        p->acento_texto,
        p->acento_suave,
        p->superficie, p->tinta, p->borde,
        p->superficie, p->tinta, p->borde,
        p->superficie, p->linea,
        p->linea,
        p->ok_suave, p->ok_tinta,
        p->adv_suave, p->adv_tinta,
        p->err_suave, p->err_tinta,
        p->tinta_suave,
        p->err, p->adv,
        p->acento_texto, p->adv);
    gtk_css_provider_load_from_data(app.css, css, -1, NULL);
    g_free(css);
    if (app.gauge) gtk_widget_queue_draw(app.gauge);
    app.bandeja_firma[0] = '\0';
}

static void rgba(cairo_t *cr, const char *hex, double alfa)
{
    GdkRGBA c;
    gdk_rgba_parse(&c, hex);
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, alfa);
}

/* ----------------------------------------------------------- Speedometer */

#define A0 (0.75 * G_PI)
#define BARRIDO (1.5 * G_PI)
static double angulo(double ghz) { return A0 + CLAMP(ghz / app.escala, 0, 1) * BARRIDO; }

static void texto(GtkWidget *w, cairo_t *cr, const char *s, int px, int peso,
                  double x, double y, const char *color, double ax)
{
    PangoLayout *ly = gtk_widget_create_pango_layout(w, s);
    PangoFontDescription *fd = pango_font_description_copy(
        pango_context_get_font_description(pango_layout_get_context(ly)));
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    pango_font_description_set_weight(fd, peso);
    pango_layout_set_font_description(ly, fd);
    PangoAttrList *at = pango_attr_list_new();
    pango_attr_list_insert(at, pango_attr_font_features_new("tnum 1"));
    pango_layout_set_attributes(ly, at);
    int lw, lh;
    pango_layout_get_pixel_size(ly, &lw, &lh);
    rgba(cr, color, 1);
    cairo_move_to(cr, x - lw * ax, y - lh / 2.0);
    pango_cairo_show_layout(cr, ly);
    pango_attr_list_unref(at);
    pango_font_description_free(fd);
    g_object_unref(ly);
}

static gboolean dibujar(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    const Paleta *p = app.pal;
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double r = MIN(W / 2 - 32, H * 0.5 - 8), cx = W / 2, cy = r + 16;
    const double grosor = 14;

    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, grosor);

    /* track and turbo zone (above the top without turbo) */
    rgba(cr, p->linea, 1);
    cairo_arc(cr, cx, cy, r, A0, A0 + BARRIDO);
    cairo_stroke(cr);
    if (turbo_pos >= 0) {
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
        rgba(cr, p->acento_suave, 1);
        cairo_arc(cr, cx, cy, r, angulo(tope_sin_turbo / 1e6), A0 + BARRIDO - 0.02);
        cairo_stroke(cr);
    }

    /* value */
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    rgba(cr, p->acento, 1);
    cairo_arc(cr, cx, cy, r, A0, angulo(app.mostrado));
    cairo_stroke(cr);

    /* ticks every 0.5 GHz, numbers on whole GHz */
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    for (int i = 0; i <= (int)(app.escala * 2); i++) {
        double a = angulo(i / 2.0), largo = (i % 2) ? 4 : 8;
        double r1 = r - grosor / 2 - 4, r2 = r1 - largo;
        cairo_set_line_width(cr, (i % 2) ? 1 : 2);
        rgba(cr, p->tinta_suave, 1);
        cairo_move_to(cr, cx + r1 * cos(a), cy + r1 * sin(a));
        cairo_line_to(cr, cx + r2 * cos(a), cy + r2 * sin(a));
        cairo_stroke(cr);
        if (i % 2 == 0) {
            char n[12];
            snprintf(n, sizeof n, "%d", i / 2);
            double rt = r2 - 12;
            texto(w, cr, n, 12, PANGO_WEIGHT_NORMAL, cx + rt * cos(a), cy + rt * sin(a) + 1,
                  p->tinta_suave, 0.5);
        }
    }

    /* current limit (without turbo): short mark across the track */
    if (app.l.max_khz > 0 && app.l.boost != 1) {
        double a = angulo(app.l.max_khz / 1e6);
        cairo_set_line_width(cr, 3);
        rgba(cr, p->tinta, 1);
        cairo_move_to(cr, cx + (r + grosor / 2 + 3) * cos(a), cy + (r + grosor / 2 + 3) * sin(a));
        cairo_line_to(cr, cx + (r - grosor / 2 - 1) * cos(a), cy + (r - grosor / 2 - 1) * sin(a));
        cairo_stroke(cr);
    }

    /* needle */
    double a = angulo(app.mostrado), ra = r - grosor - 26;
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, 3);
    rgba(cr, p->tinta, 1);
    cairo_move_to(cr, cx, cy);
    cairo_line_to(cr, cx + ra * cos(a), cy + ra * sin(a));
    cairo_stroke(cr);
    cairo_arc(cr, cx, cy, 6, 0, 2 * G_PI);
    cairo_fill(cr);

    /* reading */
    char num[16];
    snprintf(num, sizeof num, "%.2f", app.l.ghz);
    texto(w, cr, num, 40, PANGO_WEIGHT_BOLD, cx, cy + r * 0.52, p->tinta, 0.5);
    texto(w, cr, "GHz", 16, PANGO_WEIGHT_NORMAL, cx, cy + r * 0.52 + 30, p->tinta_suave, 0.5);
    return FALSE;
}

static gboolean animar(GtkWidget *w, GdkFrameClock *reloj, gpointer d)
{
    (void)d;
    double t = (double)(gdk_frame_clock_get_frame_time(reloj) - app.anim_inicio) / ANIM_US;
    if (t >= 1) {
        app.mostrado = app.hacia;
        gtk_widget_queue_draw(w);
        app.anim_id = 0;
        return G_SOURCE_REMOVE;
    }
    double e = 1 - pow(1 - t, 3);
    app.mostrado = app.desde + (app.hacia - app.desde) * e;
    gtk_widget_queue_draw(w);
    return G_SOURCE_CONTINUE;
}

static void mover_aguja(double ghz)
{
    if (ghz > app.escala) app.escala = ceil(ghz);   /* never goes past the dial */
    gboolean anim = FALSE;
    g_object_get(gtk_settings_get_default(), "gtk-enable-animations", &anim, NULL);
    if (!anim || app.primera || !gtk_widget_get_mapped(app.gauge)) {
        app.mostrado = app.hacia = ghz;
        gtk_widget_queue_draw(app.gauge);
        return;
    }
    app.desde = app.mostrado;
    app.hacia = ghz;
    app.anim_inicio = gdk_frame_clock_get_frame_time(gtk_widget_get_frame_clock(app.gauge));
    if (!app.anim_id) app.anim_id = gtk_widget_add_tick_callback(app.gauge, animar, NULL, NULL);
}

/* ------------------------------------------------------------------- Tray */

/*
 * Two tray backends, picked at runtime:
 *  - XEmbed (GtkStatusIcon): XFCE, MATE, Cinnamon... Left click toggles the window,
 *    right click opens the menu.
 *  - StatusNotifierItem through libayatana-appindicator3, loaded with dlopen only when
 *    needed (KDE Plasma, GNOME with the AppIndicator extension, Wayland sessions). The
 *    icon is a PNG redrawn in $XDG_RUNTIME_DIR/cpu-freq; every click opens the menu,
 *    which therefore also has "Show or hide the window".
 * XEmbed is used when an XEmbed tray exists; otherwise StatusNotifierItem if a watcher
 * is running. CPU_FREQ_BANDEJA=xembed|sni forces one.
 */

typedef struct _AppIndicator AppIndicator;   /* opaque: only used through dlsym'd functions */
enum { AI_CATEGORY_HARDWARE = 3, AI_STATUS_PASSIVE = 0, AI_STATUS_ACTIVE = 1 };
static struct {
    void *lib;
    AppIndicator *(*nuevo)(const char *, const char *, int);
    void (*estado)(AppIndicator *, int);
    void (*menu)(AppIndicator *, GtkMenu *);
    void (*ruta_tema)(AppIndicator *, const char *);
    void (*icono)(AppIndicator *, const char *, const char *);
    void (*titulo)(AppIndicator *, const char *);
    AppIndicator *ind;
    GtkWidget *menu_actual;
    char firma_menu[32];
    int nombre;                  /* alternates the PNG name so hosts reload it */
} sni;

static gboolean cargar_sni(void)
{
    if (sni.lib) return TRUE;
    const char *libs[] = { "libayatana-appindicator3.so.1", "libappindicator3.so.1" };
    for (size_t i = 0; i < G_N_ELEMENTS(libs) && !sni.lib; i++)
        sni.lib = dlopen(libs[i], RTLD_LAZY | RTLD_LOCAL);
    if (!sni.lib) return FALSE;
    *(void **)&sni.nuevo = dlsym(sni.lib, "app_indicator_new");
    *(void **)&sni.estado = dlsym(sni.lib, "app_indicator_set_status");
    *(void **)&sni.menu = dlsym(sni.lib, "app_indicator_set_menu");
    *(void **)&sni.ruta_tema = dlsym(sni.lib, "app_indicator_set_icon_theme_path");
    *(void **)&sni.icono = dlsym(sni.lib, "app_indicator_set_icon_full");
    *(void **)&sni.titulo = dlsym(sni.lib, "app_indicator_set_title");
    if (!sni.nuevo || !sni.estado || !sni.menu || !sni.ruta_tema || !sni.icono) {
        dlclose(sni.lib);
        sni.lib = NULL;
        return FALSE;
    }
    return TRUE;
}

/* Does the X11 screen have an XEmbed tray (selection _NET_SYSTEM_TRAY_Sn owned)? */
static gboolean hay_xembed(void)
{
#ifdef GDK_WINDOWING_X11
    GdkDisplay *d = gdk_display_get_default();
    if (d && GDK_IS_X11_DISPLAY(d)) {
        Display *x = GDK_DISPLAY_XDISPLAY(d);
        char n[32];
        snprintf(n, sizeof n, "_NET_SYSTEM_TRAY_S%d", DefaultScreen(x));
        return XGetSelectionOwner(x, XInternAtom(x, n, False)) != None;
    }
#endif
    return FALSE;
}

/* Is a StatusNotifierWatcher (KDE, GNOME extension, XFCE systray...) on the session bus? */
static gboolean hay_sni(void)
{
    GDBusConnection *c = g_application_get_dbus_connection(G_APPLICATION(app.gapp));
    if (!c) return FALSE;
    GVariant *r = g_dbus_connection_call_sync(c, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                              "org.freedesktop.DBus", "NameHasOwner",
                                              g_variant_new("(s)", "org.kde.StatusNotifierWatcher"),
                                              G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
    gboolean hay = FALSE;
    if (r) { g_variant_get(r, "(b)", &hay); g_variant_unref(r); }
    return hay;
}

static gboolean usar_sni(void)
{
    const char *e = g_getenv("CPU_FREQ_BANDEJA");
    if (e && !strcmp(e, "xembed")) return FALSE;
    if (e && !strcmp(e, "sni")) return cargar_sni();
    return !hay_xembed() && hay_sni() && cargar_sni();
}

static gboolean bandeja_activa(void) { return app.bandeja || sni.ind; }

static const char *color_estado(const Lectura *l)
{
    if (l->temp >= umbral.temp_critica) return app.pal->err;
    if (l->temp >= umbral.temp_aviso) return app.pal->adv;
    return app.pal->acento;
}

/* Mini speedometer: ring with the value and a needle, colored by temperature. */
static cairo_surface_t *dibujar_icono(int s, const Lectura *l)
{
    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, s, s);
    cairo_t *cr = cairo_create(sf);
    double cx = s / 2.0, cy = s / 2.0 + s * 0.06, r = s * 0.40, g = MAX(2.0, s * 0.14);
    cairo_set_line_width(cr, g);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.45);
    cairo_arc(cr, cx, cy, r, A0, A0 + BARRIDO);
    cairo_stroke(cr);
    rgba(cr, color_estado(l), 1);
    cairo_arc(cr, cx, cy, r, A0, angulo(l->ghz));
    cairo_stroke(cr);
    double a = angulo(l->ghz);
    cairo_set_line_width(cr, MAX(1.5, s * 0.09));
    cairo_move_to(cr, cx, cy);
    cairo_line_to(cr, cx + r * 0.7 * cos(a), cy + r * 0.7 * sin(a));
    cairo_stroke(cr);
    cairo_arc(cr, cx, cy, MAX(1.5, s * 0.08), 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_destroy(cr);
    return sf;
}

static void texto_bandeja(char *t, size_t n, const Lectura *l)
{
    int k = snprintf(t, n, "%.2f GHz", l->ghz);
    if (l->boost == 1) k += snprintf(t + k, n - (size_t)k, " · %s", _("Turbo"));
    if (l->temp >= 0) snprintf(t + k, n - (size_t)k, " · %d °C", l->temp);
}

static void mostrar_ventana(void)
{
    gtk_widget_show(app.ventana);
    gtk_window_present(GTK_WINDOW(app.ventana));
}

/*
 * Click on the tray icon or the panel plugin: if the window is in front it is hidden
 * (tray mode) or closed; otherwise it is shown. Clicking the panel takes the focus away
 * from the window just before, so "in front" includes having lost it < 0.5 s ago.
 */
static gint64 foco_perdido;

static gboolean al_perder_foco(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    foco_perdido = g_get_monotonic_time();
    return FALSE;
}

static void alternar(void)
{
    gboolean al_frente = gtk_widget_get_visible(app.ventana) &&
        (gtk_window_is_active(GTK_WINDOW(app.ventana)) ||
         g_get_monotonic_time() - foco_perdido < G_USEC_PER_SEC / 2);
    if (!al_frente) {
        mostrar_ventana();
    } else if (bandeja_activa()) {
        gtk_widget_hide(app.ventana);
        malloc_trim(0);
    } else {
        g_application_quit(G_APPLICATION(app.gapp));
    }
}

/* SNI hosts focus their own menu first, so "in front" is just "visible" there. */
static void sni_mostrar_ocultar(void)
{
    if (gtk_widget_get_visible(app.ventana)) { gtk_widget_hide(app.ventana); malloc_trim(0); }
    else mostrar_ventana();
}

static void aplicar(int pos);

static void menu_posicion(GtkCheckMenuItem *it, gpointer d)
{
    if (gtk_check_menu_item_get_active(it)) aplicar(GPOINTER_TO_INT(d));
}

/* Position currently chosen (re-read: the window may be hidden and not refreshing). */
static int posicion_marcada(void)
{
    leer_energia(&app.l);
    leer_eleccion(&app.l);
    app.real = -1;
    for (int k = 1; k < NPOS; k++) if (coincide(k, &app.l)) app.real = k;
    return app.con_acf ? app.elegido : app.real;
}

/* Tray menu: [Show or hide], "Speed limit", Auto / levels / Turbo, Open (XEmbed), Quit. */
static GtkWidget *crear_menu_bandeja(gboolean con_mostrar, int marcado)
{
    GtkWidget *m = gtk_menu_new();
    if (con_mostrar) {
        GtkWidget *mo = gtk_menu_item_new_with_label(_("Show or hide the window"));
        g_signal_connect_swapped(mo, "activate", G_CALLBACK(sni_mostrar_ocultar), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(m), mo);
        gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    }
    GtkWidget *tit = gtk_menu_item_new_with_label(_("Speed limit"));
    gtk_widget_set_sensitive(tit, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), tit);
    GSList *grupo = NULL;
    for (int k = 0; k < NPOS; k++) {
        GtkWidget *it = gtk_radio_menu_item_new_with_label(grupo, k == turbo_pos ? POS[k].nombre : POS[k].etiqueta);
        grupo = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(it));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), k == marcado);
        gtk_widget_set_sensitive(it, !app.aplicando);
        gtk_widget_set_tooltip_text(it, POS[k].ayuda);
        g_signal_connect(it, "toggled", G_CALLBACK(menu_posicion), GINT_TO_POINTER(k));
        gtk_menu_shell_append(GTK_MENU_SHELL(m), it);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    if (!con_mostrar) {
        GtkWidget *abrir = gtk_menu_item_new_with_label(_("Open"));
        g_signal_connect_swapped(abrir, "activate", G_CALLBACK(mostrar_ventana), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(m), abrir);
    }
    GtkWidget *salir = gtk_menu_item_new_with_label(_("Quit"));
    g_signal_connect_swapped(salir, "activate", G_CALLBACK(g_application_quit), app.gapp);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), salir);
    gtk_widget_show_all(m);
    return m;
}

/* Called on every refresh (also with the window hidden). */
static void actualizar_bandeja(void)
{
    const Lectura *l = &app.l;
    char firma[48], t[128];
    texto_bandeja(t, sizeof t, l);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    if (app.bandeja) {
        int s = gtk_status_icon_get_size(app.bandeja);
        if (s < 16) s = 22;
        snprintf(firma, sizeof firma, "%d|%.1f|%s", s, l->ghz, color_estado(l));
        if (strcmp(firma, app.bandeja_firma) != 0) {
            g_strlcpy(app.bandeja_firma, firma, sizeof app.bandeja_firma);
            cairo_surface_t *sf = dibujar_icono(s, l);
            GdkPixbuf *pb = gdk_pixbuf_get_from_surface(sf, 0, 0, s, s);
            gtk_status_icon_set_from_pixbuf(app.bandeja, pb);
            g_object_unref(pb);
            cairo_surface_destroy(sf);
        }
        gtk_status_icon_set_tooltip_text(app.bandeja, t);
    }
    G_GNUC_END_IGNORE_DEPRECATIONS

    if (sni.ind) {
        snprintf(firma, sizeof firma, "%.1f|%s", l->ghz, color_estado(l));
        if (strcmp(firma, app.bandeja_firma) != 0) {
            g_strlcpy(app.bandeja_firma, firma, sizeof app.bandeja_firma);
            gchar *dir = g_build_filename(g_get_user_runtime_dir(), "cpu-freq", NULL);
            char nombre[32];
            snprintf(nombre, sizeof nombre, "cpu-freq-tray-%d", sni.nombre ^= 1);
            gchar *png = g_strdup_printf("%s/%s.png", dir, nombre);
            cairo_surface_t *sf = dibujar_icono(64, l);
            if (cairo_surface_write_to_png(sf, png) == CAIRO_STATUS_SUCCESS)
                sni.icono(sni.ind, nombre, t);
            cairo_surface_destroy(sf);
            g_free(png);
            g_free(dir);
        }
        if (sni.titulo) sni.titulo(sni.ind, t);
        /* The menu lives on the host side: rebuild it when the choice changes */
        int marcado = posicion_marcada();
        snprintf(firma, sizeof firma, "%d|%d|%d", marcado, app.aplicando, NPOS);
        if (strcmp(firma, sni.firma_menu) != 0) {
            g_strlcpy(sni.firma_menu, firma, sizeof sni.firma_menu);
            GtkWidget *viejo = sni.menu_actual;
            sni.menu_actual = g_object_ref_sink(crear_menu_bandeja(TRUE, marcado));
            sni.menu(sni.ind, GTK_MENU(sni.menu_actual));
            if (viejo) { gtk_widget_destroy(viejo); g_object_unref(viejo); }
        }
    }
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS   /* GtkStatusIcon is deprecated but is what XEmbed trays use */

static void bandeja_clic(GtkStatusIcon *i, gpointer d)
{
    (void)i; (void)d;
    alternar();
}

static void bandeja_menu(GtkStatusIcon *i, guint boton, guint tiempo, gpointer d)
{
    (void)d;
    GtkWidget *m = crear_menu_bandeja(FALSE, posicion_marcada());
    g_signal_connect(m, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup(GTK_MENU(m), NULL, NULL, gtk_status_icon_position_menu, i, boton, tiempo);
}

static void poner_bandeja(gboolean si)
{
    if (si && !bandeja_activa()) {
        app.bandeja_firma[0] = '\0';
        if (usar_sni()) {
            gchar *dir = g_build_filename(g_get_user_runtime_dir(), "cpu-freq", NULL);
            g_mkdir_with_parents(dir, 0700);
            sni.ind = sni.nuevo(APP_ID, "cpu-freq", AI_CATEGORY_HARDWARE);
            sni.ruta_tema(sni.ind, dir);
            g_free(dir);
            sni.firma_menu[0] = '\0';
            actualizar_bandeja();             /* draws the icon and sets the menu */
            sni.estado(sni.ind, AI_STATUS_ACTIVE);
        } else {
            app.bandeja = gtk_status_icon_new();
            gtk_status_icon_set_title(app.bandeja, _("CPU Speed"));
            g_signal_connect(app.bandeja, "activate", G_CALLBACK(bandeja_clic), NULL);
            g_signal_connect(app.bandeja, "popup-menu", G_CALLBACK(bandeja_menu), NULL);
            g_signal_connect_swapped(app.bandeja, "size-changed", G_CALLBACK(actualizar_bandeja), NULL);
            actualizar_bandeja();
        }
        g_application_hold(G_APPLICATION(app.gapp));
    } else if (!si && bandeja_activa()) {
        if (app.bandeja) {
            g_object_unref(app.bandeja);
            app.bandeja = NULL;
        } else {
            sni.estado(sni.ind, AI_STATUS_PASSIVE);
            g_object_unref(sni.ind);
            sni.ind = NULL;
            if (sni.menu_actual) { gtk_widget_destroy(sni.menu_actual); g_object_unref(sni.menu_actual); sni.menu_actual = NULL; }
        }
        g_application_release(G_APPLICATION(app.gapp));
    }
}

G_GNUC_END_IGNORE_DEPRECATIONS

/* Start with the session: tray mode only. */
static void poner_autoarranque(gboolean si)
{
    gchar *dir = g_build_filename(g_get_user_config_dir(), "autostart", NULL);
    gchar *ruta = g_build_filename(dir, "cpu-freq.desktop", NULL);
    if (si) {
        g_mkdir_with_parents(dir, 0700);
        gchar *t = g_strdup_printf("[Desktop Entry]\nType=Application\nName=%s\n"
                                   "Exec=cpu-freq --bandeja\nIcon=cpu-freq\nTerminal=false\n"
                                   "NoDisplay=true\nX-GNOME-Autostart-enabled=true\n",
                                   _("CPU Speed"));
        g_file_set_contents(ruta, t, -1, NULL);
        g_free(t);
    } else {
        g_unlink(ruta);
    }
    g_free(ruta);
    g_free(dir);
}

/* ----------------------------------------------------------------- Update */

/* ---------------------------------------------------------------- History */

/* Last 5 minutes: 200 samples × 1.5 s. Also recorded while the window is hidden. */
#define HIST 200
#define TEMP_MIN 20.0
#define TEMP_MAX 100.0
static struct { float ghz[HIST], temp[HIST]; int n, pos; } hist;

static void registrar(const Lectura *l)
{
    hist.ghz[hist.pos] = (float)l->ghz;
    hist.temp[hist.pos] = (float)l->temp;
    hist.pos = (hist.pos + 1) % HIST;
    if (hist.n < HIST) hist.n++;
}

/* i = 0 is the oldest sample available */
static int hist_idx(int i) { return (hist.pos - hist.n + i + HIST) % HIST; }

static void trazo(cairo_t *cr, double W, double H, const float *v, double min, double max, gboolean cerrar)
{
    double paso = W / (HIST - 1), x0 = W - (hist.n - 1) * paso;
    for (int i = 0; i < hist.n; i++) {
        double y = H - CLAMP((v[hist_idx(i)] - min) / (max - min), 0, 1) * H;
        if (i == 0) cairo_move_to(cr, x0, y);
        else cairo_line_to(cr, x0 + i * paso, y);
    }
    if (cerrar) {
        cairo_line_to(cr, W, H);
        cairo_line_to(cr, x0, H);
        cairo_close_path(cr);
    }
}

static void hist_resumen(double *ghz, double *temp, double *pico);

static gboolean dibujar_grafica(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    const Paleta *p = app.pal;
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w) - 2;
    cairo_translate(cr, 0, 1);

    /* guides: bottom, middle and top of the GHz scale */
    cairo_set_line_width(cr, 1);
    rgba(cr, p->linea, 1);
    for (int i = 0; i <= 2; i++) {
        double y = floor(H * i / 2.0) + 0.5;
        cairo_move_to(cr, 0, y);
        cairo_line_to(cr, W, y);
    }
    cairo_stroke(cr);
    if (hist.n < 2) return FALSE;

    /* frequency: soft fill and accent line */
    trazo(cr, W, H, hist.ghz, 0, app.escala, TRUE);
    rgba(cr, p->acento, 0.14);
    cairo_fill(cr);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, 2);
    trazo(cr, W, H, hist.ghz, 0, app.escala, FALSE);
    rgba(cr, p->acento, 1);
    cairo_stroke(cr);

    /* average frequency: dashed line */
    double g, t, pico;
    hist_resumen(&g, &t, &pico);
    double ym = floor(H - CLAMP(g / app.escala, 0, 1) * H) + 0.5, guion[] = { 4, 3 };
    cairo_set_dash(cr, guion, 2, 0);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, 0, ym);
    cairo_line_to(cr, W, ym);
    rgba(cr, p->acento_texto, 0.8);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0);

    /* temperature (20–100 °C), only with a sensor */
    if (app.l.temp >= 0) {
        cairo_set_line_width(cr, 1.5);
        trazo(cr, W, H, hist.temp, TEMP_MIN, TEMP_MAX, FALSE);
        rgba(cr, p->adv, 1);
        cairo_stroke(cr);
    }
    return FALSE;
}

/* Average frequency and average/peak temperature over the history */
static void hist_resumen(double *ghz, double *temp, double *pico)
{
    double sg = 0, st = 0;
    int nt = 0;
    *pico = -1;
    for (int i = 0; i < hist.n; i++) {
        int k = hist_idx(i);
        sg += hist.ghz[k];
        if (hist.temp[k] >= 0) { st += hist.temp[k]; nt++; if (hist.temp[k] > *pico) *pico = hist.temp[k]; }
    }
    *ghz = hist.n ? sg / hist.n : 0;
    *temp = nt ? st / nt : -1;
}

/* The legend summarizes the window (averages and peak); the current value is already above. */
static void actualizar_grafica(void)
{
    if (!app.grafica) return;
    double g, t, pico;
    hist_resumen(&g, &t, &pico);
    char b[64];
    snprintf(b, sizeof b, _("avg %.2f GHz"), g);
    gtk_label_set_text(GTK_LABEL(app.ley_ghz), b);
    if (t >= 0) snprintf(b, sizeof b, _("avg %.0f °C · peak %.0f"), t, pico);
    gtk_label_set_text(GTK_LABEL(app.ley_temp), t >= 0 ? b : "");
    gtk_widget_queue_draw(app.grafica);
}

static void poner_chip(int fila, const char *txt, const char *clase)
{
    GtkWidget *c = app.chip[fila];
    GtkStyleContext *sc = gtk_widget_get_style_context(c);
    gtk_style_context_remove_class(sc, "ok");
    gtk_style_context_remove_class(sc, "adv");
    gtk_style_context_remove_class(sc, "err");
    if (!txt) { gtk_widget_hide(c); return; }
    gtk_style_context_add_class(sc, clase);
    gtk_label_set_text(GTK_LABEL(c), txt);
    gtk_widget_show(c);
}

static void poner_valor(int fila, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void poner_valor(int fila, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(app.valor[fila]), s);
    g_free(s);
}

static const char *nombre_governor(const char *g)
{
    static const struct { const char *k, *v; } t[] = {
        { "performance", N_("Performance") }, { "powersave", N_("Power saving") },
        { "schedutil", N_("Adaptive") },     { "ondemand", N_("On demand") },
        { "conservative", N_("Conservative") }, { "userspace", N_("Manual") },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(t); i++) if (!strcmp(g, t[i].k)) return _(t[i].v);
    return NULL;
}

static const char *estado_bateria(const char *s)
{
    if (!strcmp(s, "Charging")) return _("Charging");
    if (!strcmp(s, "Discharging")) return _("Discharging");
    if (!strcmp(s, "Full")) return _("Full");
    if (!strcmp(s, "Not charging")) return _("Not charging");
    return s[0] ? s : NULL;
}

static void texto_ram(char *b, size_t n, long mb)
{
    if (mb >= 1024) snprintf(b, n, "%.1f GB", mb / 1024.0);
    else snprintf(b, n, "%ld MB", mb);
}

static void actualizar_aviso(void)
{
    GtkStyleContext *sc = gtk_widget_get_style_context(app.aviso);
    gtk_style_context_remove_class(sc, "err");
    gtk_style_context_remove_class(sc, "adv");
    const Lectura *l = &app.l;
    char real[96], b[200];

    if (app.real >= 1) g_strlcpy(real, POS[app.real].nombre, sizeof real);
    else if (l->max_khz > 0 && l->boost >= 0)
        snprintf(real, sizeof real, l->boost == 1 ? _("%.1f GHz, turbo on")
                                                  : _("%.1f GHz, turbo off"), l->max_khz / 1e6);
    else if (l->max_khz > 0)
        snprintf(real, sizeof real, "%.1f GHz", l->max_khz / 1e6);
    else g_strlcpy(real, _("different limits per core"), sizeof real);

    if (app.error[0] && g_get_monotonic_time() < app.error_hasta) {
        g_strlcpy(b, app.error, sizeof b);
        gtk_style_context_add_class(sc, "err");
    } else if (app.aplicando) {
        g_strlcpy(b, _("Applying…"), sizeof b);
    } else if (app.elegido >= 1 && app.real != app.elegido && app.discrepa >= 2) {
        snprintf(b, sizeof b, _("The system is not keeping your choice: now %s."), real);
        gtk_style_context_add_class(sc, "adv");
    } else if (app.elegido == 0 && app.con_acf) {
        snprintf(b, sizeof b, _("auto-cpufreq decides · now %s."), real);
    } else {
        snprintf(b, sizeof b, _("Now: %s."), real);
    }
    if (!app.error[0] || g_get_monotonic_time() >= app.error_hasta) app.error[0] = '\0';
    gtk_label_set_text(GTK_LABEL(app.aviso), b);
}

static void actualizar_botones(void)
{
    app.actualizando = TRUE;
    /* With auto-cpufreq the choice is marked (the line below states the real state);
       without it, the real state. */
    int marcado = app.con_acf ? app.elegido : app.real;
    for (int i = 0; i < NPOS; i++) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.botones[i]), i == marcado);
        gtk_widget_set_sensitive(app.botones[i], !app.aplicando);
    }
    app.actualizando = FALSE;
}

static gboolean refrescar(gpointer d)
{
    (void)d;
    Lectura *l = &app.l;
    leer_cpu(l);
    leer_temp(l);
    registrar(l);
    actualizar_bandeja();

    /* With the window hidden (tray mode) only what the icon shows is read */
    if (!app.contenido || !gtk_widget_get_visible(app.ventana)) return G_SOURCE_CONTINUE;

    leer_energia(l);
    leer_memoria(l);
    if (app.fila_fija[F_REMOTO] || app.fila_fija[F_VIG]) leer_remoto(l);

    leer_eleccion(l);
    app.real = -1;
    for (int i = 1; i < NPOS; i++)
        if (coincide(i, l)) app.real = i;
    app.discrepa = (app.elegido >= 1 && app.real != app.elegido) ? app.discrepa + 1 : 0;

    gboolean portatil = l->hay_bateria || l->hay_externa;
    gchar *tit = g_utf8_strup(!app.con_acf || !portatil ? _("Speed limit")
                              : l->cargador ? _("Speed limit · on charger")
                                            : _("Speed limit · on battery"), -1);
    gtk_label_set_text(GTK_LABEL(app.titulo_lim), tit);
    g_free(tit);

    /* Hardware-dependent rows: battery and charger only on laptops */
    gtk_widget_set_visible(app.fila[F_BAT], portatil);
    gtk_widget_set_visible(app.fila[F_CARG], portatil);
    gtk_widget_set_visible(app.fila[F_REMOTO], app.fila_fija[F_REMOTO]);
    gtk_widget_set_visible(app.fila[F_VIG], app.fila_fija[F_VIG]);

    mover_aguja(l->ghz);
    actualizar_grafica();

    const char *gn = nombre_governor(l->governor);
    gchar *modo = !l->governor[0] ? g_strdup_printf(_("Mode: %s"), _("mixed"))
                : gn ? g_strdup_printf(_("Mode: %s (%s)"), gn, l->governor)
                     : g_strdup_printf(_("Mode: %s"), l->governor);
    gtk_label_set_text(GTK_LABEL(app.modo), modo);
    g_free(modo);

    if (l->temp >= 0) {
        poner_valor(F_TEMP, "%d °C", l->temp);
        if (l->temp >= umbral.temp_critica) poner_chip(F_TEMP, _("Critical"), "err");
        else if (l->temp >= umbral.temp_aviso) poner_chip(F_TEMP, _("High"), "adv");
        else poner_chip(F_TEMP, _("Normal"), "ok");
    } else {
        poner_valor(F_TEMP, "%s", _("No sensor"));
        poner_chip(F_TEMP, NULL, NULL);
    }

    if (l->bat >= 0) {
        poner_valor(F_BAT, "%d %%", l->bat);
        const char *e = estado_bateria(l->bat_estado);
        gboolean descarga = !strcmp(l->bat_estado, "Discharging");
        if (descarga && l->bat <= umbral.bateria_baja) poner_chip(F_BAT, _("Low"), "err");
        else poner_chip(F_BAT, e, descarga ? "adv" : "ok");
    } else {
        poner_valor(F_BAT, "%s", _("Not detected"));
        poner_chip(F_BAT, NULL, NULL);
    }

    poner_valor(F_CARG, "%s", l->cargador ? _("Connected") : _("Disconnected"));
    poner_chip(F_CARG, l->cargador ? NULL : _("On battery"), "adv");

    if (l->ram_mb >= 0) {
        char b[32];
        texto_ram(b, sizeof b, l->ram_mb);
        poner_valor(F_RAM, "%s", b);
        if (l->ram_mb < umbral.ram_minima) poner_chip(F_RAM, _("Very low"), "err");
        else if (l->ram_mb < umbral.ram_minima * 2) poner_chip(F_RAM, _("Tight"), "adv");
        else poner_chip(F_RAM, NULL, NULL);
    }
    if (l->swap_mb >= 0) {
        char b[32];
        texto_ram(b, sizeof b, l->swap_mb);
        poner_valor(F_SWAP, "%s", b);
    }

    poner_valor(F_REMOTO, "%s", l->remoto ? _("Active") : _("Off"));
    poner_chip(F_REMOTO, l->remoto ? _("No sleep") : NULL, "ok");
    poner_valor(F_VIG, "%s", l->vigilante ? _("Active") : _("Stopped"));
    poner_chip(F_VIG, l->vigilante ? NULL : _("No alerts"), "adv");

    /* The last visible row has no bottom line */
    for (int i = N_FILAS - 1, ultima = 1; i >= 0; i--) {
        GtkStyleContext *sc = gtk_widget_get_style_context(app.fila[i]);
        if (ultima && gtk_widget_get_visible(app.fila[i])) { gtk_style_context_add_class(sc, "ultima"); ultima = 0; }
        else gtk_style_context_remove_class(sc, "ultima");
    }

    actualizar_botones();
    actualizar_aviso();
    app.primera = FALSE;
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ Apply */

static void ayudante_termino(GPid pid, gint estado, gpointer d)
{
    int pos = GPOINTER_TO_INT(d);
    g_spawn_close_pid(pid);
    app.aplicando = FALSE;
    app.error[0] = '\0';
    if (WIFEXITED(estado) && WEXITSTATUS(estado) == 0) {
        if (!app.con_acf) app.elegido = pos;   /* with auto-cpufreq it is re-read from its file */
        app.discrepa = 0;
    } else {
        int c = WIFEXITED(estado) ? WEXITSTATUS(estado) : -1;
        if (c == 126 || c == 127)
            g_strlcpy(app.error, _("Not applied: polkit denied permission."), sizeof app.error);
        else
            snprintf(app.error, sizeof app.error, _("Could not apply (code %d)."), c);
        app.error_hasta = g_get_monotonic_time() + MSG_ERROR_S * G_USEC_PER_SEC;
    }
    refrescar(NULL);
}

/* Applies position pos through the helper (buttons and tray menu). */
static void aplicar(int pos)
{
    if (app.aplicando || pos < 0 || pos >= NPOS) return;

    if (!g_file_test(AYUDANTE, G_FILE_TEST_IS_EXECUTABLE)) {
        g_strlcpy(app.error, _("The helper is missing: install the cpu-freq package."), sizeof app.error);
        app.error_hasta = g_get_monotonic_time() + MSG_ERROR_S * G_USEC_PER_SEC;
        actualizar_botones();
        actualizar_aviso();
        return;
    }

    gchar *argv[] = { "pkexec", AYUDANTE, POS[pos].arg, NULL };
    GPid pid;
    GError *err = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                       G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, &pid, &err)) {
        snprintf(app.error, sizeof app.error, _("Could not run pkexec: %s"), err->message);
        app.error_hasta = g_get_monotonic_time() + MSG_ERROR_S * G_USEC_PER_SEC;
        g_error_free(err);
    } else {
        app.aplicando = TRUE;
        g_child_watch_add(pid, ayudante_termino, GINT_TO_POINTER(pos));
    }
    actualizar_botones();
    actualizar_aviso();
}

static void al_pulsar(GtkToggleButton *b, gpointer d)
{
    (void)b;
    if (app.actualizando) return;
    aplicar(GPOINTER_TO_INT(d));
}

/* ----------------------------------------------------------------- Window */

static void construir_contenido(void);

static GtkWidget *etiqueta(const char *s, const char *clase)
{
    GtkWidget *l = gtk_label_new(s);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (clase) gtk_style_context_add_class(gtk_widget_get_style_context(l), clase);
    return l;
}

static gboolean reconstruir(gpointer d)
{
    (void)d;
    construir_contenido();
    refrescar(NULL);
    return G_SOURCE_REMOVE;
}

static void cambiar_barra(const char *id)
{
    g_strlcpy(aj.barra, id, sizeof aj.barra);
    gboolean bandeja = !strcmp(id, "bandeja");
    poner_bandeja(bandeja);
    poner_autoarranque(bandeja);
    const char *nota = NULL;
    if (!strcmp(id, "panel")) {
        if (g_file_test(PLUGIN_PANEL, G_FILE_TEST_EXISTS)) {
            /* Never added twice. A panel started before the plugin was installed does not
               know it yet: in that case the panel is restarted and it is added again. */
            gchar *argv[] = { "/bin/sh", "-c",
                "esta() { xfconf-query -c xfce4-panel -p /plugins -lv | grep -q ' cpu-freq-panel$'; }; "
                "esta && exit 0; xfce4-panel --add=cpu-freq-panel; sleep 2; esta && exit 0; "
                "xfce4-panel -r; sleep 3; xfce4-panel --add=cpu-freq-panel", NULL };
            g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
            nota = _("The XFCE panel opened so you can add the plugin.");
        } else {
            nota = _("Install the cpu-freq-xfce package to use the panel.");
        }
    } else if (bandeja) {
        nota = _("When you close the window, the app stays in the tray.");
    }
    gtk_label_set_text(GTK_LABEL(app.nota_ajustes), nota ? nota : "");
    gtk_widget_set_visible(app.nota_ajustes, nota != NULL);
}

static void al_cambiar_ajuste(GtkComboBox *c, gpointer d)
{
    const char *id = gtk_combo_box_get_active_id(c);
    if (!id) return;
    switch (GPOINTER_TO_INT(d)) {
    case 0:
        g_strlcpy(aj.tema, id, sizeof aj.tema);
        aplicar_css();
        break;
    case 1:
        if (!strcmp(aj.idioma, id)) return;
        g_strlcpy(aj.idioma, id, sizeof aj.idioma);
        aplicar_idioma();
        g_idle_add(reconstruir, NULL);   /* do not destroy the combo inside its own signal */
        break;
    case 2:
        cambiar_barra(id);
        break;
    }
    guardar_ajustes();
}

static GtkWidget *combo(const Opcion *ops, size_t n, const char *activo, int cual)
{
    GtkWidget *c = gtk_combo_box_text_new();
    gchar *xfce = cual == 2 ? g_find_program_in_path("xfce4-panel") : NULL;
    for (size_t i = 0; i < n; i++) {
        if (cual == 2 && !strcmp(ops[i].id, "panel") && !xfce)
            continue;   /* without XFCE the plugin is not offered */
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), ops[i].id, _(ops[i].nombre));
    }
    g_free(xfce);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), activo);
    g_signal_connect(c, "changed", G_CALLBACK(al_cambiar_ajuste), GINT_TO_POINTER(cual));
    return c;
}

static GtkWidget *crear_ajustes(void)
{
    GtkWidget *b = gtk_menu_button_new();
    gtk_button_set_label(GTK_BUTTON(b), _("Settings"));
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "fantasma");
    gtk_widget_set_halign(b, GTK_ALIGN_END);

    GtkWidget *pop = gtk_popover_new(b);
    GtkWidget *g = gtk_grid_new();
    g_object_set(g, "margin", 16, "row-spacing", 12, "column-spacing", 16, NULL);
    const char *nombres[3] = { _("Theme"), _("Language"), _("Bottom bar") };
    GtkWidget *combos[3] = {
        combo(TEMAS, G_N_ELEMENTS(TEMAS), aj.tema, 0),
        combo(IDIOMAS, G_N_ELEMENTS(IDIOMAS), aj.idioma, 1),
        combo(BARRAS, G_N_ELEMENTS(BARRAS), aj.barra, 2),
    };
    for (int i = 0; i < 3; i++) {
        gtk_grid_attach(GTK_GRID(g), etiqueta(nombres[i], NULL), 0, i, 1, 1);
        gtk_widget_set_hexpand(combos[i], TRUE);
        gtk_grid_attach(GTK_GRID(g), combos[i], 1, i, 1, 1);
    }
    app.nota_ajustes = etiqueta("", "aviso");
    gtk_label_set_line_wrap(GTK_LABEL(app.nota_ajustes), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(app.nota_ajustes), 36);
    gtk_widget_set_no_show_all(app.nota_ajustes, TRUE);
    gtk_grid_attach(GTK_GRID(g), app.nota_ajustes, 0, 3, 2, 1);
    gtk_widget_show_all(g);
    gtk_container_add(GTK_CONTAINER(pop), g);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(b), pop);
    return b;
}

/*
 * Short screens (usable height < 980 px, e.g. 1366×768): two columns, ~510 px tall.
 * CPU_FREQ_COMPACTO=1/0 forces it (for testing).
 */
static gboolean es_compacto(void)
{
    const char *e = g_getenv("CPU_FREQ_COMPACTO");
    if (e) return e[0] == '1';
    GdkDisplay *d = gdk_display_get_default();
    GdkMonitor *m = d ? gdk_display_get_primary_monitor(d) : NULL;
    if (!m && d) m = gdk_display_get_monitor(d, 0);
    if (!m) return FALSE;
    GdkRectangle r;
    gdk_monitor_get_workarea(m, &r);
    return r.height < 980;
}

static void construir_contenido(void)
{
    if (app.contenido) gtk_widget_destroy(app.contenido);
    app.anim_id = 0;
    app.primera = TRUE;
    crear_posiciones();
    gtk_window_set_title(GTK_WINDOW(app.ventana), _("CPU Speed"));

    /* caja: speedometer and history; der: limit and status. On tall screens they are the same box */
    gboolean compacto = es_compacto();
    GtkWidget *raiz = app.contenido = gtk_box_new(compacto ? GTK_ORIENTATION_HORIZONTAL
                                                           : GTK_ORIENTATION_VERTICAL, 32);
    g_object_set(raiz, "margin", 24, "margin-top", 12, NULL);
    gtk_container_add(GTK_CONTAINER(app.ventana), raiz);
    GtkWidget *caja = raiz, *der = raiz;
    if (compacto) {
        caja = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        der = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_size_request(caja, 392, -1);
        gtk_widget_set_size_request(der, 392, -1);
        gtk_box_pack_start(GTK_BOX(raiz), caja, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(raiz), der, FALSE, FALSE, 0);
    } else {
        gtk_box_set_spacing(GTK_BOX(raiz), 0);
    }

    gtk_box_pack_start(GTK_BOX(caja), crear_ajustes(), FALSE, FALSE, 0);

    app.gauge = gtk_drawing_area_new();
    gtk_widget_set_size_request(app.gauge, 300, 244);
    g_signal_connect(app.gauge, "draw", G_CALLBACK(dibujar), NULL);
    gtk_box_pack_start(GTK_BOX(caja), app.gauge, FALSE, FALSE, 0);

    app.modo = gtk_label_new("…");
    gtk_style_context_add_class(gtk_widget_get_style_context(app.modo), "suave");
    gtk_box_pack_start(GTK_BOX(caja), app.modo, FALSE, FALSE, 0);

    /* History: title and legend (5-minute summary), then the graph */
    GtkWidget *hist_caja = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(hist_caja), "tarjeta");
    gtk_style_context_add_class(gtk_widget_get_style_context(hist_caja), "historial");
    gtk_widget_set_margin_top(hist_caja, 16);
    GtkWidget *cab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gchar *ult = g_utf8_strup(_("Last 5 minutes"), -1);
    gtk_box_pack_start(GTK_BOX(cab), etiqueta(ult, "etiqueta"), TRUE, TRUE, 0);
    g_free(ult);
    app.ley_temp = etiqueta("", "serie-temp");
    app.ley_ghz = etiqueta("", "serie-ghz");
    gtk_box_pack_end(GTK_BOX(cab), app.ley_temp, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(cab), app.ley_ghz, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hist_caja), cab, FALSE, FALSE, 0);
    app.grafica = gtk_drawing_area_new();
    gtk_widget_set_size_request(app.grafica, -1, 72);
    gtk_widget_set_tooltip_text(app.grafica, _("Blue: frequency (0 to the end of the dial); dashed line: its average. Amber: temperature (20–100 °C)."));
    g_signal_connect(app.grafica, "draw", G_CALLBACK(dibujar_grafica), NULL);
    gtk_box_pack_start(GTK_BOX(hist_caja), app.grafica, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(caja), hist_caja, FALSE, FALSE, 0);

    GtkWidget *t = app.titulo_lim = etiqueta("", "etiqueta");
    gtk_widget_set_tooltip_text(t, _("With auto-cpufreq, the choice is saved for the current power source (charger or battery) and survives a restart."));
    gtk_widget_set_margin_top(t, compacto ? 44 : 24);   /* compact: aligned with the dial */
    gtk_widget_set_margin_bottom(t, 8);
    gtk_box_pack_start(GTK_BOX(der), t, FALSE, FALSE, 0);

    GtkWidget *pos = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(pos), "posiciones");
    for (int i = 0; i < NPOS; i++) {
        GtkWidget *b = app.botones[i] = gtk_toggle_button_new_with_label(POS[i].etiqueta);
        gtk_widget_set_tooltip_text(b, POS[i].ayuda);
        g_signal_connect(b, "toggled", G_CALLBACK(al_pulsar), GINT_TO_POINTER(i));
        gtk_box_pack_start(GTK_BOX(pos), b, TRUE, TRUE, 0);
    }
    gtk_box_pack_start(GTK_BOX(der), pos, FALSE, FALSE, 0);

    app.aviso = etiqueta(" ", "aviso");
    gtk_label_set_ellipsize(GTK_LABEL(app.aviso), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_top(app.aviso, 8);
    gtk_widget_set_size_request(app.aviso, -1, 16);
    gtk_box_pack_start(GTK_BOX(der), app.aviso, FALSE, FALSE, 0);

    gchar *est = g_utf8_strup(_("System status"), -1);
    t = etiqueta(est, "etiqueta");
    g_free(est);
    gtk_widget_set_margin_top(t, 16);
    gtk_widget_set_margin_bottom(t, 8);
    gtk_box_pack_start(GTK_BOX(der), t, FALSE, FALSE, 0);

    GtkWidget *tarjeta = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(tarjeta), "tarjeta");
    const char *nombres[N_FILAS] = {
        _("Temperature"), _("Battery"), _("Charger"), _("Free RAM"), _("Swap used"),
        _("Remote mode"), _("Watchdog"),
    };
    for (int i = 0; i < N_FILAS; i++) {
        GtkWidget *f = app.fila[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_no_show_all(f, TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(f), "fila");
        GtkWidget *n = etiqueta(nombres[i], "suave");
        gtk_box_pack_start(GTK_BOX(f), n, TRUE, TRUE, 0);
        app.chip[i] = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(app.chip[i]), "chip");
        gtk_widget_set_valign(app.chip[i], GTK_ALIGN_CENTER);
        app.valor[i] = gtk_label_new("…");
        PangoAttrList *at = pango_attr_list_new();
        pango_attr_list_insert(at, pango_attr_font_features_new("tnum 1"));
        pango_attr_list_insert(at, pango_attr_weight_new(PANGO_WEIGHT_SEMIBOLD));
        gtk_label_set_attributes(GTK_LABEL(app.valor[i]), at);
        pango_attr_list_unref(at);
        gtk_box_pack_end(GTK_BOX(f), app.valor[i], FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(f), app.chip[i], FALSE, FALSE, 0);
        gtk_widget_show(n);
        gtk_widget_show(app.valor[i]);
        gtk_widget_show(f);
        gtk_box_pack_start(GTK_BOX(tarjeta), f, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(der), tarjeta, FALSE, FALSE, 0);
    gtk_widget_show_all(raiz);
    for (int i = 0; i < N_FILAS; i++) gtk_widget_hide(app.chip[i]);
}

/* In tray mode, closing the window only hides it. */
static gboolean al_cerrar(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)e; (void)d;
    if (bandeja_activa()) {
        gtk_widget_hide(w);
        malloc_trim(0);
        return TRUE;
    }
    return FALSE;
}

/* The screen exists now: own stylesheet and following the system theme. */
static void accion_alternar(GSimpleAction *a, GVariant *p, gpointer d);

static void al_iniciar(GApplication *ga, gpointer d)
{
    (void)d;
    app.css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(app.css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    aplicar_css();
    GtkSettings *s = gtk_settings_get_default();
    g_signal_connect_swapped(s, "notify::gtk-theme-name", G_CALLBACK(aplicar_css), NULL);
    g_signal_connect_swapped(s, "notify::gtk-application-prefer-dark-theme", G_CALLBACK(aplicar_css), NULL);

    GSimpleAction *alt = g_simple_action_new("alternar", NULL);
    g_signal_connect(alt, "activate", G_CALLBACK(accion_alternar), NULL);
    g_action_map_add_action(G_ACTION_MAP(ga), G_ACTION(alt));
    g_object_unref(alt);
}

static void activar(GApplication *ga, gpointer d);

/* "alternar" (toggle) action: used by `cpu-freq --alternar` (the panel plugin). */
static void accion_alternar(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p; (void)d;
    if (app.ventana) alternar();
    else activar(G_APPLICATION(app.gapp), NULL);
}

static void activar(GApplication *ga, gpointer d)
{
    (void)d;
    if (app.ventana) {
        mostrar_ventana();
        return;
    }
    app.ventana = gtk_application_window_new(GTK_APPLICATION(ga));
    gtk_window_set_icon_name(GTK_WINDOW(app.ventana), "cpu-freq");
    gtk_window_set_resizable(GTK_WINDOW(app.ventana), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(app.ventana), "cpufreq");
    g_signal_connect(app.ventana, "delete-event", G_CALLBACK(al_cerrar), NULL);
    g_signal_connect(app.ventana, "focus-out-event", G_CALLBACK(al_perder_foco), NULL);

    construir_contenido();
    if (!strcmp(aj.barra, "bandeja")) poner_bandeja(TRUE);
    refrescar(NULL);
    app.timer = g_timeout_add(REFRESCO_MS, refrescar, NULL);
    /* --bandeja (autostart): start hidden only if tray mode is still selected */
    if (!(app.oculta_al_inicio && bandeja_activa())) gtk_widget_show(app.ventana);
    malloc_trim(0);
}

int main(int argc, char **argv)
{
    g_set_prgname("cpu-freq");
    setlocale(LC_ALL, "");
    language_original = g_strdup(g_getenv("LANGUAGE"));
    const char *ld = g_getenv("CPU_FREQ_LOCALEDIR");   /* to test without installing */
    bindtextdomain(DOMINIO, ld ? ld : LOCALEDIR);
    bind_textdomain_codeset(DOMINIO, "UTF-8");
    textdomain(DOMINIO);

    gboolean pide_alternar = FALSE;
    for (int i = 1; i < argc; i++) {
        gboolean *flag = !strcmp(argv[i], "--bandeja")  ? &app.oculta_al_inicio
                       : !strcmp(argv[i], "--alternar") ? &pide_alternar : NULL;
        if (!flag) continue;
        *flag = TRUE;
        memmove(&argv[i], &argv[i + 1], (size_t)(argc - i) * sizeof *argv);
        argc--;
        i--;
    }

    app.elegido = -1;
    app.con_acf = hw_hay_auto_cpufreq();
    leer_conf();
    aplicar_idioma();
    setlocale(LC_NUMERIC, "C");

    /* Personal rows: only if the modo-remoto / vigilante-laptop scripts exist */
    gchar *mr = g_find_program_in_path("modo-remoto");
    gchar *mr2 = g_build_filename(g_get_home_dir(), ".local", "bin", "modo-remoto", NULL);
    gchar *vg = g_find_program_in_path("vigilante-laptop");
    gchar *vg2 = g_build_filename(g_get_home_dir(), ".local", "bin", "vigilante-laptop", NULL);
    app.fila_fija[F_REMOTO] = mr || g_file_test(mr2, G_FILE_TEST_IS_EXECUTABLE);
    app.fila_fija[F_VIG] = vg || g_file_test(vg2, G_FILE_TEST_IS_EXECUTABLE);
    g_free(mr); g_free(mr2); g_free(vg); g_free(vg2);

    gdk_set_program_class("cpu-freq");
    app.gapp = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app.gapp, "startup", G_CALLBACK(al_iniciar), NULL);
    g_signal_connect(app.gapp, "activate", G_CALLBACK(activar), NULL);

    /* --alternar with the app already running: ask it to toggle its window, then exit */
    if (pide_alternar && g_application_register(G_APPLICATION(app.gapp), NULL, NULL) &&
        g_application_get_is_remote(G_APPLICATION(app.gapp))) {
        g_action_group_activate_action(G_ACTION_GROUP(app.gapp), "alternar", NULL);
        GDBusConnection *c = g_application_get_dbus_connection(G_APPLICATION(app.gapp));
        if (c) g_dbus_connection_flush_sync(c, NULL, NULL);
        g_object_unref(app.gapp);
        return 0;
    }
    int r = g_application_run(G_APPLICATION(app.gapp), argc, argv);
    g_object_unref(app.gapp);
    return r;
}
