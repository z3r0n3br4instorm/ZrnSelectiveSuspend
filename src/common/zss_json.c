// SPDX-License-Identifier: GPL-2.0
#include "zss_json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

/* Reads a quoted string at *pp, unescaping into *out. Advances both. */
static int parse_string(const char **pp, char **out)
{
    const char *p = *pp;
    char *o = *out;

    if (*p++ != '"')
        return -1;
    while (*p && *p != '"') {
        if (*p != '\\') {
            *o++ = *p++;
            continue;
        }
        p++;
        switch (*p) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u': {
            unsigned v = 0;
            for (int i = 1; i <= 4; i++) {
                if (!isxdigit((unsigned char)p[i]))
                    return -1;
                v = v * 16 + (unsigned)(isdigit((unsigned char)p[i]) ? p[i] - '0' : (tolower(p[i]) - 'a' + 10));
            }
            p += 4;
            /* The protocol is ASCII; anything else degrades to '?'. */
            *o++ = v < 0x80 ? (char)v : '?';
            break;
        }
        case '\0': return -1;
        default: *o++ = *p; break;
        }
        p++;
    }
    if (*p != '"')
        return -1;
    *o++ = '\0';
    *pp = p + 1;
    *out = o;
    return 0;
}

int zj_parse(const char *line, struct zj_msg *m)
{
    const char *p = skip_ws(line);
    char *o;

    memset(m, 0, sizeof(*m));
    m->buf = malloc(strlen(line) + 1);
    if (!m->buf)
        return -1;
    o = m->buf;

    if (*p++ != '{')
        goto bad;
    p = skip_ws(p);
    if (*p == '}')
        return 0;

    for (;;) {
        struct zj_field *f;

        if (m->n == ZJ_MAX_FIELDS)
            goto bad;
        f = &m->f[m->n];
        p = skip_ws(p);
        f->key = o;
        if (parse_string(&p, &o))
            goto bad;
        p = skip_ws(p);
        if (*p++ != ':')
            goto bad;
        p = skip_ws(p);

        if (*p == '"') {
            f->type = ZJ_STR;
            f->s = o;
            if (parse_string(&p, &o))
                goto bad;
        } else if (!strncmp(p, "true", 4)) {
            f->type = ZJ_BOOL;
            f->i = 1;
            p += 4;
        } else if (!strncmp(p, "false", 5)) {
            f->type = ZJ_BOOL;
            f->i = 0;
            p += 5;
        } else if (*p == '-' || isdigit((unsigned char)*p)) {
            char *end;

            f->type = ZJ_INT;
            f->i = strtoll(p, &end, 10);
            if (end == p)
                goto bad;
            p = end;
        } else {
            goto bad;
        }
        m->n++;

        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}')
            return 0;
        goto bad;
    }
bad:
    zj_free(m);
    return -1;
}

void zj_free(struct zj_msg *m)
{
    free(m->buf);
    memset(m, 0, sizeof(*m));
}

static const struct zj_field *find(const struct zj_msg *m, const char *key)
{
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->f[i].key, key))
            return &m->f[i];
    return NULL;
}

const char *zj_str(const struct zj_msg *m, const char *key, const char *def)
{
    const struct zj_field *f = find(m, key);

    return f && f->type == ZJ_STR ? f->s : def;
}

long long zj_int(const struct zj_msg *m, const char *key, long long def)
{
    const struct zj_field *f = find(m, key);

    return f && f->type != ZJ_STR ? f->i : def;
}

bool zj_bool(const struct zj_msg *m, const char *key, bool def)
{
    const struct zj_field *f = find(m, key);

    return f && f->type != ZJ_STR ? f->i != 0 : def;
}

bool zj_is(const struct zj_msg *m, const char *type)
{
    return !strcmp(zj_str(m, "type", ""), type);
}

static void put(struct zj_out *o, const char *s, size_t n)
{
    if (o->len + n + 1 > o->cap) {
        o->cap = (o->len + n + 1) * 2;
        o->p = realloc(o->p, o->cap);
        if (!o->p)
            abort();
    }
    memcpy(o->p + o->len, s, n);
    o->len += n;
    o->p[o->len] = '\0';
}

static void put_quoted(struct zj_out *o, const char *s)
{
    put(o, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc[8];

        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            put(o, esc, 2);
        } else if (c == '\n') {
            put(o, "\\n", 2);
        } else if (c == '\t') {
            put(o, "\\t", 2);
        } else if (c < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            put(o, esc, 6);
        } else {
            put(o, (const char *)&c, 1);
        }
    }
    put(o, "\"", 1);
}

static void put_key(struct zj_out *o, const char *key)
{
    if (o->len > 1)
        put(o, ",", 1);
    put_quoted(o, key);
    put(o, ":", 1);
}

void zj_begin(struct zj_out *o, const char *type)
{
    memset(o, 0, sizeof(*o));
    put(o, "{", 1);
    zj_add_str(o, "type", type);
}

void zj_add_str(struct zj_out *o, const char *key, const char *val)
{
    put_key(o, key);
    put_quoted(o, val ? val : "");
}

void zj_add_int(struct zj_out *o, const char *key, long long val)
{
    char num[32];

    put_key(o, key);
    put(o, num, (size_t)snprintf(num, sizeof(num), "%lld", val));
}

void zj_add_bool(struct zj_out *o, const char *key, bool val)
{
    put_key(o, key);
    put(o, val ? "true" : "false", val ? 4 : 5);
}

char *zj_end(struct zj_out *o)
{
    put(o, "}\n", 2);
    return o->p;
}
