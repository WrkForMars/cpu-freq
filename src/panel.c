/*
 * Complemento del panel de XFCE: mini velocímetro con la frecuencia del CPU.
 * Corre dentro del panel (sin proceso aparte), lee cada 2 s; el clic abre o cierra la
 * app cpu-freq y el menú del clic derecho elige el límite (Auto, niveles, Turbo) llamando
 * al mismo ayudante con pkexec. Comparte la detección de hardware con la app (hw.h).
 */
#define _GNU_SOURCE
#include <libxfce4panel/libxfce4panel.h>
#include <libintl.h>
#include <math.h>
#include "hw.h"

#ifndef LOCALEDIR
#define LOCALEDIR "/usr/share/locale"
#endif
#define DOMINIO "cpu-freq"
#define _(s) dgettext(DOMINIO, s)

#ifndef AYUDANTE
#define AYUDANTE "/usr/libexec/cpu-freq/ayudante"
#endif
#define REFRESCO_MS 2000
#define MAX_POS (HW_MAX_NIVEL + 2)
#define A0 (0.75 * G_PI)
#define BARRIDO (1.5 * G_PI)

typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget *boton, *area, *etiqueta;
    guint timer;
    double ghz, escala;
    int turbo;
    /* menú de límites: 0 = Auto, luego niveles, al final Turbo si hay */
    GtkWidget *items[MAX_POS];
    char args[MAX_POS][16];
    long khz[MAX_POS];
    int npos, turbo_pos;
    long tope_sin_turbo;
    gboolean actualizando, aplicando;
} Panel;

static double leer_ghz(void)
{
    glob_t g;
    if (!hw_politicas(&g)) return 0;
    long suma = 0, n = 0;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char ruta[PATH_MAX];
        long v;
        snprintf(ruta, sizeof ruta, "%s/scaling_cur_freq", g.gl_pathv[i]);
        if (hw_leer_long(ruta, &v)) { suma += v; n++; }
    }
    globfree(&g);
    return n ? (double)suma / n / 1e6 : 0;
}

static double angulo(const Panel *p, double ghz)
{
    return A0 + CLAMP(ghz / p->escala, 0, 1) * BARRIDO;
}

/* Mismo dibujo que el icono de bandeja: pista, valor en azul y aguja del color del texto. */
static gboolean dibujar(GtkWidget *w, cairo_t *cr, gpointer d)
{
    Panel *p = d;
    double s = MIN(gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
    double cx = gtk_widget_get_allocated_width(w) / 2.0;
    double cy = gtk_widget_get_allocated_height(w) / 2.0 + s * 0.06;
    double r = s * 0.40, g = MAX(2.0, s * 0.14);
    GdkRGBA tinta;
    gtk_style_context_get_color(gtk_widget_get_style_context(w), GTK_STATE_FLAG_NORMAL, &tinta);

    cairo_set_line_width(cr, g);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgba(cr, tinta.red, tinta.green, tinta.blue, 0.3);
    cairo_arc(cr, cx, cy, r, A0, A0 + BARRIDO);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, 0x25 / 255.0, 0x63 / 255.0, 0xEB / 255.0);   /* acento #2563EB */
    cairo_arc(cr, cx, cy, r, A0, angulo(p, p->ghz));
    cairo_stroke(cr);

    double a = angulo(p, p->ghz);
    gdk_cairo_set_source_rgba(cr, &tinta);
    cairo_set_line_width(cr, MAX(1.5, s * 0.09));
    cairo_move_to(cr, cx, cy);
    cairo_line_to(cr, cx + r * 0.7 * cos(a), cy + r * 0.7 * sin(a));
    cairo_stroke(cr);
    cairo_arc(cr, cx, cy, MAX(1.5, s * 0.08), 0, 2 * G_PI);
    cairo_fill(cr);
    return FALSE;
}

