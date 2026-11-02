/* stategate-view: the stategate app.
 *
 * Home screen: open or create a project folder, or pick a recent one.
 * Project screen, left two-thirds: the state machine drawn from the model's
 * saved layout. Right third (Clay): the steps of the flow, each a button
 * that runs the same `stategate` CLI command a person would type, the
 * command's output, the last gate result, a simulator and the step ladder.
 * The model itself is edited by hand; the canvas reloads when it changes.
 * Step states come from `stategate status --json`, gate results from
 * .fsm/gate.json: the app keeps no status of its own. */

#define CLAY_IMPLEMENTATION
#include "third_party/clay/clay.h"
#include "third_party/clay/clay_renderer_raylib.c"

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
#include "model.h"
#include "validate.h"

#ifndef SG_RESOURCE_DIR
#define SG_RESOURCE_DIR "resources"
#endif

#define NODE_W 170.0f
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

typedef enum { SCREEN_HOME, SCREEN_PROJECT } Screen;

typedef struct {
    Screen screen;
    char project[PATH_MAX];
    char sg[PATH_MAX]; /* the stategate CLI the buttons run */
    Status st;
    Job job;
    int modal;          /* a pty job (sign-off, review) is shown over the app */
    size_t modal_from;  /* show output from here: the current question only */
    double status_at;

    /* home screen */
    char browse[PATH_MAX];
    StrList dirs, recent;
    char path_in[PATH_MAX], name_in[128];
    int focus; /* 0 none, 1 path field, 2 new-folder name, 3 AI command */
    char home_msg[256];
    int show_output; /* scroll the panel to the output card next frame */
    float ui;        /* text and spacing scale: grows with the window, Ctrl +/- adjust */
    float ui_mult;   /* the person's Ctrl +/- factor on top */

    /* Your AI: a command the prompt is piped into, or empty to copy it. */
    char ai_cmd[1024], ai_in[1024];
    int copy_on_finish;  /* put the finished job's output on the clipboard */
    int ai_loop;         /* implementing: run the AI again for each next ticket */
    char ai_ticket[64];  /* the ticket the AI was last run on */
    char job_note[256];  /* shown with the output, e.g. "copied" */

    char *model_path;
    SgModel model;
    int have_model;
    StrList model_errors;
    time_t model_mtime, gate_mtime;
    GateStatus gate;

    int cur;              /* active state index in the simulator */
    int last_from, last_to;
    char last_event[64];
    char flash[160];
    StrList history;

    Vector2 pan;
    float zoom;
    Font font;
} App;

static App app;

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

static void load_model(int first) {
    if (!app.model_path) return;
    SgModel m;
    StrList errors = {0};
    if (model_load(app.model_path, &m, &errors) == 0) {
        check_statechart(&m, &errors);
        if (!errors.n) check_model_invariants(&m, &errors);
    }
    for (int i = 0; i < app.model_errors.n; i++) free(app.model_errors.v[i]);
    app.model_errors.n = 0;
    for (int i = 0; i < errors.n; i++) sl_push(&app.model_errors, errors.v[i]);
    if (errors.n && app.have_model) return; /* keep showing the last good model */

    /* Keep the simulator where it was when the active state still exists. */
    char keep[256] = "";
    if (app.have_model) snprintf(keep, sizeof keep, "%s", app.model.states[app.cur].id);
    app.model = m; /* the old model leaks; reloads are rare and small */
    app.have_model = 1;
    int idx = *keep ? state_index(&app.model, keep) : -1;
    if (idx < 0) sim_reset();
    else { app.cur = idx; app.last_from = app.last_to = -1; }
    if (first) fit_view();
}

static void poll_files(void) {
    if (!app.model_path) {
        /* The person is writing the model: pick it up once it appears. */
        StrList e = {0};
        app.model_path = model_find(".", &e);
        if (!app.model_path) return;
        app.model_mtime = 0;
    }
    time_t mm = mtime_of(app.model_path), gm = mtime_of(".fsm/gate.json");
    if (mm != app.model_mtime) { app.model_mtime = mm; load_model(0); }
    if (gm != app.gate_mtime) { app.gate_mtime = gm; gate_status_load(&app.gate, ".fsm/gate.json"); }
}

/* ---- canvas ---- */

