#include "resources.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const Bus *find_bus(const Bus *b, int n, const char *id) {
    for (int i = 0; i < n; i++)
        if (id && !strcmp(b[i].id, id)) return &b[i];
    return NULL;
}

int model_claims(const SgModel *m, Claim **out) {
    int n = 0;
    for (int i = 0; i < m->n_actions; i++) n += m->actions[i].n_uses;
    for (int i = 0; i < m->n_services; i++) n += m->services[i].n_uses;
    *out = xcalloc((size_t)(n ? n : 1), sizeof(Claim));
    int k = 0;
    for (int i = 0; i < m->n_actions; i++)
        for (int u = 0; u < m->actions[i].n_uses; u++) {
            SB o;
            sb_init(&o);
            sb_printf(&o, "%s.%s", m->id, m->actions[i].id);
            (*out)[k].owner = o.s;
            (*out)[k++].r = &m->actions[i].uses[u];
        }
    for (int i = 0; i < m->n_services; i++)
        for (int u = 0; u < m->services[i].n_uses; u++) {
            SB o;
            sb_init(&o);
            sb_printf(&o, "%s.%s", m->id, m->services[i].id);
            (*out)[k].owner = o.s;
            (*out)[k++].r = &m->services[i].uses[u];
        }
    return n;
}

typedef struct {
    const char *pin;
    char *who;
    const char *owner; /* NULL for a bus line */
} PinUse;

