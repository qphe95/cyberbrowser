/* Liang's hyphenation: every pattern found in ".word." raises the level at the places
 * it names, and a word may break where the highest level is odd. */
#include "hyphen.h"

#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hyph_en_data.inc"

enum { HY_SLOTS = 1 << 16, HY_MAXPAT = 24 };

/* (a pattern: its letters' hash, where its text starts, how many letters) */
typedef struct { uint32_t hash; uint32_t at; uint16_t len; uint16_t used; } HySlot;

static HySlot *g_hy;
static int g_hy_maxlen;
static pthread_mutex_t g_hy_mu = PTHREAD_MUTEX_INITIALIZER;
static bool g_hy_ready;

static uint32_t hy_hash(const char *s, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static void hy_init(void)
{
    g_hy = (HySlot *)calloc(HY_SLOTS, sizeof(HySlot));
    if (!g_hy) return;
    for (const char *p = k_hyph_en; *p;) {
        const char *e = p;
        char letters[HY_MAXPAT];
        int n = 0;
        while (*e && *e != ' ') {
            if (!isdigit((unsigned char)*e) && n < HY_MAXPAT) letters[n++] = *e;
            e++;
        }
        uint32_t h = hy_hash(letters, n), i = h & (HY_SLOTS - 1);
        while (g_hy[i].used) i = (i + 1) & (HY_SLOTS - 1);
        g_hy[i] = (HySlot){ h, (uint32_t)(p - k_hyph_en), (uint16_t)n, 1 };
        if (n > g_hy_maxlen) g_hy_maxlen = n;
        p = *e ? e + 1 : e;
    }
}

/* (does the pattern at text p have just these letters?) */
static bool hy_same(const char *p, const char *s, int n)
{
    int k = 0;
    for (; *p && *p != ' '; p++) {
        if (isdigit((unsigned char)*p)) continue;
        if (k >= n || *p != s[k]) return false;
        k++;
    }
    return k == n;
}

int hyphen_lang_supported(const char *lang)
{
    if (!lang) return 0;
    return (lang[0] == 'e' || lang[0] == 'E') && (lang[1] == 'n' || lang[1] == 'N') && (!lang[2] || lang[2] == '-' || lang[2] == '_');
}

void hyphen_en_points(const char *word, int n, int min_before, int min_after, unsigned char *can)
{
    memset(can, 0, (size_t)n);
    pthread_mutex_lock(&g_hy_mu);
    if (!g_hy_ready) { hy_init(); g_hy_ready = true; }
    pthread_mutex_unlock(&g_hy_mu);
    if (!g_hy || n < 2 || n > 256) return;
    char w[260];
    unsigned char lv[262];
    w[0] = '.';
    for (int i = 0; i < n; i++) w[i + 1] = (char)tolower((unsigned char)word[i]);
    w[n + 1] = '.';
    int m = n + 2;
    memset(lv, 0, sizeof lv);
    for (int a = 0; a < m; a++)
        for (int l = 1; l <= g_hy_maxlen && a + l <= m; l++) {
            uint32_t h = hy_hash(w + a, l);
            for (uint32_t i = h & (HY_SLOTS - 1); g_hy[i].used; i = (i + 1) & (HY_SLOTS - 1)) {
                if (g_hy[i].hash != h || g_hy[i].len != l || !hy_same(k_hyph_en + g_hy[i].at, w + a, l)) continue;
                /* (a digit stands before the letter it's written before) */
                int k = 0;
                for (const char *p = k_hyph_en + g_hy[i].at; *p && *p != ' '; p++) {
                    if (isdigit((unsigned char)*p)) { if ((unsigned char)(*p - '0') > lv[a + k]) lv[a + k] = (unsigned char)(*p - '0'); }
                    else k++;
                }
                break;
            }
        }
    /* (lv[j]: before w[j], which is letter j - 1) */
    for (int k = 1; k < n; k++)
        if ((lv[k + 1] & 1) && k >= min_before && n - k >= min_after) can[k] = 1;
}
