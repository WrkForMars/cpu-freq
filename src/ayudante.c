/*
 * ayudante: fija el límite de velocidad del CPU. Se ejecuta como root vía pkexec.
 *
 * Argumento único, validado:
 *   auto      deja que auto-cpufreq decida (o, sin él, quita el límite y enciende el turbo)
 *   turbo     sin límite y con el turbo siempre encendido
 *   <kHz>     límite máximo sin turbo; debe ser una frecuencia que admita el hardware
 *             (scaling_available_frequencies o el rango cpuinfo_min..max)
 * Nunca recibe rutas: todas son fijas.
 *
 * Si auto-cpufreq está instalado, la elección se guarda en la sección de la fuente de
 * energía actual ([charger] o [battery]) de su configuración, con la marca
 * "# cpu-freq: <valor>". auto-cpufreq vigila el archivo y lo recarga solo.
 * Antes del primer cambio se guarda una copia en RESPALDO; "auto" restaura los
 * valores originales de turbo y scaling_max_freq de esa sección.
 * Además se escribe sysfs directamente para que el cambio sea inmediato.
 *
 * Códigos de salida: 0 bien · 1 fallo · 2 argumento no válido · 3 no es root
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "hw.h"

#ifndef PRUEBA_DIR
#define PRUEBA_DIR "/etc"   /* solo `make prueba` lo cambia, para probar sin root */
#endif
#define CONF       PRUEBA_DIR "/auto-cpufreq.conf"
#define CONF_TMP   PRUEBA_DIR "/.auto-cpufreq.conf.cpu-freq"
#define RESPALDO   PRUEBA_DIR "/auto-cpufreq.conf.cpu-freq-orig"
#define MAX_CONF   (64 * 1024)

static int escribir(const char *ruta, const char *valor)
{
    int fd = open(ruta, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "cpu-freq helper: cannot open %s: %s\n", ruta, strerror(errno));
        return -1;
    }
    size_t n = strlen(valor);
    ssize_t w = write(fd, valor, n);
    int err = errno;
    close(fd);
    if (w != (ssize_t)n) {
        fprintf(stderr, "cpu-freq helper: cannot write %s: %s\n", ruta, strerror(err));
        return -1;
    }
    return 0;
}

/* Lee un archivo regular completo (máx. MAX_CONF). Si no existe devuelve "" (ENOENT). */
static char *leer_archivo(const char *ruta)
{
    int fd = open(ruta, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? calloc(1, 1) : NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > MAX_CONF) {
        close(fd);
        return NULL;
    }
    char *buf = calloc(1, (size_t)st.st_size + 1);
    ssize_t n = buf ? read(fd, buf, (size_t)st.st_size) : -1;
    close(fd);
    if (n != st.st_size) { free(buf); return NULL; }
    return buf;
}

static int guardar(const char *ruta, const char *texto, int excl)
{
    int fd = open(ruta, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | (excl ? O_EXCL : O_TRUNC), 0644);
    if (fd < 0) return -1;
    size_t n = strlen(texto);
    int ok = write(fd, texto, n) == (ssize_t)n && fchmod(fd, 0644) == 0 && fsync(fd) == 0;
    return (close(fd) == 0 && ok) ? 0 : -1;
}

/* ¿La línea [l, fin) es "turbo = ..." o "scaling_max_freq = ..."? */
static int es_clave_nuestra(const char *l, const char *fin)
{
    while (l < fin && isspace((unsigned char)*l)) l++;
    static const char *claves[] = { "turbo", "scaling_max_freq" };
    for (size_t i = 0; i < 2; i++) {
        size_t n = strlen(claves[i]);
        if ((size_t)(fin - l) > n && strncmp(l, claves[i], n) == 0) {
            const char *p = l + n;
            while (p < fin && (*p == ' ' || *p == '\t')) p++;
            if (p < fin && (*p == '=' || *p == ':')) return 1;
        }
    }
    return 0;
}

static int es_marca(const char *l, const char *fin)
{
    while (l < fin && isspace((unsigned char)*l)) l++;
    return (size_t)(fin - l) >= strlen(HW_MARCA) && strncmp(l, HW_MARCA, strlen(HW_MARCA)) == 0;
}

