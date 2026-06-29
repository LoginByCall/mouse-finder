/* mouse_finder.c — Shake-to-find-mouse for Ubuntu 24 X11
 * Copyright (c) 2026 Alexander Rozhkov — MIT License
 * Build: make
 */

/* X11 headers before GTK to avoid None/Cursor conflicts */
#include <X11/Xlib.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/record.h>
#include <cairo/cairo.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

/* ── Constants ──────────────────────────────────────────────────────────── */
#define SAMPLE_MAX   64
#define WINDOW_MS    500
#define COOLDOWN_MS  1000

/* ── Types ───────────────────────────────────────────────────────────────── */
typedef struct { int cursor_size, show_duration, min_reversals, min_speed; } Settings;
typedef struct { gint64 t_ms; gint16 x; }                                    Sample;

typedef struct App {
    /* X11 */
    Display        *grab_dpy;       /* cursor + grab + record ctrl  */
    Display        *data_dpy;       /* record data stream (thread)  */
    XRecordContext  record_ctx;
    Window          root;
    Cursor          big_cursor;
    gboolean        grabbed;
    /* Settings */
    Settings        cfg;
    volatile gint   atm_min_speed;  /* read by record thread via g_atomic */
    volatile gint   atm_min_reversals;
    /* Shake state — only accessed by record thread */
    Sample          samples[SAMPLE_MAX];
    int             sample_head;
    int             sample_count;
    gint64          last_shake_ms;
    /* GTK */
    GtkStatusIcon  *tray;
    pthread_t       record_tid;
} App;

/* Forward declarations */
static gboolean on_shake(gpointer ud);
static gboolean ungrab(gpointer ud);

/* ── Settings ────────────────────────────────────────────────────────────── */
#define CFG_GROUP "mouse-finder"

static char *cfg_path(void) {
    return g_build_filename(g_get_user_config_dir(), "mouse-finder.ini", NULL);
}

static void settings_load(Settings *s) {
    *s = (Settings){ 128, 1500, 3, 500 };
    GKeyFile *kf = g_key_file_new();
    char *path = cfg_path();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        GError *e = NULL; int v;
#define LOAD(f, k) v = g_key_file_get_integer(kf, CFG_GROUP, k, &e); \
                   if (!e) { s->f = v; } g_clear_error(&e);
        LOAD(cursor_size,   "cursor_size")
        LOAD(show_duration, "show_duration")
        LOAD(min_reversals, "min_reversals")
        LOAD(min_speed,     "min_speed")
#undef LOAD
    }
    g_free(path); g_key_file_free(kf);
}

static void settings_save(const Settings *s) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_integer(kf, CFG_GROUP, "cursor_size",   s->cursor_size);
    g_key_file_set_integer(kf, CFG_GROUP, "show_duration", s->show_duration);
    g_key_file_set_integer(kf, CFG_GROUP, "min_reversals", s->min_reversals);
    g_key_file_set_integer(kf, CFG_GROUP, "min_speed",     s->min_speed);
    char *path = cfg_path();
    char *dir  = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path); g_key_file_free(kf);
}

/* ── Cursor ──────────────────────────────────────────────────────────────── */
static void arrow_path(cairo_t *cr, double s, double dx, double dy) {
    static const double pts[7][2] = {
        {1,1},{1,20},{5,16},{9,24},{11,23},{7.5,15},{13,15}
    };
    cairo_move_to(cr, (pts[0][0]+dx)*s, (pts[0][1]+dy)*s);
    for (int i = 1; i < 7; i++)
        cairo_line_to(cr, (pts[i][0]+dx)*s, (pts[i][1]+dy)*s);
    cairo_close_path(cr);
}

static Cursor cursor_create(Display *dpy, int size) {
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surf);
    double s = size / 32.0;

    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    arrow_path(cr, s, 1.5*s, 1.5*s);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    arrow_path(cr, s, 0, 0);
    cairo_set_line_width(cr, 2.5*s);
    cairo_stroke_preserve(cr);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 0, 0, 0, 1);
    arrow_path(cr, s, 0, 0);
    cairo_fill(cr);

    cairo_surface_flush(surf);
    unsigned char *data = cairo_image_surface_get_data(surf);

    XcursorImage *img = XcursorImageCreate(size, size);
    img->xhot = MAX(1, size / 32);
    img->yhot = MAX(1, size / 32);
    memcpy(img->pixels, data, (size_t)size * size * 4);

    Cursor cur = XcursorImageLoadCursor(dpy, img);
    XcursorImageDestroy(img);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return cur;
}