static float canvas_width(void) { return floorf(GetScreenWidth() * 2.0f / 3.0f); }

static float node_height(const State *s) {
    int lines = 1 + (s->entry.n ? 1 : 0) + (s->exit.n ? 1 : 0) + (s->invoke.n ? 1 : 0) + (s->has_after ? 1 : 0);
    return (22.0f + lines * 18.0f) * app.ui;
}

static Vector2 text_size(const char *s, float size);

/* Width in world units: the widest line at zoom 1, plus padding. */
static float node_width(const State *s) {
    float w = text_size(s->name, 18.0f * app.ui).x;
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
        w = fmaxf(w, text_size(line, 13.5f * app.ui).x);
    }
    if (s->has_after) {
        snprintf(line, sizeof line, "after %ld ms: %s", s->after_ms, s->after_event);
        w = fmaxf(w, text_size(line, 13.5f * app.ui).x);
    }
    return fmaxf(NODE_W * app.ui, w + 26.0f * app.ui);
}

static Vector2 world_to_screen(Vector2 w) {
    return (Vector2){w.x * app.zoom + app.pan.x, w.y * app.zoom + app.pan.y};
}

static Rectangle node_rect(const State *s) {
    Vector2 p = world_to_screen((Vector2){(float)s->x, (float)s->y});
    return (Rectangle){p.x, p.y, node_width(s) * app.zoom, node_height(s) * app.zoom};
}

static void fit_view(void) {
    if (!app.have_model || !app.model.n_states) return;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < app.model.n_states; i++) {
        const State *s = &app.model.states[i];
        minx = fminf(minx, (float)s->x);
        miny = fminf(miny, (float)s->y);
        maxx = fmaxf(maxx, (float)s->x + node_width(s));
        maxy = fmaxf(maxy, (float)s->y + node_height(s));
    }
    float cw = canvas_width(), ch = (float)GetScreenHeight();
    float margin = 60.0f;
    float zx = (cw - 2 * margin) / fmaxf(1.0f, maxx - minx), zy = (ch - 2 * margin) / fmaxf(1.0f, maxy - miny);
    app.zoom = fminf(2.0f, fmaxf(0.3f, fminf(zx, zy)));
    app.pan.x = (cw - (maxx - minx) * app.zoom) / 2 - minx * app.zoom;
    app.pan.y = (ch - (maxy - miny) * app.zoom) / 2 - miny * app.zoom;
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

typedef struct {
    char text[256];
    Vector2 at;
    int hot;
} PendingLabel;
static PendingLabel labels[256];
static int n_labels;

static void edge_label(const char *label, Vector2 at, int hot) {
    if (n_labels < 256) {
        snprintf(labels[n_labels].text, sizeof labels[n_labels].text, "%s", label);
        labels[n_labels].at = at;
        labels[n_labels].hot = hot;
        n_labels++;
    }
}