void resources_check(const Bus *buses, int nb, const Claim *c, int nc, StrList *e) {
    static const char *const NEED_I2C[] = {"sda", "scl", NULL};
    static const char *const NEED_SPI[] = {"sck", "mosi", "miso", NULL};
    static const char *const NEED_UART[] = {"tx", "rx", NULL};
    PinUse *pins = xcalloc((size_t)(nb * 3 + nc + 1), sizeof(PinUse));
    int np = 0;

    for (int i = 0; i < nb; i++) {
        const Bus *b = &buses[i];
        for (int k = 0; k < i; k++)
            if (!strcmp(buses[k].id, b->id)) sl_pushf(e, "bus %s is declared twice", b->id);
        const char *const *need = !strcmp(b->comm, "i2c") ? NEED_I2C : !strcmp(b->comm, "spi") ? NEED_SPI
                                : !strcmp(b->comm, "uart") ? NEED_UART : NULL;
        if (!need) { sl_pushf(e, "bus %s: comm '%s' must be i2c, spi or uart", b->id, b->comm); continue; }
        if (b->via) {
            const Bus *p = find_bus(buses, nb, b->via);
            if (!p) sl_pushf(e, "bus %s is behind '%s', which is not a declared bus", b->id, b->via);
            else if (strcmp(p->comm, b->comm)) sl_pushf(e, "bus %s (%s) is behind %s, which is %s", b->id, b->comm, p->id, p->comm);
            if (b->pins.n) sl_pushf(e, "bus %s is behind %s, so it has no pins of its own", b->id, b->via);
            continue;
        }
        for (int k = 0; need[k]; k++)
            if (!sl_contains(&b->roles, need[k])) sl_pushf(e, "bus %s (%s) needs its %s pin", b->id, b->comm, need[k]);
        for (int k = 0; k < b->pins.n; k++) {
            int ok = 0;
            for (int q = 0; need[q]; q++) ok |= !strcmp(need[q], b->roles.v[k]);
            if (!ok) { sl_pushf(e, "bus %s (%s) has no %s pin", b->id, b->comm, b->roles.v[k]); continue; }
            SB w;
            sb_init(&w);
            sb_printf(&w, "bus %s %s", b->id, b->roles.v[k]);
            pins[np++] = (PinUse){b->pins.v[k], w.s, NULL};
        }
    }

    for (int i = 0; i < nc; i++) {
        const Resource *r = c[i].r;
        const char *o = c[i].owner;
        if (!strcmp(r->comm, "gpio")) {
            if (!r->pin) sl_pushf(e, "%s uses gpio but names no pin", o);
            if (r->bus || r->addr >= 0 || r->cs) sl_pushf(e, "%s: a gpio use takes only a pin", o);
            if (r->pin) {
                SB w;
                sb_init(&w);
                sb_printf(&w, "%s (gpio)", o);
                pins[np++] = (PinUse){r->pin, w.s, o};
            }
            continue;
        }
        if (strcmp(r->comm, "i2c") && strcmp(r->comm, "spi") && strcmp(r->comm, "uart")) {
            sl_pushf(e, "%s: comm '%s' must be i2c, spi, uart or gpio", o, r->comm);
            continue;
        }
        if (r->pin) sl_pushf(e, "%s: pin belongs to gpio uses; an %s use names its bus", o, r->comm);
        const Bus *b = r->bus ? find_bus(buses, nb, r->bus) : NULL;
        if (!r->bus) sl_pushf(e, "%s uses %s but names no bus", o, r->comm);
        else if (!b) sl_pushf(e, "%s uses bus %s, which is not declared under 'buses'", o, r->bus);
        else if (strcmp(b->comm, r->comm)) sl_pushf(e, "%s uses %s as %s, but it is an %s bus", o, r->bus, r->comm, b->comm);
        if (!strcmp(r->comm, "i2c")) {
            if (r->addr < 0) sl_pushf(e, "%s uses i2c bus %s but names no addr", o, r->bus ? r->bus : "?");
            else if (r->addr < 0x03 || r->addr > 0x77) sl_pushf(e, "%s: i2c addr 0x%02lx is outside 0x03-0x77", o, r->addr);
            if (r->cs) sl_pushf(e, "%s: cs belongs to spi uses", o);
        } else if (!strcmp(r->comm, "spi")) {
            if (!r->cs) sl_pushf(e, "%s uses spi bus %s but names no cs pin", o, r->bus ? r->bus : "?");
            if (r->addr >= 0) sl_pushf(e, "%s: addr belongs to i2c uses", o);
            if (r->cs) {
                SB w;
                sb_init(&w);
                sb_printf(&w, "%s (spi cs on %s)", o, r->bus ? r->bus : "?");
                pins[np++] = (PinUse){r->cs, w.s, o};
            }
        } else if (r->addr >= 0 || r->cs) {
            sl_pushf(e, "%s: a uart use takes only its bus", o);
        }
        /* Two nodes at one address on one bus. */
        if (!strcmp(r->comm, "i2c") && r->bus && r->addr >= 0)
            for (int k = 0; k < i; k++) {
                const Resource *q = c[k].r;
                if (!strcmp(q->comm, "i2c") && q->bus && !strcmp(q->bus, r->bus) && q->addr == r->addr && strcmp(c[k].owner, o))
                    sl_pushf(e, "address 0x%02lx on bus %s is claimed twice: %s and %s", r->addr, r->bus, c[k].owner, o);
            }
        /* Two nodes driving one uart. */
        if (!strcmp(r->comm, "uart") && r->bus)
            for (int k = 0; k < i; k++)
                if (!strcmp(c[k].r->comm, "uart") && c[k].r->bus && !strcmp(c[k].r->bus, r->bus) && strcmp(c[k].owner, o))
                    sl_pushf(e, "uart %s is claimed twice: %s and %s", r->bus, c[k].owner, o);
    }

    /* One pin, one job. The same node naming a pin twice is fine. */
    for (int i = 0; i < np; i++)
        for (int k = 0; k < i; k++)
            if (!strcmp(pins[i].pin, pins[k].pin) && !(pins[i].owner && pins[k].owner && !strcmp(pins[i].owner, pins[k].owner))) {
                sl_pushf(e, "pin %s is used twice: %s and %s", pins[i].pin, pins[k].who, pins[i].who);
                break;
            }
    for (int i = 0; i < np; i++) free(pins[i].who);
    free(pins);
}