/* Si la línea es una cabecera [x], copia x en sec y devuelve 1. */
static int cabecera(const char *l, const char *fin, char *sec, size_t n)
{
    while (l < fin && isspace((unsigned char)*l)) l++;
    if (l >= fin || *l != '[') return 0;
    const char *c = memchr(l, ']', (size_t)(fin - l));
    if (!c) return 0;
    size_t len = (size_t)(c - l - 1);
    if (len >= n) len = n - 1;
    memcpy(sec, l + 1, len);
    sec[len] = '\0';
    return 1;
}

/* Llama a cb por cada línea (con su '\n') indicando la sección en la que está. */
typedef void (*por_linea)(const char *sec, int es_cab, const char *l, const char *fin, void *d);
static void recorrer(const char *texto, por_linea cb, void *d)
{
    char sec[64] = "";
    for (const char *l = texto; *l;) {
        const char *nl = strchr(l, '\n');
        const char *fin = nl ? nl + 1 : l + strlen(l);
        int cab = cabecera(l, fin, sec, sizeof sec);
        cb(sec, cab, l, fin, d);
        l = fin;
    }
}

typedef struct {
    const char *seccion;
    char *salida;
    size_t len, cap;
    const char *insertar;
    int encontrada;
} Edicion;

static void anexar(Edicion *e, const char *s, size_t n)
{
    if (e->len + n + 1 > e->cap) {
        e->cap = (e->len + n + 1) * 2;
        e->salida = realloc(e->salida, e->cap);
        if (!e->salida) { fprintf(stderr, "cpu-freq helper: out of memory\n"); exit(1); }
    }
    memcpy(e->salida + e->len, s, n);
    e->len += n;
    e->salida[e->len] = '\0';
}

static void editar_linea(const char *sec, int es_cab, const char *l, const char *fin, void *d)
{
    Edicion *e = d;
    int nuestra = strcmp(sec, e->seccion) == 0;
    if (nuestra && !es_cab && (es_clave_nuestra(l, fin) || es_marca(l, fin)))
        return;                                   /* se reemplaza */
    anexar(e, l, (size_t)(fin - l));
    if (nuestra && es_cab) {
        if (fin > l && fin[-1] != '\n') anexar(e, "\n", 1);
        anexar(e, e->insertar, strlen(e->insertar));
        e->encontrada = 1;
    }
}

typedef struct { const char *seccion; Edicion *e; } Extraccion;

static void extraer_linea(const char *sec, int es_cab, const char *l, const char *fin, void *d)
{
    Extraccion *x = d;
    if (!es_cab && strcmp(sec, x->seccion) == 0 && es_clave_nuestra(l, fin)) {
        anexar(x->e, l, (size_t)(fin - l));
        if (fin[-1] != '\n') anexar(x->e, "\n", 1);
    }
}

/* marca == NULL: "auto" (restaurar valores originales de la sección). */
static int configurar_auto_cpufreq(const char *seccion, const char *marca,
                                   const char *turbo, long khz)
{
    char *actual = leer_archivo(CONF);
    if (!actual) {
        fprintf(stderr, "cpu-freq helper: cannot read %s\n", CONF);
        return -1;
    }

    /* Copia del original, solo la primera vez (vacía si no había configuración) */
    if (access(RESPALDO, F_OK) != 0 && guardar(RESPALDO, actual, 1) != 0) {
        fprintf(stderr, "cpu-freq helper: cannot create %s: %s\n", RESPALDO, strerror(errno));
        free(actual);
        return -1;
    }

    Edicion ins = { 0 };
    anexar(&ins, "", 0);
    if (marca) {
        char b[160];
        int n = snprintf(b, sizeof b, HW_MARCA " %s\nturbo = %s\n", marca, turbo);
        if (khz > 0) snprintf(b + n, sizeof b - (size_t)n, "scaling_max_freq = %ld\n", khz);
        anexar(&ins, b, strlen(b));
    } else {
        char *orig = leer_archivo(RESPALDO);
        if (orig) {
            Extraccion x = { seccion, &ins };
            recorrer(orig, extraer_linea, &x);
            free(orig);
        }
    }

    Edicion e = { .seccion = seccion, .insertar = ins.salida };
    anexar(&e, "", 0);
    recorrer(actual, editar_linea, &e);
    if (!e.encontrada && ins.len) {
        char b[80];
        snprintf(b, sizeof b, "%s[%s]\n", e.len ? (e.salida[e.len - 1] != '\n' ? "\n\n" : "\n") : "",
                 seccion);
        anexar(&e, b, strlen(b));
        anexar(&e, ins.salida, ins.len);
    }

    int r = 0;
    if (strcmp(e.salida, actual) != 0) {
        unlink(CONF_TMP);
        /* rename atómico: auto-cpufreq lo ve como IN_MOVED_TO y recarga */
        if (guardar(CONF_TMP, e.salida, 1) != 0 || rename(CONF_TMP, CONF) != 0) {
            fprintf(stderr, "cpu-freq helper: cannot save %s: %s\n", CONF, strerror(errno));
            unlink(CONF_TMP);
            r = -1;
        }
    }
    free(actual);
    free(ins.salida);
    free(e.salida);
    return r;
}

