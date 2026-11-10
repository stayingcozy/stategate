#include "system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "platform.h"
#include "validate.h"
#include "ydoc.h"

static const char *const ROOT_KEYS[] = {"system", "machines", "channels", "external", NULL};
static const char *const SYSTEM_KEYS[] = {"id", "language", NULL};
static const char *const MACHINE_KEYS[] = {"id", "dir", NULL};
static const char *const CHANNEL_KEYS[] = {"from", "to", "event", "kind", "depth", "drop", NULL};
static const char *const DROPS[] = {"never", "oldest", "newest", NULL};

static int in_list(const char *const *l, const char *s) {
    for (int i = 0; l[i]; i++)
        if (s && !strcmp(l[i], s)) return 1;
    return 0;
}

static int is_ident(const char *s) {
    if (!s || !*s || !((*s >= 'a' && *s <= 'z') || *s == '_')) return 0;
    for (const char *c = s; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) return 0;
    return 1;
}

SysMachine *system_machine(const SgSystem *s, const char *id) {
    for (int i = 0; i < s->n_machines; i++)
        if (id && !strcmp(s->machines[i].id, id)) return &s->machines[i];
    return NULL;
}

static void load_machine(SysMachine *sm, StrList *errors) {
    StrList found_err = {0};
    char *name = model_find(sm->dir, &found_err);
    if (!name) {
        for (int i = 0; i < found_err.n; i++) sl_pushf(errors, "machine %s (%s/): %s", sm->id, sm->dir, found_err.v[i]);
        return;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", sm->dir, name);
    free(name);
    StrList e1 = {0}, e2 = {0}, e3 = {0};
    if (validate_all(path, &sm->model, &e1, &e2, &e3) != 0) {
        StrList *all[] = {&e1, &e2, &e3};
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < all[k]->n; i++) sl_pushf(errors, "machine %s: %s", sm->id, all[k]->v[i]);
        sl_pushf(errors, "machine %s: fix it with stategate -C %s validate", sm->id, sm->dir);
        return;
    }
    sm->loaded = 1;
}

