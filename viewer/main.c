/* stategate-view: the stategate app.
 *
 * Home screen: open or create a project folder, or pick a recent one.
 * Project screen, left two-thirds: the state machine drawn from the model's
 * saved layout. Right third (Clay): the steps of the flow, each a button
 * that runs the same `stategate` CLI command a person would type, the
 * command's output, the last gate result, a simulator and the step ladder.
 * The canvas is also an editor: states, transitions and labels drawn there
 * are written straight back to the model's YAML (src/medit.c), which stays
 * the source of truth; the canvas reloads whenever the file changes.
 * Step states come from `stategate status --json`, gate results from
 * .fsm/gate.json: the app keeps no status of its own. */

#define CLAY_IMPLEMENTATION
#include "third_party/clay/clay.h"
#include "third_party/clay/clay_renderer_raylib.c"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>

#include "cJSON.h"
#include "gate_status.h"
#include "runner.h"
#include "medit.h"
#include "model.h"
#include "validate.h"

#ifndef SG_RESOURCE_DIR
#define SG_RESOURCE_DIR "resources"
#endif

#define NODE_W 170.0f
#define MAX_GRID 256
#define FONT_BODY 0

/* ---- palette ---- */
static const Color BG = {244, 244, 241, 255};
static const Color GRID = {225, 225, 220, 255};
static const Color NODE_FILL = {255, 255, 255, 255};
static const Color NODE_EDGE = {150, 150, 145, 255};
static const Color INK = {32, 33, 36, 255};
static const Color QUIET = {110, 112, 118, 255};
static const Color ACCENT = {37, 99, 235, 255};
static const Color ACCENT_SOFT = {219, 231, 253, 255};
static const Color LOCKED = {180, 83, 9, 255};
static const Clay_Color C_PANEL = {255, 255, 255, 255};
static const Clay_Color C_CARD = {248, 248, 246, 255};
static const Clay_Color C_INK = {32, 33, 36, 255};
static const Clay_Color C_QUIET = {110, 112, 118, 255};
static const Clay_Color C_GREEN = {21, 128, 61, 255};
static const Clay_Color C_RED = {185, 28, 28, 255};
static const Clay_Color C_AMBER = {180, 83, 9, 255};
static const Clay_Color C_ACCENT = {37, 99, 235, 255};
static const Clay_Color C_BTN = {231, 236, 247, 255};
static const Clay_Color C_BTN_HOVER = {205, 217, 245, 255};
static const Clay_Color C_BTN_OFF = {238, 238, 235, 255};
static const Clay_Color C_RULE = {226, 226, 222, 255};

/* ---- app state ---- */

/* What `stategate status --json` reported. */
typedef struct {
    int ok; /* the status command ran */
    int git, has_commit, dirty, init, valid, generated, locked_items, signed_off;
    int t_draft, t_open, t_approved, t_active, t_done, t_locked;
    char current[64], current_status[16], gate[8];
} Status;

/* One ticket, as `stategate ticket list --json` printed it. */
typedef struct {
    char id[64], status[16];
    char *title, *goal, *step;
    StrList nodes, files, errors;
    int failures, max_failures;
} ViewTicket;

#define MAX_TICKETS 256

typedef enum { SCREEN_HOME, SCREEN_PROJECT } Screen;



typedef struct {
    Screen screen;
    char project[PATH_MAX];
    char sg[PATH_MAX]; /* the stategate CLI the buttons run */
    Status st;
    Job job;
    int modal;          /* a pty job (sign-off) is shown over the app */
    int inline_pty;     /* a pty job answered in the panel instead (ticket review) */
    size_t modal_from;  /* show output from here: the current question only */
    double status_at;

    /* home screen */
    char browse[PATH_MAX];
    StrList dirs, recent;
    char path_in[PATH_MAX], name_in[128];
    int focus; /* 0 none, 1 path field, 2 new-folder name */
    char home_msg[256];
    int show_output; /* scroll the panel to the output card next frame */
    float ui;        /* text and spacing scale: grows with the window, Ctrl +/- adjust */
    float ui_mult;   /* the person's Ctrl +/- factor on top */

    /* Your AI: a command the prompt is piped into, or empty to copy it. */
    int have_claude;     /* `claude` is on PATH */
    int copy_on_finish;  /* put the finished job's output on the clipboard */
    int ai_loop;         /* implementing: run the AI again for each next ticket */
    char ai_ticket[64];  /* the ticket the AI was last run on */
    char job_note[256];  /* shown with the output, e.g. "copied" */

    char *model_path;
    SgModel model;
    int have_model;
    int model_stale; /* the file does not parse: the canvas shows the last one that did */
    StrList model_errors;
    long long model_mtime; /* file_sig() of the model */
    time_t gate_mtime;
    GateStatus gate;

    int cur;              /* active state index in the simulator */
    int last_from, last_to;
    char last_event[64];
    char flash[160];
    StrList history;

    Vector2 pan;
    float zoom;
    Font font;
    int fit_pending; /* fit the view once the grid has been laid out */

    /* The canvas grid: the saved layout only orders the states into rows
     * and columns; Clay sizes the cells and keeps the gaps at any scale. */
    int col[MAX_GRID], row[MAX_GRID];
    int n_cols, n_rows;

    int sb_drag;      /* dragging the panel scrollbar */
    int step_view;    /* the step the steps card shows */
    int step_follow;  /* 1: it follows the current step */
    float sb_grab;    /* where on the thumb it was grabbed */

    /* Tickets, reloaded whenever a file in .fsm/tickets changes. */
    ViewTicket tickets[MAX_TICKETS];
    int n_tickets;
    long long tickets_sig;  /* changes when any ticket file does */
    char ticket_focus[64];  /* the ticket that last changed: the card follows it */
    int ticket_view;        /* the ticket the card shows */
    int ticket_follow;      /* 1: it follows the focus */
} App;

static App app;

/* ---- the canvas editor's state ---- */

typedef enum { IN_NONE, IN_NAME, IN_LABEL } InlineKind;

typedef struct {
    char sel[128];                    /* the selected state's id, or "" */
    char sel_from[128], sel_to[128];  /* the selected arrow, or "" */

    /* The left button went down on the canvas: on a node (move it), on its
     * dot (draw an arrow from it) or on empty space (pan). */
    int pressed, press_node, moving, linking, panning;
    Vector2 press_at, grab; /* grab: the mouse from the node's corner */
    double last_click;
    Vector2 last_at;

    /* The text field over a node (its name) or an arrow (its events). */
    InlineKind in;
    char buf[256];
    int fresh;                        /* the first key replaces the text */
    int just_opened;                  /* the key that opened it is not typed */
    char in_state[128];               /* IN_NAME */
    char in_from[128], in_to[128];    /* IN_LABEL */
    Rectangle in_box;                 /* where it was drawn last frame */

    char msg[256];                    /* the last edit's problem, or a note */
    char reveal[128];                 /* a state just made: bring it into view */
    char anchor[128];                 /* a state that stays put on screen across an edit */
    Vector2 anchor_at;
    char mover[128];                  /* the state an edit moves: not the anchor */
    char *undo[64];                   /* the model's text before each edit */
    int n_undo;
} Editor;

static Editor E;

/* Per-frame text arena: Clay keeps pointers to strings until render. */
static char arena[1 << 16];
static size_t arena_used;

static Clay_String cs(const char *s) {
    size_t n = strlen(s);
    if (arena_used + n + 1 > sizeof arena) n = sizeof arena - arena_used - 1;
    char *dst = arena + arena_used;
    /* The bundled font covers ASCII; replace anything else. */
    for (size_t i = 0; i < n; i++) dst[i] = (s[i] >= 32 && s[i] < 127) || s[i] == '\n' ? s[i] : '?';
    dst[n] = 0;
    arena_used += n + 1;
    return (Clay_String){.isStaticallyAllocated = false, .length = (int32_t)n, .chars = dst};
}

static Clay_String csf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static Clay_String csf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return cs(buf);
}