/* ── Shake detection ─────────────────────────────────────────────────────── */
static gboolean detect_shake(App *app, gint64 now_ms) {
    if (now_ms - app->last_shake_ms < COOLDOWN_MS) return FALSE;
    int count = app->sample_count;
    if (count < 6) return FALSE;

    int min_speed = g_atomic_int_get(&app->atm_min_speed);
    int min_rev   = g_atomic_int_get(&app->atm_min_reversals);
    gint64 cutoff = now_ms - WINDOW_MS;

    int reversals = 0, prev_sign = 0;
    gint64 prev_fast_t = 0;
    int start = (app->sample_head - count + SAMPLE_MAX) % SAMPLE_MAX;

    for (int i = 1; i < count; i++) {
        int ia = (start + i - 1) % SAMPLE_MAX;
        int ib = (start + i    ) % SAMPLE_MAX;
        if (app->samples[ia].t_ms < cutoff) continue;
        gint64 dt = app->samples[ib].t_ms - app->samples[ia].t_ms;
        if (dt == 0) continue;

        double vx = (double)(app->samples[ib].x - app->samples[ia].x) / (dt / 1000.0);
        if (fabs(vx) < min_speed) {
            if (prev_fast_t && (app->samples[ib].t_ms - prev_fast_t) > 200)
                prev_sign = 0;
            continue;
        }
        prev_fast_t = app->samples[ib].t_ms;
        int sign = (vx > 0) ? 1 : -1;
        if (prev_sign && sign != prev_sign) reversals++;
        prev_sign = sign;
    }
    return reversals >= min_rev;
}

static void record_callback(XPointer priv, XRecordInterceptData *data) {
    if (data->category != XRecordFromServer || data->data_len < 8) goto done;

    /* X11 protocol: byte 0 = type, bytes 24-25 = rootX (LE int16) */
    if ((data->data[0] & 0x7F) != MotionNotify) goto done;

    App *app = (App *)priv;
    gint16 rx = (gint16)(data->data[24] | (unsigned)(data->data[25] << 8));
    gint64 now = g_get_monotonic_time() / 1000;

    int idx = app->sample_head;
    app->samples[idx] = (Sample){ now, rx };
    app->sample_head  = (idx + 1) % SAMPLE_MAX;
    if (app->sample_count < SAMPLE_MAX) app->sample_count++;

    if (detect_shake(app, now)) {
        app->last_shake_ms = now;
        g_idle_add(on_shake, app);
    }
done:
    XRecordFreeData(data);
}

static void *record_thread(void *arg) {
    App *app = (App *)arg;
    XRecordEnableContext(app->data_dpy, app->record_ctx,
                         record_callback, (XPointer)app);
    return NULL;
}

static gboolean setup_record(App *app) {
    int major, minor;
    if (!XRecordQueryVersion(app->grab_dpy, &major, &minor)) return FALSE;

    app->data_dpy = XOpenDisplay(NULL);
    if (!app->data_dpy) return FALSE;

    XRecordRange *range = XRecordAllocRange();
    range->device_events.first = MotionNotify;
    range->device_events.last  = MotionNotify;
    XRecordClientSpec client = XRecordAllClients;
    app->record_ctx = XRecordCreateContext(
        app->grab_dpy, 0, &client, 1, &range, 1);
    XFree(range);

    if (!app->record_ctx) { XCloseDisplay(app->data_dpy); return FALSE; }
    XSync(app->grab_dpy, False);
    return TRUE;
}

/* ── Grab / ungrab ───────────────────────────────────────────────────────── */
static gboolean on_shake(gpointer ud) {
    App *app = (App *)ud;
    if (app->grabbed) return G_SOURCE_REMOVE;
    int r = XGrabPointer(app->grab_dpy, app->root, False,
        PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
        GrabModeAsync, GrabModeAsync, None, app->big_cursor, CurrentTime);
    if (r == GrabSuccess) {
        app->grabbed = TRUE;
        g_timeout_add((guint)app->cfg.show_duration, ungrab, app);
    }
    return G_SOURCE_REMOVE;
}

static gboolean ungrab(gpointer ud) {
    App *app = (App *)ud;
    XUngrabPointer(app->grab_dpy, CurrentTime);
    XFlush(app->grab_dpy);
    app->grabbed = FALSE;
    return G_SOURCE_REMOVE;
}

/* ── Autostart ───────────────────────────────────────────────────────────── */
static char *autostart_path(void) {
    return g_build_filename(g_get_user_config_dir(), "autostart",
                            "mouse-finder.desktop", NULL);
}

static gboolean autostart_enabled(void) {
    char *p = autostart_path();
    gboolean r = g_file_test(p, G_FILE_TEST_EXISTS);
    g_free(p); return r;
}

static void toggle_autostart(App *app) {
    char *path = autostart_path();
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_unlink(path);
    } else {
        char *dir = g_path_get_dirname(path);
        g_mkdir_with_parents(dir, 0755);
        g_free(dir);
        /* Prefer installed binary; fall back to /proc/self/exe */
        char *exe = g_find_program_in_path("mouse-finder");
        if (!exe) {
            char buf[PATH_MAX];
            ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf)-1);
            exe = (n > 0) ? (buf[n]='\0', g_strdup(buf)) : g_strdup("mouse-finder");
        }
        char *content = g_strdup_printf(
            "[Desktop Entry]\nName=Mouse Finder\nExec=%s\n"
            "Type=Application\nX-GNOME-Autostart-enabled=true\n", exe);
        g_file_set_contents(path, content, -1, NULL);
        g_free(content); g_free(exe);
    }
    g_free(path);
    (void)app;
}