static void draw_label(const char *label, Vector2 at, int hot) {
    float fs = 14.0f * app.ui * app.zoom;
    if (fs < 8) return;
    Vector2 sz = text_size(label, fs);
    Rectangle bg = {at.x - sz.x / 2 - 4, at.y - sz.y / 2 - 2, sz.x + 8, sz.y + 4};
    DrawRectangleRounded(bg, 0.4f, 4, hot ? ACCENT_SOFT : (Color){244, 244, 241, 235});
    text(label, at.x - sz.x / 2, at.y - sz.y / 2, fs, hot ? ACCENT : QUIET);
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
            Color c = hot ? ACCENT : NODE_EDGE;
            float thick = (hot ? 2.5f : 1.4f) * fmaxf(0.7f, app.zoom);
            Rectangle a = node_rect(s), b = node_rect(&m->states[j]);
            if (i == j) {
                /* Self-loop above the node. */
                Vector2 p1 = {a.x + a.width * 0.30f, a.y}, p4 = {a.x + a.width * 0.70f, a.y};
                float h = 46.0f * app.zoom;
                Vector2 c2 = {p1.x - 10 * app.zoom, a.y - h}, c3 = {p4.x + 10 * app.zoom, a.y - h};
                DrawSplineSegmentBezierCubic(p1, c2, c3, p4, thick, c);
                arrow_head(p4, c3, c);
                edge_label(label, (Vector2){a.x + a.width / 2, a.y - h * 0.80f}, hot);
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
            Vector2 mid = Vector2Add(Vector2Lerp(p, q, 0.5f), Vector2Scale(n, reverse ? 14.0f * app.zoom : 0));
            edge_label(label, mid, hot);
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
    *y += 18.0f * app.ui * app.zoom;
}

static void draw_nodes(void) {
    const SgModel *m = &app.model;
    for (int i = 0; i < m->n_states; i++) {
        const State *s = &m->states[i];
        Rectangle r = node_rect(s);
        int active = i == app.cur;
        DrawRectangleRounded(r, 0.18f, 8, active ? ACCENT_SOFT : NODE_FILL);
        DrawRectangleRoundedLinesEx(r, 0.18f, 8, (active ? 2.5f : 1.2f) * fmaxf(0.7f, app.zoom), active ? ACCENT : NODE_EDGE);
        if (s->initial) {
            Vector2 dot = {r.x - 30 * app.zoom, r.y + r.height / 2};
            DrawCircleV(dot, 5 * app.zoom, INK);
            DrawLineEx(dot, (Vector2){r.x, dot.y}, 1.4f * fmaxf(0.7f, app.zoom), INK);
            arrow_head((Vector2){r.x, dot.y}, dot, INK);
        }
        float u = app.ui * app.zoom, fs = 18.0f * u, small = 13.5f * u;
        if (fs < 6) continue;
        float x = r.x + 12 * u, y = r.y + 10 * u;
        text(s->name, x, y, fs, active ? ACCENT : INK);
        y += 26.0f * u;
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
        for (int i = 0; i < n_labels; i++) draw_label(labels[i].text, labels[i].at, labels[i].hot);
    } else {
        float u = app.ui;
        if (app.model_path) text("No valid model loaded yet. See the panel for errors.", 24 * u, 24 * u, 18 * u, QUIET);
        else {
            text("No model in this folder yet.", 24 * u, 24 * u, 22 * u, INK);
            text(TextFormat("Write %s.yaml here with your editor (examples/posture/posture.yaml is a template).",
                            strrchr(app.project, '/') ? strrchr(app.project, '/') + 1 : "model"), 24 * u, 58 * u, 16 * u, QUIET);
            text("It appears here as soon as you save it.", 24, 82, 16, QUIET);
        }
    }
    text("drag to pan, scroll to zoom, F to fit, Ctrl +/- text size", 16 * app.ui, ch - 26 * app.ui, 14 * app.ui, QUIET);
    EndScissorMode();
}

static void handle_canvas_input(void) {
    Vector2 mouse = GetMousePosition();
    if (mouse.x > canvas_width()) return;
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE))
        app.pan = Vector2Add(app.pan, GetMouseDelta());
    float wheel = GetMouseWheelMove();
    if (wheel != 0) {
        float old = app.zoom;
        app.zoom = fminf(3.0f, fmaxf(0.25f, app.zoom * (1.0f + wheel * 0.1f)));
        /* Zoom around the cursor. */
        app.pan.x = mouse.x - (mouse.x - app.pan.x) * (app.zoom / old);
        app.pan.y = mouse.y - (mouse.y - app.pan.y) * (app.zoom / old);
    }
    if (IsKeyPressed(KEY_F) && !app.focus) fit_view();
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
    A_HOME, A_QUIT, A_GIT_INIT, A_INIT, A_EDIT, A_VALIDATE, A_GEN, A_SIGNOFF, A_COMMIT,
    A_TICKET_GEN, A_REVIEW, A_GATE, A_AI_FILL, A_AI_IMPL, A_AI_SAVE,
    A_MODAL_SEND, A_MODAL_CLOSE, A_MODAL_STOP,
    A_BROWSE_INTO, A_BROWSE_UP, A_OPEN_BROWSE, A_OPEN_RECENT, A_CREATE, A_GO_PATH, A_FOCUS,
} UiAction;

typedef struct {
    Clay_ElementId id;
    UiAction action;
    int arg;
    const char *text;
} Button;

static Button buttons[256];
static int n_buttons;

static void button_reg(Clay_ElementId id, UiAction a, int arg, const char *text) {
    if (n_buttons < (int)(sizeof buttons / sizeof buttons[0])) buttons[n_buttons++] = (Button){id, a, arg, text};
}

