/* The Unicode Bidirectional Algorithm (src/bidi.c): levels and visual order.
 * In the test strings an upper-case letter stands for a Hebrew letter
 * (class R), and {RLO} etc. for the control characters. */
#include "test_runner.h"
#include "bidi.h"
#include <stdio.h>
#include <string.h>

void run_bidi_tests(void);

static const struct { const char *name; uint32_t cp; } k_ctl[] = {
    { "LRE", 0x202A }, { "RLE", 0x202B }, { "PDF", 0x202C }, { "LRO", 0x202D }, { "RLO", 0x202E },
    { "LRI", 0x2066 }, { "RLI", 0x2067 }, { "FSI", 0x2068 }, { "PDI", 0x2069 },
};

static int parse(const char *s, uint32_t *cps)
{
    int n = 0;
    while (*s) {
        if (*s == '{') {
            const char *e = strchr(s, '}');
            for (size_t i = 0; i < sizeof(k_ctl) / sizeof(k_ctl[0]); i++)
                if ((size_t)(e - s - 1) == strlen(k_ctl[i].name) && !strncmp(s + 1, k_ctl[i].name, (size_t)(e - s - 1)))
                    cps[n++] = k_ctl[i].cp;
            s = e + 1;
        } else if (*s >= 'A' && *s <= 'Z') {
            cps[n++] = 0x05D0 + (uint32_t)(*s++ - 'A');
        } else {
            cps[n++] = (unsigned char)*s++;
        }
    }
    return n;
}

/* The line drawn: base level 0, 1 or -1 (P2 / P3), the whole string one
 * line; controls left out, odd levels mirrored. */
static const char *visual(const char *logical, int base)
{
    static char out[256];
    uint32_t cps[256];
    uint8_t levels[256];
    int order[256];
    int n = parse(logical, cps);
    int para = bidi_resolve(cps, n, base, levels);
    bidi_line_levels(cps, 0, n, para, levels);
    bidi_reorder(levels, n, order);
    int o = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = cps[order[i]];
        if ((c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069)) continue;
        if (levels[order[i]] & 1) c = bidi_mirror(c);
        out[o++] = c >= 0x05D0 && c < 0x05D0 + 26 ? (char)('A' + (c - 0x05D0)) : (char)c;
    }
    out[o] = 0;
    return out;
}

#define EXPECT_VISUAL(logical, base, want) do { \
    const char *got_ = visual(logical, base); \
    if (strcmp(got_, want) != 0) printf("    %s (base %d)\n    got:  %s\n    want: %s\n", logical, base, got_, want); \
    ASSERT_TRUE(strcmp(got_, want) == 0); \
} while (0)

TEST(test_bidi_classes) {
    ASSERT_TRUE(bidi_class('a') == BIDI_L);
    ASSERT_TRUE(bidi_class(0x05D0) == BIDI_R);
    ASSERT_TRUE(bidi_class(0x0627) == BIDI_AL);
    ASSERT_TRUE(bidi_class('1') == BIDI_EN);
    ASSERT_TRUE(bidi_class(0x0661) == BIDI_AN);
    ASSERT_TRUE(bidi_class(' ') == BIDI_WS);
    ASSERT_TRUE(bidi_class('(') == BIDI_ON);
    ASSERT_TRUE(bidi_class(0x202E) == BIDI_RLO);
    ASSERT_TRUE(bidi_class(0x0300) == BIDI_NSM);
    ASSERT_TRUE(bidi_class(0x05FF) == BIDI_R);   /* (unassigned, in the Hebrew block) */
    ASSERT_TRUE(bidi_mirror('(') == ')' && bidi_mirror('a') == 'a' && bidi_mirror(0x00AB) == 0x00BB);
    return true;
}

