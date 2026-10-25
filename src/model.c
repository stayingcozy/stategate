#include "model.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ydoc.h"

static const char *const ROOT_KEYS[] = {"machine", "events", "hal", "states", "actions",
                                        "services", "invariants", "steps", NULL};
static const char *const MACHINE_KEYS[] = {"id", "version", "language", NULL};
static const char *const STATE_KEYS[] = {"id", "name", "initial", "entry", "exit", "on", "ignore",
                                         "invoke", "after", "layout", "note", NULL};
static const char *const AFTER_KEYS[] = {"ms", "event", NULL};
static const char *const LAYOUT_KEYS[] = {"x", "y", NULL};
static const char *const TRANS_KEYS[] = {"target", NULL};
static const char *const ACTION_KEYS[] = {"stub", "locked", "note", NULL};
static const char *const SERVICE_KEYS[] = {"stub", "locked", "fidelity", "emits", "note", NULL};
static const char *const INV_KEYS[] = {"id", "locked", "kind", "event", "target", "state",
                                       "limit_ms", "exit_event", "call", "max", "note", NULL};
static const char *const STEP_KEYS[] = {"id", "goal", "status", NULL};
static const char *const HAL_KEYS[] = {"call", "arg", NULL};

static void parse_states(YReader *r, yaml_node_t *seq, Model *m) {
    if (!seq) { y_err(r, NULL, "missing required 'states' list"); return; }
    if (!y_is_seq(seq)) { y_err(r, seq, "'states' must be a list"); return; }
    int n = (int)(seq->data.sequence.items.top - seq->data.sequence.items.start);
    m->states = xcalloc((size_t)n, sizeof(State));
    for (yaml_node_item_t *it = seq->data.sequence.items.start; it < seq->data.sequence.items.top; it++) {
        yaml_node_t *sn = y_node(r, *it);
        if (!y_is_map(sn)) { y_err(r, sn, "each state must be a mapping"); continue; }
        State *s = &m->states[m->n_states++];
        s->line = y_line(sn);
        y_check_keys(r, sn, "state", STATE_KEYS);
        s->id = y_str(r, y_get(r, sn, "id"), "state id");
        if (!s->id) { y_err(r, sn, "state is missing 'id'"); s->id = xstrdup("?"); }
        s->name = y_str(r, y_get(r, sn, "name"), "state name");
        if (!s->name) {
            const char *dot = strrchr(s->id, '.');
            s->name = xstrdup(dot ? dot + 1 : s->id);
        }
        s->initial = y_bool(r, y_get(r, sn, "initial"), "initial", 0);
        y_strlist(r, y_get(r, sn, "entry"), "entry", &s->entry);
        y_strlist(r, y_get(r, sn, "exit"), "exit", &s->exit);
        y_strlist(r, y_get(r, sn, "ignore"), "ignore", &s->ignore);
        y_strlist(r, y_get(r, sn, "invoke"), "invoke", &s->invoke);

        yaml_node_t *on = y_get(r, sn, "on");
        if (on && !y_is_map(on)) y_err(r, on, "'on' must map EVENT to target state id");
        if (y_is_map(on)) {
            int k = (int)(on->data.mapping.pairs.top - on->data.mapping.pairs.start);
            s->on = xcalloc((size_t)k, sizeof(Transition));
            for (yaml_node_pair_t *p = on->data.mapping.pairs.start; p < on->data.mapping.pairs.top; p++) {
                yaml_node_t *kn = y_node(r, p->key), *vn = y_node(r, p->value);
                Transition *t = &s->on[s->n_on++];
                t->event = y_str(r, kn, "event name");
                if (y_is_map(vn)) {
                    y_check_keys(r, vn, "transition", TRANS_KEYS);
                    t->target = y_str(r, y_get(r, vn, "target"), "target");
                } else {
                    t->target = y_str(r, vn, "target");
                }
                if (!t->event) t->event = xstrdup("?");
                if (!t->target) { y_err(r, vn, "transition on %s has no target", t->event); t->target = xstrdup("?"); }
            }
        }

        yaml_node_t *after = y_get(r, sn, "after");
        if (after) {
            if (!y_is_map(after)) {
                y_err(r, after, "'after' must be { ms: N, event: EVENT }");
            } else {
                y_check_keys(r, after, "after", AFTER_KEYS);
                s->has_after = 1;
                s->after_ms = y_int(r, y_get(r, after, "ms"), "after.ms", -1);
                s->after_event = y_str(r, y_get(r, after, "event"), "after.event");
                if (s->after_ms <= 0) y_err(r, after, "after.ms must be a positive integer");
                if (!s->after_event) { y_err(r, after, "after needs an event"); s->after_event = xstrdup("?"); }
            }
        }

        yaml_node_t *layout = y_get(r, sn, "layout");
        if (layout) {
            y_check_keys(r, layout, "layout", LAYOUT_KEYS);
            s->x = y_num(r, y_get(r, layout, "x"), "layout.x", 0);
            s->y = y_num(r, y_get(r, layout, "y"), "layout.y", 0);
        }
    }
}