/* A button; disabled ones draw quiet and do nothing. */
static void button(const char *label, UiAction a, int arg, int enabled, int primary) {
    static int seq;
    Clay_ElementId id = CLAY_IDI("Btn", seq++ % 100000);
    if (n_buttons == 0) seq = 1; /* ids restart each frame */
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
    app.show_output = 1;
    app.job_note[0] = 0;
    app.copy_on_finish = 0;
}

/* ---- your AI ---- */

static char *ai_path(void) {
    static char p[PATH_MAX];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(p, sizeof p, "%s/stategate/ai_command", xdg);
    else snprintf(p, sizeof p, "%s/.config/stategate/ai_command", home ? home : ".");
    return p;
}

static void ai_load(void) {
    char *text = read_file(ai_path(), NULL);
    app.ai_cmd[0] = 0;
    if (text) {
        text[strcspn(text, "\n")] = 0;
        snprintf(app.ai_cmd, sizeof app.ai_cmd, "%s", text);
        free(text);
    }
    snprintf(app.ai_in, sizeof app.ai_in, "%s", app.ai_cmd);
}

static void ai_save(void) {
    snprintf(app.ai_cmd, sizeof app.ai_cmd, "%s", app.ai_in);
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", ai_path());
    *strrchr(dir, '/') = 0;
    mkdirs(dir);
    SB b;
    sb_init(&b);
    sb_printf(&b, "%s\n", app.ai_cmd);
    write_file(ai_path(), b.s, b.len);
    free(b.s);
    app.focus = 0;
}

/* An AI step: with a command set, pipe the prompt into it; without one, put
 * the prompt on the clipboard to paste into any AI. */
static void run_ai(int drafts) {
    if (app.job.running) return;
    const char *pargs = drafts ? "ticket prompt --drafts" : "ticket prompt";
    if (!app.ai_cmd[0]) {
        if (drafts) run_cli("stategate ticket prompt --drafts", 0, "ticket", "prompt", "--drafts");
        else run_cli("stategate ticket prompt", 0, "ticket", "prompt", NULL);
        app.copy_on_finish = 1;
        return;
    }
    char script[PATH_MAX + 1200], title[1300];
    snprintf(script, sizeof script, "'%s' %s | %s", app.sg, pargs, app.ai_cmd);
    snprintf(title, sizeof title, "stategate %s | %s", pargs, app.ai_cmd);
    run_shell(title, script);
    app.ai_loop = !drafts;
    snprintf(app.ai_ticket, sizeof app.ai_ticket, "%s", app.st.current);
}

/* A job ended: copy its output, or move the AI on to the next ticket. */
static void job_finished(void) {
    if (app.copy_on_finish) {
        app.copy_on_finish = 0;
        if (app.job.exit_code == 0 && app.job.out.s) {
            SetClipboardText(app.job.out.s);
            snprintf(app.job_note, sizeof app.job_note,
                     "Copied to the clipboard: paste it into your AI.");
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
        run_ai(0);
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
    size_t n = strlen(tmp);
    if (n > 52) snprintf(o, 256, "...%s", tmp + n - 49);
    else snprintf(o, 256, "%s", tmp);
    return o;
}

static Clay_ElementDeclaration step_decl(void) {
    return (Clay_ElementDeclaration){.layout = {.childGap = PX(10), .sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = {PX(0), PX(0), PX(3), PX(3)},
                                                .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}};
}

/* The mark and the text of a step row; the caller adds its buttons after. */
static void step_head(int num, const char *title, StepMark mark, int is_next, const char *detail) {
    Clay_Color mc = mark == S_DONE ? C_GREEN : is_next ? C_ACCENT : C_QUIET;
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(PXF(26))}}}) {
        CLAY_TEXT(mark == S_DONE ? cs("OK") : num ? csf("%d", num) : cs("-"), T(14, mc));
    }
    /* No clip here: Clay 0.14 keeps at most 10 clipping containers per layout. */
    CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_GROW(0)}}}) {
        CLAY_TEXT(cs(title), T(16, is_next && mark != S_DONE ? C_ACCENT : C_INK));
        if (detail && *detail) CLAY_TEXT(cs(detail), T(13, C_QUIET));
    }
}

