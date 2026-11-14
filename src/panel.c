/*
 * Complemento del panel de XFCE: mini velocímetro con la frecuencia del CPU.
 * Corre dentro del panel (sin proceso aparte), lee cada 2 s y al hacer clic abre
 * la app cpu-freq. Comparte la detección de hardware con la app (hw.h).
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

#define REFRESCO_MS 2000
#define A0 (0.75 * G_PI)
#define BARRIDO (1.5 * G_PI)

typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget *boton, *area, *etiqueta;
    guint timer;
    double ghz, escala;
    int turbo;
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
    snprintf(tip + n, sizeof tip - (size_t)n, "\n%s", _("Click to open CPU Speed"));
    gtk_widget_set_tooltip_text(p->boton, tip);
    gtk_widget_queue_draw(p->area);
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
    refrescar(p);
    p->timer = g_timeout_add(REFRESCO_MS, refrescar, p);
}

XFCE_PANEL_PLUGIN_REGISTER(construir);