static void parse_actions(YReader *r, yaml_node_t *map, Model *m) {
    if (!map) return;
    if (!y_is_map(map)) { y_err(r, map, "'actions' must map action id to settings"); return; }
    int n = (int)(map->data.mapping.pairs.top - map->data.mapping.pairs.start);
    m->actions = xcalloc((size_t)n, sizeof(Action));
    for (yaml_node_pair_t *p = map->data.mapping.pairs.start; p < map->data.mapping.pairs.top; p++) {
        yaml_node_t *kn = y_node(r, p->key), *vn = y_node(r, p->value);
        Action *a = &m->actions[m->n_actions++];
        a->line = y_line(kn);
        a->id = y_str(r, kn, "action id");
        if (!a->id) a->id = xstrdup("?");
        if (vn && vn->type == YAML_SCALAR_NODE && !*vn->data.scalar.value) continue; /* `foo:` */
        if (!y_is_map(vn)) { y_err(r, vn, "action %s must be a mapping", a->id); continue; }
        y_check_keys(r, vn, "action", ACTION_KEYS);
        a->stub = y_bool(r, y_get(r, vn, "stub"), "stub", 0);
        a->locked = y_bool(r, y_get(r, vn, "locked"), "locked", 0);
        a->note = y_str(r, y_get(r, vn, "note"), "note");
    }
}

static void parse_services(YReader *r, yaml_node_t *map, Model *m) {
    if (!map) return;
    if (!y_is_map(map)) { y_err(r, map, "'services' must map service id to settings"); return; }
    int n = (int)(map->data.mapping.pairs.top - map->data.mapping.pairs.start);
    m->services = xcalloc((size_t)n, sizeof(Service));
    for (yaml_node_pair_t *p = map->data.mapping.pairs.start; p < map->data.mapping.pairs.top; p++) {
        yaml_node_t *kn = y_node(r, p->key), *vn = y_node(r, p->value);
        Service *s = &m->services[m->n_services++];
        s->line = y_line(kn);
        s->id = y_str(r, kn, "service id");
        if (!s->id) s->id = xstrdup("?");
        if (!y_is_map(vn)) { y_err(r, vn, "service %s must be a mapping", s->id); continue; }
        y_check_keys(r, vn, "service", SERVICE_KEYS);
        s->stub = y_bool(r, y_get(r, vn, "stub"), "stub", 0);
        s->locked = y_bool(r, y_get(r, vn, "locked"), "locked", 0);
        s->fidelity = y_str(r, y_get(r, vn, "fidelity"), "fidelity");
        s->note = y_str(r, y_get(r, vn, "note"), "note");
        y_strlist(r, y_get(r, vn, "emits"), "emits", &s->emits);
    }
}

