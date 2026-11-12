/* mouse_finder.c — Shake-to-find-mouse for Ubuntu (X11 and Wayland)
 * Copyright (c) 2026 Alexander Rozhkov — MIT License
 * Build: make
 */

/* X11 headers before GTK to avoid None/Cursor conflicts */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xcursor/Xcursor.h>
#include <cairo/cairo.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/input.h>

/* ── Constants ──────────────────────────────────────────────────────────── */
#define SAMPLE_MAX     256  /* ~2 s at 125 Hz, must cover WINDOW_MS */
#define WINDOW_MS      800
#define COOLDOWN_MS    1000
#define MAX_MOUSE_DEVS 16

/* ── Types ───────────────────────────────────────────────────────────────── */
typedef struct { int cursor_size, show_duration, min_reversals, min_speed; } Settings;
typedef struct { gint64 t_ms; gint32 x; }                                    Sample;

typedef struct App {
    /* X11 (cursor + overlay only — input no longer read via X11) */
    Display        *grab_dpy;
    Window          root;
    Window          overlay;
    Cursor          big_cursor;
    gboolean        grabbed;
    guint           ungrab_id;      /* pending ungrab timeout, 0 if none */
    /* Settings */
    Settings        cfg;
    volatile gint   atm_min_speed;  /* read by record thread via g_atomic */
    volatile gint   atm_min_reversals;
    /* Shake state — only accessed by record thread */
    Sample          samples[SAMPLE_MAX];
    int             sample_head;
    int             sample_count;
    gint64          last_shake_ms;
    gint32          x_acc;          /* running relative-X position, evdev */
    /* evdev mouse sources */
    struct pollfd   pfds[MAX_MOUSE_DEVS];  /* fd -1 = free slot, poll() skips it */
    int             pfd_ev[MAX_MOUSE_DEVS]; /* N of /dev/input/eventN in that slot */
    volatile gint   running;        /* g_atomic flag, set 0 to stop record thread */
    /* GNOME: enlarge via org.gnome.desktop.interface cursor-size (works on Wayland) */
    GSettings      *iface;
    int             saved_cursor_size;
    /* GTK */
    AppIndicator   *indicator;
    GtkWidget      *i_auto;         /* autostart menu item — label updated on toggle */
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
    *s = (Settings){ 128, 1500, 3, 300 };  /* ponytail: 3 reversals — touchpad shakes are slower than mouse (measured 2026-11-09) */
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
    const char *help_title, *help_close;
} Lang;