int system_load(const char *path, SgSystem *s, StrList *errors) {
    memset(s, 0, sizeof *s);
    s->path = xstrdup(path);
    int before = errors->n;
    YReader r;
    if (y_load(&r, path, errors) != 0) { y_free(&r); return -1; }
    yaml_node_t *root = y_root(&r);
    if (!y_is_map(root)) {
        y_err(&r, root, "system file must be a mapping at the top level");
        y_free(&r);
        return -1;
    }
    y_check_keys(&r, root, "system file", ROOT_KEYS);

    yaml_node_t *sys = y_get(&r, root, "system");
    if (!y_is_map(sys)) {
        y_err(&r, sys, "missing 'system: { id, language }'");
    } else {
        y_check_keys(&r, sys, "system", SYSTEM_KEYS);
        s->id = y_str(&r, y_get(&r, sys, "id"), "system id");
        s->language = y_str(&r, y_get(&r, sys, "language"), "language");
    }
    if (!s->id) { y_err(&r, sys, "system.id is required"); s->id = xstrdup("system"); }
    if (!s->language) { y_err(&r, sys, "system.language is required (one project, one language)"); s->language = xstrdup("?"); }
    else {
        int known = 0;
        for (int i = 0; SG_LANGUAGES[i]; i++) known |= !strcmp(s->language, SG_LANGUAGES[i]);
        if (!known) y_err(&r, sys, "language '%s' is not supported", s->language);
    }

    yaml_node_t *ms = y_get(&r, root, "machines");
    if (!y_is_seq(ms)) {
        y_err(&r, ms, "missing 'machines' list");
    } else {
        int n = (int)(ms->data.sequence.items.top - ms->data.sequence.items.start);
        s->machines = xcalloc((size_t)n, sizeof(SysMachine));
        for (yaml_node_item_t *it = ms->data.sequence.items.start; it < ms->data.sequence.items.top; it++) {
            yaml_node_t *mn = y_node(&r, *it);
            if (!y_is_map(mn)) { y_err(&r, mn, "each machine must be { id, dir }"); continue; }
            y_check_keys(&r, mn, "machine", MACHINE_KEYS);
            SysMachine *sm = &s->machines[s->n_machines++];
            sm->line = y_line(mn);
            sm->id = y_str(&r, y_get(&r, mn, "id"), "machine id");
            sm->dir = y_str(&r, y_get(&r, mn, "dir"), "machine dir");
            if (!sm->id) { y_err(&r, mn, "machine is missing 'id'"); sm->id = xstrdup("?"); }
            if (!sm->dir) sm->dir = xstrdup(sm->id);
            if (!is_ident(sm->id)) y_err(&r, mn, "machine id '%s' must be snake_case", sm->id);
            for (int k = 0; k < s->n_machines - 1; k++)
                if (!strcmp(s->machines[k].id, sm->id)) y_err(&r, mn, "machine %s is listed twice", sm->id);
        }
    }

    yaml_node_t *cs = y_get(&r, root, "channels");
    if (cs && !y_is_seq(cs)) y_err(&r, cs, "'channels' must be a list");
    if (y_is_seq(cs)) {
        int n = (int)(cs->data.sequence.items.top - cs->data.sequence.items.start);
        s->channels = xcalloc((size_t)n, sizeof(Channel));
        for (yaml_node_item_t *it = cs->data.sequence.items.start; it < cs->data.sequence.items.top; it++) {
            yaml_node_t *cn = y_node(&r, *it);
            if (!y_is_map(cn)) { y_err(&r, cn, "each channel must be { from, to, event, kind, ... }"); continue; }
            y_check_keys(&r, cn, "channel", CHANNEL_KEYS);
            Channel *c = &s->channels[s->n_channels++];
            c->line = y_line(cn);
            c->from = y_str(&r, y_get(&r, cn, "from"), "from");
            c->to = y_str(&r, y_get(&r, cn, "to"), "to");
            c->event = y_str(&r, y_get(&r, cn, "event"), "event");
            c->kind = y_str(&r, y_get(&r, cn, "kind"), "kind");
            c->depth = y_int(&r, y_get(&r, cn, "depth"), "depth", 0);
            c->drop = y_str(&r, y_get(&r, cn, "drop"), "drop");
            if (!c->from || !c->to || !c->event) {
                y_err(&r, cn, "a channel needs from, to and event");
                if (!c->from) c->from = xstrdup("?");
                if (!c->to) c->to = xstrdup("?");
                if (!c->event) c->event = xstrdup("?");
            }
            if (!c->kind) c->kind = xstrdup("queue");
            if (!strcmp(c->kind, "queue")) {
                if (c->depth <= 0) y_err(&r, cn, "queue %s -> %s (%s) needs a depth: how many events it holds", c->from, c->to, c->event);
                if (!c->drop) y_err(&r, cn, "queue %s -> %s (%s) needs a drop policy: never, oldest or newest", c->from, c->to, c->event);
                else if (!in_list(DROPS, c->drop)) y_err(&r, cn, "drop '%s' must be never, oldest or newest", c->drop);
            } else if (!strcmp(c->kind, "sync")) {
                if (y_get(&r, cn, "depth") || c->drop)
                    y_err(&r, cn, "sync channel %s -> %s (%s) is delivered at once: it takes no depth or drop", c->from, c->to, c->event);
            } else {
                y_err(&r, cn, "channel kind '%s' must be queue or sync", c->kind);
            }
            for (int k = 0; k < s->n_channels - 1; k++) {
                const Channel *o = &s->channels[k];
                if (!strcmp(o->from, c->from) && !strcmp(o->to, c->to) && !strcmp(o->event, c->event))
                    y_err(&r, cn, "channel %s -> %s (%s) is declared twice", c->from, c->to, c->event);
            }
        }
    }
    y_strlist(&r, y_get(&r, root, "external"), "external", &s->external);
    y_free(&r);

    for (int i = 0; i < s->n_machines; i++) {
        SysMachine *sm = &s->machines[i];
        if (!sg_is_dir(sm->dir)) { sl_pushf(errors, "machine %s: folder %s/ does not exist", sm->id, sm->dir); continue; }
        load_machine(sm, errors);
    }
    return errors->n == before ? 0 : -1;
}

/* Events a machine raises for itself: its services' emits, its `after`
 * timers and its max_time_in_state watchdogs. These need no sender. */
static int raised_inside(const SgModel *m, const char *event) {
    for (int i = 0; i < m->n_services; i++)
        if (sl_contains(&m->services[i].emits, event)) return 1;
    for (int i = 0; i < m->n_states; i++)
        if (m->states[i].has_after && !strcmp(m->states[i].after_event, event)) return 1;
    for (int i = 0; i < m->n_invariants; i++)
        if (m->invariants[i].kind == INV_MAX_TIME_IN_STATE && !strcmp(m->invariants[i].exit_event, event)) return 1;
    return 0;
}

