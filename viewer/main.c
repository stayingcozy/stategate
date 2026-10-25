/* stategate-view: read-only viewer and simulator for a stategate model.
 *
 * Left two-thirds: the state machine drawn from the model's saved layout.
 * Right third (Clay): the last gate result from .fsm/gate.json, a simulator
 * with one button per event, and the step ladder. The viewer never writes
 * a file, and it reports status only from gate.json. */

#define CLAY_IMPLEMENTATION
#include "third_party/clay/clay.h"
#include "third_party/clay/clay_renderer_raylib.c"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gate_status.h"
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
typedef struct {
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
    time_t mm = mtime_of(app.model_path), gm = mtime_of(".fsm/gate.json");
    if (mm != app.model_mtime) { app.model_mtime = mm; load_model(0); }
    if (gm != app.gate_mtime) { app.gate_mtime = gm; gate_status_load(&app.gate, ".fsm/gate.json"); }
}

/* ---- canvas ---- */

static float canvas_width(void) { return floorf(GetScreenWidth() * 2.0f / 3.0f); }

static float node_height(const State *s) {
    int lines = 1 + (s->entry.n ? 1 : 0) + (s->exit.n ? 1 : 0) + (s->invoke.n ? 1 : 0) + (s->has_after ? 1 : 0);
    return 22.0f + lines * 18.0f;
}

static Vector2 text_size(const char *s, float size);

/* Width in world units: the widest line at zoom 1, plus padding. */
static float node_width(const State *s) {
    float w = text_size(s->name, 18.0f).x;
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
        w = fmaxf(w, text_size(line, 13.5f).x);
    }
    if (s->has_after) {
        snprintf(line, sizeof line, "after %ld ms: %s", s->after_ms, s->after_event);
        w = fmaxf(w, text_size(line, 13.5f).x);
    }
    return fmaxf(NODE_W, w + 26.0f);
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
    app.zoom = fminf(1.6f, fmaxf(0.3f, fminf(zx, zy)));
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
    float fs = 14.0f * app.zoom;
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
    *y += 18.0f * app.zoom;
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
        float fs = 18.0f * app.zoom, small = 13.5f * app.zoom;
        if (fs < 6) continue;
        float x = r.x + 12 * app.zoom, y = r.y + 10 * app.zoom;
        text(s->name, x, y, fs, active ? ACCENT : INK);
        y += 26.0f * app.zoom;
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
        text(line, r.x + 4 * app.zoom, r.y + r.height + 6 * app.zoom, 13.0f * app.zoom, LOCKED);
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
        text("No valid model loaded yet. See the panel for errors.", 24, 24, 18, QUIET);
    }
    text("drag to pan, scroll to zoom, F to fit", 16, ch - 26, 14, QUIET);
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
    if (IsKeyPressed(KEY_F)) fit_view();
}

/* ---- panel (Clay) ---- */

static Clay_TextElementConfig *T(int size, Clay_Color color) {
    return CLAY_TEXT_CONFIG({.fontId = FONT_BODY, .fontSize = (uint16_t)size, .textColor = color, .lineHeight = (uint16_t)(size + 6)});
}

static void heading(const char *s) { CLAY_TEXT(cs(s), T(13, C_QUIET)); }

static Clay_ElementDeclaration card(void) {
    return (Clay_ElementDeclaration){
        .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(14), .childGap = 6},
        .backgroundColor = C_CARD,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = {.color = C_RULE, .width = {1, 1, 1, 1, 0}},
    };
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
        CLAY({.layout = {.childGap = 10, .sizing = {.width = CLAY_SIZING_GROW(0)}}}) {
            CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(44)}}}) {
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
    CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 6, .sizing = {.width = CLAY_SIZING_GROW(0)}}}) {
        for (int i = 0; i < app.model.events.n && i < 64; i++) {
            const char *ev = app.model.events.v[i];
            const char *target = state_target(s, ev);
            const State *t = target ? model_state(&app.model, target) : NULL;
            CLAY({.id = CLAY_IDI("Event", i),
                  .layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = {10, 10, 6, 6}, .childGap = 8},
                  .backgroundColor = target ? (Clay_Hovered() ? C_BTN_HOVER : C_BTN) : C_BTN_OFF,
                  .cornerRadius = CLAY_CORNER_RADIUS(6)}) {
                CLAY_TEXT(cs(ev), T(15, target ? C_INK : C_QUIET));
                CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
                CLAY_TEXT(t ? csf("-> %s", t->name) : cs("ignored"), T(14, target ? C_ACCENT : C_QUIET));
            }
        }
        CLAY({.id = CLAY_ID("Reset"), .layout = {.padding = {10, 10, 6, 6}},
              .backgroundColor = Clay_Hovered() ? C_BTN_HOVER : C_BTN_OFF, .cornerRadius = CLAY_CORNER_RADIUS(6)}) {
            CLAY_TEXT(cs("Reset to initial state"), T(14, C_INK));
        }
    }
    if (*app.flash) CLAY_TEXT(cs(app.flash), T(14, C_AMBER));
    for (int i = app.history.n - 1; i >= 0; i--) CLAY_TEXT(cs(app.history.v[i]), T(13, i == app.history.n - 1 ? C_INK : C_QUIET));
}

static void sim_card(void) {
    CLAY(card()) { sim_body(); }
}

static void steps_body(void) {
    heading("STEP LADDER");
    if (!app.have_model || !app.model.n_steps) { CLAY_TEXT(cs("No steps in the model"), T(14, C_QUIET)); return; }
    for (int i = 0; i < app.model.n_steps; i++) {
        const Step *st = &app.model.steps[i];
        int active = !strcmp(st->status, "active"), done = !strcmp(st->status, "done");
        CLAY_TEXT(csf("%s  %s", st->id, st->status), T(15, active ? C_ACCENT : done ? C_GREEN : C_QUIET));
        if (st->goal) CLAY_TEXT(cs(st->goal), T(13, C_INK));
    }
}