static time_t mtime_of(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

/* Changes with every write, even two in one second. */
static long long file_sig(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
#ifdef __APPLE__
    return (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec + (long long)st.st_size * 7;
#else
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec + (long long)st.st_size * 7;
#endif
}

/* ---- model + simulator ---- */

static int initial_index(const SgModel *m) {
    for (int i = 0; i < m->n_states; i++)
        if (m->states[i].initial) return i;
    return 0;
}

static int state_index(const SgModel *m, const char *id) {
    for (int i = 0; i < m->n_states; i++)
        if (!strcmp(m->states[i].id, id)) return i;
    return -1;
}

static void sim_reset(void) {
    app.cur = app.have_model ? initial_index(&app.model) : 0;
    app.last_from = app.last_to = -1;
    app.last_event[0] = 0;
    app.flash[0] = 0;
    for (int i = 0; i < app.history.n; i++) free(app.history.v[i]);
    app.history.n = 0;
}

static void sim_send(const char *event) {
    if (!app.have_model) return;
    const State *s = &app.model.states[app.cur];
    const char *target = state_target(s, event);
    if (!target) {
        snprintf(app.flash, sizeof app.flash, "%s ignores %s", s->name, event);
        return;
    }
    int to = state_index(&app.model, target);
    if (to < 0) {
        snprintf(app.flash, sizeof app.flash, "%s goes to %s, which is not a state", s->name, target);
        return;
    }
    sl_pushf(&app.history, "%s --%s--> %s", s->name, event, app.model.states[to].name);
    if (app.history.n > 8) {
        free(app.history.v[0]);
        memmove(app.history.v, app.history.v + 1, sizeof(char *) * (size_t)(app.history.n - 1));
        app.history.n--;
    }
    app.last_from = app.cur;
    app.last_to = to;
    snprintf(app.last_event, sizeof app.last_event, "%s", event);
    app.flash[0] = 0;
    app.cur = to;
}

static void fit_view(void);

static void grid_assign(void);

static void load_model(int first) {
    if (!app.model_path) return;
    SgModel m;
    StrList errors = {0};
    int parsed = model_load(app.model_path, &m, &errors) == 0;
    if (parsed) {
        check_statechart(&m, &errors);
        if (!errors.n) check_model_invariants(&m, &errors);
    }
    for (int i = 0; i < app.model_errors.n; i++) free(app.model_errors.v[i]);
    app.model_errors.n = 0;
    for (int i = 0; i < errors.n; i++) sl_push(&app.model_errors, errors.v[i]);
    /* A model that parses is drawn even while it breaks a rule (a state
     * just added is unreachable until an arrow reaches it); one that does
     * not parse leaves the last one on the canvas. */
    app.model_stale = !parsed && app.have_model;
    if (!parsed && app.have_model) return;

    /* Keep the simulator where it was when the active state still exists. */
    char keep[256] = "";
    if (!app.have_model) first = 1; /* the first good model in this folder: fit it */
    if (app.have_model) snprintf(keep, sizeof keep, "%s", app.model.states[app.cur].id);
    app.model = m; /* the old model leaks; reloads are rare and small */
    app.have_model = 1;
    int idx = *keep ? state_index(&app.model, keep) : -1;
    if (idx < 0) sim_reset();
    else { app.cur = idx; app.last_from = app.last_to = -1; }
    grid_assign();
    if (first) app.fit_pending = 1;
}

static void tickets_poll(void);
static long long tickets_signature(void);
static void tickets_reload(int follow_changes);

static void undo_clear(void);

static void poll_files(void) {
    tickets_poll();
    if (!app.model_path) {
        /* The person is writing the model: pick it up once it appears. */
        StrList e = {0};
        app.model_path = model_find(".", &e);
        if (!app.model_path) return;
        app.model_mtime = 0;
    }
    long long mm = file_sig(app.model_path);
    time_t gm = mtime_of(".fsm/gate.json");
    if (mm != app.model_mtime) {
        /* Someone else wrote the file (the canvas's own edits set the
         * signature): undo would now overwrite their change. */
        app.model_mtime = mm;
        undo_clear();
        load_model(0);
    }
    if (gm != app.gate_mtime) { app.gate_mtime = gm; gate_status_load(&app.gate, ".fsm/gate.json"); }
}

/* ---- canvas ---- */

/* The panel takes a third of the window, or more when the text is large
 * enough that a third would crowd it, up to just over half. */
static float panel_width(void) {
    float w = (float)GetScreenWidth();
    return floorf(fminf(w * 0.55f, fmaxf(w / 3.0f, 430.0f * app.ui)));
}

static float canvas_width(void) { return (float)GetScreenWidth() - panel_width(); }

static float node_height(const State *s) {
    int lines = 1 + (s->entry.n ? 1 : 0) + (s->exit.n ? 1 : 0) + (s->invoke.n ? 1 : 0) + (s->has_after ? 1 : 0);
    return (24.0f + lines * 20.0f) * app.ui;
}

static Vector2 text_size(const char *s, float size);

/* Width in world units: the widest line at zoom 1, plus padding. */
static float node_width(const State *s) {
    float w = text_size(s->name, 20.0f * app.ui).x;
    char line[256];
    const StrList *lists[3] = {&s->entry, &s->exit, &s->invoke};
    const char *prefix[3] = {"entry: ", "exit: ", "invoke: "};
    for (int k = 0; k < 3; k++) {
        if (!lists[k]->n) continue;
        snprintf(line, sizeof line, "%s", prefix[k]);
        for (int i = 0; i < lists[k]->n; i++) {
            strncat(line, i ? ", " : "", sizeof line - strlen(line) - 1);
            strncat(line, lists[k]->v[i], sizeof line - strlen(line) - 1);
        }
        w = fmaxf(w, text_size(line, 15.0f * app.ui).x);
    }
    if (s->has_after) {
        snprintf(line, sizeof line, "after %ld ms: %s", s->after_ms, s->after_event);
        w = fmaxf(w, text_size(line, 15.0f * app.ui).x);
    }
    return fmaxf(NODE_W * app.ui, w + 26.0f * app.ui);
}

/* Clusters sorted values: a gap wider than `gap` starts a new cluster. */
static int cluster_of(const double *sorted, int n, double v, double gap) {
    int c = 0;
    for (int i = 1; i < n && sorted[i] <= v + 1e-9; i++)
        if (sorted[i] - sorted[i - 1] > gap) c++;
    return c;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* The saved layout's x and y pick each state's column and row: states a
 * little apart share one. Two states that land in one cell get a column
 * each, so no two nodes can ever overlap. */
static void grid_assign(void) {
    const SgModel *m = &app.model;
    int n = m->n_states < MAX_GRID ? m->n_states : MAX_GRID;
    static double xs[MAX_GRID], ys[MAX_GRID];
    for (int i = 0; i < n; i++) { xs[i] = m->states[i].x; ys[i] = m->states[i].y; }
    qsort(xs, (size_t)n, sizeof xs[0], cmp_double);
    qsort(ys, (size_t)n, sizeof ys[0], cmp_double);
    app.n_cols = app.n_rows = 0;
    for (int i = 0; i < n; i++) {
        app.col[i] = cluster_of(xs, n, m->states[i].x, 100.0);
        app.row[i] = cluster_of(ys, n, m->states[i].y, 60.0);
    }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < i; j++) {
            if (app.col[j] != app.col[i] || app.row[j] != app.row[i]) continue;
            int c = app.col[i];
            for (int k = 0; k < n; k++) if (app.col[k] > c) app.col[k]++;
            app.col[i] = c + 1;
            j = -1; /* check the new cell again */
        }
    for (int i = 0; i < n; i++) {
        if (app.col[i] + 1 > app.n_cols) app.n_cols = app.col[i] + 1;
        if (app.row[i] + 1 > app.n_rows) app.n_rows = app.row[i] + 1;
    }
}

/* Where Clay put the node this frame, in screen pixels. */
static Rectangle node_rect(const State *s) {
    int i = (int)(s - app.model.states);
    Clay_ElementData d = Clay_GetElementData(CLAY_IDI("Node", i));
    if (!d.found) return (Rectangle){-1e5f, -1e5f, 0, 0};
    return (Rectangle){d.boundingBox.x, d.boundingBox.y, d.boundingBox.width, d.boundingBox.height};
}

/* Fit the grid Clay laid out (at the current zoom) into the canvas. */
static void fit_view(void) {
    Clay_ElementData g = Clay_GetElementData(CLAY_ID("Grid"));
    if (!app.have_model || !g.found || app.zoom <= 0) return;
    float w = g.boundingBox.width / app.zoom, h = g.boundingBox.height / app.zoom;
    float cw = canvas_width(), ch = (float)GetScreenHeight(), margin = 30.0f * app.ui;
    /* The editor's toolbar sits over the top of the canvas: fit below it. */
    Clay_ElementData bar = Clay_GetElementData(CLAY_ID("EditBar"));
    float top = bar.found ? bar.boundingBox.y + bar.boundingBox.height : 0;
    ch -= top;
    float zx = (cw - 2 * margin) / fmaxf(1.0f, w), zy = (ch - 2 * margin) / fmaxf(1.0f, h);
    /* Never shrink below readable text: a big machine in a small canvas
     * then overflows, and drag or scroll shows the rest. */
    app.zoom = fminf(2.0f, fmaxf(0.7f, fminf(zx, zy)));
    app.pan.x = fmaxf(0, (cw - w * app.zoom) / 2);
    app.pan.y = top + fmaxf(0, (ch - h * app.zoom) / 2);
}

static void text(const char *s, float x, float y, float size, Color c) {
    DrawTextEx(app.font, s, (Vector2){x, y}, size, 0, c);
}

static Vector2 text_size(const char *s, float size) { return MeasureTextEx(app.font, s, size, 0); }

static Vector2 rect_center(Rectangle r) { return (Vector2){r.x + r.width / 2, r.y + r.height / 2}; }

/* Point where the ray from the rectangle's centre towards `to` leaves it. */
static Vector2 rect_exit(Rectangle r, Vector2 to) {
    Vector2 c = rect_center(r);
    float dx = to.x - c.x, dy = to.y - c.y;
    if (fabsf(dx) < 1e-3f && fabsf(dy) < 1e-3f) return c;
    float tx = fabsf(dx) > 1e-3f ? (r.width / 2) / fabsf(dx) : 1e9f;
    float ty = fabsf(dy) > 1e-3f ? (r.height / 2) / fabsf(dy) : 1e9f;
    float t = fminf(tx, ty);
    return (Vector2){c.x + dx * t, c.y + dy * t};
}

static void arrow_head(Vector2 tip, Vector2 from, Color c) {
    Vector2 d = Vector2Normalize(Vector2Subtract(tip, from));
    Vector2 n = {-d.y, d.x};
    float L = 11.0f * fmaxf(0.7f, app.zoom), W = 5.5f * fmaxf(0.7f, app.zoom);
    Vector2 b = Vector2Subtract(tip, Vector2Scale(d, L));
    DrawTriangle(tip, Vector2Add(b, Vector2Scale(n, W)), Vector2Subtract(b, Vector2Scale(n, W)), c);
    DrawTriangle(tip, Vector2Subtract(b, Vector2Scale(n, W)), Vector2Add(b, Vector2Scale(n, W)), c);
}

/* Edge labels, drawn after the nodes. Their boxes stay until the next
 * frame, so a click can find the arrow it is on. */
typedef struct {
    char text[256];
    Vector2 at;
    int hot, sel;
    int from, to;   /* state indexes */
    Rectangle box;
} PendingLabel;
static PendingLabel labels[256];
static int n_labels;

static void edge_label(const char *label, Vector2 at, int hot, int from, int to, int sel) {
    if (n_labels < 256) {
        PendingLabel *l = &labels[n_labels++];
        snprintf(l->text, sizeof l->text, "%s", label);
        l->at = at;
        l->hot = hot;
        l->sel = sel;
        l->from = from;
        l->to = to;
        l->box = (Rectangle){at.x, at.y, 0, 0};
    }
}

static void draw_label(PendingLabel *l) {
    float fs = 15.0f * app.ui * app.zoom;
    if (fs < 8) return;
    Vector2 sz = text_size(l->text, fs), at = l->at;
    Rectangle bg = {at.x - sz.x / 2 - 4, at.y - sz.y / 2 - 2, sz.x + 8, sz.y + 4};
    l->box = bg;
    DrawRectangleRounded(bg, 0.4f, 4, l->hot || l->sel ? ACCENT_SOFT : (Color){244, 244, 241, 235});
    if (l->sel) DrawRectangleRoundedLinesEx(bg, 0.4f, 4, 1.5f, ACCENT);
    text(l->text, at.x - sz.x / 2, at.y - sz.y / 2, fs, l->hot || l->sel ? ACCENT : QUIET);
}

static int edge_selected(const State *a, const State *b) {
    return E.sel_from[0] && !strcmp(E.sel_from, a->id) && !strcmp(E.sel_to, b->id);
}

static void draw_edges(void) {
    const SgModel *m = &app.model;
    for (int i = 0; i < m->n_states; i++) {
        const State *s = &m->states[i];
        for (int j = 0; j < m->n_states; j++) {
            /* One edge per (from, to) pair, labelled with every event. */
            char label[256] = "";
            int hot = 0;
            for (int k = 0; k < s->n_on; k++) {
                if (strcmp(s->on[k].target, m->states[j].id)) continue;
                if (*label) strncat(label, ", ", sizeof label - strlen(label) - 1);
                strncat(label, s->on[k].event, sizeof label - strlen(label) - 1);
                if (app.last_from == i && app.last_to == j && !strcmp(app.last_event, s->on[k].event)) hot = 1;
            }
            if (!*label) continue;
            int sel = edge_selected(s, &m->states[j]);
            Color c = hot || sel ? ACCENT : NODE_EDGE;
            float thick = (hot || sel ? 2.5f : 1.4f) * fmaxf(0.7f, app.zoom);
            Rectangle a = node_rect(s), b = node_rect(&m->states[j]);
            if (i == j) {
                /* Self-loop above the node. */
                Vector2 p1 = {a.x + a.width * 0.30f, a.y}, p4 = {a.x + a.width * 0.70f, a.y};
                float h = 46.0f * app.zoom;
                Vector2 c2 = {p1.x - 10 * app.zoom, a.y - h}, c3 = {p4.x + 10 * app.zoom, a.y - h};
                DrawSplineSegmentBezierCubic(p1, c2, c3, p4, thick, c);
                arrow_head(p4, c3, c);
                edge_label(label, (Vector2){a.x + a.width / 2, a.y - h * 0.80f}, hot, i, j, sel);
                continue;
            }
            /* Offset both directions of a two-way pair so they don't overlap. */
            int reverse = state_target(&m->states[j], s->id) != NULL;
            for (int k = 0; !reverse && k < m->states[j].n_on; k++)
                if (!strcmp(m->states[j].on[k].target, s->id)) reverse = 1;
            Vector2 ca = rect_center(a), cb = rect_center(b);
            Vector2 d = Vector2Normalize(Vector2Subtract(cb, ca));
            Vector2 n = {-d.y, d.x};
            float off = reverse ? 12.0f * app.zoom : 0.0f;
            Vector2 sa = Vector2Add(ca, Vector2Scale(n, off)), sb = Vector2Add(cb, Vector2Scale(n, off));
            Vector2 p = rect_exit((Rectangle){a.x, a.y, a.width, a.height}, sb);
            Vector2 q = rect_exit((Rectangle){b.x, b.y, b.width, b.height}, sa);
            p = Vector2Add(p, Vector2Scale(n, off));
            q = Vector2Add(q, Vector2Scale(n, off));
            DrawLineEx(p, q, thick, c);
            arrow_head(q, p, c);
            /* Each label of a two-way pair sits on its own edge's side, far
             * enough out that the two never overlap. */
            Vector2 ls = text_size(label, 15.0f * app.ui * app.zoom);
            float out = fabsf(n.x) * (ls.x / 2 + 8) + fabsf(n.y) * (ls.y / 2 + 4);
            Vector2 mid = Vector2Add(Vector2Lerp(p, q, 0.5f), Vector2Scale(n, reverse ? out : 0));
            edge_label(label, mid, hot, i, j, sel);
        }
    }
}

static int action_locked(const char *id) {
    const Action *a = model_action(&app.model, id);
    return a && a->locked;
}

static void draw_list_line(const char *prefix, const StrList *l, float x, float *y, float fs) {
    char line[256];
    int locked = 0;
    snprintf(line, sizeof line, "%s", prefix);
    for (int k = 0; k < l->n; k++) {
        strncat(line, k ? ", " : "", sizeof line - strlen(line) - 1);
        strncat(line, l->v[k], sizeof line - strlen(line) - 1);
        if (action_locked(l->v[k])) locked = 1;
    }
    text(line, x, *y, fs, locked ? LOCKED : QUIET);
    *y += 20.0f * app.ui * app.zoom;
}

/* Events a state neither handles nor ignores: decisions still open. */
static int undecided(const State *s, const char **out, int max) {
    int n = 0;
    for (int k = 0; k < app.model.events.n; k++) {
        const char *ev = app.model.events.v[k];
        if (state_target(s, ev) || sl_contains(&s->ignore, ev)) continue;
        if (out && n < max) out[n] = ev;
        n++;
    }
    return n;
}

/* The "N undecided" badge on each node, kept for clicks. */
static Rectangle badges[MAX_GRID];

static void draw_badge(int i, Rectangle r) {
    if (i >= MAX_GRID) return;
    badges[i] = (Rectangle){0};
    int n = undecided(&app.model.states[i], NULL, 0);
    if (!n) return;
    float fs = fmaxf(10.0f, 13.0f * app.ui * app.zoom);
    char t[48];
    snprintf(t, sizeof t, "%d undecided", n);
    Vector2 ts = text_size(t, fs);
    /* On the bottom edge, clear of self-loops above and arrows at the sides. */
    Rectangle b = {r.x + r.width - ts.x - 18, r.y + r.height - ts.y / 2 - 3, ts.x + 12, ts.y + 6};
    badges[i] = b;
    DrawRectangleRounded(b, 0.5f, 6, (Color){254, 243, 199, 255});
    DrawRectangleRoundedLinesEx(b, 0.5f, 6, 1.2f, LOCKED);
    text(t, b.x + 6, b.y + 3, fs, LOCKED);
}

static void draw_nodes(void) {
    const SgModel *m = &app.model;
    for (int i = 0; i < m->n_states; i++) {
        const State *s = &m->states[i];
        Rectangle r = node_rect(s);
        int active = i == app.cur;
        DrawRectangleRounded(r, 0.18f, 8, active ? ACCENT_SOFT : NODE_FILL);
        DrawRectangleRoundedLinesEx(r, 0.18f, 8, (active ? 2.5f : 1.2f) * fmaxf(0.7f, app.zoom), active ? ACCENT : NODE_EDGE);
        if (!strcmp(E.sel, s->id)) {
            Rectangle o = {r.x - 4, r.y - 4, r.width + 8, r.height + 8};
            DrawRectangleRoundedLinesEx(o, 0.18f, 8, 2.0f, (Color){37, 99, 235, 150});
        }
        draw_badge(i, r);
        if (s->initial) {
            Vector2 dot = {r.x - 30 * app.zoom, r.y + r.height / 2};
            DrawCircleV(dot, 5 * app.zoom, INK);
            DrawLineEx(dot, (Vector2){r.x, dot.y}, 1.4f * fmaxf(0.7f, app.zoom), INK);
            arrow_head((Vector2){r.x, dot.y}, dot, INK);
        }
        float u = app.ui * app.zoom, fs = 20.0f * u, small = 15.0f * u;
        if (fs < 6) continue;
        float x = r.x + 12 * u, y = r.y + 10 * u;
        text(s->name, x, y, fs, active ? ACCENT : INK);
        y += 28.0f * u;
        if (small < 7) continue;
        if (s->entry.n) draw_list_line("entry: ", &s->entry, x, &y, small);
        if (s->exit.n) draw_list_line("exit: ", &s->exit, x, &y, small);
        if (s->invoke.n) draw_list_line("invoke: ", &s->invoke, x, &y, small);
        if (s->has_after) {
            char line[128];
            snprintf(line, sizeof line, "after %ld ms: %s", s->after_ms, s->after_event);
            text(line, x, y, small, QUIET);
        }
    }
    /* Locked max-time invariants hang under their state. */
    for (int i = 0; i < m->n_invariants; i++) {
        const Invariant *v = &m->invariants[i];
        if (v->kind != INV_MAX_TIME_IN_STATE) continue;
        const State *s = model_state(m, v->state);
        if (!s) continue;
        Rectangle r = node_rect(s);
        char line[160];
        snprintf(line, sizeof line, "locked: at most %ld ms (%s)", v->limit_ms, v->id);
        text(line, r.x + 4 * app.zoom, r.y + r.height + 6 * app.zoom, 13.0f * app.ui * app.zoom, LOCKED);
    }
}

static void draw_editing(void);

static void draw_canvas(void) {
    float cw = canvas_width(), ch = (float)GetScreenHeight();
    BeginScissorMode(0, 0, (int)cw, (int)ch);
    DrawRectangle(0, 0, (int)cw, (int)ch, BG);
    float step = 40.0f * app.zoom;
    if (step > 8)
        for (float gx = fmodf(app.pan.x, step); gx < cw; gx += step)
            for (float gy = fmodf(app.pan.y, step); gy < ch; gy += step) DrawPixelV((Vector2){gx, gy}, GRID);
    if (app.have_model) {
        n_labels = 0;
        draw_edges();
        draw_nodes();
        /* Labels last, so no node hides them. */
        for (int i = 0; i < n_labels; i++) draw_label(&labels[i]);
        draw_editing();
    } else {
        float u = app.ui;
        if (app.model_path) text("No valid model loaded yet. See the panel for errors.", 24 * u, 24 * u, 18 * u, QUIET);
        else if (!app.st.ok) text("No model in this folder yet.", 24 * u, 24 * u, 22 * u, INK);
        /* otherwise canvas_notice_card says what to do */
    }
    text("drag empty space to pan, scroll to zoom, F to fit, Del deletes, Ctrl+Z undoes, Ctrl +/- text size", 16 * app.ui, ch - 26 * app.ui, 14 * app.ui, QUIET);
    EndScissorMode();
}

static void sl_clear(StrList *l);

/* ---- the canvas editor ----
 *
 * Every edit reads the model file, changes its text with src/medit.c and
 * writes it back; the canvas then reloads from the file like any change. */

static void undo_clear(void) {
    for (int i = 0; i < E.n_undo; i++) free(E.undo[i]);
    E.n_undo = 0;
}

static char *edit_begin(void) {
    if (!app.model_path) return NULL;
    char *t = read_file(app.model_path, NULL);
    if (!t) snprintf(E.msg, sizeof E.msg, "Cannot read %s.", app.model_path);
    return t;
}

/* Writes `after` when the edit worked and changed something; `before`
 * becomes the undo step. Takes both buffers. */
static int edit_end(char *before, char *after, int rc, const char *err) {
    if (rc != 0 || !after) {
        snprintf(E.msg, sizeof E.msg, "%s", err && *err ? err : "That edit did not work.");
        free(before);
        free(after);
        return -1;
    }
    E.msg[0] = 0;
    if (!strcmp(before, after)) { free(before); free(after); return 0; }
    if (write_file(app.model_path, after, strlen(after)) != 0) {
        snprintf(E.msg, sizeof E.msg, "Cannot write %s.", app.model_path);
        free(before);
        free(after);
        return -1;
    }
    if (E.n_undo == (int)(sizeof E.undo / sizeof E.undo[0])) {
        free(E.undo[0]);
        memmove(E.undo, E.undo + 1, sizeof E.undo[0] * (size_t)(E.n_undo - 1));
        E.n_undo--;
    }
    E.undo[E.n_undo++] = before;
    free(after);
    /* A new first row or column moves the whole grid: keep one state where
     * it was on screen so the view doesn't jump. */
    E.anchor[0] = 0;
    for (int i = 0; i < app.model.n_states; i++) {
        if (!strcmp(app.model.states[i].id, E.mover)) continue;
        Rectangle r = node_rect(&app.model.states[i]);
        snprintf(E.anchor, sizeof E.anchor, "%s", app.model.states[i].id);
        E.anchor_at = (Vector2){r.x, r.y};
        break;
    }
    E.mover[0] = 0;
    app.model_mtime = file_sig(app.model_path);
    load_model(0);
    return 0;
}

static void edit_undo(void) {
    if (!E.n_undo || !app.model_path) return;
    char *t = E.undo[--E.n_undo];
    if (write_file(app.model_path, t, strlen(t)) == 0) {
        app.model_mtime = file_sig(app.model_path);
        load_model(0);
        snprintf(E.msg, sizeof E.msg, "Undone.");
    }
    free(t);
}

static void select_none(void) { E.sel[0] = E.sel_from[0] = E.sel_to[0] = 0; }

static void select_state(const char *id) {
    select_none();
    snprintf(E.sel, sizeof E.sel, "%s", id);
}

static void select_edge(const char *from, const char *to) {
    select_none();
    snprintf(E.sel_from, sizeof E.sel_from, "%s", from);
    snprintf(E.sel_to, sizeof E.sel_to, "%s", to);
}

static int node_at(Vector2 p) {
    for (int i = app.model.n_states - 1; i >= 0; i--)
        if (CheckCollisionPointRec(p, node_rect(&app.model.states[i]))) return i;
    return -1;
}

/* The dot on a node's right edge that arrows are drawn from. */
static float dot_radius(void) { return fmaxf(5.0f, 6.5f * app.ui * fmaxf(0.7f, app.zoom)); }
static Vector2 dot_pos(Rectangle r) { return (Vector2){r.x + r.width, r.y + r.height / 2}; }

static int dot_at(Vector2 p) {
    for (int i = app.model.n_states - 1; i >= 0; i--) {
        Rectangle r = node_rect(&app.model.states[i]);
        if (r.width > 0 && Vector2Distance(p, dot_pos(r)) <= dot_radius() + 4) return i;
    }
    return -1;
}

static int label_at(Vector2 p) {
    for (int i = n_labels - 1; i >= 0; i--)
        if (CheckCollisionPointRec(p, labels[i].box)) return i;
    return -1;
}

/* The events on from -> to, as the label shows them: "A, B". */
static void edge_events(const State *from, const char *to, char *out, size_t n) {
    out[0] = 0;
    for (int k = 0; k < from->n_on; k++) {
        if (strcmp(from->on[k].target, to)) continue;
        if (*out) strncat(out, ", ", n - strlen(out) - 1);
        strncat(out, from->on[k].event, n - strlen(out) - 1);
    }
}

/* Where a point falls on one axis of the grid, as a layout value: inside
 * a column (row) it takes that column's x (y); in a gap it gets a value
 * between its neighbours, and when they are too close for a new column to
 * fit, the states after the gap move over. The saved layout only orders
 * states (see grid_assign), so this is all a drop needs to say. */
static double axis_place(int axis, float p, int mover, double *nv) {
    const SgModel *m = &app.model;
    int n = m->n_states < MAX_GRID ? m->n_states : MAX_GRID;
    int kn = axis ? app.n_rows : app.n_cols;
    double gap = axis ? 60 : 100, step = axis ? 200 : 300;
    if (!n || !kn) return axis ? 120 : 80;
    static double lo[MAX_GRID], hi[MAX_GRID];
    static float a[MAX_GRID], b[MAX_GRID];
    for (int k = 0; k < kn; k++) {
        lo[k] = 1e18; hi[k] = -1e18;
        Clay_ElementData d = Clay_GetElementData(CLAY_IDI("Cell", axis ? k : k * MAX_GRID));
        if (!d.found) return axis ? m->states[0].y : m->states[0].x;
        a[k] = axis ? d.boundingBox.y : d.boundingBox.x;
        b[k] = a[k] + (axis ? d.boundingBox.height : d.boundingBox.width);
    }
    double own = mover >= 0 ? (axis ? m->states[mover].y : m->states[mover].x) : 0;
    for (int i = 0; i < n; i++) {
        if (i == mover) continue;
        int k = axis ? app.row[i] : app.col[i];
        double v = axis ? m->states[i].y : m->states[i].x;
        lo[k] = fmin(lo[k], v);
        hi[k] = fmax(hi[k], v);
    }
    for (int k = 0; k < kn; k++) if (lo[k] > hi[k]) lo[k] = hi[k] = own; /* only the mover was there */
    for (int k = 0; k < kn; k++) {
        if (p < a[k] || p > b[k]) continue;
        if (mover >= 0 && (axis ? app.row[mover] : app.col[mover]) == k) return own;
        return lo[k];
    }
    if (p < a[0]) return lo[0] - step;
    if (p > b[kn - 1]) return hi[kn - 1] + step;
    for (int k = 1; k < kn; k++) {
        if (p <= b[k - 1] || p >= a[k]) continue;
        double A = hi[k - 1], Bv = lo[k];
        if (Bv - A > 2 * gap + 2) return floor((A + Bv) / 2);
        for (int i = 0; i < n; i++)
            if (i != mover && nv[i] >= Bv) nv[i] += step;
        return A + step / 2;
    }
    return own;
}

/* The layout for a state dropped (or made) at p; states it pushes over
 * are moved in the text too. */
static int place_at(Vector2 p, int mover, double *x, double *y, char **t, char *err, size_t errn) {
    const SgModel *m = &app.model;
    int n = m->n_states < MAX_GRID ? m->n_states : MAX_GRID;
    static double nx[MAX_GRID], ny[MAX_GRID];
    for (int i = 0; i < n; i++) { nx[i] = m->states[i].x; ny[i] = m->states[i].y; }
    *x = axis_place(0, p.x, mover, nx);
    *y = axis_place(1, p.y, mover, ny);
    for (int i = 0; i < n; i++) {
        if (i == mover || (nx[i] == m->states[i].x && ny[i] == m->states[i].y)) continue;
        if (me_set_layout(t, m->states[i].id, nx[i], ny[i], err, errn)) return -1;
    }
    return 0;
}

static void open_name_field(const char *id, int fresh) {
    const State *s = model_state(&app.model, id);
    if (!s) return;
    E.in = IN_NAME;
    E.just_opened = 1;
    snprintf(E.in_state, sizeof E.in_state, "%s", id);
    snprintf(E.buf, sizeof E.buf, "%s", s->name);
    E.fresh = fresh;
}

static void open_label_field(const char *from, const char *to) {
    const State *s = model_state(&app.model, from);
    if (!s) return;
    E.in = IN_LABEL;
    E.just_opened = 1;
    snprintf(E.in_from, sizeof E.in_from, "%s", from);
    snprintf(E.in_to, sizeof E.in_to, "%s", to);
    edge_events(s, to, E.buf, sizeof E.buf);
    E.fresh = 0;
    select_edge(from, to);
}

/* A name no state has yet: State1, State2, ... */
static void free_name(char *out, size_t n) {
    for (int k = 1; k < 10000; k++) {
        snprintf(out, n, "State%d", k);
        int used = 0;
        for (int i = 0; i < app.model.n_states && !used; i++) used = !strcmp(app.model.states[i].name, out);
        if (!used) return;
    }
}

/* A new state at p (or, with at == NULL, right of the rightmost column).
 * Fills id with its id; returns 0 on success. */
static int ed_new_state(const Vector2 *at, char *id, size_t idn) {
    char *t = edit_begin(), err[256] = "", name[32];
    if (!t) return -1;
    char *orig = xstrdup(t);
    free_name(name, sizeof name);
    double x = 80, y = 120;
    int rc = 0;
    if (at) rc = place_at(*at, -1, &x, &y, &t, err, sizeof err);
    else if (app.model.n_states) {
        x = -1e18;
        y = 1e18;
        for (int i = 0; i < app.model.n_states; i++) {
            x = fmax(x, app.model.states[i].x);
            y = fmin(y, app.model.states[i].y);
        }
        x += 300;
    }
    if (!rc) rc = me_add_state(&t, name, x, y, err, sizeof err);
    if (edit_end(orig, t, rc, err) != 0) return -1;
    snprintf(id, idn, "%s.%s", app.model.id, name);
    select_state(id);
    snprintf(E.reveal, sizeof E.reveal, "%s", id);
    return 0;
}

static void ed_move(int i, Vector2 center) {
    if (i < 0 || i >= app.model.n_states) return;
    char *t = edit_begin(), err[256] = "";
    if (!t) return;
    char *orig = xstrdup(t), id[128];
    snprintf(id, sizeof id, "%s", app.model.states[i].id);
    snprintf(E.mover, sizeof E.mover, "%s", id);
    double x, y;
    int rc = place_at(center, i, &x, &y, &t, err, sizeof err);
    if (!rc) rc = me_set_layout(&t, id, x, y, err, sizeof err);
    edit_end(orig, t, rc, err);
}

static void ed_commit_name(void) {
    E.in = IN_NONE;
    char name[128];
    size_t n = 0;
    for (const char *p = E.buf; *p && n + 1 < sizeof name; p++)
        if (isalnum((unsigned char)*p)) name[n++] = *p;
    name[n] = 0;
    if (n) name[0] = (char)toupper((unsigned char)name[0]);
    const State *s = model_state(&app.model, E.in_state);
    if (!s || !n || !strcmp(name, s->name)) return;
    char *t = edit_begin(), err[256] = "";
    if (!t) return;
    char *orig = xstrdup(t), old[128];
    snprintf(old, sizeof old, "%s", E.in_state);
    int rc = me_rename_state(&t, old, name, err, sizeof err);
    if (edit_end(orig, t, rc, err) == 0) {
        char id[300];
        snprintf(id, sizeof id, "%s.%s", app.model.id, name);
        select_state(id);
    } else {
        open_name_field(old, 0); /* keep the field open on a bad name */
        snprintf(E.buf, sizeof E.buf, "%s", name);
    }
}

/* The label field holds the arrow's events, comma separated. Events taken
 * out are removed from the arrow; new ones are added (a new event is
 * declared, and every other state ignores it). */
static void ed_commit_label(void) {
    StrList want = {0};
    char buf[256];
    snprintf(buf, sizeof buf, "%s", E.buf);
    for (char *save = NULL, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;
        size_t n = strlen(tok);
        while (n && tok[n - 1] == ' ') tok[--n] = 0;
        for (char *c = tok; *c; c++) *c = *c == ' ' ? '_' : (char)toupper((unsigned char)*c);
        if (!n) continue;
        if (!me_is_event_name(tok)) {
            snprintf(E.msg, sizeof E.msg, "%s is not an event name: use UPPER_SNAKE_CASE, like COIN or TIMER_DONE.", tok);
            sl_clear(&want);
            return; /* the field stays open */
        }
        if (!sl_contains(&want, tok)) sl_push(&want, tok);
    }
    E.in = IN_NONE;
    const State *from = model_state(&app.model, E.in_from);
    if (!from || !model_state(&app.model, E.in_to)) { sl_clear(&want); return; }
    char from_id[128], to_id[128];
    snprintf(from_id, sizeof from_id, "%s", E.in_from);
    snprintf(to_id, sizeof to_id, "%s", E.in_to);
    StrList had = {0};
    for (int k = 0; k < from->n_on; k++)
        if (!strcmp(from->on[k].target, to_id)) sl_push(&had, from->on[k].event);
    char *t = edit_begin(), err[256] = "";
    if (!t) { sl_clear(&want); sl_clear(&had); return; }
    char *orig = xstrdup(t);
    int rc = 0;
    for (int i = 0; !rc && i < had.n; i++)
        if (!sl_contains(&want, had.v[i])) rc = me_remove_transition(&t, from_id, had.v[i], err, sizeof err);
    char moved[200] = "";
    for (int i = 0; !rc && i < want.n; i++) {
        const char *was = state_target(from, want.v[i]);
        if (was && strcmp(was, to_id)) snprintf(moved, sizeof moved, "%s now goes here instead of to %s.", want.v[i], was);
        rc = me_set_transition(&t, from_id, want.v[i], to_id, err, sizeof err);
    }
    if (edit_end(orig, t, rc, err) == 0 && *moved) snprintf(E.msg, sizeof E.msg, "%s", moved);
    if (want.n) select_edge(from_id, to_id);
    else select_none();
    sl_clear(&want);
    sl_clear(&had);
}

static void ed_commit_field(void) {
    if (E.in == IN_NAME) ed_commit_name();
    else if (E.in == IN_LABEL) ed_commit_label();
}

static void ed_delete(void) {
    char *t = edit_begin(), err[256] = "";
    if (!t) return;
    char *orig = xstrdup(t);
    int rc = 0;
    if (E.sel[0]) {
        rc = me_delete_state(&t, E.sel, err, sizeof err);
    } else if (E.sel_from[0]) {
        const State *from = model_state(&app.model, E.sel_from);
        StrList evs = {0};
        for (int k = 0; from && k < from->n_on; k++)
            if (!strcmp(from->on[k].target, E.sel_to)) sl_push(&evs, from->on[k].event);
        for (int i = 0; !rc && i < evs.n; i++) rc = me_remove_transition(&t, E.sel_from, evs.v[i], err, sizeof err);
        sl_clear(&evs);
    } else {
        free(orig);
        free(t);
        return;
    }
    if (edit_end(orig, t, rc, err) == 0) select_none();
}

/* The selected state ignores event `k` (an index into events), or every
 * event it is undecided about when k < 0. */
static void ed_ignore(int k) {
    const State *s = E.sel[0] ? model_state(&app.model, E.sel) : NULL;
    if (!s) return;
    const char *open[256];
    int n = undecided(s, open, 256);
    if (n > 256) n = 256;
    char *t = edit_begin(), err[256] = "";
    if (!t) return;
    char *orig = xstrdup(t), id[128];
    snprintf(id, sizeof id, "%s", s->id);
    int rc = 0;
    for (int i = 0; !rc && i < n; i++)
        if (k < 0 || (k < app.model.events.n && !strcmp(open[i], app.model.events.v[k])))
            rc = me_ignore_event(&t, id, open[i], err, sizeof err);
    edit_end(orig, t, rc, err);
}

static void ed_make_initial(void) {
    if (!E.sel[0]) return;
    char *t = edit_begin(), err[256] = "";
    if (!t) return;
    char *orig = xstrdup(t);
    int rc = me_set_initial(&t, E.sel, err, sizeof err);
    edit_end(orig, t, rc, err);
}

static void ed_rename_selection(void) {
    if (E.sel[0]) open_name_field(E.sel, 0);
    else if (E.sel_from[0]) open_label_field(E.sel_from, E.sel_to);
}

/* A new model: the black dot and one state that loops back on itself. */
static void new_model(void) {
    if (app.model_path) return;
    const char *base = strrchr(app.project, '/') ? strrchr(app.project, '/') + 1 : app.project;
    char mid[64];
    size_t n = 0;
    for (const char *p = base; *p && n + 1 < sizeof mid; p++) {
        char c = (char)tolower((unsigned char)*p);
        if (!isalnum((unsigned char)c)) c = '_';
        if (c == '_' && (n == 0 || mid[n - 1] == '_')) continue;
        if (n == 0 && isdigit((unsigned char)c)) mid[n++] = 'm';
        mid[n++] = c;
    }
    while (n && mid[n - 1] == '_') n--;
    mid[n] = 0;
    if (!n) snprintf(mid, sizeof mid, "machine");
    char path[128];
    snprintf(path, sizeof path, "%s.yaml", mid);
    if (file_exists(path)) { snprintf(E.msg, sizeof E.msg, "%s is already there.", path); return; }
    char *t = me_new_model(mid);
    int rc = write_file(path, t, strlen(t));
    free(t);
    if (rc != 0) { snprintf(app.flash, sizeof app.flash, "cannot write %s", path); return; }
    app.model_path = xstrdup(path);
    app.model_mtime = file_sig(path);
    undo_clear();
    load_model(1);
    select_none();
}

/* Left button on the canvas: select, move, link, pan, double-click. */
static void edit_mouse(Vector2 mouse, int can_press) {
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && can_press) {
        double now = GetTime();
        int dbl = now - E.last_click < 0.4 && Vector2Distance(mouse, E.last_at) < 8;
        E.last_click = dbl ? 0 : now;
        E.last_at = mouse;
        if (E.in) {
            if (CheckCollisionPointRec(mouse, E.in_box)) return;
            ed_commit_field(); /* clicking elsewhere keeps what was typed */
            if (E.in) return;  /* it was not a valid name */
        }
        E.pressed = 1;
        E.press_at = mouse;
        E.press_node = -1;
        E.moving = E.linking = E.panning = 0;
        if (!app.have_model) { E.panning = 1; return; }
        int dot = dot_at(mouse), nd = node_at(mouse), lb = label_at(mouse), badge = -1;
        for (int i = 0; i < app.model.n_states && i < MAX_GRID; i++)
            if (badges[i].width > 0 && CheckCollisionPointRec(mouse, badges[i])) badge = i;
        if (badge >= 0) {
            /* The toolbar then lists the open events, each with Ignore. */
            select_state(app.model.states[badge].id);
            E.pressed = 0;
            return;
        }
        if (dot >= 0 && !dbl) {
            E.press_node = dot;
            E.linking = 1;
            select_state(app.model.states[dot].id);
        } else if (nd >= 0) {
            select_state(app.model.states[nd].id);
            if (dbl) { open_name_field(app.model.states[nd].id, 0); E.pressed = 0; return; }
            Rectangle r = node_rect(&app.model.states[nd]);
            E.press_node = nd;
            E.grab = (Vector2){mouse.x - r.x, mouse.y - r.y};
        } else if (lb >= 0 && labels[lb].from < app.model.n_states && labels[lb].to < app.model.n_states) {
            const char *f = app.model.states[labels[lb].from].id, *to = app.model.states[labels[lb].to].id;
            select_edge(f, to);
            if (dbl) { open_label_field(f, to); E.pressed = 0; }
        } else {
            select_none();
            if (dbl) {
                char id[300];
                if (ed_new_state(&mouse, id, sizeof id) == 0) open_name_field(id, 1);
                E.pressed = 0;
                return;
            }
            E.panning = 1;
        }
    }
    if (!E.pressed) return;
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (E.press_node >= 0 && !E.linking && !E.moving && Vector2Distance(mouse, E.press_at) > 5) E.moving = 1;
        return;
    }
    /* released */
    int from = E.press_node;
    if (E.moving && from >= 0 && from < app.model.n_states) {
        Rectangle r = node_rect(&app.model.states[from]);
        ed_move(from, (Vector2){mouse.x - E.grab.x + r.width / 2, mouse.y - E.grab.y + r.height / 2});
    } else if (E.linking && from >= 0 && from < app.model.n_states && Vector2Distance(mouse, E.press_at) > 5) {
        char from_id[128];
        snprintf(from_id, sizeof from_id, "%s", app.model.states[from].id);
        int to = node_at(mouse);
        if (to >= 0) open_label_field(from_id, app.model.states[to].id);
        else if (mouse.x < canvas_width()) {
            /* Dropped on empty space: a new state there, with the arrow to it. */
            char id[300];
            if (ed_new_state(&mouse, id, sizeof id) == 0) open_label_field(from_id, id);
        }
    }
    E.pressed = E.moving = E.linking = E.panning = 0;
    E.press_node = -1;
}