static void steps_card_body(void) {
    const Status *s = &app.st;
    int busy = app.job.running;
    int has_model = app.model_path != NULL;
    int tickets_any = s->t_open + s->t_approved + s->t_active + s->t_done + s->t_locked;
    int saved = s->git && s->has_commit && !s->dirty;
    StepMark m[12];
    m[1] = S_DONE;
    m[2] = s->git ? S_DONE : S_TODO;
    m[3] = s->init ? S_DONE : S_TODO;
    m[4] = has_model && s->valid ? S_DONE : S_TODO;
    m[5] = s->valid ? S_DONE : S_TODO;
    m[6] = s->generated ? S_DONE : S_TODO;
    m[7] = s->valid && s->signed_off ? S_DONE : S_TODO;
    /* Once a ticket is active, `ticket run` commits each ticket's work. */
    int ticket_work = s->t_active > 0;
    m[0] = saved || ticket_work ? S_DONE : S_TODO; /* commit */
    m[8] = tickets_any && !s->t_draft ? S_DONE : S_TODO;
    m[9] = tickets_any && !s->t_open && !s->t_draft ? S_DONE : S_TODO;
    m[10] = tickets_any && !s->t_draft && !s->t_open && !s->t_approved && !s->t_active && !s->t_locked ? S_DONE : S_LATER;
    m[11] = !s->t_active && !strcmp(s->gate, "green") ? S_DONE : S_TODO;
    static const int order[] = {2, 3, 4, 5, 6, 7, 0, 8, 9, 10, 11};
    int next = -1;
    for (int i = 0; i < 11 && next < 0; i++) if (m[order[i]] != S_DONE) next = order[i];

    heading("STEPS  (each button runs the stategate command shown)");
    CLAY(step_decl()) {
        step_head(1, "Project folder", m[1], 0, short_path(app.project));
        button("Switch", A_HOME, 0, !busy, 0);
    }
    CLAY(step_decl()) {
        step_head(2, "Git repository", m[2], next == 2, s->git ? (s->has_commit ? "ready" : "ready, no commits yet") : "$ git init");
        if (!s->git) button("git init", A_GIT_INIT, 0, !busy, next == 2);
    }
    CLAY(step_decl()) {
        step_head(3, "Set up stategate", m[3], next == 3, s->init ? "AGENTS.md, CLAUDE.md, CHANGELOG.md, pre-commit hook" : "$ stategate init");
        if (!s->init) button("Set up", A_INIT, 0, !busy, next == 3);
    }
    CLAY(step_decl()) {
        step_head(4, "Write the state machine (by hand)", m[4], next == 4,
                  !has_model ? "create the model .yaml in this folder with your editor" :
                  s->valid ? app.model_path : "the model has errors: see Validate");
        if (has_model) button("Open in editor", A_EDIT, 0, 1, 0);
    }
    CLAY(step_decl()) {
        step_head(5, "Validate", m[5], next == 5, "$ stategate validate");
        button("Validate", A_VALIDATE, 0, !busy && has_model, next == 5);
    }
    CLAY(step_decl()) {
        step_head(6, "Generate code and tests", m[6], next == 6, s->generated ? "up to date with the model" : "$ stategate gen");
        button("Generate", A_GEN, 0, !busy && s->valid, next == 6);
    }
    CLAY(step_decl()) {
        step_head(7, "Sign off locked safety items", m[7], next == 7,
                  !s->valid ? "$ stategate signoff" : !s->locked_items ? "nothing is locked: not needed" :
                  s->signed_off ? "signed" : "$ stategate signoff  (you confirm)");
        if (s->valid && s->locked_items && !s->signed_off) button("Sign off...", A_SIGNOFF, 0, !busy, next == 7);
    }
    CLAY(step_decl()) {
        step_head(0, "Save to git", m[0], next == 0,
                  !s->git ? "needs a repository" : saved ? "everything is committed" :
                  ticket_work ? "Run gate commits each ticket's work when it passes" : "$ git add -A && git commit  (needed before review)");
        if (s->git && !saved && !ticket_work) button("Commit", A_COMMIT, 0, !busy && s->dirty, next == 0 && s->dirty);
    }
    CLAY(step_decl()) {
        step_head(8, "Tickets: draft, AI fills in", m[8], next == 8,
                  s->t_draft ? TextFormat("%d draft(s) need a title and goal from your AI", s->t_draft)
                             : tickets_any ? TextFormat("%d ticket(s) written", tickets_any) : "$ stategate ticket gen");
        button("Draft tickets", A_TICKET_GEN, 0, !busy && s->generated && s->init, next == 8 && !s->t_draft);
        button(app.ai_cmd[0] ? "Fill with AI" : "Copy AI prompt", A_AI_FILL, 0, !busy && s->t_draft > 0, next == 8 && s->t_draft);
    }
    CLAY(step_decl()) {
        step_head(9, "Review tickets: approve or skip", m[9], next == 9,
                  s->t_open ? TextFormat("%d open ticket(s) to review", s->t_open) : "$ stategate ticket start  (you decide)");
        button("Review...", A_REVIEW, 0, !busy && s->t_open > 0 && saved, next == 9);
    }
    CLAY(step_decl()) {
        step_head(10, "AI writes node logic, ticket by ticket", m[10], next == 10,
                  *s->current ? TextFormat("active: %s (%s), %d approved after it. %s", s->current, s->current_status, s->t_approved,
                                           app.ai_cmd[0] ? "Your AI works each ticket until green." : "Paste the prompt into your AI.") :
                  s->t_approved ? TextFormat("%d approved, waiting", s->t_approved) : "needs an approved ticket");
        button(app.ai_cmd[0] ? "Implement with AI" : "Copy AI prompt", A_AI_IMPL, 0,
               !busy && !strcmp(s->current_status, "active"), next == 10 && !strcmp(s->current_status, "active"));
    }
    CLAY(step_decl()) {
        step_head(11, "Run the 11 checks", m[11], next == 11,
                  !strcmp(s->current_status, "active") ? "$ stategate ticket run  (green commits and finishes the ticket)" : "$ stategate gate");
        button("Run gate", A_GATE, 0, !busy && has_model, next == 11);
    }
}

