#ifndef CYBER_HYPHEN_H
#define CYBER_HYPHEN_H

/* Dictionary hyphenation (hyphens: auto). */

/* Is there a dictionary for this language tag? */
int hyphen_lang_supported(const char *lang);

/* Where US English lets a word of n ASCII letters break: can[k] set when a hyphen may
 * go before letter k (0 < k < n), before min_before letters and after min_after kept out. */
void hyphen_en_points(const char *word, int n, int min_before, int min_after, unsigned char *can);

#endif