static void handle_canvas_input(void) {
    Vector2 mouse = GetMousePosition();
    int in_canvas = mouse.x <= canvas_width();
    int over_bar = Clay_PointerOver(CLAY_ID("EditBar")) || Clay_PointerOver(CLAY_ID("Notice"));
    edit_mouse(mouse, in_canvas && !over_bar);
    if (!in_canvas && !E.pressed) return;
    if ((E.panning && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE))
        app.pan = Vector2Add(app.pan, GetMouseDelta());
    float wheel = in_canvas ? GetMouseWheelMove() : 0;
    if (wheel != 0) {
        float old = app.zoom;
        app.zoom = fminf(3.0f, fmaxf(0.25f, app.zoom * (1.0f + wheel * 0.1f)));
        /* Zoom around the cursor. */
        app.pan.x = mouse.x - (mouse.x - app.pan.x) * (app.zoom / old);
        app.pan.y = mouse.y - (mouse.y - app.pan.y) * (app.zoom / old);
    }
    if (app.focus || E.in) return; /* keys go to the text field */
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
    if (IsKeyPressed(KEY_F) && !ctrl) app.fit_pending = 1;
    if (!app.have_model) return;
    if (ctrl && IsKeyPressed(KEY_Z)) edit_undo();
    if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE)) ed_delete();
    if (IsKeyPressed(KEY_F2) || IsKeyPressed(KEY_ENTER)) ed_rename_selection();
    if (IsKeyPressed(KEY_ESCAPE)) select_none();
}

/* Typing into the canvas's text field. */
static void edit_typing(void) {
    int c;
    while ((c = GetCharPressed()) > 0) {
        int ok = E.in == IN_NAME ? isalnum(c) : (isalnum(c) || c == '_' || c == ',' || c == ' ');
        if (!ok || c >= 127) continue;
        if (E.fresh) { E.buf[0] = 0; E.fresh = 0; }
        size_t n = strlen(E.buf);
        if (E.in == IN_LABEL) c = toupper(c);
        if (n + 1 < sizeof E.buf) { E.buf[n] = (char)c; E.buf[n + 1] = 0; }
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (E.fresh) { E.buf[0] = 0; E.fresh = 0; }
        else if (*E.buf) E.buf[strlen(E.buf) - 1] = 0;
    }
    if (E.just_opened) E.just_opened = 0;
    else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) ed_commit_field();
    else if (IsKeyPressed(KEY_ESCAPE)) { E.in = IN_NONE; E.msg[0] = 0; }
}