static int escribir_turbo(int turbo)
{
    int inv;
    const char *rt = hw_ruta_turbo(&inv);
    char valor[4];
    if (turbo < 0 || !rt || !hw_valor_turbo(turbo, valor, sizeof valor)) return 0;
    return escribir(rt, valor) != 0;
}

/* khz <= 0: máximo del hardware en cada política. turbo < 0: no tocarlo. */
static int escribir_sysfs(long khz, int turbo)
{
    glob_t g;
    if (!hw_politicas(&g)) {
        fprintf(stderr, "cpu-freq helper: cpufreq not found\n");
        return 1;
    }
    /* Encender el turbo primero: en Intel sube cpuinfo_max_freq y el máximo se lee después */
    int fallos = turbo == 1 ? escribir_turbo(1) : 0;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char ruta[PATH_MAX], valor[24];
        long v = khz;
        if (v <= 0) {
            snprintf(ruta, sizeof ruta, "%s/cpuinfo_max_freq", g.gl_pathv[i]);
            if (!hw_leer_long(ruta, &v)) { fallos++; continue; }
        }
        snprintf(ruta, sizeof ruta, "%s/scaling_max_freq", g.gl_pathv[i]);
        snprintf(valor, sizeof valor, "%ld", v);
        if (escribir(ruta, valor) != 0) fallos++;
    }
    globfree(&g);
    if (turbo == 0) fallos += escribir_turbo(0);
    return fallos;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: ayudante auto|turbo|<kHz>\n");
        return 2;
    }

    HwFrec h;
    if (!hw_frecuencias(&h)) {
        fprintf(stderr, "cpu-freq helper: this computer does not expose cpufreq\n");
        return 1;
    }

    const char *a = argv[1];
    long khz = 0;
    int es_auto = !strcmp(a, "auto"), es_turbo = !strcmp(a, "turbo");
    if (!es_auto && !es_turbo) {
        size_t n = strlen(a);
        char *fin;
        if (n < 4 || n > 8 || strspn(a, "0123456789") != n) {
            fprintf(stderr, "cpu-freq helper: invalid argument (use auto, turbo or a frequency in kHz)\n");
            return 2;
        }
        khz = strtol(a, &fin, 10);
        if (!hw_valida_khz(&h, khz)) {
            fprintf(stderr, "cpu-freq helper: %ld kHz is not a frequency this processor supports\n", khz);
            return 2;
        }
    }

#ifndef PRUEBA
    if (geteuid() != 0) {
        fprintf(stderr, "cpu-freq helper: must run as root (use pkexec)\n");
        return 3;
    }
#endif
    umask(022);

    int con_acf = hw_hay_auto_cpufreq();
    int fallos = 0;

    if (con_acf) {
        const char *seccion = hw_cargador(NULL, NULL) ? "charger" : "battery";
        char marca[16];
        snprintf(marca, sizeof marca, "%s", es_turbo ? "turbo" : a);
        if (configurar_auto_cpufreq(seccion, es_auto ? NULL : marca,
                                    es_turbo ? "always" : "never", khz) != 0)
            fallos++;
    }

#ifdef PRUEBA
    (void)escribir_sysfs;   /* la prueba no toca sysfs */
#else
    /* Efecto inmediato. En "auto" con auto-cpufreq, él decide en su siguiente ciclo. */
    if (!es_auto)
        fallos += escribir_sysfs(khz, es_turbo);
    else if (!con_acf)
        fallos += escribir_sysfs(0, 1);
#endif

    return fallos ? 1 : 0;
}