static const Lang L_RU = {
    "Настройки…", "Автостарт: ВКЛ ✓", "Автостарт: ВЫКЛ", "О программе…", "Выход",
    "Mouse Finder — Настройки", "Отмена", "Применить",
    "Размер курсора (px)", "Длительность (мс)", "Реверсов для тряски", "Мин. скорость (px/s)",
    "Встряхни мышью — курсор увеличится и станет заметным.",
    "Mouse Finder — Справка",
    "Закрыть"
};
static const Lang L_EN = {
    "Settings…", "Autostart: ON ✓", "Autostart: OFF", "About…", "Quit",
    "Mouse Finder — Settings", "Cancel", "Apply",
    "Cursor size (px)", "Duration (ms)", "Reversals to detect", "Min speed (px/s)",
    "Shake the mouse — the cursor enlarges to help you spot it.",
    "Mouse Finder — Help",
    "Close"
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

/* ponytail: pointer devices found via kernel's own "mouseN" legacy handler
   classification in /proc/bus/input/devices — avoids adding a libudev dep
   just to ask udev the same question. Opens only devices not already open;
   called periodically for hotplug (re-plugged / BT mice, resume). */
static int scan_mouse_devices(App *app) {
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (!f) return 0;

    char line[512], handlers[256] = "";
    int opened = 0;

    for (;;) {
        gboolean eof = !fgets(line, sizeof(line), f);
        if (eof || line[0] == '\n') {
            char *ev = handlers[0] ? strstr(handlers, "event") : NULL;
            if (ev) {
                int num = atoi(ev + 5), slot = -1;
                for (int i = 0; i < MAX_MOUSE_DEVS; i++) {
                    if (app->pfds[i].fd >= 0 && app->pfd_ev[i] == num) { slot = -2; break; }
                    if (app->pfds[i].fd < 0 && slot == -1) slot = i;
                }
                if (slot >= 0) {
                    char path[64];
                    snprintf(path, sizeof(path), "/dev/input/event%d", num);
                    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                    if (fd >= 0) {
                        app->pfds[slot] = (struct pollfd){ .fd = fd, .events = POLLIN };
                        app->pfd_ev[slot] = num;
                        opened++;
                    }
                }
            }
            handlers[0] = '\0';
            if (eof) break;
            continue;
        }
        if (strncmp(line, "H: Handlers=", 12) == 0 && strstr(line, "mouse"))
            g_strlcpy(handlers, line + 12, sizeof(handlers));
    }
    fclose(f);
    return opened;
}

static int open_mouse_count(const App *app) {
    int n = 0;
    for (int i = 0; i < MAX_MOUSE_DEVS; i++) n += app->pfds[i].fd >= 0;
    return n;
}

static void *record_thread(void *arg) {
    App *app = (App *)arg;
    struct pollfd *pfds = app->pfds;
    gint64 next_scan = 0;

    while (g_atomic_int_get(&app->running)) {
        gint64 t = g_get_monotonic_time() / 1000;
        if (t >= next_scan) { scan_mouse_devices(app); next_scan = t + 2000; }

        if (poll(pfds, MAX_MOUSE_DEVS, 200) <= 0) continue;

        for (int i = 0; i < MAX_MOUSE_DEVS; i++) {
            if (pfds[i].fd < 0 || !pfds[i].revents) continue;
            /* Unplugged device: evdev reports POLLHUP|POLLERR forever. Without
               closing it, poll() returns at once on every call — 100% CPU. */
            if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                close(pfds[i].fd);
                pfds[i].fd = -1;
                continue;
            }
            struct input_event ev;
            ssize_t n;
            while ((n = read(pfds[i].fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
                /* ponytail: touchpads report ABS_X, mice REL_X; one shared x_acc,
                   buffer reset on finger-down hides the abs/rel jump. */
                if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 1) {
                    app->sample_count = 0; continue;
                }
                if (ev.type == EV_ABS && ev.code == ABS_X) app->x_acc = ev.value;
                else if (ev.type == EV_REL && ev.code == REL_X) app->x_acc += ev.value;
                else continue;

                gint64 now = g_get_monotonic_time() / 1000;
                int idx = app->sample_head;
                app->samples[idx] = (Sample){ now, app->x_acc };
                app->sample_head  = (idx + 1) % SAMPLE_MAX;
                if (app->sample_count < SAMPLE_MAX) app->sample_count++;

                if (detect_shake(app, now)) {
                    app->last_shake_ms = now;
                    g_idle_add(on_shake, app);
                }
            }
        }
    }
    return NULL;
}

static gboolean setup_record(App *app) {
    for (int i = 0; i < MAX_MOUSE_DEVS; i++) app->pfds[i].fd = -1;
    scan_mouse_devices(app);
    return open_mouse_count(app) > 0;
}

/* ── Overlay / ungrab ────────────────────────────────────────────────────── */
/* Enlargement has two layers:
   1. GNOME cursor-size: cursors the compositor draws itself (Shell UI, window
      frames, cursor-shape-v1 clients).
   2. Fullscreen transparent override-redirect XWayland window with our big
      cursor, stacked above all windows. Apps that draw their own cursor re-read
      cursor-size only when the pointer re-enters them (bug 2026-11-12: "big only
      after touching the window border"); under the overlay the pointer is over
      our window, so our cursor shows everywhere.
   ponytail: clicks during show_duration hit the overlay; the first click closes
   it early and is lost. Input pass-through would also drop our cursor. */
static Window overlay_create(App *app) {
    Display *d = app->grab_dpy;
    XVisualInfo vi;
    if (!XMatchVisualInfo(d, DefaultScreen(d), 32, TrueColor, &vi)) return 0;
    XSetWindowAttributes a = {0};
    a.override_redirect = True;
    a.colormap     = XCreateColormap(d, app->root, vi.visual, AllocNone);
    a.cursor       = app->big_cursor;
    a.event_mask   = ButtonPressMask;
    return XCreateWindow(d, app->root, 0, 0, 1, 1, 0, 32, InputOutput, vi.visual,
                         CWOverrideRedirect | CWColormap | CWBackPixel | CWBorderPixel |
                         CWCursor | CWEventMask, &a);
}

static gboolean on_x_event(gint fd, GIOCondition cond, gpointer ud) {
    App *app = (App *)ud;
    gboolean clicked = FALSE;
    (void)fd; (void)cond;
    while (XPending(app->grab_dpy)) {
        XEvent ev;
        XNextEvent(app->grab_dpy, &ev);
        if (ev.type == ButtonPress) clicked = TRUE;
    }
    if (clicked && app->grabbed) {
        if (g_getenv("MOUSE_FINDER_DEBUG")) g_printerr("overlay click\n");
        g_source_remove(app->ungrab_id);
        ungrab(app);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_shake(gpointer ud) {
    App *app = (App *)ud;
    if (g_getenv("MOUSE_FINDER_DEBUG")) g_printerr("shake detected\n");
    if (app->grabbed) return G_SOURCE_REMOVE;

    if (app->iface) {
        /* ponytail: if saved == ours, a previous run died mid-shake: fall back to GNOME default. */
        app->saved_cursor_size = g_settings_get_int(app->iface, "cursor-size");
        if (app->saved_cursor_size == app->cfg.cursor_size) app->saved_cursor_size = 24;
        g_settings_set_int(app->iface, "cursor-size", app->cfg.cursor_size);
    }
    if (app->overlay) {
        /* root geometry fetched each time: follows monitor hotplug, unlike XDisplayWidth */
        Window r; int x, y; unsigned w, h, bw, depth;
        XGetGeometry(app->grab_dpy, app->root, &r, &x, &y, &w, &h, &bw, &depth);
        XMoveResizeWindow(app->grab_dpy, app->overlay, 0, 0, w, h);
        XMapRaised(app->grab_dpy, app->overlay);
        XFlush(app->grab_dpy);
    }
    app->grabbed   = TRUE;
    app->ungrab_id = g_timeout_add((guint)app->cfg.show_duration, ungrab, app);
    return G_SOURCE_REMOVE;
}

static gboolean ungrab(gpointer ud) {
    App *app = (App *)ud;
    if (app->iface) g_settings_set_int(app->iface, "cursor-size", app->saved_cursor_size);
    if (app->overlay) {
        XUnmapWindow(app->grab_dpy, app->overlay);
        XFlush(app->grab_dpy);
    }
    app->grabbed   = FALSE;
    app->ungrab_id = 0;
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

/* Label set right after toggling: the AppIndicator menu is rendered by the shell
   over D-Bus, so GtkMenu "show" never fires (bug 2026-11-12: label stuck at ON). */
static void update_autostart_label(App *app) {
    gtk_menu_item_set_label(GTK_MENU_ITEM(app->i_auto),
                            autostart_enabled() ? L->menu_autostart_on : L->menu_autostart_off);
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
    update_autostart_label(app);
}

/* ── Settings dialog ─────────────────────────────────────────────────────── */
static void apply_settings(App *app, const Settings *s) {
    if (s->cursor_size != app->cfg.cursor_size) {
        Cursor old = app->big_cursor;
        app->big_cursor = cursor_create(app->grab_dpy, s->cursor_size);
        if (app->overlay) XDefineCursor(app->grab_dpy, app->overlay, app->big_cursor);
        XFreeCursor(app->grab_dpy, old);
    }
    g_atomic_int_set(&app->atm_min_speed,     s->min_speed);
    g_atomic_int_set(&app->atm_min_reversals, s->min_reversals);
    app->cfg = *s;
}

/* ── Help: 7 languages, switchable inside the help window ─────────────── */
typedef struct { const char *name, *code, *text; } HelpLang;   /* text: Pango markup */
static const HelpLang HELP[] = {
  { "English", "en",
    "<b>How it works</b>\n"
    "Quickly move the mouse, or your finger on the touchpad, <b>left and right</b>. When a shake is detected, the cursor is enlarged for a moment so it is easy to spot. Only horizontal movement over the last 0.8 s is taken into account. After a trigger the next one is possible no sooner than 1 s later.\n"
    "\n"
    "<b>Cursor size (px)</b> — how big the cursor becomes while shown. A normal cursor is 24 px. Larger is easier to spot, especially on big or 4K screens. Default 128.\n"
    "\n"
    "<b>Duration (ms)</b> — how long the cursor stays enlarged before returning to normal size. 1000 ms = 1 s. Default 1500.\n"
    "\n"
    "<b>Reversals to detect</b> — how many times the movement must change direction (left→right or right→left) within 0.8 s to count as a shake. Fewer triggers more easily but also by accident during normal work; more needs a more vigorous shake. 2 suits a touchpad, 3 a mouse. Default 3.\n"
    "\n"
    "<b>Min speed (px/s)</b> — movement slower than this is not counted as shaking, so ordinary cursor movement does not trigger it. If movement stays slow for more than 0.2 s, the direction-change count starts over. Speed is measured in the device's own units (sensor counts for a mouse, touchpad coordinates for a touchpad), not screen pixels, so the right value depends on the device. Lower triggers on slower shakes, but also more often by accident. Default 300.\n"
    "\n"
    "<b>Tuning</b>: if shaking does not trigger, lower Reversals or Min speed; if it triggers by accident, raise them.\n"
    "\n"
    "Settings are stored in ~/.config/mouse-finder.ini." },
  { "Русский", "ru",
    "<b>Как это работает</b>\n"
    "Быстро поводите мышью или пальцем по тачпаду <b>влево-вправо</b>. Когда программа замечает тряску, курсор на время увеличивается, чтобы его было легко найти на экране. Учитывается только горизонтальное движение и только последние 0,8 с. После срабатывания следующее возможно не раньше чем через 1 с.\n"
    "\n"
    "<b>Размер курсора (px)</b> — каким станет курсор во время показа. Обычный курсор — 24 px. Чем больше значение, тем заметнее курсор, особенно на большом или 4K-мониторе. По умолчанию 128.\n"
    "\n"
    "<b>Длительность (мс)</b> — сколько миллисекунд курсор остаётся увеличенным, потом возвращается к обычному размеру. 1000 мс = 1 с. По умолчанию 1500.\n"
    "\n"
    "<b>Реверсов для тряски</b> — сколько раз движение должно сменить направление (влево→вправо или вправо→влево) за 0,8 с, чтобы это считалось тряской. Меньше — срабатывает легче, но чаще случайно при обычной работе; больше — нужна более энергичная тряска. Для тачпада удобно 2, для мыши — 3. По умолчанию 3.\n"
    "\n"
    "<b>Мин. скорость (px/s)</b> — движения медленнее этой скорости тряской не считаются, так что обычное перемещение курсора её не вызывает. Если движение замедлилось дольше чем на 0,2 с, счёт смен направления начинается заново. Скорость меряется в единицах самого устройства (у мыши — шаги датчика, у тачпада — его координаты), а не в пикселях экрана, поэтому значение зависит от модели. Меньше — срабатывает от более медленной тряски, но и чаще случайно. По умолчанию 300.\n"
    "\n"
    "<b>Подбор</b>: если тряска не срабатывает, уменьшите «Реверсов» или «Мин. скорость»; если срабатывает случайно — увеличьте их.\n"
    "\n"
    "Настройки хранятся в ~/.config/mouse-finder.ini." },
  { "中文", "zh",
    "<b>工作原理</b>\n"
    "快速<b>左右</b>移动鼠标，或在触控板上左右滑动手指。检测到晃动后，光标会暂时放大，便于在屏幕上找到它。只统计水平方向的移动，且只看最近 0.8 秒。触发一次后，至少 1 秒后才能再次触发。\n"
    "\n"
    "<b>光标大小</b>（Cursor size，px）— 显示时光标放大到的尺寸。普通光标为 24 px。数值越大越醒目，在大屏或 4K 显示器上尤其有用。默认 128。\n"
    "\n"
    "<b>持续时间</b>（Duration，ms）— 光标保持放大的毫秒数，之后恢复正常大小。1000 ms = 1 秒。默认 1500。\n"
    "\n"
    "<b>反向次数</b>（Reversals to detect）— 在 0.8 秒内移动方向需要改变多少次（左→右或右→左）才算晃动。数值越小越容易触发，但正常操作时也更容易误触发；数值越大，需要晃动得越用力。触控板建议 2，鼠标建议 3。默认 3。\n"
    "\n"
    "<b>最低速度</b>（Min speed，px/s）— 低于此速度的移动不计为晃动，因此普通的光标移动不会触发。如果移动变慢超过 0.2 秒，方向变化的计数会重新开始。速度以设备自身的单位计量（鼠标为传感器计数，触控板为其坐标），而不是屏幕像素，因此合适的数值取决于具体设备。数值越小，较慢的晃动也能触发，但误触发也会更多。默认 300。\n"
    "\n"
    "<b>调整建议</b>：如果晃动没有触发，请降低“反向次数”或“最低速度”；如果出现误触发，请提高它们。\n"
    "\n"
    "设置保存在 ~/.config/mouse-finder.ini。" },
  { "Español", "es",
    "<b>Cómo funciona</b>\n"
    "Mueva rápidamente el ratón, o el dedo sobre el panel táctil, <b>de izquierda a derecha</b>. Cuando se detecta una sacudida, el cursor se agranda un momento para que sea fácil de encontrar. Solo se tiene en cuenta el movimiento horizontal de los últimos 0,8 s. Tras una activación, la siguiente no puede producirse antes de 1 s.\n"
    "\n"
    "<b>Tamaño del cursor</b> (Cursor size, px) — tamaño que alcanza el cursor mientras se muestra. Un cursor normal mide 24 px. Cuanto mayor, más visible, sobre todo en pantallas grandes o 4K. Por defecto 128.\n"
    "\n"
    "<b>Duración</b> (Duration, ms) — cuántos milisegundos permanece agrandado el cursor antes de volver a su tamaño normal. 1000 ms = 1 s. Por defecto 1500.\n"
    "\n"
    "<b>Cambios de dirección</b> (Reversals to detect) — cuántas veces debe cambiar de dirección el movimiento (izquierda→derecha o derecha→izquierda) en 0,8 s para contar como sacudida. Con menos se activa más fácilmente, pero también por accidente durante el trabajo normal; con más hace falta una sacudida más enérgica. 2 va bien para un panel táctil, 3 para un ratón. Por defecto 3.\n"
    "\n"
    "<b>Velocidad mínima</b> (Min speed, px/s) — el movimiento más lento que esto no cuenta como sacudida, así que mover el cursor con normalidad no la activa. Si el movimiento se mantiene lento más de 0,2 s, el recuento de cambios de dirección vuelve a empezar. La velocidad se mide en unidades propias del dispositivo (pasos del sensor en un ratón, coordenadas en un panel táctil), no en píxeles de pantalla, por lo que el valor adecuado depende del dispositivo. Un valor menor se activa con sacudidas más lentas, pero también más a menudo por accidente. Por defecto 300.\n"
    "\n"
    "<b>Ajuste</b>: si la sacudida no se activa, reduzca «Cambios de dirección» o «Velocidad mínima»; si se activa por accidente, auméntelos.\n"
    "\n"
    "La configuración se guarda en ~/.config/mouse-finder.ini." },
  { "Français", "fr",
    "<b>Fonctionnement</b>\n"
    "Déplacez rapidement la souris, ou le doigt sur le pavé tactile, <b>de gauche à droite</b>. Lorsqu'une secousse est détectée, le curseur est agrandi un instant pour être facile à repérer. Seul le mouvement horizontal des 0,8 dernières secondes est pris en compte. Après un déclenchement, le suivant n'est possible qu'au bout d'une seconde.\n"
    "\n"
    "<b>Taille du curseur</b> (Cursor size, px) — taille du curseur pendant l'affichage. Un curseur normal fait 24 px. Plus il est grand, plus il est visible, surtout sur un grand écran ou un écran 4K. Par défaut 128.\n"
    "\n"
    "<b>Durée</b> (Duration, ms) — nombre de millisecondes pendant lesquelles le curseur reste agrandi avant de reprendre sa taille normale. 1000 ms = 1 s. Par défaut 1500.\n"
    "\n"
    "<b>Changements de direction</b> (Reversals to detect) — nombre de fois où le mouvement doit changer de sens (gauche→droite ou droite→gauche) en 0,8 s pour compter comme une secousse. Moins : déclenchement plus facile, mais aussi accidentel pendant le travail normal ; plus : il faut secouer plus énergiquement. 2 convient à un pavé tactile, 3 à une souris. Par défaut 3.\n"
    "\n"
    "<b>Vitesse minimale</b> (Min speed, px/s) — un mouvement plus lent n'est pas compté comme une secousse ; déplacer normalement le curseur ne la déclenche donc pas. Si le mouvement reste lent plus de 0,2 s, le comptage des changements de direction repart de zéro. La vitesse est mesurée dans les unités propres au périphérique (pas du capteur pour une souris, coordonnées pour un pavé tactile), et non en pixels d'écran : la bonne valeur dépend donc du périphérique. Plus bas : déclenchement avec des secousses plus lentes, mais aussi plus souvent par accident. Par défaut 300.\n"
    "\n"
    "<b>Réglage</b> : si la secousse ne se déclenche pas, baissez « Changements de direction » ou « Vitesse minimale » ; si elle se déclenche par accident, augmentez-les.\n"
    "\n"
    "Les réglages sont enregistrés dans ~/.config/mouse-finder.ini." },
  { "Deutsch", "de",
    "<b>So funktioniert es</b>\n"
    "Bewegen Sie die Maus oder den Finger auf dem Touchpad schnell <b>nach links und rechts</b>. Wird ein Schütteln erkannt, wird der Mauszeiger kurz vergrößert, damit er leicht zu finden ist. Berücksichtigt wird nur die waagerechte Bewegung der letzten 0,8 s. Nach einer Auslösung ist die nächste frühestens nach 1 s möglich.\n"
    "\n"
    "<b>Zeigergröße</b> (Cursor size, px) — wie groß der Zeiger während der Anzeige wird. Ein normaler Zeiger ist 24 px groß. Je größer, desto auffälliger, besonders auf großen oder 4K-Bildschirmen. Standard 128.\n"
    "\n"
    "<b>Dauer</b> (Duration, ms) — wie viele Millisekunden der Zeiger vergrößert bleibt, bevor er zur normalen Größe zurückkehrt. 1000 ms = 1 s. Standard 1500.\n"
    "\n"
    "<b>Richtungswechsel</b> (Reversals to detect) — wie oft die Bewegung innerhalb von 0,8 s die Richtung wechseln muss (links→rechts oder rechts→links), damit es als Schütteln gilt. Weniger löst leichter aus, aber auch versehentlich bei normaler Arbeit; mehr erfordert kräftigeres Schütteln. 2 passt für ein Touchpad, 3 für eine Maus. Standard 3.\n"
    "\n"
    "<b>Mindestgeschwindigkeit</b> (Min speed, px/s) — langsamere Bewegungen zählen nicht als Schütteln, normales Bewegen des Zeigers löst also nichts aus. Bleibt die Bewegung länger als 0,2 s langsam, beginnt die Zählung der Richtungswechsel von vorn. Die Geschwindigkeit wird in den eigenen Einheiten des Geräts gemessen (Sensorschritte bei einer Maus, Koordinaten bei einem Touchpad), nicht in Bildschirmpixeln; der passende Wert hängt daher vom Gerät ab. Niedriger löst schon bei langsamerem Schütteln aus, aber auch häufiger versehentlich. Standard 300.\n"
    "\n"
    "<b>Einstellen</b>: Löst das Schütteln nicht aus, senken Sie „Richtungswechsel“ oder „Mindestgeschwindigkeit“; löst es versehentlich aus, erhöhen Sie sie.\n"
    "\n"
    "Die Einstellungen werden in ~/.config/mouse-finder.ini gespeichert." },
  { "Italiano", "it",
    "<b>Come funziona</b>\n"
    "Muovi rapidamente il mouse, o il dito sul touchpad, <b>a destra e a sinistra</b>. Quando viene rilevato uno scuotimento, il cursore viene ingrandito per un momento, così è facile individuarlo. Conta solo il movimento orizzontale degli ultimi 0,8 s. Dopo un'attivazione, la successiva è possibile non prima di 1 s.\n"
    "\n"
    "<b>Dimensione del cursore</b> (Cursor size, px) — quanto diventa grande il cursore durante la visualizzazione. Un cursore normale è di 24 px. Più è grande, più è visibile, soprattutto su schermi grandi o 4K. Predefinito 128.\n"
    "\n"
    "<b>Durata</b> (Duration, ms) — per quanti millisecondi il cursore resta ingrandito prima di tornare alle dimensioni normali. 1000 ms = 1 s. Predefinito 1500.\n"
    "\n"
    "<b>Cambi di direzione</b> (Reversals to detect) — quante volte il movimento deve cambiare direzione (sinistra→destra o destra→sinistra) entro 0,8 s per essere considerato uno scuotimento. Meno: si attiva più facilmente, ma anche per errore durante il lavoro normale; più: serve uno scuotimento più energico. 2 è adatto a un touchpad, 3 a un mouse. Predefinito 3.\n"
    "\n"
    "<b>Velocità minima</b> (Min speed, px/s) — i movimenti più lenti non contano come scuotimento, quindi il normale spostamento del cursore non lo attiva. Se il movimento resta lento per più di 0,2 s, il conteggio dei cambi di direzione ricomincia da capo. La velocità è misurata nelle unità proprie del dispositivo (passi del sensore per un mouse, coordinate per un touchpad), non in pixel dello schermo, quindi il valore giusto dipende dal dispositivo. Più basso: si attiva anche con scuotimenti più lenti, ma anche più spesso per errore. Predefinito 300.\n"
    "\n"
    "<b>Regolazione</b>: se lo scuotimento non si attiva, abbassa «Cambi di direzione» o «Velocità minima»; se si attiva per errore, aumentali.\n"
    "\n"
    "Le impostazioni sono salvate in ~/.config/mouse-finder.ini." },
};
#define N_HELP ((int)(sizeof(HELP) / sizeof(HELP[0])))

static void on_help_lang(GtkComboBox *cb, gpointer lbl) {
    /* set_markup, not a "%s" markup constructor: those escape the argument */
    gtk_label_set_markup(GTK_LABEL(lbl), HELP[gtk_combo_box_get_active(cb)].text);
}

static void show_help(GtkWindow *parent) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        L->help_title, parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        L->help_close, GTK_RESPONSE_CLOSE, NULL);
    GtkBox *box = GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg)));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_set_spacing(box, 8);

    const char *loc = g_getenv("LANG");
    if (!loc) loc = "";
    int cur = 0;
    GtkWidget *cb = gtk_combo_box_text_new();
    for (int i = 0; i < N_HELP; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb), HELP[i].name);
        if (strncmp(loc, HELP[i].code, 2) == 0) cur = i;
    }
    gtk_widget_set_halign(cb, GTK_ALIGN_END);

    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 70);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_label_set_yalign(GTK_LABEL(lbl), 0);
    gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sw, 580, 460);
    gtk_container_add(GTK_CONTAINER(sw), lbl);

    gtk_box_pack_start(box, cb, FALSE, FALSE, 0);
    gtk_box_pack_start(box, sw, TRUE, TRUE, 0);
    g_signal_connect(cb, "changed", G_CALLBACK(on_help_lang), lbl);
    gtk_combo_box_set_active(GTK_COMBO_BOX(cb), cur);   /* fires "changed" → fills label */

    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void show_settings(App *app) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        L->dlg_title, NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        L->dlg_cancel, GTK_RESPONSE_CANCEL,
        L->dlg_apply,  GTK_RESPONSE_OK, NULL);
    gtk_widget_set_tooltip_text(
        gtk_dialog_add_button(GTK_DIALOG(dlg), "?", GTK_RESPONSE_HELP), L->help_title);

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

    int resp;
    while ((resp = gtk_dialog_run(GTK_DIALOG(dlg))) == GTK_RESPONSE_HELP)
        show_help(GTK_WINDOW(dlg));
    if (resp == GTK_RESPONSE_OK) {
        for (int i = 0; i < n; i++)
            *rows[i].val = (int)gtk_range_get_value(GTK_RANGE(scales[i]));
        apply_settings(app, &ns);
        settings_save(&app->cfg);
    }
    gtk_widget_destroy(dlg);
}