/* What the editor draws over the machine: the dot to draw arrows from,
 * the node being moved, the arrow being drawn and the text field. */
static void draw_editing(void) {
    if (!app.have_model) return;
    const SgModel *m = &app.model;
    Vector2 mouse = GetMousePosition();
    float z = fmaxf(0.7f, app.zoom), u = app.ui * app.zoom;
    int hover = mouse.x <= canvas_width() ? node_at(mouse) : -1, dhover = mouse.x <= canvas_width() ? dot_at(mouse) : -1;
    for (int i = 0; i < m->n_states; i++) {
        int sel = !strcmp(E.sel, m->states[i].id);
        if (i != hover && i != dhover && !sel && !(E.linking && i == E.press_node)) continue;
        Rectangle r = node_rect(&m->states[i]);
        Vector2 d = dot_pos(r);
        DrawCircleV(d, dot_radius(), i == dhover ? ACCENT : NODE_FILL);
        DrawCircleLinesV(d, dot_radius(), ACCENT);
    }
    if (E.moving && E.press_node >= 0 && E.press_node < m->n_states) {
        Rectangle r = node_rect(&m->states[E.press_node]);
        Rectangle g = {mouse.x - E.grab.x, mouse.y - E.grab.y, r.width, r.height};
        DrawRectangleRounded(g, 0.18f, 8, (Color){219, 231, 253, 160});
        DrawRectangleRoundedLinesEx(g, 0.18f, 8, 1.5f * z, ACCENT);
    }
    if (E.linking && E.press_node >= 0 && E.press_node < m->n_states && Vector2Distance(mouse, E.press_at) > 5) {
        Vector2 a = dot_pos(node_rect(&m->states[E.press_node])), b = mouse;
        int to = node_at(mouse);
        if (to >= 0) {
            Rectangle tr = node_rect(&m->states[to]);
            DrawRectangleRoundedLinesEx((Rectangle){tr.x - 4, tr.y - 4, tr.width + 8, tr.height + 8}, 0.18f, 8, 2.0f, ACCENT);
            if (to != E.press_node) b = rect_exit(tr, a);
        }
        DrawLineEx(a, b, 2.0f * z, ACCENT);
        if (Vector2Distance(a, b) > 12) arrow_head(b, a, ACCENT);
    }
    if (!E.in) { E.in_box = (Rectangle){0}; return; }
    /* The text field, over the name or on the arrow. */
    float fs = fmaxf(12.0f, (E.in == IN_NAME ? 20.0f : 15.0f) * u);
    Vector2 ts = text_size(E.buf, fs);
    Rectangle box;
    if (E.in == IN_NAME) {
        const State *s = model_state(m, E.in_state);
        if (!s) { E.in = IN_NONE; return; }
        Rectangle r = node_rect(s);
        box = (Rectangle){r.x + 4, r.y + 4, fmaxf(r.width - 8, ts.x + 20), fs + 12};
    } else {
        const State *a = model_state(m, E.in_from), *b = model_state(m, E.in_to);
        if (!a || !b) { E.in = IN_NONE; return; }
        Vector2 at = {0, 0};
        int found = 0;
        for (int i = 0; i < n_labels; i++) {
            if (labels[i].from == (int)(a - m->states) && labels[i].to == (int)(b - m->states)) { at = labels[i].at; found = 1; }
        }
        Rectangle ra = node_rect(a), rb = node_rect(b);
        if (!found && a == b) at = (Vector2){ra.x + ra.width / 2, ra.y - 37.0f * app.zoom};
        else if (!found) {
            /* No arrow yet: show where it will go. */
            Vector2 p = rect_exit(ra, rect_center(rb)), q = rect_exit(rb, rect_center(ra));
            DrawLineEx(p, q, 1.6f * z, (Color){37, 99, 235, 140});
            arrow_head(q, p, (Color){37, 99, 235, 140});
            at = Vector2Lerp(p, q, 0.5f);
        }
        float w = fmaxf(ts.x + 20, 150.0f * app.ui);
        box = (Rectangle){at.x - w / 2, at.y - (fs + 12) / 2, w, fs + 12};
    }
    E.in_box = box;
    DrawRectangleRounded(box, 0.25f, 6, NODE_FILL);
    DrawRectangleRoundedLinesEx(box, 0.25f, 6, 2.0f, ACCENT);
    float tx = box.x + 8, ty = box.y + (box.height - ts.y) / 2;
    if (!*E.buf && E.in == IN_LABEL) text("EVENT_NAME", tx, ty, fs, (Color){170, 172, 178, 255});
    text(E.buf, tx, ty, fs, E.fresh ? ACCENT : INK);
    if (((int)(GetTime() * 2)) % 2 == 0) {
        float cx = tx + ts.x + 2;
        DrawLineEx((Vector2){cx, ty}, (Vector2){cx, ty + ts.y}, 1.5f, INK);
    }
}

/* ---- panel (Clay) ---- */

/* Every size in the panel is in points at scale 1; PX() scales it to the
 * window, like rem units in CSS, so the panel grows with the screen. */
#define PX(n) ((uint16_t)((n) * app.ui + 0.5f))
#define PXF(n) ((float)(n) * app.ui)

static Clay_TextElementConfig *T(int size, Clay_Color color) {
    return CLAY_TEXT_CONFIG({.fontId = FONT_BODY, .fontSize = PX(size), .textColor = color, .lineHeight = PX(size + 6)});
}

static void heading(const char *s) { CLAY_TEXT(cs(s), T(13, C_QUIET)); }

static Clay_ElementDeclaration card(void) {
    return (Clay_ElementDeclaration){
        .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(PX(14)), .childGap = PX(6)},
        .backgroundColor = C_CARD,
        .cornerRadius = CLAY_CORNER_RADIUS(PX(8)),
        .border = {.color = C_RULE, .width = {1, 1, 1, 1, 0}},
    };
}

/* ---- buttons: registered while the layout is built, dispatched after ---- */

typedef enum {
    A_NONE, A_EVENT, A_RESET,
    A_HOME, A_INIT, A_STEP_BACK, A_STEP_FWD, A_EDIT, A_VALIDATE, A_GEN, A_SIGNOFF, A_COMMIT,
    A_TICKET_GEN, A_REVIEW, A_GATE, A_AI_FILL, A_AI_IMPL, A_AI_CONNECT, A_AI_CHECK, A_AI_CONFIG,
    A_MODAL_SEND, A_MODAL_CLOSE, A_MODAL_STOP, A_TICKET_PREV, A_TICKET_NEXT,
    A_BROWSE_INTO, A_BROWSE_UP, A_OPEN_BROWSE, A_OPEN_RECENT, A_CREATE, A_GO_PATH, A_FOCUS,
    A_NEW_MODEL, A_ED_NEW, A_ED_RENAME, A_ED_INITIAL, A_ED_DELETE, A_ED_UNDO, A_ED_IGNORE,
} UiAction;

typedef struct {
    Clay_ElementId id;
    UiAction action;
    int arg;
    const char *text;
    int keep_scroll; /* a Steps card button: the panel stays where it is */
} Button;

static Button buttons[256];
static int n_buttons;
static int buttons_keep_scroll; /* set while the Steps card registers its buttons */

static void button_reg(Clay_ElementId id, UiAction a, int arg, const char *text) {
    if (n_buttons < (int)(sizeof buttons / sizeof buttons[0])) buttons[n_buttons++] = (Button){id, a, arg, text, buttons_keep_scroll};
}

/* A button; disabled ones draw quiet and do nothing. */
static int button_seq; /* ids restart with each layout */

static void button(const char *label, UiAction a, int arg, int enabled, int primary) {
    Clay_ElementId id = CLAY_IDI("Btn", button_seq++);
    CLAY({.id = id,
          .layout = {.padding = {PX(12), PX(12), PX(6), PX(6)}},
          .backgroundColor = !enabled ? C_BTN_OFF : primary ? (Clay_Hovered() ? (Clay_Color){29, 78, 216, 255} : C_ACCENT)
                                                            : (Clay_Hovered() ? C_BTN_HOVER : C_BTN),
          .cornerRadius = CLAY_CORNER_RADIUS(PX(6))}) {
        CLAY_TEXT(cs(label), T(14, !enabled ? C_QUIET : primary ? (Clay_Color){255, 255, 255, 255} : C_INK));
    }
    if (enabled) button_reg(id, a, arg, NULL);
}

/* ---- running CLI commands ---- */

static void refresh_status(void);

static void run_cli(const char *title, int use_pty, const char *a1, const char *a2, const char *a3) {
    if (app.job.running) return;
    char *argv[6] = {app.sg, (char *)a1, (char *)a2, (char *)a3, NULL, NULL};
    if (job_start(&app.job, title, argv, use_pty) != 0) {
        sb_init(&app.job.out);
        sb_printf(&app.job.out, "could not start %s\n", app.sg);
    }
    app.modal = use_pty;
    app.inline_pty = 0;
    app.modal_from = 0;
    app.show_output = !use_pty;
    app.job_note[0] = 0;
    app.copy_on_finish = 0;
}

static void run_shell(const char *title, const char *script) {
    if (app.job.running) return;
    char *argv[] = {"/bin/sh", "-c", (char *)script, NULL};
    job_start(&app.job, title, argv, 0);
    app.modal = 0;
    app.inline_pty = 0;
    app.show_output = 1;
    app.job_note[0] = 0;
    app.copy_on_finish = 0;
}

/* ---- your AI ---- */

static void ai_load(void) {
    int st;
    free(run_capture("command -v claude >/dev/null 2>&1", &st));
    app.have_claude = st == 0;
}

/* An AI step: an MCP server request (Claude Code with only the stategate
 * MCP tools), or the prompt on the clipboard to paste into any AI. */
static void run_ai(int drafts, int copy) {
    if (app.job.running) return;
    if (copy) {
        if (drafts) run_cli("stategate ticket prompt --drafts", 0, "ticket", "prompt", "--drafts");
        else run_cli("stategate ticket prompt", 0, "ticket", "prompt", NULL);
        app.copy_on_finish = 1;
        return;
    }
    /* The tools carry the task: get_task returns the brief. */
    if (drafts) run_cli("stategate ai fill", 0, "ai", "fill", NULL);
    else run_cli("stategate ai work", 0, "ai", "work", NULL);
    app.ai_loop = !drafts;
    snprintf(app.ai_ticket, sizeof app.ai_ticket, "%s", app.st.current);
}

/* A job ended: copy its output, or move the AI on to the next ticket. */
static void job_finished(void) {
    if (app.copy_on_finish) {
        app.copy_on_finish = 0;
        if (app.job.exit_code == 0 && app.job.out.s) {
            SetClipboardText(app.job.out.s);
            snprintf(app.job_note, sizeof app.job_note, "%s",
                     strstr(app.job.title, "mcp config") ? "Copied to the clipboard: paste it into your MCP client's servers config."
                                                         : "Copied to the clipboard: paste it into your AI.");
        }
        return;
    }
    if (!app.ai_loop) return;
    app.ai_loop = 0;
    if (app.job.exit_code != 0) return;
    /* `ticket run` starts the next approved ticket on green: carry on with
     * it. The same ticket still active means the AI stopped short of green. */
    if (!strcmp(app.st.current_status, "active") && strcmp(app.st.current, app.ai_ticket)) {
        char prev[64];
        snprintf(prev, sizeof prev, "%s", app.ai_ticket);
        run_ai(0, 0);
        snprintf(app.job_note, sizeof app.job_note, "%s is done; now %s.", prev, app.st.current);
    } else if (!strcmp(app.st.current_status, "active")) {
        snprintf(app.job_note, sizeof app.job_note, "%s is still active: the gate is not green yet. See the gate below.", app.ai_ticket);
    } else if (!strcmp(app.st.current_status, "locked")) {
        snprintf(app.job_note, sizeof app.job_note, "%s is locked after 3 red runs: split it (stategate ticket split).", app.ai_ticket);
    } else {
        snprintf(app.job_note, sizeof app.job_note, "No ticket left to implement.");
    }
}

static int ends_with(const SB *b, const char *tail) {
    size_t n = strlen(tail);
    return b->s && b->len >= n && !memcmp(b->s + b->len - n, tail, n);
}

/* The question a pty job is waiting on, if any. */
typedef enum { Q_NONE, Q_SIGNOFF, Q_REVIEW } Question;

static Question job_question(void) {
    if (!app.job.running || !app.job.pty) return Q_NONE;
    if (ends_with(&app.job.out, "Type 'sign off' to confirm: ")) return Q_SIGNOFF;
    if (ends_with(&app.job.out, "approve / skip / quit? ")) return Q_REVIEW;
    return Q_NONE;
}

/* Last n lines of s from offset `from`, into the frame arena. */
static Clay_String tail_lines(const SB *b, size_t from, int n) {
    if (!b->s || from >= b->len) return cs("");
    const char *start = b->s + from, *end = b->s + b->len;
    const char *p = end;
    int lines = 0;
    while (p > start) {
        if (p[-1] == '\n' && p != end && ++lines >= n) break;
        p--;
    }
    while (p < end && *p == '\n') p++;
    char tmp[16384];
    size_t len = (size_t)(end - p);
    if (len >= sizeof tmp) { p = end - (sizeof tmp - 1); len = sizeof tmp - 1; }
    memcpy(tmp, p, len);
    tmp[len] = 0;
    while (len && (tmp[len - 1] == '\n' || tmp[len - 1] == ' ')) tmp[--len] = 0;
    return cs(tmp);
}

/* ---- status ---- */

static int jb(cJSON *o, const char *k) { return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k)); }
static int ji(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}
static void js(cJSON *o, const char *k, char *out, size_t n) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    snprintf(out, n, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

static void refresh_status(void) {
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof cmd, "'%s' status --json 2>/dev/null", app.sg);
    int rc;
    char *out = run_capture(cmd, &rc);
    cJSON *o = cJSON_Parse(out);
    free(out);
    Status st = {0};
    if (o) {
        st.ok = 1;
        st.git = jb(o, "git");
        st.has_commit = jb(o, "has_commit");
        st.dirty = jb(o, "dirty");
        st.init = jb(o, "init");
        st.valid = jb(o, "valid");
        st.generated = jb(o, "generated");
        st.locked_items = jb(o, "locked_items");
        st.signed_off = jb(o, "signed_off");
        cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "tickets");
        st.t_draft = ji(t, "draft");
        st.t_open = ji(t, "open");
        st.t_approved = ji(t, "approved");
        st.t_active = ji(t, "active");
        st.t_done = ji(t, "done");
        st.t_locked = ji(t, "locked");
        js(o, "current_ticket", st.current, sizeof st.current);
        js(o, "current_status", st.current_status, sizeof st.current_status);
        js(o, "gate", st.gate, sizeof st.gate);
        cJSON_Delete(o);
    }
    app.st = st;
    app.status_at = GetTime();
}

/* ---- tickets ---- */

static void sl_clear(StrList *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

static void jlist(cJSON *o, const char *k, StrList *out) {
    cJSON *a = cJSON_GetObjectItemCaseSensitive(o, k), *e;
    cJSON_ArrayForEach(e, a) if (cJSON_IsString(e)) sl_push(out, e->valuestring);
}

static char *jdup(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return xstrdup(cJSON_IsString(v) ? v->valuestring : "");
}

/* Changes whenever a ticket file is written, added or removed. */
static long long tickets_signature(void) {
    DIR *d = opendir(".fsm/tickets");
    if (!d) return 0;
    long long sig = 1;
    struct dirent *e;
    char path[PATH_MAX];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        struct stat st;
        snprintf(path, sizeof path, ".fsm/tickets/%s", e->d_name);
        if (stat(path, &st) == 0)
            sig = sig * 31 + (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec + st.st_size;
    }
    closedir(d);
    return sig;
}

static int ticket_index(const char *id) {
    for (int i = 0; i < app.n_tickets; i++) if (!strcmp(app.tickets[i].id, id)) return i;
    return -1;
}

/* Reads every ticket from `stategate ticket list --json`. The ticket that
 * changed (your AI filled it in, you approved it) becomes the focus, so the
 * card shows each edit as it lands. */
static void tickets_reload(int follow_changes) {
    char cmd[PATH_MAX + 48];
    snprintf(cmd, sizeof cmd, "'%s' ticket list --json 2>/dev/null", app.sg);
    int rc;
    char *out = run_capture(cmd, &rc);
    cJSON *o = cJSON_Parse(out);
    free(out);
    if (!o) return; /* a half-written file: the next change reloads */
    char changed[64] = "";
    static ViewTicket next[MAX_TICKETS];
    int n = 0;
    cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(o, "tickets")) {
        if (n == MAX_TICKETS) break;
        ViewTicket *v = &next[n++];
        memset(v, 0, sizeof *v);
        js(t, "id", v->id, sizeof v->id);
        js(t, "status", v->status, sizeof v->status);
        v->title = jdup(t, "title");
        v->goal = jdup(t, "goal");
        v->step = jdup(t, "step");
        jlist(t, "nodes", &v->nodes);
        jlist(t, "files", &v->files);
        jlist(t, "errors", &v->errors);
        v->failures = ji(t, "failures");
        v->max_failures = ji(t, "max_failures");
        int k = ticket_index(v->id);
        const ViewTicket *old = k >= 0 ? &app.tickets[k] : NULL;
        if (!old || strcmp(old->status, v->status) || strcmp(old->title, v->title) || strcmp(old->goal, v->goal) ||
            v->errors.n != old->errors.n)
            snprintf(changed, sizeof changed, "%s", v->id);
    }
    cJSON_Delete(o);
    for (int i = 0; i < app.n_tickets; i++) {
        ViewTicket *v = &app.tickets[i];
        free(v->title); free(v->goal); free(v->step);
        sl_clear(&v->nodes); sl_clear(&v->files); sl_clear(&v->errors);
    }
    memcpy(app.tickets, next, sizeof(ViewTicket) * (size_t)n);
    app.n_tickets = n;
    /* Opening a project just shows what is there; later changes are followed. */
    if (follow_changes && changed[0]) {
        snprintf(app.ticket_focus, sizeof app.ticket_focus, "%s", changed);
        app.ticket_follow = 1;
    }
}