void system_check(const SgSystem *s, StrList *e) {
    for (int i = 0; i < s->n_machines; i++) {
        const SysMachine *sm = &s->machines[i];
        if (!sm->loaded) continue;
        if (strcmp(sm->model.id, sm->id))
            sl_pushf(e, "machine %s: its model says machine id '%s'; the two must match (generated names use it)", sm->id, sm->model.id);
        if (strcmp(sm->model.language, s->language))
            sl_pushf(e, "machine %s is %s but the system is %s: one project, one language", sm->id, sm->model.language, s->language);
    }

    for (int i = 0; i < s->n_channels; i++) {
        const Channel *c = &s->channels[i];
        const SysMachine *from = system_machine(s, c->from), *to = system_machine(s, c->to);
        if (!from) sl_pushf(e, "channel %s -> %s (%s): no machine %s", c->from, c->to, c->event, c->from);
        if (!to) sl_pushf(e, "channel %s -> %s (%s): no machine %s", c->from, c->to, c->event, c->to);
        if (from && to && from == to) sl_pushf(e, "channel %s -> %s (%s): a machine does not send to itself", c->from, c->to, c->event);
        if (from && from->loaded && !sl_contains(&from->model.sends, c->event))
            sl_pushf(e, "channel %s -> %s carries %s, but %s does not list %s under 'sends'", c->from, c->to, c->event, c->from, c->event);
        if (to && to->loaded && model_event_index(&to->model, c->event) < 0)
            sl_pushf(e, "channel %s -> %s delivers %s, which %s does not declare under 'events': every state of %s must handle or ignore it",
                     c->from, c->to, c->event, c->to, c->to);
    }

    for (int i = 0; i < s->n_machines; i++) {
        const SysMachine *sm = &s->machines[i];
        if (!sm->loaded) continue;
        const SgModel *m = &sm->model;
        for (int k = 0; k < m->sends.n; k++) {
            int carried = 0;
            for (int c = 0; c < s->n_channels; c++)
                if (!strcmp(s->channels[c].from, sm->id) && !strcmp(s->channels[c].event, m->sends.v[k])) carried = 1;
            if (!carried) sl_pushf(e, "%s sends %s, but no channel carries it from %s", sm->id, m->sends.v[k], sm->id);
        }
        for (int k = 0; k < m->events.n; k++) {
            const char *ev = m->events.v[k];
            if (raised_inside(m, ev) || sl_contains(&s->external, ev)) continue;
            int inbound = 0;
            for (int c = 0; c < s->n_channels; c++)
                if (!strcmp(s->channels[c].to, sm->id) && !strcmp(s->channels[c].event, ev)) inbound = 1;
            if (!inbound)
                sl_pushf(e, "%s receives %s, but nothing sends it: add a channel, or list %s under 'external' if it comes from outside (an ISR, the phone)",
                         sm->id, ev, ev);
        }
    }

    for (int k = 0; k < s->external.n; k++) {
        int used = 0;
        for (int i = 0; i < s->n_machines; i++)
            if (s->machines[i].loaded && model_event_index(&s->machines[i].model, s->external.v[k]) >= 0) used = 1;
        if (!used) sl_pushf(e, "external event %s is not received by any machine", s->external.v[k]);
    }
}

/* ---- commands at a system root ---- */

static int load_and_check(SgSystem *s, int quiet) {
    StrList e1 = {0}, e2 = {0};
    system_load(SYSTEM_PATH, s, &e1);
    system_check(s, &e2);
    if (!quiet || e1.n || e2.n) {
        if (e1.n) { fprintf(stderr, "system schema:\n"); for (int i = 0; i < e1.n; i++) fprintf(stderr, "  %s\n", e1.v[i]); }
        if (e2.n) { fprintf(stderr, "system wiring:\n"); for (int i = 0; i < e2.n; i++) fprintf(stderr, "  %s\n", e2.v[i]); }
    }
    return e1.n + e2.n;
}

/* Runs `stategate -C <dir> <cmd> [arg]` and prints its output, indented. */
static int run_in(const char *self, const SysMachine *sm, const char *cmd, const char *arg) {
    const char *argv[] = {self, "-C", sm->dir, cmd, arg, NULL};
    int st;
    char *out = run_argv(argv, 1, &st);
    printf("== %s (%s/)\n", sm->id, sm->dir);
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) printf("   %s\n", line);
    free(out);
    return st;
}

