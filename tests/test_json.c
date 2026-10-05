// SPDX-License-Identifier: GPL-2.0
/* Unit tests for the wire encoding. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zss_json.h"

static int failures;

#define EXPECT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void roundtrip(void)
{
    struct zj_out o;
    struct zj_msg m;
    char *line;

    zj_begin(&o, "state");
    zj_add_str(&o, "gpu", "0000:01:00.0");
    zj_add_int(&o, "devices", 3);
    zj_add_int(&o, "negative", -42);
    zj_add_bool(&o, "holds", true);
    zj_add_bool(&o, "migratable", false);
    zj_add_str(&o, "reason", "uses \"quotes\", a \\ backslash,\na newline and a\ttab");
    line = zj_end(&o);

    EXPECT(line[strlen(line) - 1] == '\n');
    EXPECT(strchr(line, '\n') == line + strlen(line) - 1); /* exactly one line */
    EXPECT(zj_parse(line, &m) == 0);
    EXPECT(zj_is(&m, "state"));
    EXPECT(!strcmp(zj_str(&m, "gpu", ""), "0000:01:00.0"));
    EXPECT(zj_int(&m, "devices", 0) == 3);
    EXPECT(zj_int(&m, "negative", 0) == -42);
    EXPECT(zj_bool(&m, "holds", false));
    EXPECT(!zj_bool(&m, "migratable", true));
    EXPECT(!strcmp(zj_str(&m, "reason", ""), "uses \"quotes\", a \\ backslash,\na newline and a\ttab"));
    zj_free(&m);
    free(line);
}

static void defaults(void)
{
    struct zj_msg m;

    EXPECT(zj_parse("{\"type\":\"x\",\"n\":7}", &m) == 0);
    EXPECT(!strcmp(zj_str(&m, "missing", "dflt"), "dflt"));
    EXPECT(zj_int(&m, "missing", 9) == 9);
    EXPECT(zj_bool(&m, "missing", true));
    /* Asking for the wrong type yields the default, not garbage. */
    EXPECT(!strcmp(zj_str(&m, "n", "dflt"), "dflt"));
    EXPECT(zj_int(&m, "type", 5) == 5);
    zj_free(&m);

    EXPECT(zj_parse("  { }  ", &m) == 0);
    EXPECT(m.n == 0);
    zj_free(&m);

    EXPECT(zj_parse("{ \"a\" : \"b\" , \"c\" : true }", &m) == 0);
    EXPECT(!strcmp(zj_str(&m, "a", ""), "b"));
    EXPECT(zj_bool(&m, "c", false));
    zj_free(&m);

    EXPECT(zj_parse("{\"a\":\"\\u0041\\u00e9\"}", &m) == 0);
    EXPECT(!strcmp(zj_str(&m, "a", ""), "A?"));
    zj_free(&m);
}

static void malformed(void)
{
    static const char *const bad[] = {
        "", "nonsense", "{", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{\"a\":1 \"b\":2}", "{\"a\":\"unterminated}",
        "[1,2]", "{\"a\":{\"nested\":1}}", "{\"a\":[1]}", "{\"a\":nul}", "{\"a\":\"\\u12\"}",
    };
    struct zj_msg m;

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (zj_parse(bad[i], &m) == 0) {
            fprintf(stderr, "FAIL: accepted malformed input: %s\n", bad[i]);
            failures++;
            zj_free(&m);
        }
    }
}

static void too_many_fields(void)
{
    struct zj_out o;
    struct zj_msg m;
    char key[8], *line;

    zj_begin(&o, "big");
    for (int i = 0; i < ZJ_MAX_FIELDS + 4; i++) {
        snprintf(key, sizeof(key), "k%d", i);
        zj_add_int(&o, key, i);
    }
    line = zj_end(&o);
    EXPECT(zj_parse(line, &m) != 0);
    free(line);
}

int main(void)
{
    roundtrip();
    defaults();
    malformed();
    too_many_fields();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("json: ok");
    return 0;
}
