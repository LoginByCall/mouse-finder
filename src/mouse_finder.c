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
    Window          overlay;
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
    GtkWidget      *active_menu;
    gint64          menu_closed_ms;
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
    *s = (Settings){ 128, 1500, 4, 500 };
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

/* ── Localisation ────────────────────────────────────────────────────────── */
typedef struct {
    const char *menu_settings, *menu_autostart_on, *menu_autostart_off,
               *menu_about, *menu_quit;
    const char *dlg_title, *dlg_cancel, *dlg_apply;
    const char *lbl_cursor, *lbl_duration, *lbl_reversals, *lbl_speed;
    const char *about_comments;
} Lang;

static const Lang L_RU = {
    "Настройки…", "Автостарт: ВКЛ ✓", "Автостарт: ВЫКЛ", "О программе…", "Выход",
    "Mouse Finder — Настройки", "Отмена", "Применить",
    "Размер курсора (px)", "Длительность (мс)", "Реверсов для тряски", "Мин. скорость (px/s)",
    "Встряхни мышью — курсор увеличится и станет заметным.\nРаботает в сессии Ubuntu X11."
};
static const Lang L_EN = {
    "Settings…", "Autostart: ON ✓", "Autostart: OFF", "About…", "Quit",
    "Mouse Finder — Settings", "Cancel", "Apply",
    "Cursor size (px)", "Duration (ms)", "Reversals to detect", "Min speed (px/s)",
    "Shake the mouse — the cursor enlarges to help you spot it.\nRequires an Ubuntu X11 session."
};
static const Lang *L = &L_EN;

static void lang_init(void) {
    const char *loc = g_getenv("LANG");
    if (!loc) loc = g_getenv("LANGUAGE");
    if (!loc) loc = "";
    if (strncmp(loc, "ru", 2) == 0) L = &L_RU;
}

/* ── Theme ───────────────────────────────────────────────────────────────── */
static void apply_color_scheme(GSettings *gs) {
    gchar *scheme = g_settings_get_string(gs, "color-scheme");
    g_object_set(gtk_settings_get_default(),
                 "gtk-application-prefer-dark-theme",
                 (gboolean)(scheme && strstr(scheme, "dark")),
                 NULL);
    g_free(scheme);
}

static void on_color_scheme_changed(GSettings *gs, const char *key, gpointer unused) {
    (void)key; (void)unused;
    apply_color_scheme(gs);
}

static void theme_init(void) {
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    if (!g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE))
        return;
    GSettings *gs = g_settings_new("org.gnome.desktop.interface");
    apply_color_scheme(gs);
    /* ponytail: gs intentionally not unref'd — lives for process lifetime to keep signal active */
    g_signal_connect(gs, "changed::color-scheme", G_CALLBACK(on_color_scheme_changed), NULL);
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

    /* X11 protocol MotionNotify: byte 0 = type, bytes 20-21 = root-x (LE int16) */
    if ((data->data[0] & 0x7F) != MotionNotify) goto done;

    App *app = (App *)priv;
    gint16 rx = (gint16)(data->data[20] | (unsigned)(data->data[21] << 8));
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

/* ── Overlay / ungrab ────────────────────────────────────────────────────── */
static gboolean on_shake(gpointer ud) {
    App *app = (App *)ud;
    if (app->grabbed) return G_SOURCE_REMOVE;

    /* ponytail: InputOnly overlay instead of XGrabPointer —
       XGrabPointer fails when GNOME Shell holds its own grab;
       InputOnly window sets cursor without stealing input */
    int scr = DefaultScreen(app->grab_dpy);
    XSetWindowAttributes attr = {0};
    attr.override_redirect = True;
    attr.cursor = app->big_cursor;

    int w = XDisplayWidth(app->grab_dpy, scr);
    int h = XDisplayHeight(app->grab_dpy, scr);

    app->overlay = XCreateWindow(
        app->grab_dpy, app->root,
        0, 0, (unsigned)w, (unsigned)h,
        0, CopyFromParent, InputOnly, CopyFromParent,
        CWOverrideRedirect | CWCursor, &attr);
    XMapRaised(app->grab_dpy, app->overlay);
    XFlush(app->grab_dpy);

    app->grabbed = TRUE;
    g_timeout_add((guint)app->cfg.show_duration, ungrab, app);
    return G_SOURCE_REMOVE;
}