/* ── Tray icon ───────────────────────────────────────────────────────────── */

/* Nova "cursor-arrow-1" solid icon, white, 22×22 RGBA PNG, embedded at compile time */
static const unsigned char TRAY_ICON_PNG[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,
    0x44,0x52,0x00,0x00,0x00,0x16,0x00,0x00,0x00,0x16,0x08,0x06,0x00,0x00,
    0x00,0xc4,0xb4,0x6c,0x3b,0x00,0x00,0x01,0x68,0x49,0x44,0x41,0x54,0x38,
    0xcb,0xed,0x94,0x4d,0x2b,0x45,0x51,0x14,0x86,0x9f,0xcb,0x2d,0x63,0x23,
    0xf9,0x1c,0x51,0x3e,0xee,0xc8,0x0f,0x60,0xa2,0xf8,0x07,0xca,0xc0,0x0f,
    0xf0,0x5b,0x4c,0x0c,0xfc,0x09,0x52,0x32,0x10,0x29,0xdc,0x81,0x01,0x85,
    0x7c,0xc4,0x40,0x99,0xf8,0x88,0x52,0x37,0x23,0x29,0xd7,0x63,0x60,0x1d,
    0x9d,0x4e,0xce,0xbd,0x87,0x91,0x81,0xb7,0x76,0x9d,0xb5,0xdb,0xe7,0xd9,
    0xef,0x5e,0x6b,0xed,0x0d,0xff,0xca,0x4a,0x45,0xed,0x53,0x27,0xd5,0x4e,
    0xb5,0xac,0xfe,0x9a,0x57,0xce,0xc4,0x43,0xc0,0x3c,0x70,0x0c,0x5c,0x02,
    0xd7,0xea,0x29,0x70,0x0d,0xbc,0x02,0x94,0x4a,0xa5,0x5f,0x81,0x4f,0x81,
    0x23,0x60,0x36,0xe2,0x1a,0x70,0x08,0x5c,0x00,0x4f,0x40,0x55,0x3d,0x03,
    0x6a,0xcd,0x36,0xc8,0x82,0x1f,0xc2,0x69,0xa2,0x76,0x60,0x22,0x46,0x1d,
    0x38,0x07,0xae,0x80,0x3b,0x75,0x1f,0xd8,0x03,0xee,0x80,0x7a,0xee,0x46,
    0x91,0x63,0xd4,0x69,0xf5,0xd9,0xc6,0x7a,0x53,0x6f,0xd5,0x5d,0x75,0x5d,
    0x9d,0xca,0xd6,0xa3,0x25,0xf9,0x48,0xed,0x78,0x06,0x1c,0x34,0x49,0x61,
    0x2b,0xd0,0x0d,0x8c,0x03,0x2f,0x91,0xaa,0x7c,0x85,0xe3,0x36,0x75,0xc1,
    0x62,0x5a,0x51,0xbb,0xbe,0xeb,0x9e,0xf2,0x37,0xfc,0xd7,0x28,0x54,0x33,
    0xad,0x01,0x73,0xc0,0x63,0x81,0xb5,0x5f,0xae,0xc7,0xd5,0x93,0x06,0x4e,
    0xcf,0xd5,0xd1,0xa4,0x2e,0x3f,0x01,0xb7,0xab,0x4b,0x4d,0xd2,0x50,0x55,
    0x07,0xf3,0xe0,0x2d,0x39,0xfc,0x1a,0x70,0x9f,0x99,0x7b,0xe3,0xb3,0xdd,
    0xde,0x23,0x1e,0x03,0x16,0x81,0xe1,0xc4,0x50,0x51,0xd7,0x33,0xea,0x4d,
    0xb8,0xab,0xab,0xab,0x71,0xfc,0x9d,0x8c,0xf3,0x4d,0xb5,0x52,0x28,0x2d,
    0xa9,0x77,0xa3,0x1a,0x3f,0x2f,0xab,0x1d,0x31,0x3f,0xa4,0x6e,0x65,0xe0,
    0x1b,0x85,0xe0,0xa9,0xcb,0xb2,0x9a,0x6e,0xa9,0xd4,0x18,0x09,0xa7,0x69,
    0x6d,0xab,0x03,0x45,0xe1,0x53,0xe1,0xfc,0x6b,0x71,0x0a,0x5e,0x09,0xa7,
    0x89,0x96,0xd4,0xde,0x1f,0x75,0x4a,0x83,0x13,0x55,0xc2,0xe9,0xb2,0xda,
    0x93,0x86,0x16,0x7b,0x03,0x73,0xe0,0xa1,0x7e,0x3e,0x2f,0xd5,0x4d,0xd1,
    0x27,0xf5,0x6f,0xea,0x03,0xa8,0x29,0x0f,0xa7,0xc6,0x72,0x07,0x89,0x00,
    0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82
};

