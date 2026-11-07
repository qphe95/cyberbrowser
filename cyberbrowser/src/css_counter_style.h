/*
 * css_counter_style.h - counter styles (CSS Counter Styles 3): the
 * predefined ones and the page's @counter-style rules, and a counter
 * value's text in one of them.
 */
#ifndef CSS_COUNTER_STYLE_H
#define CSS_COUNTER_STYLE_H

#include <stdbool.h>
#include <stddef.h>

/* The page's @counter-style rules: forgotten for a new set of sheets, then
 * each sheet's text scanned (the later rule of a name wins). */
void cstyle_forget(void);
void cstyle_scan_css(const char *css, size_t len);

/* Value v in style (a counter style's name, or a symbols() function): its
 * representation (counter() and counters()), or the marker's text: the
 * style's prefix, the representation and its suffix (a list item's). */
void cstyle_format(const char *style, long long v, char *out, size_t cap);
void cstyle_marker(const char *style, long long v, char *out, size_t cap);

/* A name some counter style has (list-style-type keeps an unknown one: it
 * is decimal until a rule names it). */
bool cstyle_known(const char *style);

#endif