static gboolean ungrab(gpointer ud) {
    App *app = (App *)ud;
    XDestroyWindow(app->grab_dpy, app->overlay);
    XFlush(app->grab_dpy);
    app->overlay = 0;
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
        L->dlg_title, NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        L->dlg_cancel, GTK_RESPONSE_CANCEL,
        L->dlg_apply,  GTK_RESPONSE_OK, NULL);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_container_add(
        GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), grid);

    Settings ns = app->cfg;
    struct { const char *lbl; int *val; double lo, hi, step; } rows[] = {
        {L->lbl_cursor,    &ns.cursor_size,   32,  256,  32},
        {L->lbl_duration,  &ns.show_duration, 500, 3000, 100},
        {L->lbl_reversals, &ns.min_reversals, 2,   6,    1 },
        {L->lbl_speed,     &ns.min_speed,     100, 1000, 50},
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

/* ── Tray icon ───────────────────────────────────────────────────────────── */
/*
 * Design: black cursor arrow with white outline (readable on light/dark panels)
 * + 3 horizontal motion lines left of the cursor (shake indicator, fading).
 * Rendered at runtime via Cairo → GdkPixbuf; no external image files needed.
 */
static GdkPixbuf *tray_icon_pixbuf(int size) {
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surf);

    /* s: cursor coord space is ~14×25 units; scale to leave left margin for lines */
    double s  = size / 27.0;
    double dx = 7.5;   /* cursor x-offset in cursor units → clears motion lines */

    /* ── Motion lines (shake indicator) ──────────────────────────────── */
    /* 3 horizontal strokes, longest/most-opaque at top, fading downward  */
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, 1.5 * s);
    const struct { double y, x2, a; } ml[] = {
        { 5.5, 5.5, 0.80 },
        { 9.5, 4.0, 0.50 },
        {13.5, 2.5, 0.25 },
    };
    for (int i = 0; i < 3; i++) {
        cairo_set_source_rgba(cr, 0, 0, 0, ml[i].a);
        cairo_move_to(cr, 0.5  * s, ml[i].y * s);
        cairo_line_to(cr, ml[i].x2 * s, ml[i].y * s);
        cairo_stroke(cr);
    }

    /* ── Arrow cursor ─────────────────────────────────────────────────── */
    /* Shadow */
    cairo_set_source_rgba(cr, 0, 0, 0, 0.20);
    arrow_path(cr, s, dx + 0.8, 0.8);
    cairo_fill(cr);

    /* Black outline: stroke + fill first, then overwrite body with white */
    cairo_set_source_rgba(cr, 0, 0, 0, 1);
    arrow_path(cr, s, dx, 0.0);
    cairo_set_line_width(cr, 2.0 * s);
    cairo_stroke_preserve(cr);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    arrow_path(cr, s, dx, 0.0);
    cairo_fill(cr);

    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(surf, 0, 0, size, size);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return pb;
}

/* ── Tray ────────────────────────────────────────────────────────────────── */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

static void show_about(App *app) {
    (void)app;
    GtkWidget *dlg = gtk_about_dialog_new();
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dlg), "Mouse Finder");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dlg), "1.0");
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dlg), L->about_comments);
    gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dlg), "© 2026 Alexander Rozhkov");
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dlg), GTK_LICENSE_MIT_X11);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void on_menu_deactivate(GtkMenuShell *shell, App *app) {
    (void)shell;
    app->active_menu    = NULL;
    app->menu_closed_ms = g_get_monotonic_time() / 1000;
}

static void tray_popup(GtkStatusIcon *icon, guint btn, guint t, App *app) {
    /* Toggle: if menu is open, close it */
    if (app->active_menu) {
        gtk_menu_popdown(GTK_MENU(app->active_menu));
        return;
    }
    /* Don't reopen when click caused the menu to close (< 300 ms ago) */
    if (g_get_monotonic_time() / 1000 - app->menu_closed_ms < 300) return;

    GtkWidget *menu = gtk_menu_new();

    GtkWidget *i_cfg = gtk_menu_item_new_with_label(L->menu_settings);
    g_signal_connect_swapped(i_cfg, "activate", G_CALLBACK(show_settings), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i_cfg);

    const char *albl = autostart_enabled() ? L->menu_autostart_on : L->menu_autostart_off;
    GtkWidget *i_auto = gtk_menu_item_new_with_label(albl);
    g_signal_connect_swapped(i_auto, "activate", G_CALLBACK(toggle_autostart), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i_auto);

    GtkWidget *i_about = gtk_menu_item_new_with_label(L->menu_about);
    g_signal_connect_swapped(i_about, "activate", G_CALLBACK(show_about), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i_about);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *i_quit = gtk_menu_item_new_with_label(L->menu_quit);
    g_signal_connect(i_quit, "activate", G_CALLBACK(gtk_main_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i_quit);

    g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_deactivate), app);
    app->active_menu = menu;

    gtk_widget_show_all(menu);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL,
                   gtk_status_icon_position_menu, icon, btn, t);
}

static void tray_activate(GtkStatusIcon *icon, App *app) {
    tray_popup(icon, 1, gtk_get_current_event_time(), app);
}

G_GNUC_END_IGNORE_DEPRECATIONS

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    XInitThreads();
    gtk_init(&argc, &argv);
    lang_init();
    theme_init();

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
    GdkPixbuf *icon = tray_icon_pixbuf(22);
    app.tray = gtk_status_icon_new_from_pixbuf(icon);
    g_object_unref(icon);
    gtk_status_icon_set_tooltip_text(app.tray, "Mouse Finder — shake to find cursor");
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_signal_connect(app.tray, "popup-menu", G_CALLBACK(tray_popup),   &app);
    g_signal_connect(app.tray, "activate",   G_CALLBACK(tray_activate), &app);

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