static void tickets_poll(void) {
    long long sig = tickets_signature();
    if (sig == app.tickets_sig) return;
    app.tickets_sig = sig;
    tickets_reload(1);
}

/* ---- the steps card ---- */

typedef enum { S_DONE, S_TODO, S_LATER } StepMark;

/* ~ for the home folder, and only the tail of a long path. */
static const char *short_path(const char *p) {
    static char out[4][256];
    static int k;
    char *o = out[k++ % 4];
    const char *home = getenv("HOME");
    char tmp[PATH_MAX];
    if (home && *home && !strncmp(p, home, strlen(home))) snprintf(tmp, sizeof tmp, "~%s", p + strlen(home));
    else snprintf(tmp, sizeof tmp, "%s", p);
    /* As many characters as fit beside a step's button at this text size. */
    size_t n = strlen(tmp), max = (size_t)fmaxf(20, (panel_width() - PXF(190)) / PXF(7));
    if (max > 200) max = 200;
    if (n > max) snprintf(o, 256, "...%s", tmp + n - (max - 3));
    else snprintf(o, 256, "%s", tmp);
    return o;
}

/* The steps, in order. The card shows one at a time: the first one not
 * done, unless the arrows took the person to another. */
enum {
    ST_INIT, ST_MODEL, ST_VALIDATE, ST_GEN, ST_SIGNOFF, ST_SAVE,
    ST_TICKETS, ST_REVIEW, ST_IMPLEMENT, ST_GATE, N_STEPS
};

static int initialized(const Status *s) { return s->git && s->init; }

static void step_marks(StepMark m[N_STEPS]) {
    const Status *s = &app.st;
    int has_model = app.model_path != NULL;
    int tickets_any = s->t_open + s->t_approved + s->t_active + s->t_done + s->t_locked;
    int saved = s->git && s->has_commit && !s->dirty;
    m[ST_INIT] = initialized(s) ? S_DONE : S_TODO;
    m[ST_MODEL] = has_model && s->valid ? S_DONE : S_TODO;
    m[ST_VALIDATE] = s->valid ? S_DONE : S_TODO;
    m[ST_GEN] = s->generated ? S_DONE : S_TODO;
    m[ST_SIGNOFF] = s->valid && s->signed_off ? S_DONE : S_TODO;
    /* Once a ticket is active, `ticket run` commits each ticket's work. */
    m[ST_SAVE] = saved || s->t_active > 0 ? S_DONE : S_TODO;
    m[ST_TICKETS] = tickets_any && !s->t_draft ? S_DONE : S_TODO;
    m[ST_REVIEW] = tickets_any && !s->t_open && !s->t_draft ? S_DONE : S_TODO;
    m[ST_IMPLEMENT] = tickets_any && !s->t_draft && !s->t_open && !s->t_approved && !s->t_active && !s->t_locked ? S_DONE : S_TODO;
    m[ST_GATE] = !s->t_active && !strcmp(s->gate, "green") ? S_DONE : S_TODO;
}

/* The step to work on: the first one not done, or the last when all are. */
static int current_step(void) {
    StepMark m[N_STEPS];
    step_marks(m);
    for (int i = 0; i < N_STEPS; i++) if (m[i] != S_DONE) return i;
    return N_STEPS - 1;
}

/* The step the card shows. It follows the current step until the person
 * steps away with an arrow, and follows again once they come back to it. */
static int shown_step(void) {
    int cur = current_step();
    if (app.step_follow || app.step_view < 0 || app.step_view >= N_STEPS) {
        app.step_view = cur;
        app.step_follow = 1;
    }
    return app.step_view;
}

static void step_go(int delta) {
    int v = shown_step() + delta;
    if (v < 0 || v >= N_STEPS) return;
    app.step_view = v;
    app.step_follow = v == current_step();
}

static const char *const STEP_TITLES[N_STEPS] = {
    "Initialize the project", "Draw the state machine", "Validate", "Generate code and tests",
    "Sign off locked safety items", "Save to git", "Tickets: draft, AI fills in", "Review tickets: approve or skip",
    "AI writes node logic, ticket by ticket", "Run the 11 checks",
};

static void arrow(const char *label, UiAction a, int enabled) {
    if (enabled) button(label, a, 0, 1, 0);
    else CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(PXF(36))}}}) {}
}

static void steps_card_body(void) {
    const Status *s = &app.st;
    int busy = app.job.running;
    int has_model = app.model_path != NULL;
    int tickets_any = s->t_open + s->t_approved + s->t_active + s->t_done + s->t_locked;
    int saved = s->git && s->has_commit && !s->dirty;
    StepMark m[N_STEPS];
    step_marks(m);
    int cur = current_step(), k = shown_step();
    int all_done = m[N_STEPS - 1] == S_DONE && cur == N_STEPS - 1;
    int is_next = k == cur && m[k] != S_DONE;

    /* Back, where we are, forward. */
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = PX(8), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        arrow("<", A_STEP_BACK, k > 0);
        CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = PX(6),
                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}}) {
            CLAY_TEXT(csf("STEP %d OF %d%s", k + 1, N_STEPS, k == cur ? "" : k < cur ? "  (done)" : "  (later)"), T(13, C_QUIET));
            /* One dot per step: green done, blue the one shown. */
            CLAY({.layout = {.childGap = PX(5)}}) {
                for (int i = 0; i < N_STEPS; i++)
                    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(PXF(i == k ? 18 : 8)), CLAY_SIZING_FIXED(PXF(8))}},
                          .backgroundColor = i == k ? C_ACCENT : m[i] == S_DONE ? C_GREEN : C_RULE,
                          .cornerRadius = CLAY_CORNER_RADIUS(PX(4))}) {}
            }
        }
        arrow(">", A_STEP_FWD, k < N_STEPS - 1);
    }

    const char *detail = "";
    switch (k) {
    case ST_INIT:
        detail = initialized(s) ? "git repository, AGENTS.md, CLAUDE.md, CHANGELOG.md and the pre-commit hook are in place"
                 : !s->git ? "$ git init && stategate init  (AGENTS.md, CLAUDE.md, CHANGELOG.md, pre-commit hook)"
                           : "$ stategate init  (AGENTS.md, CLAUDE.md, CHANGELOG.md, pre-commit hook)";
        break;
    case ST_MODEL:
        detail = !has_model ? "start a new state machine and draw it on the canvas, or write the model .yaml in this folder with your editor"
                 : s->valid ? app.model_path : "the model has errors: see the box above the steps";
        break;
    case ST_VALIDATE: detail = "$ stategate validate"; break;
    case ST_GEN: detail = s->generated ? "up to date with the model" : "$ stategate gen"; break;
    case ST_SIGNOFF:
        detail = !s->valid ? "$ stategate signoff" : !s->locked_items ? "nothing is locked: not needed"
                 : s->signed_off ? "signed" : "$ stategate signoff  (you confirm)";
        break;
    case ST_SAVE:
        detail = !s->git ? "needs a repository" : saved ? "everything is committed"
                 : s->t_active > 0 ? "Run gate commits each ticket's work when it passes"
                                   : "$ git add -A && git commit  (needed before review)";
        break;
    case ST_TICKETS:
        detail = s->t_draft ? TextFormat("%d draft(s) need a title and goal from your AI", s->t_draft)
                 : tickets_any ? TextFormat("%d ticket(s) written", tickets_any) : "$ stategate ticket gen";
        break;
    case ST_REVIEW:
        detail = s->t_open ? TextFormat("%d open ticket(s) to review", s->t_open) : "$ stategate ticket start  (you decide)";
        break;
    case ST_IMPLEMENT:
        detail = *s->current ? TextFormat("active: %s (%s), %d approved after it. %s", s->current, s->current_status, s->t_approved,
                                          "An MCP server request works each ticket until green.")
                 : s->t_approved ? TextFormat("%d approved, waiting", s->t_approved) : "needs an approved ticket";
        break;
    case ST_GATE:
        detail = !strcmp(s->current_status, "active") ? "$ stategate ticket run  (green commits and finishes the ticket)"
                                                      : "$ stategate gate";
        break;
    }

    CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = PX(6),
                     .padding = {PX(4), PX(4), PX(10), PX(4)}}}) {
        CLAY({.layout = {.childGap = PX(10), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            if (m[k] == S_DONE) CLAY_TEXT(cs("OK"), T(16, C_GREEN));
            CLAY_TEXT(cs(STEP_TITLES[k]), T(20, is_next ? C_ACCENT : C_INK));
        }
        CLAY_TEXT(cs(detail), T(14, C_QUIET));
        if (all_done) CLAY_TEXT(cs("Every step is done. Change the model to start the next round."), T(14, C_GREEN));
    }

    CLAY({.layout = {.childGap = PX(8), .padding = {PX(4), PX(4), PX(4), PX(4)}}}) {
        switch (k) {
        case ST_INIT: if (!initialized(s)) button("Initialize", A_INIT, 0, !busy, 1); break;
        case ST_MODEL:
            if (has_model) button("Open in editor", A_EDIT, 0, 1, 0);
            else button("New state machine", A_NEW_MODEL, 0, initialized(s), is_next);
            break;
        case ST_VALIDATE: button("Validate", A_VALIDATE, 0, !busy && has_model, is_next); break;
        case ST_GEN: button("Generate", A_GEN, 0, !busy && s->valid, is_next); break;
        case ST_SIGNOFF:
            if (s->valid && s->locked_items && !s->signed_off) button("Sign off...", A_SIGNOFF, 0, !busy, is_next);
            break;
        case ST_SAVE:
            if (s->git && !saved && s->t_active == 0) button("Commit", A_COMMIT, 0, !busy && s->dirty, is_next && s->dirty);
            break;
        case ST_TICKETS:
            button("Draft tickets", A_TICKET_GEN, 0, !busy && s->generated && s->init, is_next && !s->t_draft);
            button("MCP Server Request", A_AI_FILL, 0, !busy && s->t_draft > 0 && app.have_claude, is_next && s->t_draft && app.have_claude);
            button("Copy Prompt", A_AI_FILL, 1, !busy && s->t_draft > 0, is_next && s->t_draft && !app.have_claude);
            break;
        case ST_REVIEW: button("Start review", A_REVIEW, 0, !busy && s->t_open > 0 && saved, is_next); break;
        case ST_IMPLEMENT:
        {
            int active = !strcmp(s->current_status, "active");
            button("MCP Server Request", A_AI_IMPL, 0, !busy && active && app.have_claude, is_next && active && app.have_claude);
            button("Copy Prompt", A_AI_IMPL, 1, !busy && active, is_next && active && !app.have_claude);
        }
            break;
        case ST_GATE: button("Run gate", A_GATE, 0, !busy && has_model, is_next); break;
        }
    }
}

/* Over the canvas while the folder is not set up: the same Initialize. */
/* A card centered on the canvas when there is nothing to draw yet:
 * the folder needs Initialize, or the model file is not there. */
static void canvas_notice_card(void) {
    if (!app.st.ok || app.have_model) return;
    int need_init = !initialized(&app.st);
    if (!need_init && app.model_path) return; /* a model that does not parse: the panel shows why */
    const char *name = strrchr(app.project, '/') ? strrchr(app.project, '/') + 1 : "model";
    CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_FIXED(fminf(PXF(560), canvas_width() * 0.8f))},
                     .padding = CLAY_PADDING_ALL(PX(22)), .childGap = PX(10)},
          .backgroundColor = C_CARD, .cornerRadius = CLAY_CORNER_RADIUS(PX(10)), .border = {.color = C_RULE, .width = {1, 1, 1, 1, 0}},
          .id = CLAY_ID("Notice"),
          .floating = {.attachTo = CLAY_ATTACH_TO_PARENT, .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_CENTER, .parent = CLAY_ATTACH_POINT_CENTER_CENTER}}}) {
        if (need_init) {
            CLAY_TEXT(cs("This folder is not set up for stategate yet"), T(20, C_INK));
            CLAY_TEXT(cs(app.st.git ? "Initialize writes AGENTS.md, CLAUDE.md, CHANGELOG.md and the pre-commit hook (stategate init)."
                                    : "Initialize makes it a git repository and writes AGENTS.md, CLAUDE.md, CHANGELOG.md and the pre-commit hook (git init, then stategate init)."),
                      T(14, C_QUIET));
            button("Initialize", A_INIT, 0, !app.job.running, 1);
        } else {
            CLAY_TEXT(cs("No model in this folder yet"), T(20, C_INK));
            CLAY_TEXT(cs("Start one here: the black dot and one state that loops back on itself. Then draw the rest on the canvas; "
                         "every change is saved to the model's YAML."), T(14, C_QUIET));
            button("New state machine", A_NEW_MODEL, 0, 1, 1);
            CLAY_TEXT(csf("Or write %s.yaml here with your editor (examples/posture/posture.yaml is a template): it appears as soon as you save it.", name),
                      T(13, C_QUIET));
        }
    }
}

/* Working through the steps keeps the panel where it is: the output card
 * sits right below, and jumping to it would push the step out of view. */
static void steps_card(void) {
    buttons_keep_scroll = 1;
    CLAY(card()) { steps_card_body(); }
    buttons_keep_scroll = 0;
}

/* ---- the ticket card: below the steps, the ticket being written or reviewed ---- */

/* The ticket `stategate ticket start` is asking about: its last "[i/n] ID"
 * line. Returns 1 and fills id, i and n when the review is waiting. */
static int review_asking(char id[64], int *i, int *n) {
    if (job_question() != Q_REVIEW) return 0;
    const SB *b = &app.job.out;
    for (size_t k = b->len; k-- > 0;) {
        if (b->s[k] != '[' || (k > 0 && b->s[k - 1] != '\n')) continue;
        char tmp[64];
        if (sscanf(b->s + k, "[%d/%d] %63s", i, n, tmp) == 3) {
            snprintf(id, 64, "%s", tmp);
            return 1;
        }
    }
    return 0;
}

/* The ticket the card follows: the one under review, else the one that last
 * changed, else the first still needing someone (draft, open, active). */
static int ticket_focus_index(void) {
    char id[64];
    int qi, qn;
    if (review_asking(id, &qi, &qn) && ticket_index(id) >= 0) return ticket_index(id);
    if (app.ticket_focus[0] && ticket_index(app.ticket_focus) >= 0) return ticket_index(app.ticket_focus);
    static const char *const order[] = {"draft", "open", "active", "locked", "approved"};
    for (int k = 0; k < 5; k++)
        for (int i = 0; i < app.n_tickets; i++) if (!strcmp(app.tickets[i].status, order[k])) return i;
    return app.n_tickets - 1;
}

static int shown_ticket(void) {
    if (app.ticket_follow || app.ticket_view < 0 || app.ticket_view >= app.n_tickets) {
        app.ticket_view = ticket_focus_index();
        app.ticket_follow = 1;
    }
    return app.ticket_view;
}

static void ticket_go(int delta) {
    int v = shown_ticket() + delta;
    if (v < 0 || v >= app.n_tickets) return;
    app.ticket_view = v;
    app.ticket_follow = v == ticket_focus_index();
}

static Clay_Color ticket_color(const char *status) {
    if (!strcmp(status, "draft")) return C_AMBER;
    if (!strcmp(status, "open")) return C_ACCENT;
    if (!strcmp(status, "locked")) return C_RED;
    if (!strcmp(status, "approved") || !strcmp(status, "active") || !strcmp(status, "done")) return C_GREEN;
    return C_QUIET;
}

static const char *ticket_state_line(const ViewTicket *t) {
    if (!strcmp(t->status, "draft")) return "DRAFT: your AI writes the title and goal";
    if (!strcmp(t->status, "open")) return "OPEN: waiting for your review";
    if (!strcmp(t->status, "approved")) return "APPROVED: waiting its turn for your AI";
    if (!strcmp(t->status, "active")) return "ACTIVE: your AI is working on it";
    if (!strcmp(t->status, "done")) return "DONE: the gate was green";
    if (!strcmp(t->status, "locked")) return "LOCKED: too many red runs, split it";
    if (!strcmp(t->status, "split")) return "SPLIT into smaller tickets";
    return t->status;
}

/* One titled block of a ticket; `who` says where its words come from. */
static void ticket_block_head(const char *name, const char *who);

#define TICKET_BLOCK(name, who)                                                                                      \
    CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_GROW(0)},                  \
                     .padding = {PX(12), PX(12), PX(8), PX(10)}, .childGap = PX(4)},                                  \
          .backgroundColor = C_PANEL, .cornerRadius = CLAY_CORNER_RADIUS(PX(6)),                                      \
          .border = {.color = C_RULE, .width = {1, 1, 1, 1, 0}}})                                                     \
    for (int once_ = (ticket_block_head(name, who), 1); once_; once_ = 0)

static void ticket_block_head(const char *name, const char *who) {
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = PX(8)}}) {
        CLAY_TEXT(cs(name), T(12, C_QUIET));
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
        CLAY_TEXT(cs(who), T(12, C_QUIET));
    }
}

/* An AI-written field: its text, or what it is waiting on. */
static void ai_text(const char *v, int size, int writing) {
    if (v && *v) CLAY_TEXT(cs(v), T(size, C_INK));
    else CLAY_TEXT(cs(writing ? "your AI is writing this..." : "not written yet"), T(14, writing ? C_ACCENT : C_AMBER));
}

