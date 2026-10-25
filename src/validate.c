#include "validate.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int is_snake_ident(const char *s) {
    if (!s || !*s || !(islower((unsigned char)*s) || *s == '_')) return 0;
    for (; *s; s++)
        if (!(islower((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '_')) return 0;
    return 1;
}

static int is_upper_snake(const char *s) {
    if (!s || !*s || !isupper((unsigned char)*s)) return 0;
    for (; *s; s++)
        if (!(isupper((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '_')) return 0;
    return 1;
}

static int is_type_ident(const char *s) {
    if (!s || !*s || !isupper((unsigned char)*s)) return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s)) return 0;
    return 1;
}

static int action_used(const Model *m, const char *id) {
    for (int i = 0; i < m->n_states; i++)
        if (sl_contains(&m->states[i].entry, id) || sl_contains(&m->states[i].exit, id)) return 1;
    return 0;
}

static int service_used(const Model *m, const char *id) {
    for (int i = 0; i < m->n_states; i++)
        if (sl_contains(&m->states[i].invoke, id)) return 1;
    return 0;
}

void check_statechart(const Model *m, StrList *e) {
    if (!is_snake_ident(m->id)) sl_pushf(e, "machine id '%s' must be lower_snake_case", m->id);

    if (m->events.n == 0) sl_pushf(e, "model declares no events");
    for (int i = 0; i < m->events.n; i++) {
        if (!is_upper_snake(m->events.v[i])) sl_pushf(e, "event '%s' must be UPPER_SNAKE_CASE", m->events.v[i]);
        for (int j = 0; j < i; j++)
            if (!strcmp(m->events.v[i], m->events.v[j])) sl_pushf(e, "event %s is declared twice", m->events.v[i]);
    }

    for (int i = 0; i < m->n_hal; i++) {
        if (!is_snake_ident(m->hal[i].call)) sl_pushf(e, "hal call '%s' must be lower_snake_case", m->hal[i].call);
        if (!strcmp(m->hal[i].call, "now_ms") || !strcmp(m->hal[i].call, "log"))
            sl_pushf(e, "hal call '%s' is built in; pick another name", m->hal[i].call);
        for (int j = 0; j < i; j++)
            if (!strcmp(m->hal[i].call, m->hal[j].call)) sl_pushf(e, "hal call %s is declared twice", m->hal[i].call);
    }

    if (m->n_states == 0) { sl_pushf(e, "model has no states"); return; }

    char prefix[256];
    snprintf(prefix, sizeof prefix, "%s.", m->id);
    int initials = 0;
    for (int i = 0; i < m->n_states; i++) {
        const State *s = &m->states[i];
        if (strncmp(s->id, prefix, strlen(prefix)) != 0)
            sl_pushf(e, "state id '%s' must start with '%s'", s->id, prefix);
        if (!is_type_ident(s->name))
            sl_pushf(e, "state %s: name '%s' must be CamelCase letters and digits", s->id, s->name);
        for (int j = 0; j < i; j++) {
            if (!strcmp(s->id, m->states[j].id)) sl_pushf(e, "state id %s is used twice", s->id);
            if (!strcmp(s->name, m->states[j].name)) sl_pushf(e, "state name %s is used twice", s->name);
        }
        if (s->initial) initials++;

        for (int t = 0; t < s->n_on; t++) {
            const Transition *tr = &s->on[t];
            if (model_event_index(m, tr->event) < 0)
                sl_pushf(e, "%s: transition on unknown event %s", s->id, tr->event);
            if (!model_state(m, tr->target))
                sl_pushf(e, "%s: %s goes to unknown state %s (dangling transition)", s->id, tr->event, tr->target);
            for (int u = 0; u < t; u++)
                if (!strcmp(s->on[u].event, tr->event)) sl_pushf(e, "%s handles %s twice", s->id, tr->event);
        }
        for (int k = 0; k < s->ignore.n; k++) {
            const char *ev = s->ignore.v[k];
            if (model_event_index(m, ev) < 0) sl_pushf(e, "%s: ignores unknown event %s", s->id, ev);
            if (state_target(s, ev)) sl_pushf(e, "%s both handles and ignores %s", s->id, ev);
        }
        /* Totality: every (state, event) pair is a decision the architect made. */
        for (int k = 0; k < m->events.n; k++) {
            const char *ev = m->events.v[k];
            if (!state_target(s, ev) && !sl_contains(&s->ignore, ev))
                sl_pushf(e, "%s neither handles nor ignores %s (add it to 'on' or 'ignore')", s->id, ev);
        }
        for (int k = 0; k < s->entry.n; k++)
            if (!model_action(m, s->entry.v[k])) sl_pushf(e, "%s: entry action %s is not declared under 'actions'", s->id, s->entry.v[k]);
        for (int k = 0; k < s->exit.n; k++)
            if (!model_action(m, s->exit.v[k])) sl_pushf(e, "%s: exit action %s is not declared under 'actions'", s->id, s->exit.v[k]);
        for (int k = 0; k < s->invoke.n; k++)
            if (!model_service(m, s->invoke.v[k])) sl_pushf(e, "%s: invokes undeclared service %s", s->id, s->invoke.v[k]);
        if (s->has_after) {
            if (model_event_index(m, s->after_event) < 0)
                sl_pushf(e, "%s: after.event %s is not a declared event", s->id, s->after_event);
            else if (!state_target(s, s->after_event))
                sl_pushf(e, "%s: after.event %s is not handled by this state, so the timer would do nothing", s->id, s->after_event);
        }
    }
    if (initials != 1) sl_pushf(e, "exactly one state must be initial (found %d)", initials);

    for (int i = 0; i < m->n_actions; i++) {
        const Action *a = &m->actions[i];
        if (!is_snake_ident(a->id)) sl_pushf(e, "action id '%s' must be lower_snake_case", a->id);
        if (!action_used(m, a->id)) sl_pushf(e, "action %s is declared but no state uses it", a->id);
    }
    for (int i = 0; i < m->n_services; i++) {
        const Service *s = &m->services[i];
        if (!is_snake_ident(s->id)) sl_pushf(e, "service id '%s' must be lower_snake_case", s->id);
        if (!service_used(m, s->id)) sl_pushf(e, "service %s is declared but no state invokes it", s->id);
        for (int k = 0; k < s->emits.n; k++)
            if (model_event_index(m, s->emits.v[k]) < 0) sl_pushf(e, "service %s emits unknown event %s", s->id, s->emits.v[k]);
    }

    int active = 0;
    for (int i = 0; i < m->n_steps; i++) {
        if (m->steps[i].status && !strcmp(m->steps[i].status, "active")) active++;
        for (int j = 0; j < i; j++)
            if (!strcmp(m->steps[i].id, m->steps[j].id)) sl_pushf(e, "step id %s is used twice", m->steps[i].id);
    }
    if (active > 1) sl_pushf(e, "only one step may be active (found %d)", active);

    /* Reachability from the initial state. */
    int *seen = xcalloc((size_t)m->n_states, sizeof(int));
    int *queue = xcalloc((size_t)m->n_states, sizeof(int));
    int qh = 0, qt = 0;
    for (int i = 0; i < m->n_states; i++)
        if (m->states[i].initial) { seen[i] = 1; queue[qt++] = i; break; }
    while (qh < qt) {
        const State *s = &m->states[queue[qh++]];
        for (int t = 0; t < s->n_on; t++) {
            for (int j = 0; j < m->n_states; j++) {
                if (!seen[j] && !strcmp(m->states[j].id, s->on[t].target)) {
                    seen[j] = 1;
                    queue[qt++] = j;
                }
            }
        }
    }
    if (initials == 1)
        for (int i = 0; i < m->n_states; i++)
            if (!seen[i]) sl_pushf(e, "state %s is unreachable from the initial state", m->states[i].id);
}

void check_model_invariants(const Model *m, StrList *e) {
    for (int i = 0; i < m->n_invariants; i++) {
        const Invariant *v = &m->invariants[i];
        for (int j = 0; j < i; j++)
            if (!strcmp(v->id, m->invariants[j].id)) sl_pushf(e, "invariant id %s is used twice", v->id);
        switch (v->kind) {
        case INV_EVERY_STATE_HANDLES:
            if (model_event_index(m, v->event) < 0) { sl_pushf(e, "%s: unknown event %s", v->id, v->event); break; }
            if (!model_state(m, v->target)) { sl_pushf(e, "%s: unknown target state %s", v->id, v->target); break; }
            for (int s = 0; s < m->n_states; s++) {
                const char *t = state_target(&m->states[s], v->event);
                if (!t)
                    sl_pushf(e, "%s violated: %s ignores %s (must go to %s)", v->id, m->states[s].id, v->event, v->target);
                else if (strcmp(t, v->target))
                    sl_pushf(e, "%s violated: %s sends %s to %s (must go to %s)", v->id, m->states[s].id, v->event, t, v->target);
            }
            break;
        case INV_MAX_TIME_IN_STATE: {
            const State *s = model_state(m, v->state);
            if (!s) { sl_pushf(e, "%s: unknown state %s", v->id, v->state); break; }
            const char *t = state_target(s, v->exit_event);
            if (!t) sl_pushf(e, "%s violated: %s does not handle its exit_event %s", v->id, v->state, v->exit_event);
            else if (!strcmp(t, v->state)) sl_pushf(e, "%s violated: exit_event %s keeps %s in the same state", v->id, v->exit_event, v->state);
            break;
        }
        case INV_HAL_CLAMP: {
            const HalCall *h = model_hal(m, v->call);
            if (!h) { sl_pushf(e, "%s: hal call %s is not declared under 'hal'", v->id, v->call); break; }
            long cap = !strcmp(h->arg, "u8") ? 255L : !strcmp(h->arg, "u16") ? 65535L : 2147483647L;
            if (v->max > cap) sl_pushf(e, "%s: max %ld does not fit %s", v->id, v->max, h->arg);
            break;
        }
        case INV_UNKNOWN:
            break;
        }
    }
}

int validate_all(const char *path, Model *m, StrList *e1, StrList *e2, StrList *e3) {
    if (model_load(path, m, e1) != 0) return -1;
    check_statechart(m, e2);
    if (e2->n) return -1;
    check_model_invariants(m, e3);
    return e3->n ? -1 : 0;
}