/* Posición elegida: la marca de auto-cpufreq o, sin él, la que coincide con el sistema. */
static int posicion_actual(const Panel *p)
{
    if (hw_hay_auto_cpufreq()) {
        char m[16];
        if (!hw_marca(hw_cargador(NULL, NULL), m, sizeof m)) return 0;
        if (strchr(m, '.')) snprintf(m, sizeof m, "%ld", lround(g_ascii_strtod(m, NULL) * 1e6));
        for (int i = 1; i < p->npos; i++) if (!strcmp(m, p->args[i])) return i;
        return 0;
    }
    glob_t g;
    long max = -1;
    if (hw_politicas(&g)) {
        char ruta[PATH_MAX];
        snprintf(ruta, sizeof ruta, "%s/scaling_max_freq", g.gl_pathv[0]);
        hw_leer_long(ruta, &max);
        globfree(&g);
    }
    int t = hw_turbo();
    if (p->turbo_pos >= 0 && t == 1 && max >= p->tope_sin_turbo) return p->turbo_pos;
    for (int i = 1; i < p->npos; i++)
        if (i != p->turbo_pos && max == p->khz[i] && t != 1) return i;
    return -1;
}

static void marcar_menu(Panel *p)
{
    int actual = posicion_actual(p);
    p->actualizando = TRUE;
    for (int i = 0; i < p->npos; i++) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(p->items[i]), i == actual);
        gtk_widget_set_sensitive(p->items[i], !p->aplicando);
    }
    p->actualizando = FALSE;
}

static void ayudante_termino(GPid pid, gint estado, gpointer d)
{
    Panel *p = d;
    (void)estado;
    g_spawn_close_pid(pid);
    p->aplicando = FALSE;
    marcar_menu(p);
}

static void al_elegir(GtkCheckMenuItem *it, gpointer d)
{
    Panel *p = d;
    if (p->actualizando || p->aplicando || !gtk_check_menu_item_get_active(it)) return;
    int i = 0;
    while (i < p->npos && p->items[i] != GTK_WIDGET(it)) i++;
    if (i == p->npos) return;
    gchar *argv[] = { "pkexec", AYUDANTE, p->args[i], NULL };
    GPid pid;
    if (g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                      G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &pid, NULL)) {
        p->aplicando = TRUE;
        g_child_watch_add(pid, ayudante_termino, p);
    }
    marcar_menu(p);
}

/* Añade "Speed limit", Auto, los niveles y Turbo al principio del menú del panel. */
static void crear_menu(Panel *p)
{
    HwFrec h;
    long niveles[HW_MAX_NIVEL];
    int n = hw_frecuencias(&h) ? hw_niveles(&h, niveles) : 0;
    int turbo = hw_turbo() >= 0;
    char etiquetas[MAX_POS][32];

    p->npos = 0;
    p->turbo_pos = -1;
    snprintf(p->args[0], sizeof p->args[0], "auto");
    snprintf(etiquetas[0], sizeof etiquetas[0], "%s", _("Auto"));
    p->npos = 1;
    for (int i = 0; i < n; i++, p->npos++) {
        snprintf(p->args[p->npos], sizeof p->args[0], "%ld", niveles[i]);
        snprintf(etiquetas[p->npos], sizeof etiquetas[0], "%.1f GHz", niveles[i] / 1e6);
        p->khz[p->npos] = niveles[i];
    }
    p->tope_sin_turbo = n ? niveles[n - 1] : h.max;
    if (turbo && n) {
        p->turbo_pos = p->npos;
        snprintf(p->args[p->npos], sizeof p->args[0], "turbo");
        snprintf(etiquetas[p->npos], sizeof etiquetas[0], _("%s + turbo"), etiquetas[n]);
        p->npos++;
    }

    /* xfce_panel_plugin_menu_insert_item añade arriba: se insertan en orden inverso */
    GtkWidget *sep = gtk_separator_menu_item_new();
    gtk_widget_show(sep);
    xfce_panel_plugin_menu_insert_item(p->plugin, GTK_MENU_ITEM(sep));
    GSList *grupo = NULL;
    for (int i = 0; i < p->npos; i++) {
        p->items[i] = gtk_radio_menu_item_new_with_label(grupo, etiquetas[i]);
        grupo = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(p->items[i]));
        g_signal_connect(p->items[i], "toggled", G_CALLBACK(al_elegir), p);
        gtk_widget_show(p->items[i]);
    }
    for (int i = p->npos - 1; i >= 0; i--)
        xfce_panel_plugin_menu_insert_item(p->plugin, GTK_MENU_ITEM(p->items[i]));
    GtkWidget *tit = gtk_menu_item_new_with_label(_("Speed limit"));
    gtk_widget_set_sensitive(tit, FALSE);
    gtk_widget_show(tit);
    xfce_panel_plugin_menu_insert_item(p->plugin, GTK_MENU_ITEM(tit));
    marcar_menu(p);
}