static void ticket_card_body(void) {
    int k = shown_ticket(), focus = ticket_focus_index();
    const ViewTicket *t = &app.tickets[k];
    char qid[64];
    int qi = 0, qn = 0;
    int asking = review_asking(qid, &qi, &qn) && !strcmp(qid, t->id);
    int reviewing = app.inline_pty && app.job.running;
    int writing = app.job.running && (strstr(app.job.title, "--drafts") || strstr(app.job.title, "ai fill")) && !strcmp(t->status, "draft");

    /* Back, which ticket, forward: like the steps above. */
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = PX(8), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        arrow("<", A_TICKET_PREV, k > 0);
        CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = PX(6),
                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}}) {
            if (reviewing && qn) CLAY_TEXT(csf("REVIEWING %d OF %d", qi, qn), T(13, C_ACCENT));
            else CLAY_TEXT(csf("TICKET %d OF %d%s", k + 1, app.n_tickets, k == focus ? "" : "  (not the current one)"), T(13, C_QUIET));
            CLAY({.layout = {.childGap = PX(5)}}) {
                for (int i = 0; i < app.n_tickets && i < 40; i++)
                    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(PXF(i == k ? 18 : 8)), CLAY_SIZING_FIXED(PXF(8))}},
                          .backgroundColor = ticket_color(app.tickets[i].status), .cornerRadius = CLAY_CORNER_RADIUS(PX(4))}) {}
            }
        }
        arrow(">", A_TICKET_NEXT, k < app.n_tickets - 1);
    }

    CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = PX(2),
                     .padding = {PX(4), PX(4), PX(6), PX(2)}}}) {
        CLAY_TEXT(cs(t->id), T(20, C_INK));
        CLAY_TEXT(cs(ticket_state_line(t)), T(13, ticket_color(t->status)));
    }

    /* The review's question, answered here: the same words typed at
     * `stategate ticket start` in a terminal. */
    if (asking) {
        CLAY_TEXT(cs("Approve this ticket? Your AI will work on it, limited to the files below."), T(14, C_INK));
        CLAY({.layout = {.childGap = PX(8), .padding = {PX(4), PX(4), PX(4), PX(4)}}}) {
            button("Approve", A_MODAL_SEND, 2, 1, 1);
            button("Skip", A_MODAL_SEND, 3, 1, 0);
            button("Stop reviewing", A_MODAL_SEND, 4, 1, 0);
        }
    } else if (reviewing) {
        CLAY({.layout = {.childGap = PX(8), .padding = {PX(4), PX(4), PX(4), PX(4)}, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            CLAY_TEXT(cs(qn ? TextFormat("Reviewing %s: use the arrows to come back to it.", qid) : "Reviewing..."), T(14, C_ACCENT));
            button("Stop", A_MODAL_STOP, 0, 1, 0);
        }
    }

    CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = PX(8)}}) {
        TICKET_BLOCK("TITLE", "written by your AI") ai_text(t->title, 17, writing);
        TICKET_BLOCK("GOAL", "written by your AI") ai_text(t->goal, 14, writing);
        TICKET_BLOCK("NODES", "from the model") {
            for (int i = 0; i < t->nodes.n; i++) CLAY_TEXT(cs(t->nodes.v[i]), T(14, C_INK));
            if (!t->nodes.n) CLAY_TEXT(cs("(none)"), T(14, C_QUIET));
        }
        TICKET_BLOCK("FILES YOUR AI MAY CHANGE", "from ticket gen") {
            for (int i = 0; i < t->files.n; i++) CLAY_TEXT(cs(t->files.v[i]), T(14, C_INK));
            if (!t->files.n) CLAY_TEXT(cs("(none)"), T(14, C_QUIET));
        }
        TICKET_BLOCK("STEP", "from the model") CLAY_TEXT(cs(t->step), T(14, C_INK));
        if (t->errors.n) {
            TICKET_BLOCK("LAST RED GATE RUN", TextFormat("%d of %d failed runs", t->failures, t->max_failures)) {
                for (int i = 0; i < t->errors.n && i < 8; i++) CLAY_TEXT(cs(t->errors.v[i]), T(13, C_RED));
            }
        }
    }
}

static void ticket_card(void) {
    int review = app.inline_pty && app.job.running;
    if (!app.n_tickets || (shown_step() < ST_TICKETS && !review)) return;
    /* Each new review question brings the card back to its ticket. */
    static char last_asked[64];
    char qid[64] = "";
    int qi, qn;
    if (review_asking(qid, &qi, &qn) && strcmp(qid, last_asked)) app.ticket_follow = 1;
    snprintf(last_asked, sizeof last_asked, "%s", qid);
    buttons_keep_scroll = 1;
    Clay_ElementDeclaration d = card();
    d.id = CLAY_ID("Ticket");
    CLAY(d) { ticket_card_body(); }
    buttons_keep_scroll = 0;
}

static void output_body(void) {
    const Job *j = &app.job;
    heading("OUTPUT");
    if (!j->title[0]) { CLAY_TEXT(cs("Nothing run yet. Each step's button shows its output here."), T(14, C_QUIET)); return; }
    CLAY_TEXT(csf("$ %s", j->title), T(14, C_INK));
    CLAY_TEXT(cs(j->running ? "running..." : j->exit_code == 0 ? "finished" : TextFormat("failed (exit %d)", j->exit_code)),
              T(14, j->running ? C_ACCENT : j->exit_code == 0 ? C_GREEN : C_RED));
    if (app.job_note[0]) CLAY_TEXT(cs(app.job_note), T(14, C_ACCENT));
    if (!j->pty || app.inline_pty) CLAY_TEXT(tail_lines(&j->out, 0, 24), T(13, C_INK));
}

static void text_field(int which, char *buf, const char *placeholder, float width);


static void ai_card(void) {
    CLAY(card()) {
        heading("YOUR AI");
        CLAY_TEXT(cs("The AI steps (tickets and implementing) each have two buttons:"), T(13, C_QUIET));
        CLAY_TEXT(cs("MCP Server Request runs Claude Code headless with only the stategate MCP tools: it reads the project, "
                     "fills in tickets, writes the active ticket's files and runs the gate, and can do nothing else."),
                  T(13, C_INK));
        CLAY_TEXT(cs("$ stategate ai fill   /   $ stategate ai work"), T(13, C_QUIET));
        if (!app.have_claude)
            CLAY_TEXT(cs("claude is not on PATH: install Claude Code to use MCP Server Request."), T(14, C_AMBER));
        CLAY_TEXT(cs("Copy Prompt puts the prompt on the clipboard for you to paste into any AI."), T(13, C_INK));
        CLAY_TEXT(cs("$ stategate ticket prompt --drafts   /   $ stategate ticket prompt"), T(13, C_QUIET));
        CLAY_TEXT(cs("Any MCP client can use the same tools: connect your own Claude Code chat in this folder, or copy the "
                     "server entry into another client's MCP settings (Cursor, Claude Desktop, ...)."),
                  T(13, C_QUIET));
        CLAY({.layout = {.childGap = PX(8)}}) {
            button("Connect Claude Code", A_AI_CONNECT, 0, !app.job.running && app.have_claude, 0);
            button("Check", A_AI_CHECK, 0, !app.job.running && app.have_claude, 0);
            button("Copy MCP config", A_AI_CONFIG, 0, !app.job.running, 0);
        }
    }
}

static void output_card(void) {
    Clay_ElementDeclaration d = card();
    d.id = CLAY_ID("Output");
    d.clip.horizontal = true;
    CLAY(d) { output_body(); }
}

/* Card bodies return early freely: the CLAY() block that wraps them
 * must always close, so no return may sit inside it. */
static void gate_body(void) {
    const GateStatus *g = &app.gate;
    heading("GATE  (from .fsm/gate.json)");
    if (!g->present) {
        CLAY_TEXT(cs("No gate run yet. Run: stategate gate"), T(18, C_QUIET));
        for (int i = 0; i < g->errors.n; i++) CLAY_TEXT(cs(g->errors.v[i]), T(14, C_RED));
        return;
    }
    CLAY_TEXT(cs(g->ok ? (g->release ? "GREEN (release)" : "GREEN") : (g->release ? "RED (release)" : "RED")),
              T(28, g->ok ? C_GREEN : C_RED));
    CLAY_TEXT(csf("last run %s", g->time ? g->time : "?"), T(14, C_QUIET));
    if (g->ticket)
        CLAY_TEXT(csf("Ticket %s: %s, %d of %d failed runs", g->ticket, g->ticket_status ? g->ticket_status : "?",
                      g->failures, g->max_failures),
                  T(16, g->ticket_status && !strcmp(g->ticket_status, "locked") ? C_RED : C_INK));
    else
        CLAY_TEXT(cs("No active ticket"), T(16, C_QUIET));
    int stubs_counted = 1;
    for (int i = 0; i < g->n_checks; i++)
        if (!strcmp(g->checks[i].name, "stubs") && !strcmp(g->checks[i].status, "skip")) stubs_counted = 0;
    if (stubs_counted) CLAY_TEXT(csf("Stubs: %d", g->stub_count), T(16, g->stub_count ? C_AMBER : C_INK));
    else CLAY_TEXT(cs("Stubs: not counted (the gate stopped earlier)"), T(16, C_QUIET));
    for (int i = 0; i < g->stubs.n; i++) CLAY_TEXT(csf("  %s", g->stubs.v[i]), T(14, C_AMBER));
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(1)}}, .backgroundColor = C_RULE}) {}
    if (g->ok) {
        /* Green: one line is enough. Red: every check, with the failure's messages. */
        CLAY_TEXT(csf("All %d checks pass", g->n_checks), T(14, C_GREEN));
        return;
    }
    for (int i = 0; i < g->n_checks; i++) {
        const GateCheck *c = &g->checks[i];
        int pass = !strcmp(c->status, "pass"), fail = !strcmp(c->status, "fail");
        CLAY({.layout = {.childGap = PX(10), .sizing = {.width = CLAY_SIZING_GROW(0)}}}) {
            CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(PXF(44))}}}) {
                CLAY_TEXT(cs(pass ? "PASS" : fail ? "FAIL" : "skip"), T(14, pass ? C_GREEN : fail ? C_RED : C_QUIET));
            }
            CLAY_TEXT(csf("%2d  %s", i + 1, c->name), T(14, fail ? C_RED : C_INK));
        }
        if (fail)
            for (int k = 0; k < c->messages.n && k < 6; k++) CLAY_TEXT(cs(c->messages.v[k]), T(13, C_RED));
    }
}

static void gate_card(void) {
    CLAY(card()) { gate_body(); }
}

static void sim_body(void) {
    heading("SIMULATOR  (click an event)");
    if (!app.have_model) { CLAY_TEXT(cs("No model"), T(16, C_QUIET)); return; }
    const State *s = &app.model.states[app.cur];
    CLAY_TEXT(csf("Active: %s", s->name), T(20, C_ACCENT));
    CLAY_TEXT(cs(s->id), T(13, C_QUIET));
    CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = PX(6), .sizing = {.width = CLAY_SIZING_GROW(0)}}}) {
        for (int i = 0; i < app.model.events.n && i < 64; i++) {
            const char *ev = app.model.events.v[i];
            const char *target = state_target(s, ev);
            const State *t = target ? model_state(&app.model, target) : NULL;
            Clay_ElementId id = CLAY_IDI("Event", i);
            CLAY({.id = id,
                  .layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = {PX(10), PX(10), PX(6), PX(6)}, .childGap = PX(8)},
                  .backgroundColor = target ? (Clay_Hovered() ? C_BTN_HOVER : C_BTN) : C_BTN_OFF,
                  .cornerRadius = CLAY_CORNER_RADIUS(PX(6))}) {
                CLAY_TEXT(cs(ev), T(15, target ? C_INK : C_QUIET));
                CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
                CLAY_TEXT(t ? csf("-> %s", t->name) : cs("ignored"), T(14, target ? C_ACCENT : C_QUIET));
            }
            button_reg(id, A_EVENT, i, NULL);
        }
        Clay_ElementId rid = CLAY_ID("Reset");
        CLAY({.id = rid, .layout = {.padding = {PX(10), PX(10), PX(6), PX(6)}},
              .backgroundColor = Clay_Hovered() ? C_BTN_HOVER : C_BTN_OFF, .cornerRadius = CLAY_CORNER_RADIUS(PX(6))}) {
            CLAY_TEXT(cs("Reset to initial state"), T(14, C_INK));
        }
        button_reg(rid, A_RESET, 0, NULL);
    }
    if (*app.flash) CLAY_TEXT(cs(app.flash), T(14, C_AMBER));
    for (int i = app.history.n - 1; i >= 0; i--) CLAY_TEXT(cs(app.history.v[i]), T(13, i == app.history.n - 1 ? C_INK : C_QUIET));
}

static void sim_card(void) {
    CLAY(card()) { sim_body(); }
}

static void ladder_body(void) {
    heading("STEP LADDER");
    if (!app.have_model || !app.model.n_steps) { CLAY_TEXT(cs("No steps in the model"), T(14, C_QUIET)); return; }
    for (int i = 0; i < app.model.n_steps; i++) {
        const Step *st = &app.model.steps[i];
        int active = !strcmp(st->status, "active"), done = !strcmp(st->status, "done");
        CLAY_TEXT(csf("%s  %s", st->id, st->status), T(15, active ? C_ACCENT : done ? C_GREEN : C_QUIET));
        if (st->goal) CLAY_TEXT(cs(st->goal), T(13, C_INK));
    }
}

static void ladder_card(void) {
    CLAY(card()) { ladder_body(); }
}

static void errors_card(void) {
    if (!app.model_errors.n) return;
    CLAY(card()) {
        heading(app.have_model && app.model_stale ? "MODEL ERRORS  (showing the last model that parsed)" : "MODEL ERRORS");
        for (int i = 0; i < app.model_errors.n && i < 12; i++) CLAY_TEXT(cs(app.model_errors.v[i]), T(13, C_RED));
    }
}

static void header_row(void) {
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = PX(8), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        CLAY_TEXT(cs("stategate"), T(22, C_INK));
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
        button("Projects", A_HOME, 0, !app.job.running, 0);
    }
}

/* ---- the modal: a pty command waiting for the person ---- */

static void modal_body(void) {
    const Job *j = &app.job;
    Question q = job_question();
    CLAY_TEXT(csf("$ %s", j->title), T(15, C_QUIET));
    CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(PX(12))},
          .backgroundColor = C_PANEL, .cornerRadius = CLAY_CORNER_RADIUS(PX(6))}) {
        CLAY_TEXT(tail_lines(&j->out, app.modal_from, 40), T(15, C_INK));
    }
    CLAY({.layout = {.childGap = PX(10)}}) {
        if (q == Q_SIGNOFF) {
            button("Sign off", A_MODAL_SEND, 1, 1, 1);
            button("Cancel", A_MODAL_SEND, 0, 1, 0);
        } else if (q == Q_REVIEW) {
            button("Approve", A_MODAL_SEND, 2, 1, 1);
            button("Skip", A_MODAL_SEND, 3, 1, 0);
            button("Stop reviewing", A_MODAL_SEND, 4, 1, 0);
        } else if (j->running) {
            CLAY_TEXT(cs("working..."), T(15, C_ACCENT));
            button("Stop", A_MODAL_STOP, 0, 1, 0);
        } else {
            CLAY_TEXT(cs(j->exit_code == 0 ? "Done." : "Finished without a change."), T(15, j->exit_code == 0 ? C_GREEN : C_AMBER));
            button("Close", A_MODAL_CLOSE, 0, 1, 1);
        }
    }
}

static void modal_layer(void) {
    if (!app.modal) return;
    CLAY({.id = CLAY_ID("Modal"),
          .layout = {.sizing = {CLAY_SIZING_FIXED((float)GetScreenWidth()), CLAY_SIZING_FIXED((float)GetScreenHeight())},
                     .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = {20, 20, 25, 120},
          .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 10}}) {
        CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_FIXED(fminf(PXF(760), GetScreenWidth() * 0.9f))}, .padding = CLAY_PADDING_ALL(PX(20)), .childGap = PX(12)},
              .backgroundColor = C_CARD, .cornerRadius = CLAY_CORNER_RADIUS(PX(10)), .border = {.color = C_RULE, .width = {1, 1, 1, 1, 0}}}) {
            modal_body();
        }
    }
}

/* The machine's nodes as a Clay grid: columns of cells, every cell in a row
 * as tall as the row's tallest node and every cell in a column as wide as
 * its widest, with fixed gaps between. Clay only places the boxes; the
 * canvas draws them, at the positions Clay computed, under the edges. */
