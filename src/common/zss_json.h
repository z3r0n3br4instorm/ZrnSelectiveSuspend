/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal JSON for the ZSS wire protocol. Every message is one flat object
 * on one line: string, integer and boolean values only. See docs/protocol.md.
 */
#ifndef ZSS_JSON_H
#define ZSS_JSON_H

#include <stdbool.h>
#include <stddef.h>

#define ZJ_MAX_FIELDS 24

enum zj_type { ZJ_STR, ZJ_INT, ZJ_BOOL };

struct zj_field {
    const char *key;
    enum zj_type type;
    const char *s;
    long long i;
};

struct zj_msg {
    struct zj_field f[ZJ_MAX_FIELDS];
    int n;
    char *buf; /* owns every key and string value */
};

/* Parses one line. Returns 0 on success, -1 on malformed input. */
int zj_parse(const char *line, struct zj_msg *m);
void zj_free(struct zj_msg *m);

const char *zj_str(const struct zj_msg *m, const char *key, const char *def);
long long zj_int(const struct zj_msg *m, const char *key, long long def);
bool zj_bool(const struct zj_msg *m, const char *key, bool def);
bool zj_is(const struct zj_msg *m, const char *type);

struct zj_out {
    char *p;
    size_t len, cap;
};

void zj_begin(struct zj_out *o, const char *type);
void zj_add_str(struct zj_out *o, const char *key, const char *val);
void zj_add_int(struct zj_out *o, const char *key, long long val);
void zj_add_bool(struct zj_out *o, const char *key, bool val);
/* Closes the object and appends '\n'. The caller frees the result. */
char *zj_end(struct zj_out *o);

#endif