static void steps_card(void) {
    CLAY(card()) { steps_card_body(); }
}

static void output_body(void) {
    const Job *j = &app.job;
    heading("OUTPUT");
    if (!j->title[0]) { CLAY_TEXT(cs("Nothing run yet. Each step's button shows its output here."), T(14, C_QUIET)); return; }
    CLAY_TEXT(csf("$ %s", j->title), T(14, C_INK));
    CLAY_TEXT(cs(j->running ? "running..." : j->exit_code == 0 ? "finished" : TextFormat("failed (exit %d)", j->exit_code)),
              T(14, j->running ? C_ACCENT : j->exit_code == 0 ? C_GREEN : C_RED));
    if (app.job_note[0]) CLAY_TEXT(cs(app.job_note), T(14, C_ACCENT));
    if (!j->pty) CLAY_TEXT(tail_lines(&j->out, 0, 24), T(13, C_INK));
}

static void text_field(int which, char *buf, const char *placeholder, float width);

static void ai_card(void) {
    CLAY(card()) {
        heading("YOUR AI");
        CLAY_TEXT(cs(app.ai_cmd[0] ? "The AI buttons pipe their prompt into this command, run in the project folder:"
                                   : "No command set: the AI buttons copy the prompt for you to paste. Or set a command the prompt is piped into:"),
                  T(13, C_QUIET));
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = PX(8), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            text_field(3, app.ai_in, "e.g. claude -p --permission-mode acceptEdits ...", 0);
            button("Save", A_AI_SAVE, 0, strcmp(app.ai_in, app.ai_cmd) != 0, strcmp(app.ai_in, app.ai_cmd) != 0);
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
        heading(app.have_model ? "MODEL ERRORS  (showing the last valid model)" : "MODEL ERRORS");
        for (int i = 0; i < app.model_errors.n && i < 12; i++) CLAY_TEXT(cs(app.model_errors.v[i]), T(13, C_RED));
    }
}