static void steps_card(void) {
    CLAY(card()) { steps_body(); }
}

static void errors_card(void) {
    if (!app.model_errors.n) return;
    CLAY(card()) {
        heading(app.have_model ? "MODEL ERRORS  (showing the last valid model)" : "MODEL ERRORS");
        for (int i = 0; i < app.model_errors.n && i < 12; i++) CLAY_TEXT(cs(app.model_errors.v[i]), T(13, C_RED));
    }
}

static Clay_RenderCommandArray build_panel(void) {
    Clay_BeginLayout();
    CLAY({.id = CLAY_ID("Root"), .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {
        CLAY({.id = CLAY_ID("CanvasSpace"), .layout = {.sizing = {.width = CLAY_SIZING_PERCENT(2.0f / 3.0f), .height = CLAY_SIZING_GROW(0)}}}) {}
        CLAY({.id = CLAY_ID("Panel"),
              .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(14), .childGap = 12},
              .backgroundColor = C_PANEL,
              .border = {.color = C_RULE, .width = {.left = 1}},
              .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}}) {
            CLAY_TEXT(cs("stategate"), T(22, C_INK));
            CLAY_TEXT(csf("%s  (live reload)", app.model_path), T(13, C_QUIET));
            errors_card();
            gate_card();
            sim_card();
            steps_card();
        }
    }
    return Clay_EndLayout();
}

static void handle_panel_clicks(void) {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || !app.have_model) return;
    for (int i = 0; i < app.model.events.n && i < 64; i++)
        if (Clay_PointerOver(Clay_GetElementIdWithIndex(CLAY_STRING("Event"), (uint32_t)i))) sim_send(app.model.events.v[i]);
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("Reset")))) sim_reset();
}

static void clay_error(Clay_ErrorData e) { fprintf(stderr, "clay: %.*s\n", e.errorText.length, e.errorText.chars); }

int main(int argc, char **argv) {
    const char *dir = ".", *shot = NULL, *sends = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--send") && i + 1 < argc) sends = argv[++i];
        else if (argv[i][0] != '-') dir = argv[i];
        else {
            fprintf(stderr, "usage: stategate-view [project dir] [--send EV1,EV2] [--screenshot out.png]\n");
            return 2;
        }
    }
    if (chdir(dir) != 0) { perror(dir); return 2; }
    StrList errors = {0};
    char *path = model_find(".", &errors);
    if (!path) { fprintf(stderr, "%s\n", errors.v[0]); return 2; }
    app.model_path = path;
    app.zoom = 1.0f;

    SetTraceLogLevel(LOG_WARNING);
    Clay_Raylib_Initialize(1440, 900, "stategate", FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | (shot ? FLAG_WINDOW_HIDDEN : 0));
    SetTargetFPS(60);
    uint64_t mem = Clay_MinMemorySize();
    Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(mem, malloc(mem)),
                    (Clay_Dimensions){(float)GetScreenWidth(), (float)GetScreenHeight()}, (Clay_ErrorHandler){clay_error, 0});

    Font fonts[1];
    char font_path[1024];
    snprintf(font_path, sizeof font_path, "%s/Roboto-Regular.ttf", SG_RESOURCE_DIR);
#ifdef SG_SOURCE_RESOURCE_DIR
    if (!FileExists(font_path)) snprintf(font_path, sizeof font_path, "%s/Roboto-Regular.ttf", SG_SOURCE_RESOURCE_DIR);
#endif
    fonts[0] = FileExists(font_path) ? LoadFontEx(font_path, 48, 0, 0) : GetFontDefault();
    SetTextureFilter(fonts[0].texture, TEXTURE_FILTER_BILINEAR);
    app.font = fonts[0];
    Clay_SetMeasureTextFunction(Raylib_MeasureText, fonts);

    app.model_mtime = mtime_of(app.model_path);
    load_model(1);
    app.gate_mtime = mtime_of(".fsm/gate.json");
    gate_status_load(&app.gate, ".fsm/gate.json");
    if (sends) {
        char *list = xstrdup(sends), *save = NULL;
        for (char *e = strtok_r(list, ",", &save); e; e = strtok_r(NULL, ",", &save)) sim_send(e);
    }

    double last_poll = 0;
    int frames = 0;
    while (!WindowShouldClose()) {
        if (GetTime() - last_poll > 0.5) { poll_files(); last_poll = GetTime(); }
        arena_used = 0;
        Clay_SetLayoutDimensions((Clay_Dimensions){(float)GetScreenWidth(), (float)GetScreenHeight()});
        Vector2 mouse = GetMousePosition();
        Clay_SetPointerState((Clay_Vector2){mouse.x, mouse.y}, IsMouseButtonDown(MOUSE_BUTTON_LEFT));
        if (mouse.x > canvas_width()) {
            Vector2 wheel = GetMouseWheelMoveV();
            Clay_UpdateScrollContainers(true, (Clay_Vector2){wheel.x, wheel.y * 3}, GetFrameTime());
        } else {
            Clay_UpdateScrollContainers(false, (Clay_Vector2){0, 0}, GetFrameTime());
        }
        handle_canvas_input();
        Clay_RenderCommandArray cmds = build_panel();
        handle_panel_clicks();

        BeginDrawing();
        ClearBackground(BG);
        draw_canvas();
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
    Clay_Raylib_Close();
    return 0;
}