static void canvas_grid(void) {
    if (!app.have_model) return;
    const SgModel *m = &app.model;
    int n = m->n_states < MAX_GRID ? m->n_states : MAX_GRID;
    static float colw[MAX_GRID], rowh[MAX_GRID];
    for (int c = 0; c < app.n_cols; c++) colw[c] = 0;
    for (int r = 0; r < app.n_rows; r++) rowh[r] = 0;
    for (int i = 0; i < n; i++) {
        colw[app.col[i]] = fmaxf(colw[app.col[i]], node_width(&m->states[i]));
        rowh[app.row[i]] = fmaxf(rowh[app.row[i]], node_height(&m->states[i]));
    }
    float z = app.zoom * app.ui;
    /* Room between nodes for edge labels, above for self-loops, and on the
     * left for the initial-state dot. A label on an edge between two nodes
     * of one row must fit in the gap between them. */
    float need = 100 * app.ui;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            if (i == j || app.row[i] != app.row[j]) continue;
            char label[256] = "";
            const State *a = &m->states[i];
            for (int k = 0; k < a->n_on; k++) {
                if (strcmp(a->on[k].target, m->states[j].id)) continue;
                if (*label) strncat(label, ", ", sizeof label - strlen(label) - 1);
                strncat(label, a->on[k].event, sizeof label - strlen(label) - 1);
            }
            if (*label) need = fmaxf(need, text_size(label, 15.0f * app.ui).x + 40 * app.ui);
        }
    uint16_t colgap = (uint16_t)(need * app.zoom), rowgap = (uint16_t)(90 * z);
    CLAY({.id = CLAY_ID("Grid"),
          .layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = colgap,
                     .padding = {(uint16_t)(44 * z), (uint16_t)(16 * z), (uint16_t)(70 * z), (uint16_t)(30 * z)}},
          .floating = {.attachTo = CLAY_ATTACH_TO_PARENT, .offset = {app.pan.x, app.pan.y},
                       .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH}}) {
        for (int c = 0; c < app.n_cols; c++) {
            CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = rowgap}}) {
                for (int r = 0; r < app.n_rows; r++) {
                    CLAY({.id = CLAY_IDI("Cell", c * MAX_GRID + r),
                          .layout = {.sizing = {CLAY_SIZING_FIXED(colw[c] * app.zoom), CLAY_SIZING_FIXED(rowh[r] * app.zoom)},
                                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}}}) {
                        for (int i = 0; i < n; i++) {
                            if (app.col[i] != c || app.row[i] != r) continue;
                            const State *st = &m->states[i];
                            CLAY({.id = CLAY_IDI("Node", i),
                                  .layout = {.sizing = {CLAY_SIZING_FIXED(node_width(st) * app.zoom), CLAY_SIZING_FIXED(node_height(st) * app.zoom)}}}) {}
                        }
                    }
                }
            }
        }
    }
}

/* The editor's toolbar, over the canvas's top-left corner. */
static void edit_bar(void) {
    if (!app.have_model) return;
    int st = E.sel[0] ? state_index(&app.model, E.sel) : -1;
    int edge = E.sel_from[0] && model_state(&app.model, E.sel_from) && model_state(&app.model, E.sel_to);
    CLAY({.id = CLAY_ID("EditBar"),
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(PX(8)), .childGap = PX(6),
                     .sizing = {.width = CLAY_SIZING_FIT(0, canvas_width() - PXF(24))}},
          .backgroundColor = {255, 255, 255, 235},
          .cornerRadius = CLAY_CORNER_RADIUS(PX(8)),
          .border = {.color = C_RULE, .width = {1, 1, 1, 1, 0}},
          .floating = {.attachTo = CLAY_ATTACH_TO_PARENT, .offset = {PXF(12), PXF(12)}, .zIndex = 5}}) {
        CLAY({.layout = {.childGap = PX(6), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            button("New state", A_ED_NEW, 0, 1, 0);
            button("Rename", A_ED_RENAME, 0, st >= 0 || edge, 0);
            button("Make initial", A_ED_INITIAL, 0, st >= 0 && !app.model.states[st].initial, 0);
            button("Delete", A_ED_DELETE, 0, st >= 0 || edge, 0);
            button("Undo", A_ED_UNDO, 0, E.n_undo > 0, 0);
        }
        const char *open[8];
        int n_open = st >= 0 && !E.in ? undecided(&app.model.states[st], open, 8) : 0;
        if (n_open) {
            CLAY_TEXT(csf("%s neither handles nor ignores %d event%s. Drag its dot to draw a transition for one, or ignore it here:",
                          app.model.states[st].name, n_open, n_open == 1 ? "" : "s"), T(13, C_AMBER));
            CLAY({.layout = {.childGap = PX(6), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
                for (int i = 0; i < n_open && i < 6; i++)
                    button(TextFormat("Ignore %s", open[i]), A_ED_IGNORE, model_event_index(&app.model, open[i]), 1, 0);
                if (n_open > 1) button(TextFormat("Ignore all %d", n_open), A_ED_IGNORE, -1, 1, 0);
            }
        }
        if (E.msg[0]) CLAY_TEXT(cs(E.msg), T(13, C_AMBER));
        else if (n_open) {}
        else if (E.in == IN_LABEL) CLAY_TEXT(cs("Type the event (UPPER_SNAKE_CASE; commas for more than one), then Enter. Esc cancels."), T(13, C_QUIET));
        else if (E.in == IN_NAME) CLAY_TEXT(cs("Type the state's name (CamelCase), then Enter. Esc cancels."), T(13, C_QUIET));
        else if (st >= 0) CLAY_TEXT(csf("%s: drag it to move, drag its dot to another state for a transition (back onto itself for a loop).", app.model.states[st].name), T(13, C_QUIET));
        else if (edge) CLAY_TEXT(cs("Double-click the label to change its events. Delete removes the transition."), T(13, C_QUIET));
        else CLAY_TEXT(cs("Double-click empty space for a state, or a name or label to rename it. Every edit is saved to the YAML."), T(13, C_QUIET));
    }
}

static Clay_RenderCommandArray build_project(void) {
    Clay_BeginLayout();
    CLAY({.id = CLAY_ID("Root"), .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {
        CLAY({.id = CLAY_ID("CanvasSpace"), .layout = {.sizing = {.width = CLAY_SIZING_FIXED(canvas_width()), .height = CLAY_SIZING_GROW(0)}}}) {
            canvas_grid();
            edit_bar();
            canvas_notice_card();
        }
        CLAY({.id = CLAY_ID("Panel"),
              .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {CLAY_SIZING_FIXED(panel_width()), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(PX(14)), .childGap = PX(12)},
              .backgroundColor = C_PANEL,
              .border = {.color = C_RULE, .width = {.left = 1}},
              .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}}) {
            header_row();
            errors_card();
            steps_card();
            ticket_card();
            output_card();
            ai_card();
            gate_card();
            sim_card();
            ladder_card();
        }
        modal_layer();
    }
    return Clay_EndLayout();
}

/* ---- home screen: open or create a project ---- */

static char *recent_path(void) {
    static char p[PATH_MAX];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(p, sizeof p, "%s/stategate/recent", xdg);
    else snprintf(p, sizeof p, "%s/.config/stategate/recent", home ? home : ".");
    return p;
}

static void recent_load(void) {
    for (int i = 0; i < app.recent.n; i++) free(app.recent.v[i]);
    app.recent.n = 0;
    char *text = read_file(recent_path(), NULL);
    if (!text) return;
    char *save = NULL;
    for (char *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        struct stat st;
        if (*l && stat(l, &st) == 0 && S_ISDIR(st.st_mode) && !sl_contains(&app.recent, l) && app.recent.n < 10) sl_push(&app.recent, l);
    }
    free(text);
}

static void recent_add(const char *dir) {
    recent_load();
    SB b;
    sb_init(&b);
    sb_printf(&b, "%s\n", dir);
    for (int i = 0; i < app.recent.n && i < 9; i++)
        if (strcmp(app.recent.v[i], dir)) sb_printf(&b, "%s\n", app.recent.v[i]);
    char dirpath[PATH_MAX];
    snprintf(dirpath, sizeof dirpath, "%s", recent_path());
    *strrchr(dirpath, '/') = 0;
    mkdirs(dirpath);
    write_file(recent_path(), b.s, b.len);
    free(b.s);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void browse_to(const char *dir) {
    char real[PATH_MAX];
    if (!realpath(dir, real)) { snprintf(app.home_msg, sizeof app.home_msg, "no such folder: %s", dir); return; }
    DIR *d = opendir(real);
    if (!d) { snprintf(app.home_msg, sizeof app.home_msg, "cannot open %s", real); return; }
    snprintf(app.browse, sizeof app.browse, "%s", real);
    snprintf(app.path_in, sizeof app.path_in, "%s", real);
    for (int i = 0; i < app.dirs.n; i++) free(app.dirs.v[i]);
    app.dirs.n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", real, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) sl_push(&app.dirs, e->d_name);
    }
    closedir(d);
    qsort(app.dirs.v, (size_t)app.dirs.n, sizeof(char *), cmp_str);
    app.home_msg[0] = 0;
}

static int no_recent; /* --screenshot runs leave the recent list alone */

static void open_project(const char *dir) {
    char real[PATH_MAX];
    if (!realpath(dir, real) || chdir(real) != 0) {
        snprintf(app.home_msg, sizeof app.home_msg, "cannot open %s", dir);
        app.screen = SCREEN_HOME;
        return;
    }
    app.step_follow = 1; /* the steps card starts on the step to work on */
    snprintf(app.project, sizeof app.project, "%s", real);
    if (!no_recent) recent_add(real);
    StrList errors = {0};
    app.model_path = model_find(".", &errors);
    app.have_model = 0;
    for (int i = 0; i < app.model_errors.n; i++) free(app.model_errors.v[i]);
    app.model_errors.n = 0;
    app.zoom = 1.0f;
    app.pan = (Vector2){0, 0};
    sim_reset();
    app.model_mtime = app.model_path ? file_sig(app.model_path) : 0;
    undo_clear();
    select_none();
    E.in = IN_NONE;
    E.msg[0] = 0;
    load_model(1);
    gate_status_free(&app.gate);
    app.gate_mtime = mtime_of(".fsm/gate.json");
    gate_status_load(&app.gate, ".fsm/gate.json");
    job_clear(&app.job);
    app.modal = 0;
    app.screen = SCREEN_PROJECT;
    app.ticket_focus[0] = 0;
    app.ticket_follow = 1;
    app.tickets_sig = tickets_signature();
    tickets_reload(0);
    refresh_status();
    SetWindowTitle(TextFormat("stategate - %s", real));
}

static void text_field(int which, char *buf, const char *placeholder, float width) {
    Clay_ElementId id = CLAY_IDI("Field", which);
    int focused = app.focus == which;
    CLAY({.id = id,
          .layout = {.sizing = {.width = width > 0 ? CLAY_SIZING_FIXED(PXF(width)) : CLAY_SIZING_GROW(0)}, .padding = {PX(10), PX(10), PX(7), PX(7)}},
          .backgroundColor = C_PANEL, .cornerRadius = CLAY_CORNER_RADIUS(PX(6)), .clip = {.horizontal = true},
          .border = {.color = focused ? C_ACCENT : C_RULE, .width = {1, 1, 1, 1, 0}}}) {
        /* While typing, show the end of a long value: the field cannot scroll. */
        size_t keep = which == 3 ? 46 : 90, len = strlen(buf);
        const char *shown = focused && len > keep ? buf + len - keep : buf;
        if (*buf || focused) CLAY_TEXT(csf("%s%s%s", shown != buf ? "..." : "", shown, focused && ((int)(GetTime() * 2) % 2) ? "|" : ""), T(15, C_INK));
        else CLAY_TEXT(cs(placeholder), T(15, C_QUIET));
    }
    button_reg(id, A_FOCUS, which, NULL);
}

static void home_open_body(void) {
    heading("OPEN OR CREATE A PROJECT FOLDER");
    CLAY({.layout = {.childGap = PX(8), .sizing = {.width = CLAY_SIZING_GROW(0)}}}) {
        text_field(1, app.path_in, "/path/to/folder", 0);
        button("Go", A_GO_PATH, 0, 1, 0);
        button("Up", A_BROWSE_UP, 0, 1, 0);
    }
    CLAY({.id = CLAY_ID("Dirs"), .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(PXF(420))}, .padding = CLAY_PADDING_ALL(PX(6)), .childGap = PX(2)},
          .backgroundColor = C_PANEL, .cornerRadius = CLAY_CORNER_RADIUS(PX(6)), .border = {.color = C_RULE, .width = {1, 1, 1, 1, 0}},
          .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}}) {
        if (!app.dirs.n) CLAY_TEXT(cs("(no folders here)"), T(14, C_QUIET));
        for (int i = 0; i < app.dirs.n && i < 400; i++) {
            Clay_ElementId id = CLAY_IDI("Dir", i);
            CLAY({.id = id, .layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = {PX(8), PX(8), PX(4), PX(4)}},
                  .backgroundColor = Clay_Hovered() ? C_BTN_HOVER : C_PANEL, .cornerRadius = CLAY_CORNER_RADIUS(PX(4))}) {
                CLAY_TEXT(csf("%s/", app.dirs.v[i]), T(15, C_INK));
            }
            button_reg(id, A_BROWSE_INTO, i, NULL);
        }
    }
    CLAY({.layout = {.childGap = PX(8), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        button("Open this folder", A_OPEN_BROWSE, 0, 1, 1);
        CLAY_TEXT(cs("or new folder here:"), T(14, C_QUIET));
        text_field(2, app.name_in, "folder name", 220);
        button("Create and open", A_CREATE, 0, app.name_in[0] != 0, 0);
    }
    if (app.home_msg[0]) CLAY_TEXT(cs(app.home_msg), T(14, C_RED));
}

static void home_recent_body(void) {
    heading("RECENT PROJECTS");
    if (!app.recent.n) CLAY_TEXT(cs("None yet."), T(14, C_QUIET));
    for (int i = 0; i < app.recent.n; i++) {
        Clay_ElementId id = CLAY_IDI("Recent", i);
        const char *slash = strrchr(app.recent.v[i], '/');
        CLAY({.id = id, .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = {PX(10), PX(10), PX(6), PX(6)}},
              .backgroundColor = Clay_Hovered() ? C_BTN_HOVER : C_BTN, .cornerRadius = CLAY_CORNER_RADIUS(PX(6))}) {
            CLAY_TEXT(cs(slash ? slash + 1 : app.recent.v[i]), T(16, C_INK));
            CLAY_TEXT(cs(short_path(app.recent.v[i])), T(12, C_QUIET));
        }
        button_reg(id, A_OPEN_RECENT, i, NULL);
    }
}

static Clay_RenderCommandArray build_home(void) {
    Clay_BeginLayout();
    CLAY({.id = CLAY_ID("Home"), .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = {PX(60), PX(60), PX(40), PX(40)}, .childGap = PX(20)},
          .backgroundColor = {244, 244, 241, 255}}) {
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                CLAY_TEXT(cs("stategate"), T(34, C_INK));
                CLAY_TEXT(cs("Design the state machine, let your AI write the nodes, let the gate decide."), T(16, C_QUIET));
            }
        }
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = PX(20)}}) {
            CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_PERCENT(0.62f)}, .padding = CLAY_PADDING_ALL(PX(16)), .childGap = PX(10)},
                  .backgroundColor = C_CARD, .cornerRadius = CLAY_CORNER_RADIUS(PX(8)), .border = {.color = C_RULE, .width = {1, 1, 1, 1, 0}}}) {
                home_open_body();
            }
            CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(PX(16)), .childGap = PX(8)},
                  .backgroundColor = C_CARD, .cornerRadius = CLAY_CORNER_RADIUS(PX(8)), .border = {.color = C_RULE, .width = {1, 1, 1, 1, 0}}}) {
                home_recent_body();
            }
        }
    }
    return Clay_EndLayout();
}

static void go_home(void) {
    app.screen = SCREEN_HOME;
    app.focus = 0;
    recent_load();
    browse_to(app.project[0] ? app.project : app.browse[0] ? app.browse : (getenv("HOME") ? getenv("HOME") : "/"));
    if (app.project[0]) {
        char parent[PATH_MAX];
        snprintf(parent, sizeof parent, "%s/..", app.project);
        browse_to(parent);
    }
    SetWindowTitle("stategate");
}

/* ---- input ---- */

static void open_in_editor(const char *path) {
    char cmd[PATH_MAX + 64];
#ifdef __APPLE__
    snprintf(cmd, sizeof cmd, "open '%s' >/dev/null 2>&1 &", path);
#else
    snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", path);
#endif
    if (system(cmd) != 0) { /* nothing to do: the editor is the person's own */ }
}