TEST(test_bidi_reorder) {
    EXPECT_VISUAL("car means CAR.", 0, "car means RAC.");
    EXPECT_VISUAL("car MEANS CAR.", 1, ".RAC SNAEM car");
    /* (P2: "car" first, so left to right; "MEANS CAR" is one run) */
    EXPECT_VISUAL("car MEANS CAR.", -1, "car RAC SNAEM.");
    EXPECT_VISUAL("ABC 123", 0, "123 CBA");
    EXPECT_VISUAL("123 456", 1, "456 123");
    /* (UAX #9's N0 example) */
    EXPECT_VISUAL("AB(CD[&ef]!)gh", 1, "gh(![ef&]DC)BA");
    EXPECT_VISUAL("a(b)c", 1, "a(b)c");
    /* (L1: white space at the line's end at the paragraph level) */
    EXPECT_VISUAL("abc  ", 1, "  abc");
    return true;
}

TEST(test_bidi_explicit) {
    EXPECT_VISUAL("aaa{RLO}ccb{PDF}ddd", 0, "aaabccddd");
    EXPECT_VISUAL("a{RLO}b{LRO}cd{PDF}e{PDF}f", 0, "aecdbf");
    EXPECT_VISUAL("a {RLI}b C{PDI} d", 0, "a C b d");
    EXPECT_VISUAL("{RLE}ab{PDF}", 0, "ab");
    ASSERT_TRUE(bidi_first_strong_level((const uint32_t[]){ 0x2067, 0x05D0, 0x2069, 'b' }, 4) == 0);
    ASSERT_TRUE(bidi_first_strong_level((const uint32_t[]){ ' ', 0x05D0, 'b' }, 3) == 1);
    ASSERT_TRUE(bidi_first_strong_level((const uint32_t[]){ ' ', '1' }, 2) == -1);
    return true;
}

/* BIDI_CHAR_TEST=<path to BidiCharacterTest.txt>: Unicode's conformance
 * cases (code points; direction 0 L, 1 R, 2 auto; paragraph level; levels
 * after L1, x for removed; visual order without the removed), each paragraph
 * one line.  Skipped when not given. */
#include <stdlib.h>
TEST(test_bidi_conformance) {
    const char *path = getenv("BIDI_CHAR_TEST");
    if (!path || !*path) return true;
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("    (no %s)\n", path);
        return true;
    }
    static char line[8192];
    int cases = 0, failed = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char *fields[5];
        char *p = line;
        for (int i = 0; i < 5; i++) {
            fields[i] = p;
            p = strchr(p, ';');
            if (!p && i < 4) break;
            if (p) *p++ = 0;
        }
        if (!p && !fields[4]) continue;
        uint32_t cps[512];
        int n = 0;
        for (char *q = fields[0]; *q && n < 512;) {
            char *e;
            unsigned long v = strtoul(q, &e, 16);
            if (e == q) break;
            cps[n++] = (uint32_t)v;
            q = e;
        }
        int dir = atoi(fields[1]), want_para = atoi(fields[2]);
        uint8_t levels[512];
        int order[512];
        int para = bidi_resolve(cps, n, dir == 2 ? -1 : dir, levels);
        bidi_line_levels(cps, 0, n, para, levels);
        bool ok = para == want_para;
        /* levels (x: removed, not compared) */
        char *q = fields[3];
        bool removed[512];
        for (int i = 0; i < n; i++) {
            while (*q == ' ') q++;
            removed[i] = *q == 'x';
            if (removed[i]) q++;
            else {
                char *e;
                long v = strtol(q, &e, 10);
                if (v != levels[i]) ok = false;
                q = e;
            }
        }
        /* visual order of what is not removed */
        bidi_reorder(levels, n, order);
        q = fields[4];
        for (int i = 0; i < n && ok; i++) {
            if (removed[order[i]]) continue;
            char *e;
            long v = strtol(q, &e, 10);
            if (e == q || v != order[i]) ok = false;
            q = e;
        }
        cases++;
        if (!ok && ++failed <= 5) printf("    conformance failure: %s;%s;%s\n", fields[0], fields[1], fields[2]);
    }
    fclose(f);
    printf("    BidiCharacterTest: %d cases, %d failed\n", cases, failed);
    ASSERT_TRUE(failed == 0);
    return true;
}

void run_bidi_tests(void) {
    printf("\n--- Bidi (UAX #9) tests ---\n");
    RUN_TEST(test_bidi_classes);
    RUN_TEST(test_bidi_reorder);
    RUN_TEST(test_bidi_explicit);
    RUN_TEST(test_bidi_conformance);
}