/* ── Settings dialog ─────────────────────────────────────────────────────── */
static void apply_settings(App *app, const Settings *s) {
    if (s->cursor_size != app->cfg.cursor_size) {
        XFreeCursor(app->grab_dpy, app->big_cursor);
        app->big_cursor = cursor_create(app->grab_dpy, s->cursor_size);
    }
    g_atomic_int_set(&app->atm_min_speed,     s->min_speed);
    g_atomic_int_set(&app->atm_min_reversals, s->min_reversals);
    app->cfg = *s;
}

static void show_settings(App *app) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Mouse Finder — Настройки", NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Отмена", GTK_RESPONSE_CANCEL,
        "Применить", GTK_RESPONSE_OK, NULL);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_container_add(
        GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), grid);

    Settings ns = app->cfg;
    struct { const char *lbl; int *val; double lo, hi, step; } rows[] = {
        {"Размер курсора (px)",  &ns.cursor_size,   32,  256,  32},
        {"Длительность (мс)",   &ns.show_duration, 500, 3000, 100},
        {"Реверсов для тряски", &ns.min_reversals, 2,   6,    1 },
        {"Мин. скорость (px/s)",&ns.min_speed,     100, 1000, 50},
    };
    int n = (int)(sizeof(rows)/sizeof(rows[0]));
    GtkWidget *scales[4];

    for (int i = 0; i < n; i++) {
        GtkWidget *lbl = gtk_label_new(rows[i].lbl);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, i, 1, 1);

        GtkWidget *sc = gtk_scale_new_with_range(
            GTK_ORIENTATION_HORIZONTAL, rows[i].lo, rows[i].hi, rows[i].step);
        gtk_range_set_value(GTK_RANGE(sc), *rows[i].val);
        gtk_widget_set_size_request(sc, 220, -1);
        gtk_scale_set_digits(GTK_SCALE(sc), 0);
        gtk_grid_attach(GTK_GRID(grid), sc, 1, i, 1, 1);
        scales[i] = sc;
    }
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        for (int i = 0; i < n; i++)
            *rows[i].val = (int)gtk_range_get_value(GTK_RANGE(scales[i]));
        apply_settings(app, &ns);
        settings_save(&app->cfg);
    }
    gtk_widget_destroy(dlg);
}

/* ── Tray ────────────────────────────────────────────────────────────────── */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

static void tray_popup(GtkStatusIcon *icon, guint btn, guint t, App *app) {
    GtkWidget *menu = gtk_menu_new();

    GtkWidget *i_cfg = gtk_menu_item_new_with_label("Настройки…");
    g_signal_connect_swapped(i_cfg, "activate", G_CALLBACK(show_settings), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i_cfg);

    const char *albl = autostart_enabled() ? "Автостарт: ВКЛ ✓" : "Автостарт: ВЫКЛ";
    GtkWidget *i_auto = gtk_menu_item_new_with_label(albl);
    g_signal_connect_swapped(i_auto, "activate", G_CALLBACK(toggle_autostart), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i_auto);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *i_quit = gtk_menu_item_new_with_label("Выход");
    g_signal_connect(i_quit, "activate", G_CALLBACK(gtk_main_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i_quit);

    gtk_widget_show_all(menu);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL,
                   gtk_status_icon_position_menu, icon, btn, t);
}

G_GNUC_END_IGNORE_DEPRECATIONS

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    XInitThreads();
    gtk_init(&argc, &argv);

    App app = {0};
    settings_load(&app.cfg);
    g_atomic_int_set(&app.atm_min_speed,     app.cfg.min_speed);
    g_atomic_int_set(&app.atm_min_reversals, app.cfg.min_reversals);

    app.grab_dpy = XOpenDisplay(NULL);
    if (!app.grab_dpy) { g_printerr("Cannot open X display\n"); return 1; }
    app.root       = DefaultRootWindow(app.grab_dpy);
    app.big_cursor = cursor_create(app.grab_dpy, app.cfg.cursor_size);

    if (!setup_record(&app)) {
        g_printerr("XRecord extension not available (need X11 session)\n");
        return 1;
    }
    pthread_create(&app.record_tid, NULL, record_thread, &app);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    app.tray = gtk_status_icon_new_from_icon_name("input-mouse");
    gtk_status_icon_set_tooltip_text(app.tray, "Mouse Finder — shake to find cursor");
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_signal_connect(app.tray, "popup-menu", G_CALLBACK(tray_popup), &app);

    gtk_main();

    /* Cleanup */
    XRecordDisableContext(app.grab_dpy, app.record_ctx);
    XFlush(app.grab_dpy);
    pthread_join(app.record_tid, NULL);
    XRecordFreeContext(app.grab_dpy, app.record_ctx);
    XCloseDisplay(app.data_dpy);
    XFreeCursor(app.grab_dpy, app.big_cursor);
    XCloseDisplay(app.grab_dpy);
    g_object_unref(app.tray);
    return 0;
}