static int system_mermaid(const SgSystem *s) {
    printf("flowchart LR\n");
    for (int i = 0; i < s->n_machines; i++) printf("    %s[%s]\n", s->machines[i].id, s->machines[i].id);
    for (int i = 0; i < s->n_channels; i++) {
        const Channel *c = &s->channels[i];
        if (!strcmp(c->kind, "sync")) printf("    %s -->|%s sync| %s\n", c->from, c->event, c->to);
        else printf("    %s -->|%s queue %ld, drop %s| %s\n", c->from, c->event, c->depth, c->drop ? c->drop : "?", c->to);
    }
    if (s->external.n) printf("    outside((outside))\n");
    for (int k = 0; k < s->external.n; k++)
        for (int i = 0; i < s->n_machines; i++)
            if (s->machines[i].loaded && model_event_index(&s->machines[i].model, s->external.v[k]) >= 0)
                printf("    outside -.->|%s| %s\n", s->external.v[k], s->machines[i].id);
    return 0;
}

int cmd_system(const char *cmd, int argc, char **argv, const char *argv0) {
    char self[SG_PATH_MAX];
    sg_self_exe(self, sizeof self, argv0);
    SgSystem s;

    if (!strcmp(cmd, "validate")) {
        int n = load_and_check(&s, 0);
        if (n) return 1;
        printf("%s is valid: %d machines, %d channels, %d external events\n", SYSTEM_PATH, s.n_machines, s.n_channels, s.external.n);
        return 0;
    }
    if (!strcmp(cmd, "export")) {
        if (argc < 1 || strcmp(argv[0], "mermaid")) { fprintf(stderr, "usage: stategate export mermaid\n"); return 2; }
        StrList e = {0};
        system_load(SYSTEM_PATH, &s, &e);
        return system_mermaid(&s);
    }
    if (!strcmp(cmd, "gen")) {
        StrList e = {0};
        if (system_load(SYSTEM_PATH, &s, &e) != 0 && !s.n_machines) {
            for (int i = 0; i < e.n; i++) fprintf(stderr, "%s\n", e.v[i]);
            return 1;
        }
        int bad = 0;
        for (int i = 0; i < s.n_machines; i++) bad |= run_in(self, &s.machines[i], "gen", NULL) != 0;
        return bad;
    }
    if (!strcmp(cmd, "init")) {
        StrList e = {0};
        system_load(SYSTEM_PATH, &s, &e);
        for (int i = 0; i < s.n_machines; i++)
            if (sg_is_dir(s.machines[i].dir)) run_in(self, &s.machines[i], "init", NULL);
        int st;
        const char *top_cmd[] = {"git", "rev-parse", "--show-toplevel", NULL};
        char *top = run_argv(top_cmd, 0, &st);
        top[strcspn(top, "\r\n")] = 0;
        char cwd[SG_PATH_MAX];
        if (st == 0 && sg_getcwd(cwd, sizeof cwd) && sg_path_eq(top, cwd)) {
            const char *hook = "#!/bin/sh\n# Installed by stategate init. The gate decides what lands.\nexec stategate gate\n";
            write_file(".git/hooks/pre-commit", hook, strlen(hook));
            chmod(".git/hooks/pre-commit", 0755);
            printf("installed .git/hooks/pre-commit (runs the system gate)\n");
        } else {
            printf("not a git repository root: pre-commit hook not installed\n");
        }
        return 0;
    }
    if (!strcmp(cmd, "gate")) {
        int release = argc > 0 && !strcmp(argv[0], "--release");
        printf("stategate gate  system %s\n", SYSTEM_PATH);
        int n = load_and_check(&s, 1);
        if (n) {
            printf("SYSTEM GATE RED  at the wiring (%d problem%s above)\n", n, n == 1 ? "" : "s");
            return 1;
        }
        printf("  PASS  wiring: %d machines, %d channels\n", s.n_machines, s.n_channels);
        int red = 0;
        for (int i = 0; i < s.n_machines; i++)
            if (run_in(self, &s.machines[i], "gate", release ? "--release" : NULL) != 0) red++;
        if (red) {
            printf("SYSTEM GATE RED  %d of %d machines red\n", red, s.n_machines);
            return 1;
        }
        printf("SYSTEM GATE GREEN  %d machines\n", s.n_machines);
        return 0;
    }
    StrList e = {0};
    system_load(SYSTEM_PATH, &s, &e);
    fprintf(stderr,
            "%s is a system root: run validate, gen, gate, init or export mermaid here, and every other\n"
            "command in a machine's folder, e.g. stategate -C %s %s\n",
            SYSTEM_PATH, s.n_machines ? s.machines[0].dir : "<machine>", cmd);
    return 2;
}
