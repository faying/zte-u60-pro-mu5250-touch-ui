/*
 * net_view.c - parse zwrt-datad's /v2/screen "net" (see net_view.h). No
 * rules here any more: they are in datad's screen.rs.
 *
 * SPDX-License-Identifier: MIT
 */
#include "net_view.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void str(const char *obj, const char *key, char *out, size_t cap)
{
    if (!json_get(obj, key, out, cap) || !strcmp(out, "null")) out[0] = 0;
}

static int flag(const char *obj, const char *key)
{
    char v[8];
    return json_get(obj, key, v, sizeof v) && !strcmp(v, "true");
}

static ui_net_tone_t tone(const char *obj, const char *key)
{
    char v[12];
    if (!json_get(obj, key, v, sizeof v)) return UI_NET_NEUTRAL;
    if (!strcmp(v, "ok"))   return UI_NET_OK;
    if (!strcmp(v, "warn")) return UI_NET_WARN;
    if (!strcmp(v, "bad"))  return UI_NET_BAD;
    return UI_NET_NEUTRAL;
}

static ui_net_cause_t cause(const char *obj)
{
    static const char *const names[] = { "none", "limit", "weak", "noise", "crowd", "narrow" };
    char v[12];
    if (!json_get(obj, "cause", v, sizeof v)) return UI_CAUSE_NONE;
    for (int i = 0; i < (int)(sizeof names / sizeof *names); i++)
        if (!strcmp(v, names[i])) return (ui_net_cause_t)i;
    return UI_CAUSE_NONE;
}

static void carrier(const char *o, nv_carrier_t *c)
{
    char kind[8];
    memset(c, 0, sizeof *c);
    str(o, "kind", kind, sizeof kind);
    c->kind = !strcmp(kind, "nr") ? 'n' : 'B';
    c->band = (int)json_get_int(o, "band", 0);
    c->pci = (int)json_get_int(o, "pci", 0);
    c->bw = (int)json_get_int(o, "bw", 0);
    c->active = flag(o, "active");
    c->arfcn = json_get_int(o, "arfcn", 0);
    str(o, "rsrp", c->rsrp, sizeof c->rsrp);
    str(o, "rsrq", c->rsrq, sizeof c->rsrq);
    str(o, "sinr", c->sinr, sizeof c->sinr);
    str(o, "label", c->label, sizeof c->label);
    str(o, "label_short", c->label_short, sizeof c->label_short);
    c->sinr_tone = tone(o, "sinr_tone");
}

int net_view_parse(const char *net, net_view_t *v)
{
    static char arr[4096], item[768], st[1024];
    const char *p;

    memset(v, 0, sizeof *v);
    v->bars_tier = -1;
    if (!net || !json_get(net, "story", st, sizeof st)) return 0;

    if (json_get(net, "carriers", arr, sizeof arr) && arr[0] == '[')
        for (p = arr; v->ca_n < NV_CA_MAX && (p = json_arr_next(p, item, sizeof item)) != NULL; )
            if (item[0] == '{') carrier(item, &v->ca[v->ca_n++]);
    v->act_n = (int)json_get_int(net, "act_n", 0);
    v->roam_known = flag(net, "roam_known");
    v->roam = flag(net, "roam");
    v->nosvc = flag(net, "nosvc");
    v->sim_usable = flag(net, "sim_usable");
    v->other = flag(net, "other");
    str(net, "logo", v->logo, sizeof v->logo);
    str(net, "fine", v->fine, sizeof v->fine);
    str(net, "name", v->name, sizeof v->name);
    str(net, "where", v->where, sizeof v->where);
    str(net, "ca_val", v->ca_val, sizeof v->ca_val);
    str(net, "ca_sub", v->ca_sub, sizeof v->ca_sub);
    v->bars_tier = (int)json_get_int(net, "bars_tier", -1);
    str(net, "mode_word", v->mode_word, sizeof v->mode_word);
    v->mode_auto = flag(net, "mode_auto");

    ui_net_story_t *s = &v->story;
    s->tone = tone(st, "tone");
    s->cause = cause(st);
    s->sig_tone = tone(st, "sig_tone");
    s->noise_tone = tone(st, "noise_tone");
    str(st, "headline", s->headline, sizeof s->headline);
    str(st, "hint", s->hint, sizeof s->hint);
    str(st, "rat", s->rat, sizeof s->rat);
    str(st, "link", s->link, sizeof s->link);
    str(st, "sig", s->sig, sizeof s->sig);
    str(st, "noise", s->noise, sizeof s->noise);
    str(st, "load", s->load, sizeof s->load);
    str(st, "limit", s->limit, sizeof s->limit);
    return s->headline[0] != 0;
}

void net_view_placeholder(net_view_t *v, const char *headline, const char *why)
{
    memset(v, 0, sizeof *v);
    v->bars_tier = -1;
    v->sim_usable = 1;
    v->story.tone = UI_NET_NEUTRAL;
    snprintf(v->story.headline, sizeof v->story.headline, "%s", headline ? headline : "");
    /* the card's top line (operator · RAT · roaming) has nothing to say: say why */
    snprintf(v->name, sizeof v->name, "%s", why ? why : "");
    snprintf(v->ca_val, sizeof v->ca_val, "—");
    snprintf(v->mode_word, sizeof v->mode_word, "-");
}

int nv_parse_ca(const char *s, nv_carrier_t *out, int max, char kind)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", s);
    int n = 0;
    char *save = NULL;
    for (char *rec = strtok_r(buf, ";", &save); rec && n < max; rec = strtok_r(NULL, ";", &save)) {
        double idx, pci, unk1, band, arfcn, bw, unk2, rsrp, rsrq, sinr, rssi;
        int nf = sscanf(rec, "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf",
                        &idx, &pci, &unk1, &band, &arfcn, &bw, &unk2,
                        &rsrp, &rsrq, &sinr, &rssi);
        (void)unk1; (void)unk2; (void)rsrq; (void)sinr; (void)rssi;
        memset(&out[n], 0, sizeof out[n]);
        out[n].kind = kind;
        if (nf == 5) {          /* 5-field lteca "PCI,band,?,EARFCN,bw" (B27) */
            out[n].pci = (int)idx;
            out[n].band = (int)pci;
            out[n].arfcn = (long)band;
            out[n].bw = (int)arfcn;
            out[n].active = 1;
            n++;
            continue;
        }
        if (nf != 11)
            continue;
        out[n].pci = (int)pci;
        out[n].band = (int)band;
        out[n].arfcn = (long)arfcn;
        out[n].bw = (int)bw;
        out[n].active = rsrp > -140.0;
        n++;
    }
    return n;
}