static void parse_invariants(YReader *r, yaml_node_t *seq, Model *m) {
    if (!seq) return;
    if (!y_is_seq(seq)) { y_err(r, seq, "'invariants' must be a list"); return; }
    int n = (int)(seq->data.sequence.items.top - seq->data.sequence.items.start);
    m->invariants = xcalloc((size_t)n, sizeof(Invariant));
    for (yaml_node_item_t *it = seq->data.sequence.items.start; it < seq->data.sequence.items.top; it++) {
        yaml_node_t *in = y_node(r, *it);
        if (!y_is_map(in)) { y_err(r, in, "each invariant must be a mapping"); continue; }
        Invariant *v = &m->invariants[m->n_invariants++];
        v->line = y_line(in);
        y_check_keys(r, in, "invariant", INV_KEYS);
        v->id = y_str(r, y_get(r, in, "id"), "invariant id");
        if (!v->id) { y_err(r, in, "invariant is missing 'id'"); v->id = xstrdup("?"); }
        v->locked = y_bool(r, y_get(r, in, "locked"), "locked", 0);
        v->kind_name = y_str(r, y_get(r, in, "kind"), "kind");
        if (!v->kind_name) v->kind_name = xstrdup("");
        if (!strcmp(v->kind_name, "every_state_handles")) v->kind = INV_EVERY_STATE_HANDLES;
        else if (!strcmp(v->kind_name, "max_time_in_state")) v->kind = INV_MAX_TIME_IN_STATE;
        else if (!strcmp(v->kind_name, "hal_clamp")) v->kind = INV_HAL_CLAMP;
        else y_err(r, in, "invariant %s: kind must be every_state_handles, max_time_in_state or hal_clamp", v->id);
        v->event = y_str(r, y_get(r, in, "event"), "event");
        v->target = y_str(r, y_get(r, in, "target"), "target");
        v->state = y_str(r, y_get(r, in, "state"), "state");
        v->exit_event = y_str(r, y_get(r, in, "exit_event"), "exit_event");
        v->limit_ms = y_int(r, y_get(r, in, "limit_ms"), "limit_ms", -1);
        v->call = y_str(r, y_get(r, in, "call"), "call");
        v->max = y_int(r, y_get(r, in, "max"), "max", -1);
        switch (v->kind) {
        case INV_EVERY_STATE_HANDLES:
            if (!v->event || !v->target) y_err(r, in, "invariant %s needs 'event' and 'target'", v->id);
            break;
        case INV_MAX_TIME_IN_STATE:
            if (!v->state || !v->exit_event || v->limit_ms <= 0)
                y_err(r, in, "invariant %s needs 'state', 'exit_event' and a positive 'limit_ms'", v->id);
            break;
        case INV_HAL_CLAMP:
            if (!v->call || v->max < 0) y_err(r, in, "invariant %s needs 'call' and a non-negative 'max'", v->id);
            break;
        case INV_UNKNOWN:
            break;
        }
    }
}

static void parse_steps(YReader *r, yaml_node_t *seq, Model *m) {
    if (!seq) return;
    if (!y_is_seq(seq)) { y_err(r, seq, "'steps' must be a list"); return; }
    int n = (int)(seq->data.sequence.items.top - seq->data.sequence.items.start);
    m->steps = xcalloc((size_t)n, sizeof(Step));
    for (yaml_node_item_t *it = seq->data.sequence.items.start; it < seq->data.sequence.items.top; it++) {
        yaml_node_t *sn = y_node(r, *it);
        if (!y_is_map(sn)) { y_err(r, sn, "each step must be a mapping"); continue; }
        Step *s = &m->steps[m->n_steps++];
        y_check_keys(r, sn, "step", STEP_KEYS);
        s->id = y_str(r, y_get(r, sn, "id"), "step id");
        s->goal = y_str(r, y_get(r, sn, "goal"), "goal");
        s->status = y_str(r, y_get(r, sn, "status"), "status");
        if (!s->id) { y_err(r, sn, "step is missing 'id'"); s->id = xstrdup("?"); }
        if (!s->status) s->status = xstrdup("planned");
        if (strcmp(s->status, "planned") && strcmp(s->status, "active") && strcmp(s->status, "done"))
            y_err(r, sn, "step %s: status must be planned, active or done", s->id);
    }
}

static void parse_hal(YReader *r, yaml_node_t *seq, Model *m) {
    if (!seq) return;
    if (!y_is_seq(seq)) { y_err(r, seq, "'hal' must be a list"); return; }
    int n = (int)(seq->data.sequence.items.top - seq->data.sequence.items.start);
    m->hal = xcalloc((size_t)n, sizeof(HalCall));
    for (yaml_node_item_t *it = seq->data.sequence.items.start; it < seq->data.sequence.items.top; it++) {
        yaml_node_t *hn = y_node(r, *it);
        if (!y_is_map(hn)) { y_err(r, hn, "each hal entry must be { call: name, arg: type }"); continue; }
        HalCall *h = &m->hal[m->n_hal++];
        y_check_keys(r, hn, "hal", HAL_KEYS);
        h->call = y_str(r, y_get(r, hn, "call"), "call");
        h->arg = y_str(r, y_get(r, hn, "arg"), "arg");
        if (!h->call) { y_err(r, hn, "hal entry is missing 'call'"); h->call = xstrdup("?"); }
        if (!h->arg) h->arg = xstrdup("u8");
        const char *ok[] = {"u8", "u16", "u32", "i32", NULL};
        int good = 0;
        for (int i = 0; ok[i]; i++) if (!strcmp(ok[i], h->arg)) good = 1;
        if (!good) y_err(r, hn, "hal %s: arg must be u8, u16, u32 or i32", h->call);
    }
}