static gboolean refrescar(gpointer d)
{
    Panel *p = d;
    p->ghz = leer_ghz();
    p->turbo = hw_turbo();
    if (p->ghz > p->escala) p->escala = ceil(p->ghz);

    char t[32], tip[160];
    snprintf(t, sizeof t, "%.1f GHz", p->ghz);
    gtk_label_set_text(GTK_LABEL(p->etiqueta), t);
    int n = snprintf(tip, sizeof tip, "%.2f GHz", p->ghz);
    if (p->turbo >= 0)
        n += snprintf(tip + n, sizeof tip - (size_t)n, " · %s %s", _("Turbo"),
                      p->turbo ? _("Active") : _("Off"));
    snprintf(tip + n, sizeof tip - (size_t)n, "\n%s", _("Click to open or close CPU Speed. Right-click to set the speed limit."));
    gtk_widget_set_tooltip_text(p->boton, tip);
    gtk_widget_queue_draw(p->area);
    if (!p->aplicando) marcar_menu(p);
    return G_SOURCE_CONTINUE;
}

static void abrir(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    g_spawn_command_line_async("cpu-freq --alternar", NULL);   /* abre o cierra la ventana */
}

static gboolean al_cambiar_tamano(XfcePanelPlugin *plugin, gint tamano, gpointer d)
{
    Panel *p = d;
    gint icono = xfce_panel_plugin_get_icon_size(plugin);
    gtk_widget_set_size_request(p->area, icono, icono);
    (void)tamano;
    return TRUE;
}

static void al_cambiar_modo(XfcePanelPlugin *plugin, XfcePanelPluginMode modo, gpointer d)
{
    Panel *p = d;
    /* En paneles verticales el texto no cabe: solo el velocímetro */
    gtk_widget_set_visible(p->etiqueta, modo == XFCE_PANEL_PLUGIN_MODE_HORIZONTAL);
    al_cambiar_tamano(plugin, xfce_panel_plugin_get_size(plugin), p);
}

static void liberar(XfcePanelPlugin *plugin, gpointer d)
{
    Panel *p = d;
    (void)plugin;
    if (p->timer) g_source_remove(p->timer);
    g_free(p);
}

static void construir(XfcePanelPlugin *plugin)
{
    bindtextdomain(DOMINIO, LOCALEDIR);
    bind_textdomain_codeset(DOMINIO, "UTF-8");

    Panel *p = g_new0(Panel, 1);
    p->plugin = plugin;
    HwFrec h;
    int turbo = hw_turbo() >= 0;
    p->escala = hw_frecuencias(&h) ? MAX(2.0, ceil(h.max / 1e6) + (turbo ? 1 : 0)) : 4.0;

    p->boton = xfce_panel_create_button();
    GtkWidget *caja = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    p->area = gtk_drawing_area_new();
    p->etiqueta = gtk_label_new("…");
    PangoAttrList *at = pango_attr_list_new();
    pango_attr_list_insert(at, pango_attr_font_features_new("tnum 1"));
    gtk_label_set_attributes(GTK_LABEL(p->etiqueta), at);
    pango_attr_list_unref(at);
    gtk_box_pack_start(GTK_BOX(caja), p->area, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(caja), p->etiqueta, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(p->boton), caja);
    gtk_container_add(GTK_CONTAINER(plugin), p->boton);
    xfce_panel_plugin_add_action_widget(plugin, p->boton);

    g_signal_connect(p->area, "draw", G_CALLBACK(dibujar), p);
    g_signal_connect(p->boton, "clicked", G_CALLBACK(abrir), p);
    g_signal_connect(plugin, "size-changed", G_CALLBACK(al_cambiar_tamano), p);
    g_signal_connect(plugin, "mode-changed", G_CALLBACK(al_cambiar_modo), p);
    g_signal_connect(plugin, "free-data", G_CALLBACK(liberar), p);

    gtk_widget_show_all(p->boton);
    al_cambiar_modo(plugin, xfce_panel_plugin_get_mode(plugin), p);
    crear_menu(p);
    refrescar(p);
    p->timer = g_timeout_add(REFRESCO_MS, refrescar, p);
}

XFCE_PANEL_PLUGIN_REGISTER(construir);
