#include "session_state.h"
#include <string.h>
#include <stdbool.h>

char g_visitor_data[256] = {0};

void session_set_visitor_data(const char *value) {
    if (!value) {
        g_visitor_data[0] = '\0';
        return;
    }
    size_t len = strlen(value);
    if (len >= sizeof(g_visitor_data)) len = sizeof(g_visitor_data) - 1;
    memcpy(g_visitor_data, value, len);
    g_visitor_data[len] = '\0';
}

bool session_extract_cookie_value(const char *cookies, const char *name,
                                  char *out, size_t out_len) {
    if (!cookies || !name || !out || out_len == 0) return false;
    size_t name_len = strlen(name);
    const char *p = cookies;
    while (*p) {
        /* skip leading whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            p += name_len + 1;
            const char *end = p;
            while (*end && *end != ';') end++;
            size_t len = end - p;
            if (len >= out_len) len = out_len - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            return true;
        }
        /* skip to next cookie */
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
    }
    return false;
}

bool session_extract_json_field(const char *text, const char *field_name,
                                char *out, size_t out_len) {
    if (!text || !field_name || !out || out_len == 0) return false;
    size_t fn_len = strlen(field_name);
    const char *p = text;
    while ((p = strstr(p, field_name)) != NULL) {
        p += fn_len;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        char quote = 0;
        if (*p == '"' || *p == '\'') {
            quote = *p++;
        }
        const char *end = p;
        if (quote) {
            while (*end && *end != quote) end++;
        } else {
            while (*end && *end != ',' && *end != '}' && *end != ' ' && *end != '\t') end++;
        }
        size_t len = end - p;
        if (len >= out_len) len = out_len - 1;
        memcpy(out, p, len);
        out[len] = '\0';
        return true;
    }
    return false;
}