int model_load(const char *path, Model *m, StrList *errors) {
    memset(m, 0, sizeof *m);
    m->path = xstrdup(path);
    size_t len;
    char *bytes = read_file(path, &len);
    if (!bytes) {
        sl_pushf(errors, "%s: cannot read model", path);
        return -1;
    }
    char hex[65];
    sha256_hex(bytes, len, hex);
    m->hash = xstrdup(hex);
    free(bytes);

    int before = errors->n;
    YReader r;
    if (y_load(&r, path, errors) != 0) { y_free(&r); return -1; }
    yaml_node_t *root = y_root(&r);
    if (!y_is_map(root)) {
        y_err(&r, root, "model must be a mapping at the top level");
        y_free(&r);
        return -1;
    }
    y_check_keys(&r, root, "model", ROOT_KEYS);

    yaml_node_t *mach = y_get(&r, root, "machine");
    if (!y_is_map(mach)) {
        y_err(&r, mach, "missing 'machine: { id, version, language }'");
    } else {
        y_check_keys(&r, mach, "machine", MACHINE_KEYS);
        m->id = y_str(&r, y_get(&r, mach, "id"), "machine id");
        m->version = y_int(&r, y_get(&r, mach, "version"), "version", 1);
        m->language = y_str(&r, y_get(&r, mach, "language"), "language");
    }
    if (!m->id) { y_err(&r, mach, "machine.id is required"); m->id = xstrdup("machine"); }
    if (!m->language) m->language = xstrdup("rust");
    if (strcmp(m->language, "rust") != 0)
        y_err(&r, mach, "language '%s' is not supported yet (only rust)", m->language);

    yaml_node_t *events = y_get(&r, root, "events");
    if (!events) y_err(&r, root, "missing required 'events' list");
    y_strlist(&r, events, "events", &m->events);

    parse_hal(&r, y_get(&r, root, "hal"), m);
    parse_states(&r, y_get(&r, root, "states"), m);
    parse_actions(&r, y_get(&r, root, "actions"), m);
    parse_services(&r, y_get(&r, root, "services"), m);
    parse_invariants(&r, y_get(&r, root, "invariants"), m);
    parse_steps(&r, y_get(&r, root, "steps"), m);

    y_free(&r);
    return errors->n == before ? 0 : -1;
}

State *model_state(const Model *m, const char *id) {
    for (int i = 0; i < m->n_states; i++)
        if (!strcmp(m->states[i].id, id)) return &m->states[i];
    return NULL;
}

Action *model_action(const Model *m, const char *id) {
    for (int i = 0; i < m->n_actions; i++)
        if (!strcmp(m->actions[i].id, id)) return &m->actions[i];
    return NULL;
}

Service *model_service(const Model *m, const char *id) {
    for (int i = 0; i < m->n_services; i++)
        if (!strcmp(m->services[i].id, id)) return &m->services[i];
    return NULL;
}

HalCall *model_hal(const Model *m, const char *call) {
    for (int i = 0; i < m->n_hal; i++)
        if (!strcmp(m->hal[i].call, call)) return &m->hal[i];
    return NULL;
}

int model_event_index(const Model *m, const char *event) {
    for (int i = 0; i < m->events.n; i++)
        if (!strcmp(m->events.v[i], event)) return i;
    return -1;
}

const char *state_target(const State *s, const char *event) {
    for (int i = 0; i < s->n_on; i++)
        if (!strcmp(s->on[i].event, event)) return s->on[i].target;
    return NULL;
}

char *model_find(const char *dir, StrList *errors) {
    DIR *d = opendir(dir);
    if (!d) { sl_pushf(errors, "%s: cannot open directory", dir); return NULL; }
    StrList found = {0};
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.') continue;
        if (n > 5 && !strcmp(e->d_name + n - 5, ".yaml")) sl_push(&found, e->d_name);
    }
    closedir(d);
    if (found.n == 1) return xstrdup(found.v[0]);
    if (found.n == 0)
        sl_pushf(errors, "no model found: put exactly one *.yaml model file in the project root");
    else
        sl_pushf(errors, "found %d *.yaml files in the project root; pass the model path explicitly", found.n);
    return NULL;
}