static void setup_indicator_icon(void) {
    char *apps = g_build_filename(g_get_home_dir(), ".local", "share", "icons",
                                  "hicolor", "22x22", "apps", NULL);
    g_mkdir_with_parents(apps, 0755);
    char *file = g_build_filename(apps, "mf-icon.png", NULL);
    g_file_set_contents(file, (const gchar *)TRAY_ICON_PNG,
                        (gssize)sizeof(TRAY_ICON_PNG), NULL);
    g_free(file);
    g_free(apps);
}

/* ── Tray ────────────────────────────────────────────────────────────────── */

static void show_about(App *app) {
    (void)app;
    GtkWidget *dlg = gtk_about_dialog_new();
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dlg), "Mouse Finder");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dlg), "1.2");
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dlg), L->about_comments);
    gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dlg), "© 2026 Alexander Rozhkov");
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dlg), GTK_LICENSE_MIT_X11);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static GtkWidget *build_tray_menu(App *app) {
    GtkWidget *menu = gtk_menu_new();

    GtkWidget *i_cfg = gtk_menu_item_new_with_label(L->menu_settings);
    g_signal_connect_swapped(i_cfg, "activate", G_CALLBACK(show_settings), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i_cfg);

    app->i_auto = gtk_menu_item_new_with_label(L->menu_autostart_off);
    g_signal_connect_swapped(app->i_auto, "activate", G_CALLBACK(toggle_autostart), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), app->i_auto);

    GtkWidget *i_about = gtk_menu_item_new_with_label(L->menu_about);
    g_signal_connect_swapped(i_about, "activate", G_CALLBACK(show_about), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i_about);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *i_quit = gtk_menu_item_new_with_label(L->menu_quit);
    g_signal_connect(i_quit, "activate", G_CALLBACK(gtk_main_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i_quit);

    update_autostart_label(app);
    gtk_widget_show_all(menu);
    return menu;
}


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
    app.overlay    = overlay_create(&app);
    g_unix_fd_add(ConnectionNumber(app.grab_dpy), G_IO_IN, on_x_event, &app);
    if (g_settings_schema_source_lookup(g_settings_schema_source_get_default(),
                                        "org.gnome.desktop.interface", TRUE))
        app.iface = g_settings_new("org.gnome.desktop.interface");

    if (!setup_record(&app)) {
        g_printerr("No accessible mouse device found under /dev/input.\n"
                   "Check that the mouse-finder udev rule is installed "
                   "(re-run 'sudo apt install --reinstall ./mouse-finder_*.deb'\n"
                   "or add your user to the 'input' group and re-login), "
                   "then reconnect the mouse.\n");
        return 1;
    }
    g_atomic_int_set(&app.running, 1);
    pthread_create(&app.record_tid, NULL, record_thread, &app);

    setup_indicator_icon();
    char *icon_theme = g_build_filename(g_get_home_dir(), ".local", "share", "icons", NULL);
    app.indicator = app_indicator_new("mouse-finder", "mf-icon",
                                      APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_icon_theme_path(app.indicator, icon_theme);
    g_free(icon_theme);
    app_indicator_set_status(app.indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(app.indicator, GTK_MENU(build_tray_menu(&app)));

    gtk_main();

    /* Cleanup */
    if (app.grabbed) ungrab(&app);
    g_atomic_int_set(&app.running, 0);
    pthread_join(app.record_tid, NULL);
    for (int i = 0; i < MAX_MOUSE_DEVS; i++) if (app.pfds[i].fd >= 0) close(app.pfds[i].fd);
    XFreeCursor(app.grab_dpy, app.big_cursor);
    XCloseDisplay(app.grab_dpy);
    return 0;
}