static void header_row(void) {
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = PX(8), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        CLAY_TEXT(cs("stategate"), T(22, C_INK));
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
        button("Projects", A_HOME, 0, !app.job.running, 0);
        button("Quit", A_QUIT, 0, 1, 0);
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

static Clay_RenderCommandArray build_project(void) {
    Clay_BeginLayout();
    CLAY({.id = CLAY_ID("Root"), .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {
        CLAY({.id = CLAY_ID("CanvasSpace"), .layout = {.sizing = {.width = CLAY_SIZING_PERCENT(2.0f / 3.0f), .height = CLAY_SIZING_GROW(0)}}}) {}
        CLAY({.id = CLAY_ID("Panel"),
              .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {CLAY_SIZING_PERCENT(1.0f / 3.0f), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(PX(14)), .childGap = PX(12)},
              .backgroundColor = C_PANEL,
              .border = {.color = C_RULE, .width = {.left = 1}},
              .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}}) {
            header_row();
            errors_card();
            steps_card();
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
    app.model_mtime = app.model_path ? mtime_of(app.model_path) : 0;
    load_model(1);
    gate_status_free(&app.gate);
    app.gate_mtime = mtime_of(".fsm/gate.json");
    gate_status_load(&app.gate, ".fsm/gate.json");
    job_clear(&app.job);
    app.modal = 0;
    app.screen = SCREEN_PROJECT;
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
    CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(PXF(420))}, .padding = CLAY_PADDING_ALL(PX(6)), .childGap = PX(2)},
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
            CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
            button("Quit", A_QUIT, 0, 1, 0);
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

static int quit_requested;

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
    case A_QUIT: quit_requested = 1; break;
    case A_GIT_INIT: { char *argv[] = {"git", "init", NULL}; job_start(&app.job, "git init", argv, 0); app.modal = 0; break; }
    case A_INIT: run_cli("stategate init", 0, "init", NULL, NULL); break;
    case A_EDIT: if (app.model_path) open_in_editor(app.model_path); break;
    case A_VALIDATE: run_cli("stategate validate", 0, "validate", NULL, NULL); break;
    case A_GEN: run_cli("stategate gen", 0, "gen", NULL, NULL); break;
    case A_SIGNOFF: run_cli("stategate signoff", 1, "signoff", NULL, NULL); break;
    case A_COMMIT: run_shell("git add -A && git commit -m \"stategate: model and generated code\"",
                             "git add -A && git commit -q -m 'stategate: model and generated code' && git log --oneline -1"); break;
    case A_TICKET_GEN: run_cli("stategate ticket gen", 0, "ticket", "gen", NULL); break;
    case A_REVIEW: run_cli("stategate ticket start", 1, "ticket", "start", NULL); break;
    case A_AI_FILL: run_ai(1); break;
    case A_AI_IMPL: run_ai(0); break;
    case A_AI_SAVE: ai_save(); break;
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
        hit = 1;
        break;
    }
    if (!hit) app.focus = 0;
}

static void handle_typing(void) {
    if (!app.focus) return;
    char *buf = app.focus == 1 ? app.path_in : app.focus == 2 ? app.name_in : app.ai_in;
    size_t cap = app.focus == 1 ? sizeof app.path_in : app.focus == 2 ? sizeof app.name_in : sizeof app.ai_in;
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
    if (IsKeyPressed(KEY_ENTER)) dispatch(app.focus == 1 ? A_GO_PATH : app.focus == 2 ? A_CREATE : A_AI_SAVE, 0);
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
    while (!WindowShouldClose() && !quit_requested) {
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
            if (prev > 0 && fabsf(prev - app.ui) > 0.001f && app.screen == SCREEN_PROJECT) fit_view();
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
        Clay_SetLayoutDimensions((Clay_Dimensions){(float)GetScreenWidth(), (float)GetScreenHeight()});
        Vector2 mouse = GetMousePosition();
        Clay_SetPointerState((Clay_Vector2){mouse.x, mouse.y}, IsMouseButtonDown(MOUSE_BUTTON_LEFT));
        int panel_scroll = app.screen == SCREEN_HOME || mouse.x > canvas_width() || app.modal;
        Vector2 wheel = panel_scroll ? GetMouseWheelMoveV() : (Vector2){0, 0};
        Clay_UpdateScrollContainers(true, (Clay_Vector2){wheel.x, wheel.y * 3}, GetFrameTime());
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
        handle_clicks();

        BeginDrawing();
        ClearBackground(BG);
        if (app.screen == SCREEN_PROJECT) draw_canvas();
        Clay_Raylib_Render(cmds, fonts);
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