static void dispatch(UiAction a, int arg) {
    switch (a) {
    case A_NONE: break;
    case A_EVENT: if (app.have_model && arg < app.model.events.n) sim_send(app.model.events.v[arg]); break;
    case A_RESET: sim_reset(); break;
    case A_HOME: go_home(); break;
    case A_INIT: {
        /* One step: a repository if there is none, then stategate init. */
        char script[PATH_MAX + 64];
        snprintf(script, sizeof script, "[ -d .git ] || git init -q && '%s' init", app.sg);
        run_shell(app.st.git ? "stategate init" : "git init && stategate init", script);
        break;
    }
    case A_STEP_BACK: step_go(-1); break;
    case A_STEP_FWD: step_go(1); break;
    case A_EDIT: if (app.model_path) open_in_editor(app.model_path); break;
    case A_VALIDATE: run_cli("stategate validate", 0, "validate", NULL, NULL); break;
    case A_GEN: run_cli("stategate gen", 0, "gen", NULL, NULL); break;
    case A_SIGNOFF: run_cli("stategate signoff", 1, "signoff", NULL, NULL); break;
    case A_COMMIT: run_shell("git add -A && git commit -m \"stategate: model and generated code\"",
                             "git add -A && git commit -q -m 'stategate: model and generated code' && git log --oneline -1"); break;
    case A_TICKET_GEN: run_cli("stategate ticket gen", 0, "ticket", "gen", NULL); break;
    case A_REVIEW:
        /* On a pty, as at a terminal, but answered in the ticket card. */
        run_cli("stategate ticket start", 1, "ticket", "start", NULL);
        app.modal = 0;
        app.inline_pty = 1;
        break;
    case A_TICKET_PREV: ticket_go(-1); break;
    case A_TICKET_NEXT: ticket_go(1); break;
    case A_AI_FILL: run_ai(1, arg); break;
    case A_AI_IMPL: run_ai(0, arg); break;
    case A_AI_CONNECT: run_cli("stategate ai connect", 0, "ai", "connect", NULL); break;
    case A_AI_CHECK: run_cli("stategate ai check", 0, "ai", "check", NULL); break;
    case A_AI_CONFIG:
        run_cli("stategate mcp config", 0, "mcp", "config", NULL);
        app.copy_on_finish = 1;
        break;
    case A_GATE:
        if (!strcmp(app.st.current_status, "active")) run_cli("stategate ticket run", 0, "ticket", "run", NULL);
        else run_cli("stategate gate", 0, "gate", NULL, NULL);
        break;
    case A_MODAL_SEND: {
        static const char *answers[] = {"no", "sign off", "approve", "skip", "quit"};
        if (arg >= 0 && arg < 5) {
            app.modal_from = app.job.out.len;
            job_send(&app.job, answers[arg]);
        }
        break;
    }
    case A_MODAL_STOP: job_stop(&app.job); break;
    case A_MODAL_CLOSE: app.modal = 0; refresh_status(); break;
    case A_BROWSE_INTO:
        if (arg < app.dirs.n) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", app.browse, app.dirs.v[arg]);
            browse_to(p);
        }
        break;
    case A_BROWSE_UP: { char p[PATH_MAX]; snprintf(p, sizeof p, "%s/..", app.browse); browse_to(p); break; }
    case A_GO_PATH: {
        char p[PATH_MAX];
        if (app.path_in[0] == '~') snprintf(p, sizeof p, "%s%s", getenv("HOME") ? getenv("HOME") : "", app.path_in + 1);
        else snprintf(p, sizeof p, "%s", app.path_in);
        browse_to(p);
        break;
    }
    case A_OPEN_BROWSE: open_project(app.browse); break;
    case A_OPEN_RECENT: if (arg < app.recent.n) open_project(app.recent.v[arg]); break;
    case A_CREATE: {
        if (strchr(app.name_in, '/') || !strcmp(app.name_in, ".") || !strcmp(app.name_in, "..")) {
            snprintf(app.home_msg, sizeof app.home_msg, "a folder name, not a path");
            break;
        }
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", app.browse, app.name_in);
        if (mkdir(p, 0755) != 0 && access(p, F_OK) != 0) { snprintf(app.home_msg, sizeof app.home_msg, "cannot create %s", p); break; }
        app.name_in[0] = 0;
        open_project(p);
        break;
    }
    case A_FOCUS: app.focus = arg; break;
    case A_NEW_MODEL: new_model(); break;
    case A_ED_NEW: {
        if (E.in) ed_commit_field();
        char id[300];
        if (ed_new_state(NULL, id, sizeof id) == 0) open_name_field(id, 1);
        break;
    }
    case A_ED_RENAME: ed_rename_selection(); break;
    case A_ED_INITIAL: ed_make_initial(); break;
    case A_ED_DELETE: ed_delete(); break;
    case A_ED_UNDO: edit_undo(); break;
    case A_ED_IGNORE: ed_ignore(arg); break;
    }
}

static void handle_clicks(void) {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    int modal = app.screen == SCREEN_PROJECT && app.modal;
    int hit = 0;
    /* Later registrations draw on top (the modal), so search from the end. */
    for (int i = n_buttons - 1; i >= 0; i--) {
        if (!Clay_PointerOver(buttons[i].id)) continue;
        UiAction a = buttons[i].action;
        if (modal && a != A_MODAL_SEND && a != A_MODAL_CLOSE && a != A_MODAL_STOP) continue;
        dispatch(a, buttons[i].arg);
        if (buttons[i].keep_scroll) app.show_output = 0;
        hit = 1;
        break;
    }
    if (!hit) app.focus = 0;
}

static void handle_typing(void) {
    if (app.screen == SCREEN_PROJECT && E.in) { edit_typing(); return; }
    if (!app.focus) return;
    char *buf = app.focus == 1 ? app.path_in : app.name_in;
    size_t cap = app.focus == 1 ? sizeof app.path_in : sizeof app.name_in;
    int c;
    while ((c = GetCharPressed()) > 0) {
        size_t n = strlen(buf);
        if (c >= 32 && c < 127 && n + 1 < cap) { buf[n] = (char)c; buf[n + 1] = 0; }
    }
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
    if (ctrl && IsKeyPressed(KEY_V)) {
        const char *clip = GetClipboardText();
        for (size_t n = strlen(buf); clip && *clip && n + 1 < cap; clip++)
            if (*clip >= 32 && *clip < 127) { buf[n++] = *clip; buf[n] = 0; }
    }
    if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && *buf) buf[strlen(buf) - 1] = 0;
    if (IsKeyPressed(KEY_ENTER)) dispatch(app.focus == 1 ? A_GO_PATH : A_CREATE, 0);
}

static void clay_error(Clay_ErrorData e) { fprintf(stderr, "clay: %.*s\n", e.errorText.length, e.errorText.chars); }

/* The CLI the buttons run: next to this binary when it is there (a build
 * tree, or both installed together), else whatever `stategate` is on PATH. */
static void find_cli(void) {
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) {
        self[n] = 0;
        char *slash = strrchr(self, '/');
        if (slash) {
            *slash = 0;
            snprintf(app.sg, sizeof app.sg, "%s/stategate", self);
            if (access(app.sg, X_OK) == 0) {
                /* The pre-commit hook calls `stategate` by name: let it find
                 * this same build even when it is not installed on PATH. */
                const char *path = getenv("PATH");
                char *np = malloc(strlen(self) + (path ? strlen(path) : 0) + 2);
                sprintf(np, "%s:%s", self, path ? path : "");
                setenv("PATH", np, 1);
                free(np);
                return;
            }
        }
    }
    snprintf(app.sg, sizeof app.sg, "stategate");
}

/* Scrolls a Clay scroll container by dy pixels, kept inside its content. */
static void scroll_by(Clay_ElementId id, float dy) {
    Clay_ScrollContainerData sc = Clay_GetScrollContainerData(id);
    if (!sc.found) return;
    float min = fminf(0, -(sc.contentDimensions.height - sc.scrollContainerDimensions.height));
    sc.scrollPosition->y = fminf(0, fmaxf(min, sc.scrollPosition->y + dy));
}

/* The panel's scrollbar thumb, in screen pixels; height 0 when it all fits. */
static Rectangle scroll_thumb(Clay_ElementId id) {
    Clay_ScrollContainerData sc = Clay_GetScrollContainerData(id);
    Clay_ElementData el = Clay_GetElementData(id);
    if (!sc.found || !el.found || sc.contentDimensions.height <= sc.scrollContainerDimensions.height + 1) return (Rectangle){0};
    Clay_BoundingBox b = el.boundingBox;
    float view = sc.scrollContainerDimensions.height, content = sc.contentDimensions.height;
    float h = fmaxf(PXF(30), b.height * view / content);
    float t = -sc.scrollPosition->y / (content - view);
    return (Rectangle){b.x + b.width - PXF(8), b.y + (b.height - h) * t, PXF(6), h};
}

static void draw_scrollbar(Clay_ElementId id) {
    Rectangle r = scroll_thumb(id);
    if (r.height <= 0) return;
    DrawRectangleRounded(r, 1.0f, 6, app.sb_drag ? (Color){120, 120, 120, 220} : (Color){160, 160, 160, 160});
}

static void handle_scroll(Vector2 mouse) {
    Clay_ElementId id = app.screen == SCREEN_HOME ? CLAY_ID("Dirs") : CLAY_ID("Panel");
    if (app.screen == SCREEN_PROJECT && app.modal) return;
    /* Drag the thumb, or click the track to jump there. */
    if (app.screen == SCREEN_PROJECT) {
        Rectangle t = scroll_thumb(id);
        Clay_ElementData el = Clay_GetElementData(id);
        if (t.height > 0 && el.found && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            mouse.x >= t.x - PXF(6) && mouse.x <= t.x + t.width + PXF(4)) {
            app.sb_drag = 1;
            app.sb_grab = mouse.y >= t.y && mouse.y <= t.y + t.height ? mouse.y - t.y : t.height / 2;
        }
        if (app.sb_drag && IsMouseButtonDown(MOUSE_BUTTON_LEFT) && t.height > 0) {
            Clay_ScrollContainerData sc = Clay_GetScrollContainerData(id);
            float track = el.boundingBox.height - t.height;
            float f = track > 0 ? (mouse.y - app.sb_grab - el.boundingBox.y) / track : 0;
            f = fminf(1, fmaxf(0, f));
            sc.scrollPosition->y = -f * (sc.contentDimensions.height - sc.scrollContainerDimensions.height);
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) app.sb_drag = 0;
    }
    int over = app.screen == SCREEN_HOME ? Clay_PointerOver(id) : mouse.x > canvas_width();
    if (!over) return;
    /* One wheel notch moves about four lines; touchpads send fractions. */
    float dy = GetMouseWheelMoveV().y * PXF(80);
    if (!app.focus) {
        if (IsKeyPressed(KEY_PAGE_DOWN)) dy -= GetScreenHeight() * 0.8f;
        if (IsKeyPressed(KEY_PAGE_UP)) dy += GetScreenHeight() * 0.8f;
        if (IsKeyPressed(KEY_HOME)) dy += 1e6f;
        if (IsKeyPressed(KEY_END)) dy -= 1e6f;
    }
    if (dy != 0) scroll_by(id, dy);
}

/* The base scale follows the window height: 1 at 760 px tall, so a
 * maximized window on a 1080p screen is about 1.4. */
static float ui_scale(void) {
    float base = fminf(3.0f, fmaxf(1.0f, GetScreenHeight() / 720.0f));
    return base * (app.ui_mult > 0 ? app.ui_mult : 1.0f);
}

static char *ui_path(void) {
    static char p[PATH_MAX];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(p, sizeof p, "%s/stategate/text_size", xdg);
    else snprintf(p, sizeof p, "%s/.config/stategate/text_size", home ? home : ".");
    return p;
}

static float ui_mult_load(void) {
    char *t = read_file(ui_path(), NULL);
    float m = t ? strtof(t, NULL) : 1.0f;
    free(t);
    return m >= 0.5f && m <= 2.5f ? m : 1.0f;
}

static void ui_mult_save(float m) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", ui_path());
    *strrchr(dir, '/') = 0;
    mkdirs(dir);
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%.3f\n", m);
    write_file(ui_path(), buf, (size_t)n);
}

int main(int argc, char **argv) {
    const char *dir = NULL, *shot = NULL, *sends = NULL;
    int windowed = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--send") && i + 1 < argc) sends = argv[++i];
        else if (!strcmp(argv[i], "--windowed")) windowed = 1;
        else if (argv[i][0] != '-') dir = argv[i];
        else {
            fprintf(stderr, "usage: stategate-view [project dir] [--windowed] [--send EV1,EV2] [--screenshot out.png]\n"
                            "  no project dir: start on the projects screen\n");
            return 2;
        }
    }
    find_cli();
    ai_load();
    app.zoom = 1.0f;

    SetTraceLogLevel(LOG_WARNING);
    Clay_Raylib_Initialize(1440, 900, "stategate", FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | (shot ? FLAG_WINDOW_HIDDEN : 0));
    SetTargetFPS(60);
    SetExitKey(KEY_NULL); /* Esc closes dialogs, not the app */
    if (!shot && !windowed) {
        /* Maximized, with the title bar: most of the screen, still a window. */
        int mon = GetCurrentMonitor();
        int mw = GetMonitorWidth(mon), mh = GetMonitorHeight(mon);
        if (mw > 0 && mh > 0) {
            SetWindowSize(mw * 9 / 10, mh * 9 / 10);
            SetWindowPosition(mw / 20, mh / 20);
        }
        MaximizeWindow();
    }
    app.ui_mult = ui_mult_load();
    app.ui = ui_scale();
    uint64_t mem = Clay_MinMemorySize();
    Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(mem, malloc(mem)),
                    (Clay_Dimensions){(float)GetScreenWidth(), (float)GetScreenHeight()}, (Clay_ErrorHandler){clay_error, 0});

    Font fonts[1];
    char font_path[1024];
    snprintf(font_path, sizeof font_path, "%s/Roboto-Regular.ttf", SG_RESOURCE_DIR);
#ifdef SG_SOURCE_RESOURCE_DIR
    if (!FileExists(font_path)) snprintf(font_path, sizeof font_path, "%s/Roboto-Regular.ttf", SG_SOURCE_RESOURCE_DIR);
#endif
    fonts[0] = FileExists(font_path) ? LoadFontEx(font_path, 64, 0, 0) : GetFontDefault();
    SetTextureFilter(fonts[0].texture, TEXTURE_FILTER_BILINEAR);
    app.font = fonts[0];
    Clay_SetMeasureTextFunction(Raylib_MeasureText, fonts);

    job_clear(&app.job);
    no_recent = shot != NULL;
    if (dir) open_project(dir);
    if (app.screen != SCREEN_PROJECT) go_home();
    if (sends && app.screen == SCREEN_PROJECT) {
        char *list = xstrdup(sends), *save = NULL;
        for (char *e = strtok_r(list, ",", &save); e; e = strtok_r(NULL, ",", &save)) sim_send(e);
    }

    double last_poll = 0;
    int frames = 0;
    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_F11)) ToggleBorderlessWindowed();
        {
            /* Ctrl +, Ctrl - and Ctrl 0 size the text, as in a browser. */
            int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
            float m = app.ui_mult;
            if (ctrl && (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))) m *= 1.1f;
            if (ctrl && (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT))) m /= 1.1f;
            if (ctrl && (IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_KP_0))) m = 1.0f;
            m = fminf(2.5f, fmaxf(0.5f, m));
            if (m != app.ui_mult) { app.ui_mult = m; ui_mult_save(m); }
            float prev = app.ui;
            app.ui = ui_scale();
            if (prev > 0 && fabsf(prev - app.ui) > 0.001f && app.screen == SCREEN_PROJECT) app.fit_pending = 1;
        }
        if (app.screen == SCREEN_PROJECT) {
            if (GetTime() - last_poll > 0.5) { poll_files(); last_poll = GetTime(); }
            if (job_poll(&app.job) && !app.job.running) {
                refresh_status();
                if (!app.job.pty) app.modal = 0;
                job_finished();
            }
            if (!app.job.running && GetTime() - app.status_at > 3.0) refresh_status();
            if (IsKeyPressed(KEY_ESCAPE) && app.modal) {
                if (app.job.running) job_stop(&app.job);
                else app.modal = 0;
            }
        }
        arena_used = 0;
        n_buttons = 0;
        button_seq = 0;
        Clay_SetLayoutDimensions((Clay_Dimensions){(float)GetScreenWidth(), (float)GetScreenHeight()});
        Vector2 mouse = GetMousePosition();
        Clay_SetPointerState((Clay_Vector2){mouse.x, mouse.y}, IsMouseButtonDown(MOUSE_BUTTON_LEFT));
        /* Scrolling is ours, not Clay's: Clay's wheel step is a fixed 10 px
         * and its drag-to-scroll fights clicks. */
        Clay_UpdateScrollContainers(false, (Clay_Vector2){0, 0}, GetFrameTime());
        handle_scroll(mouse);
        if (app.screen == SCREEN_PROJECT && !app.modal) handle_canvas_input();
        handle_typing();
        if (app.show_output && app.screen == SCREEN_PROJECT && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            /* Wait for the release: while the button is down, Clay's drag scrolling owns the offset. */
            /* Bring the output card to the top of the panel so a result is seen. */
            Clay_ScrollContainerData sc = Clay_GetScrollContainerData(CLAY_ID("Panel"));
            Clay_ElementData out = Clay_GetElementData(CLAY_ID("Output"));
            if (sc.found && out.found) {
                float y = sc.scrollPosition->y - (out.boundingBox.y - 14);
                float min = -(sc.contentDimensions.height - sc.scrollContainerDimensions.height);
                sc.scrollPosition->y = y > 0 ? 0 : y < min ? (min < 0 ? min : 0) : y;
            }
            app.show_output = 0;
        }
        Clay_RenderCommandArray cmds = app.screen == SCREEN_HOME ? build_home() : build_project();
        if (app.fit_pending && app.screen == SCREEN_PROJECT && Clay_GetElementData(CLAY_ID("Grid")).found) {
            /* Fit, then lay out again so this frame draws the fitted view. */
            fit_view();
            app.fit_pending = 0;
            arena_used = 0;
            n_buttons = 0;
            button_seq = 0;
            cmds = build_project();
        }
        if (E.anchor[0] && app.screen == SCREEN_PROJECT) {
            const State *s = app.have_model ? model_state(&app.model, E.anchor) : NULL;
            Rectangle r = s ? node_rect(s) : (Rectangle){0};
            if (s && r.width > 0 && (fabsf(r.x - E.anchor_at.x) > 0.5f || fabsf(r.y - E.anchor_at.y) > 0.5f)) {
                app.pan.x += E.anchor_at.x - r.x;
                app.pan.y += E.anchor_at.y - r.y;
                arena_used = 0;
                n_buttons = 0;
                button_seq = 0;
                cmds = build_project();
            }
            E.anchor[0] = 0;
        }
        if (E.reveal[0] && app.screen == SCREEN_PROJECT) {
            /* A state made off screen: fit the machine so it shows. */
            const State *s = app.have_model ? model_state(&app.model, E.reveal) : NULL;
            Rectangle r = s ? node_rect(s) : (Rectangle){0};
            if (s && (r.x < 0 || r.y < 0 || r.x + r.width > canvas_width() || r.y + r.height > GetScreenHeight())) {
                fit_view();
                arena_used = 0;
                n_buttons = 0;
                button_seq = 0;
                cmds = build_project();
            }
            E.reveal[0] = 0;
        }
        handle_clicks();

        BeginDrawing();
        ClearBackground(BG);
        if (app.screen == SCREEN_PROJECT) draw_canvas();
        Clay_Raylib_Render(cmds, fonts);
        if (app.screen == SCREEN_PROJECT) draw_scrollbar(CLAY_ID("Panel"));
        EndDrawing();

        if (shot && ++frames == 3) {
            /* Not TakeScreenshot(): it drops the directory part of the path. */
            Image img = LoadImageFromScreen();
            ExportImage(img, shot);
            UnloadImage(img);
            break;
        }
    }
    if (app.job.running) job_stop(&app.job);
    Clay_Raylib_Close();
    return 0;
}
