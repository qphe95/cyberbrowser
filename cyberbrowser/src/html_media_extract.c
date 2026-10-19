#include "cyber_env.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include "html_media_extract.h"
#include "dom_fetch.h"
#include "html_dom.h"
#include "http_download.h"
#include "js_quickjs.h"
#include "platform.h"
#include "url_utils.h"
#include "cyber_profile.h"

extern const char *g_cyber_start_url;

/* Return the origin (scheme://host) of g_cyber_start_url, falling back to
 * https://localhost when no start URL is available. */
static const char *cyber_get_origin_base(void) {
    static char origin[1024] = {0};
    static int initialized = 0;
    if (!initialized) {
        const char *start = g_cyber_start_url && g_cyber_start_url[0] ? g_cyber_start_url : "https://localhost/";
        const char *scheme_end = strstr(start, "://");
        const char *path_start = scheme_end ? strchr(scheme_end + 3, '/') : NULL;
        size_t len = path_start ? (size_t)(path_start - start) : strlen(start);
        if (len >= sizeof(origin)) len = sizeof(origin) - 1;
        memcpy(origin, start, len);
        origin[len] = '\0';
        if (len == 0) {
            strncpy(origin, "https://localhost", sizeof(origin) - 1);
            origin[sizeof(origin) - 1] = '\0';
        }
        initialized = 1;
    }
    return origin;
}

/* Logging wrapper that uses platform abstraction */
static void log_to_file(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    platform_vlog(LOG_LEVEL_INFO, tag, fmt, args);
    va_end(args);
}

// HTML Entity decoding helper - converts HTML entities to actual characters
// Handles: &lt; &gt; &amp; &quot; &apos; &#123; (decimal) &#x7B; (hex)
static int decode_html_entity(const char *input, char *output, size_t output_len) {
    if (!input || !output || output_len == 0) return 0;
    
    const char *p = input;
    char *out = output;
    size_t remaining = output_len - 1;  // Reserve space for null terminator
    
    while (*p && remaining > 0) {
        if (*p == '&') {
            const char *end = strchr(p, ';');
            if (end && end - p < 20) {  // Reasonable entity length
                size_t entity_len = end - p - 1;  // Length without '&' and ';'
                const char *entity = p + 1;
                char decoded = 0;
                int valid_entity = 0;
                
                // Named entities
                if (strncmp(entity, "lt", entity_len) == 0 && entity_len == 2) {
                    decoded = '<';
                    valid_entity = 1;
                } else if (strncmp(entity, "gt", entity_len) == 0 && entity_len == 2) {
                    decoded = '>';
                    valid_entity = 1;
                } else if (strncmp(entity, "amp", entity_len) == 0 && entity_len == 3) {
                    decoded = '&';
                    valid_entity = 1;
                } else if (strncmp(entity, "quot", entity_len) == 0 && entity_len == 4) {
                    decoded = '"';
                    valid_entity = 1;
                } else if (strncmp(entity, "apos", entity_len) == 0 && entity_len == 4) {
                    decoded = '\'';
                    valid_entity = 1;
                } else if (strncmp(entity, "nbsp", entity_len) == 0 && entity_len == 4) {
                    decoded = ' ';
                    valid_entity = 1;
                }
                // Numeric entities: &#123; (decimal)
                else if (*entity == '#' && entity_len > 1) {
                    const char *num_start = entity + 1;
                    if (*num_start == 'x' || *num_start == 'X') {
                        // Hex entity: &#x3b; or &#x7B;
                        long val = strtol(num_start + 1, NULL, 16);
                        if (val > 0 && val <= 0xFF) {
                            decoded = (char)val;
                            valid_entity = 1;
                        }
                    } else {
                        // Decimal entity: &#59;
                        long val = strtol(num_start, NULL, 10);
                        if (val > 0 && val <= 0xFF) {
                            decoded = (char)val;
                            valid_entity = 1;
                        }
                    }
                }
                
                if (valid_entity) {
                    *out++ = decoded;
                    remaining--;
                    p = end + 1;  // Skip past the entity
                    continue;
                }
            }
        }
        
        // Not an entity or entity too long, copy as-is
        *out++ = *p++;
        remaining--;
    }
    
    *out = '\0';
    return (int)(out - output);
}

// Decode hex-escaped content (\x3b -> ;)
// Handles \xNN format escape sequences commonly found in obfuscated JSON
static int decode_hex_escapes(const char *input, char *output, size_t output_len) {
    if (!input || !output || output_len == 0) return 0;
    
    const char *p = input;
    char *out = output;
    size_t remaining = output_len - 1;
    
    while (*p && remaining > 0) {
        // Check for \xNN pattern (hex escape sequence)
        if (*p == '\\' && *(p + 1) == 'x' && 
            isxdigit((unsigned char)*(p + 2)) && 
            isxdigit((unsigned char)*(p + 3))) {
            // Decode hex value
            int val1 = tolower((unsigned char)*(p + 2));
            int val2 = tolower((unsigned char)*(p + 3));
            int hex_val = ((val1 >= 'a' ? val1 - 'a' + 10 : val1 - '0') << 4) |
                          (val2 >= 'a' ? val2 - 'a' + 10 : val2 - '0');
            
            // Accept any valid byte value (0x00-0xFF) that's not null
            // This includes all printable ASCII, common symbols like = ; & %, etc.
            if (hex_val != 0) {
                *out++ = (char)hex_val;
                remaining--;
                p += 4;  // Skip entire \xNN sequence
                continue;
            }
        }
        
        // Regular character copy
        *out++ = *p++;
        remaining--;
    }
    
    *out = '\0';
    return (int)(out - output);
}

// Full HTML unescape - combines entity and hex decoding
static char* html_unescape(const char *input, size_t input_len) {
    if (!input || input_len == 0) return NULL;
    
    // Allocate output buffer (same size as input, will be smaller or equal)
    char *output = (char*)malloc(input_len + 1);
    if (!output) return NULL;
    
    // First pass: decode HTML entities
    char *temp = (char*)malloc(input_len + 1);
    if (!temp) {
        free(output);
        return NULL;
    }
    
    decode_html_entity(input, temp, input_len + 1);
    
    // Second pass: decode hex escapes
    decode_hex_escapes(temp, output, input_len + 1);
    
    free(temp);
    return output;
}

// UTF-8 validation and repair
// Fixes common UTF-8 encoding issues like truncated sequences or invalid bytes
static char* repair_utf8(const char *input, size_t input_len) {
    if (!input || input_len == 0) return NULL;
    
    char *output = (char*)malloc(input_len + 1);
    if (!output) return NULL;
    
    const uint8_t *p = (const uint8_t *)input;
    char *out = output;
    size_t remaining = input_len;
    
    while (remaining > 0) {
        uint8_t c = *p;
        
        // Single-byte ASCII (0x00-0x7F)
        if ((c & 0x80) == 0) {
            *out++ = c;
            p++;
            remaining--;
        }
        // Two-byte sequence (0xC2-0xDF, 0x80-0xBF)
        else if ((c & 0xE0) == 0xC0) {
            if (remaining >= 2 && (p[1] & 0xC0) == 0x80) {
                // Valid 2-byte sequence
                *out++ = c;
                *out++ = p[1];
                p += 2;
                remaining -= 2;
            } else {
                // Truncated or invalid, skip
                p++;
                remaining--;
            }
        }
        // Three-byte sequence (0xE0-0xEF, 0x80-0xBF, 0x80-0xBF)
        else if ((c & 0xF0) == 0xE0) {
            if (remaining >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
                // Valid 3-byte sequence
                *out++ = c;
                *out++ = p[1];
                *out++ = p[2];
                p += 3;
                remaining -= 3;
            } else {
                // Truncated or invalid, skip
                p++;
                remaining--;
            }
        }
        // Four-byte sequence (0xF0-0xF4, 0x80-0xBF, 0x80-0xBF, 0x80-0xBF)
        else if ((c & 0xF8) == 0xF0) {
            if (remaining >= 4 && (p[1] & 0xC0) == 0x80 && 
                (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
                // Valid 4-byte sequence
                *out++ = c;
                *out++ = p[1];
                *out++ = p[2];
                *out++ = p[3];
                p += 4;
                remaining -= 4;
            } else {
                // Truncated or invalid, skip
                p++;
                remaining--;
            }
        }
        // Invalid byte (continuation byte without start, or invalid start byte)
        else {
            // Skip invalid byte
            p++;
            remaining--;
        }
    }
    
    *out = '\0';
    return output;
}

// Clean and decode extracted JSON content
// Handles all three issues: HTML entities, hex escapes, and UTF-8 issues
static char* clean_json_content(const char *input, size_t input_len) {
    if (!input || input_len == 0) return NULL;
    
    // Step 1: Decode HTML entities and hex escapes
    char *decoded = html_unescape(input, input_len);
    if (!decoded) return NULL;
    
    // Step 2: Repair UTF-8 sequences
    size_t decoded_len = strlen(decoded);
    char *repaired = repair_utf8(decoded, decoded_len);
    free(decoded);
    
    return repaired;
}

#define LOG_TAG "html_extract"
#define LOG_INFO(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOG_ERROR(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)
#define LOG_WARN(...) platform_log(LOG_LEVEL_WARN, LOG_TAG, __VA_ARGS__)

#define MAX_SCRIPT_URLS 32
#define SCRIPT_URL_MAX_LEN 512
#define MAX_SCRIPTS 64  // Total scripts (external + inline)
#define MAX_HTML_SIZE (20 * 1024 * 1024)  // 20MB max for large pages with big JSON payloads

// Script types
typedef enum {
    SCRIPT_TYPE_EXTERNAL,
    SCRIPT_TYPE_INLINE,
    SCRIPT_TYPE_JSON_LD    // application/ld+json structured data
} ScriptType;

// Script info with parse order tracking
typedef struct {
    int parse_order;           // Order in which script appears in HTML (0 = first)
    ScriptType type;           // External, inline, or JSON-LD
    char mime_type[64];        // MIME type from type attribute (e.g., "application/ld+json")
    char url[SCRIPT_URL_MAX_LEN];  // For external scripts: URL to fetch
    char *content;             // For inline scripts: content; for external: fetched content
    size_t content_len;        // Length of content
} ScriptInfo;


static char *url_normalize(const char *base, const char *rel, char *out, size_t out_len) {
    if (!rel || !out || out_len == 0) return NULL;
    
    // Already absolute (including data:, blob:, javascript:, etc.)
    if (url_has_scheme(rel)) {
        strncpy(out, rel, out_len - 1);
        out[out_len - 1] = '\0';
        return out;
    }
    
    // Protocol-relative
    if (strncmp(rel, "//", 2) == 0) {
        snprintf(out, out_len, "https:%s", rel);
        return out;
    }
    
    // Relative to base
    if (base) {
        const char *base_end = base;
        const char *last_slash = strrchr(base, '/');
        if (last_slash) {
            size_t base_len = last_slash - base + 1;
            snprintf(out, out_len, "%.*s%s", (int)base_len, base, rel);
        } else {
            snprintf(out, out_len, "%s/%s", base, rel);
        }
        return out;
    }
    
    strncpy(out, rel, out_len - 1);
    out[out_len - 1] = '\0';
    return out;
}

// URL decode helper



static bool script_tag_name_at(const char *p, const char *name, size_t n) {
    /* 'name' (lower case) followed by whitespace, '/' or '>' */
    if (strncasecmp(p, name, n) != 0) return false;
    char c = p[n];
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ' || c == '/' || c == '>';
}

// Find the end of a <script> element's text, per the HTML tokenizer's
// script data states (https://html.spec.whatwg.org/#script-data-state):
// the text ends at the first "</script" followed by whitespace, '/' or '>',
// except inside "<!-- ... <script> ... </script>" (the double-escaped
// state).  JavaScript strings and comments are NOT special: a browser ends
// the script at "</script>" even inside a JS string.  Returns a pointer to
// the '<' of the end tag, or NULL.
static const char* find_script_end(const char *content_start) {
    if (!content_start) return NULL;
    enum { SD_DATA, SD_ESCAPED, SD_DOUBLE_ESCAPED } st = SD_DATA;
    const char *p = content_start;
    while ((p = strpbrk(p, "<-")) != NULL) {
        if (*p == '-') {
            if (st != SD_DATA && p[1] == '-' && p[2] == '>') {
                st = SD_DATA;
                p += 3;
            } else {
                p++;
            }
            continue;
        }
        /* *p == '<' */
        if (p[1] == '/' && script_tag_name_at(p + 2, "script", 6)) {
            if (st != SD_DOUBLE_ESCAPED) return p;
            st = SD_ESCAPED;
            p += 8;
        } else if (st == SD_DATA && p[1] == '!' && p[2] == '-' && p[3] == '-') {
            st = SD_ESCAPED;
            p += 2;    /* leave "--" so "<!-->" sees its "-->" */
        } else if (st == SD_ESCAPED && script_tag_name_at(p + 1, "script", 6)) {
            st = SD_DOUBLE_ESCAPED;
            p += 7;
        } else {
            p++;
        }
    }
    return NULL;  // No closing tag found
}

// Position just past the '>' of the end tag found by find_script_end().
static const char* script_close_end(const char *end_tag) {
    const char *gt = strchr(end_tag, '>');
    return gt ? gt + 1 : end_tag + strlen(end_tag);
}

// Extract inline scripts from HTML (scripts without src attribute)
// These contain initialization code for page config/data payloads.
int html_extract_inline_scripts(const char *html, char **out_scripts, int max_scripts) {
    if (!html || !out_scripts || max_scripts <= 0) return 0;
    
    int count = 0;
    const char *p = html;
    
    while ((p = strstr(p, "<script")) != NULL && count < max_scripts) {
        // Find end of opening tag properly (handling quotes)
        const char *tag_start = p;
        const char *tag_end = tag_start + 7; // Skip "<script"
        bool in_quote = false;
        char quote_char = 0;
        
        while (*tag_end) {
            if (!in_quote) {
                if (*tag_end == '"' || *tag_end == '\'') {
                    in_quote = true;
                    quote_char = *tag_end;
                } else if (*tag_end == '>') {
                    break;
                }
            } else {
                if (*tag_end == quote_char) {
                    in_quote = false;
                }
            }
            tag_end++;
        }
        
        if (*tag_end != '>') break; // No closing bracket found
        
        // Check for src= in the tag (must be outside quotes)
        bool has_src = false;
        const char *check = tag_start;
        in_quote = false;
        quote_char = 0;
        
        while (check < tag_end) {
            if (!in_quote) {
                if (*check == '"' || *check == '\'') {
                    in_quote = true;
                    quote_char = *check;
                } else if (strncmp(check, "src=", 4) == 0) {
                    has_src = true;
                    break;
                }
            } else {
                if (*check == quote_char) {
                    in_quote = false;
                }
            }
            check++;
        }
        
        if (has_src) {
            // This is an external script, skip
            p = tag_end + 1;
            continue;
        }
        
        // Check for type="text/javascript" or no type (default)
        bool is_js = true;
        const char *type_attr = tag_start;
        while ((type_attr = strstr(type_attr, "type=")) != NULL && type_attr < tag_end) {
            // Check if this type= is inside quotes
            bool type_in_quote = false;
            char type_quote_char = 0;
            for (const char *c = tag_start; c < type_attr; c++) {
                if (!type_in_quote) {
                    if (*c == '"' || *c == '\'') {
                        type_in_quote = true;
                        type_quote_char = *c;
                    }
                } else {
                    if (*c == type_quote_char) {
                        type_in_quote = false;
                    }
                }
            }
            
            if (!type_in_quote) {
                // Check if it's JavaScript type
                const char *type_val = type_attr + 5;
                while (*type_val && isspace((unsigned char)*type_val)) type_val++;
                char quote = *type_val;
                if (quote == '"' || quote == '\'') {
                    type_val++;
                    if (strncmp(type_val, "text/javascript", 15) != 0 &&
                        strncmp(type_val, "application/javascript", 22) != 0 &&
                        strncmp(type_val, "module", 6) != 0) {
                        // Not JavaScript, skip
                        is_js = false;
                        break;
                    }
                }
            }
            type_attr++;
        }
        
        if (!is_js) {
            p = tag_end + 1;
            continue;
        }
        
        // Find the closing </script> tag using robust parser (handles strings)
        const char *content_start = tag_end + 1;
        const char *script_end = find_script_end(content_start);
        
        if (!script_end) {
            LOG_WARN("No closing </script> tag found");
            break;
        }
        
        size_t content_len = script_end - content_start;
        
        // Skip empty scripts or very short ones
        if (content_len < 50) {
            p = script_close_end(script_end);
            continue;
        }
        
        // Extract the script content with proper handling for large scripts
        char *script = (char*)malloc(content_len + 1);
        if (script) {
            memcpy(script, content_start, content_len);
            script[content_len] = '\0';
            
            // Only keep script if it has meaningful content
            if (content_len > 50) {
                out_scripts[count] = script;
                LOG_INFO("Extracted inline script %d: %zu bytes", count, content_len);
                count++;
            } else {
                free(script);
            }
        } else {
            LOG_ERROR("Failed to allocate %zu bytes for script", content_len + 1);
        }
        
        p = script_close_end(script_end);
    }
    
    LOG_INFO("Extracted %d inline initialization scripts", count);
    return count;
}

// Free script info array
static void free_script_infos(ScriptInfo *scripts, int count) {
    for (int i = 0; i < count; i++) {
        if (scripts[i].content) {
            free(scripts[i].content);
            scripts[i].content = NULL;
        }
    }
}

// Comparison function for qsort - sort by parse_order
static int compare_script_info(const void *a, const void *b) {
    const ScriptInfo *sa = (const ScriptInfo *)a;
    const ScriptInfo *sb = (const ScriptInfo *)b;
    return sa->parse_order - sb->parse_order;
}

// Extract all scripts (both external and inline) in parse order
// Returns number of scripts found, fills the scripts array
static int extract_scripts_in_order(const char *html, ScriptInfo *scripts, int max_scripts) {
    if (!html || !scripts || max_scripts <= 0) return 0;
    
    int count = 0;
    int parse_order = 0;
    const char *p = html;
    
    while ((p = strstr(p, "<script")) != NULL && count < max_scripts) {
        const char *tag_start = p;
        const char *tag_end = tag_start + 7; // Skip "<script"
        
        // Find end of opening tag properly (handling quotes)
        bool in_quote = false;
        char quote_char = 0;
        while (*tag_end) {
            if (!in_quote) {
                if (*tag_end == '"' || *tag_end == '\'') {
                    in_quote = true;
                    quote_char = *tag_end;
                } else if (*tag_end == '>') {
                    break;
                }
            } else {
                if (*tag_end == quote_char) {
                    in_quote = false;
                }
            }
            tag_end++;
        }
        
        if (*tag_end != '>') break; // No closing bracket found
        
        // Check for type attribute - must be JavaScript or module
        bool is_js = true;
        bool is_json_ld = false;
        char mime_type[64] = "text/javascript";  // Default MIME type
        bool has_src = false;
        const char *src_start = NULL;
        size_t src_len = 0;
        
        // Parse attributes within the tag
        const char *attr = tag_start + 7;  // After "<script"
        while (attr < tag_end) {
            // Skip whitespace
            while (attr < tag_end && isspace((unsigned char)*attr)) attr++;
            if (attr >= tag_end) break;
            
            // Check for src attribute
            if (strncasecmp(attr, "src=", 4) == 0) {
                has_src = true;
                attr += 4;
                while (attr < tag_end && isspace((unsigned char)*attr)) attr++;
                if (attr < tag_end) {
                    char quote = *attr;
                    if (quote == '"' || quote == '\'') {
                        attr++;  // Skip quote
                        src_start = attr;
                        const char *end = strchr(attr, quote);
                        if (end && end < tag_end) {
                            src_len = end - attr;
                            attr = end + 1;
                        }
                    } else {
                        // Unquoted src
                        src_start = attr;
                        while (attr < tag_end && !isspace((unsigned char)*attr)) attr++;
                        src_len = attr - src_start;
                    }
                }
                continue;
            }
            
            // Check for type attribute
            if (strncasecmp(attr, "type=", 5) == 0) {
                attr += 5;
                while (attr < tag_end && isspace((unsigned char)*attr)) attr++;
                if (attr < tag_end) {
                    char quote = *attr;
                    if (quote == '"' || quote == '\'') {
                        attr++;
                        const char *type_val = attr;
                        const char *end = strchr(attr, quote);
                        if (end && end < tag_end) {
                            size_t type_len = end - type_val;
                            // Store MIME type
                            if (type_len > 0 && type_len < sizeof(mime_type)) {
                                strncpy(mime_type, type_val, type_len);
                                mime_type[type_len] = '\0';
                            }
                            // Check if it's a valid JS type
                            if (type_len > 0 &&
                                strncasecmp(type_val, "text/javascript", 15) != 0 &&
                                strncasecmp(type_val, "application/javascript", 22) != 0 &&
                                strncasecmp(type_val, "module", 6) != 0) {
                                is_js = false;
                                // Check if it's JSON-LD
                                if (strncasecmp(type_val, "application/ld+json", 19) == 0) {
                                    is_json_ld = true;
                                }
                            }
                            attr = end + 1;
                        }
                    }
                }
                continue;
            }
            
            // Skip to next attribute
            attr++;
        }
        
        if (!is_js) {
            p = tag_end + 1;
            continue;
        }
        
        if (has_src && src_start && src_len > 0 && src_len < SCRIPT_URL_MAX_LEN) {
            // External script
            strncpy(scripts[count].url, src_start, src_len);
            scripts[count].url[src_len] = '\0';
            // HTML attribute values are entity-encoded (e.g. &amp; in query
            // strings); decode before resolving, like the DOM parser does.
            html_decode_entities(scripts[count].url, src_len);
            
            // Convert relative to absolute URL
            if (strncmp(scripts[count].url, "//", 2) == 0) {
                char temp[SCRIPT_URL_MAX_LEN];
                snprintf(temp, sizeof(temp), "https:%s", scripts[count].url);
                strcpy(scripts[count].url, temp);
            } else if (scripts[count].url[0] == '/') {
                char temp[SCRIPT_URL_MAX_LEN];
                snprintf(temp, sizeof(temp), "%s%s", cyber_get_origin_base(), scripts[count].url);
                strcpy(scripts[count].url, temp);
            } else if (strncmp(scripts[count].url, "http", 4) != 0) {
                // Skip non-HTTP URLs
                p = tag_end + 1;
                continue;
            }
            
            scripts[count].parse_order = parse_order++;
            scripts[count].type = SCRIPT_TYPE_EXTERNAL;
            strncpy(scripts[count].mime_type, mime_type, sizeof(scripts[count].mime_type) - 1);
            scripts[count].mime_type[sizeof(scripts[count].mime_type) - 1] = '\0';
            scripts[count].content = NULL;
            scripts[count].content_len = 0;
            
            LOG_INFO("Found external script [%d]: %.80s... (type: %s)", 
                     scripts[count].parse_order, scripts[count].url, mime_type);
            count++;
            p = tag_end + 1;
            
        } else {
            // Inline script - find the closing </script> tag
            const char *content_start = tag_end + 1;
            
            // Use robust script end finder that handles strings with </script>
            const char *script_end = find_script_end(content_start);
            
            if (!script_end) {
                LOG_WARN("No closing </script> tag found");
                break;
            }
            
            size_t content_len = script_end - content_start;
            
            // Skip empty scripts or very short ones
            if (content_len < 50) {
                p = script_close_end(script_end);
                continue;
            }
            
            // Warn about very large scripts but still process them
            if (content_len > 500000) {
                LOG_INFO("Found large inline script: %zu bytes (may be data payload)", content_len);
            }
            
            // Extract the script content with proper size handling for large payloads
            char *script_content = NULL;
            
            // For very large scripts, verify we can allocate the memory
            if (content_len > 1000000) {
                // Try to allocate, if it fails, skip this script
                script_content = (char*)malloc(content_len + 1);
                if (!script_content) {
                    LOG_ERROR("Failed to allocate %zu bytes for script content", content_len + 1);
                    p = script_close_end(script_end);
                    continue;
                }
            } else {
                script_content = (char*)malloc(content_len + 1);
                if (!script_content) {
                    LOG_ERROR("Failed to allocate %zu bytes for script content", content_len + 1);
                    p = script_close_end(script_end);
                    continue;
                }
            }
            
            memcpy(script_content, content_start, content_len);
            script_content[content_len] = '\0';
            
            scripts[count].url[0] = '\0';
            scripts[count].parse_order = parse_order++;
            scripts[count].type = SCRIPT_TYPE_INLINE;
            strncpy(scripts[count].mime_type, mime_type, sizeof(scripts[count].mime_type) - 1);
            scripts[count].mime_type[sizeof(scripts[count].mime_type) - 1] = '\0';
            scripts[count].content = script_content;
            scripts[count].content_len = content_len;
            
            LOG_INFO("Found inline script [%d]: %zu bytes (type: %s)", 
                     scripts[count].parse_order, content_len, mime_type);
            count++;
            
            p = script_close_end(script_end);
        }
    }
    
    LOG_INFO("Extracted %d scripts in parse order", count);
    return count;
}

int timer_process_due(JSContextHandle ctx);

// Pump timers and microtasks after a network response.
static void pump_timers_and_jobs_after_fetch(void) {
    if (!SAFE_VALID(g_js_context)) return;
    
    int iterations = 0;
    while (iterations < 20) {
        int processed = timer_process_due(g_js_context);
        int jobs = 0;
        JSContextHandle pctx = GC_HANDLE_NULL;
        JSRuntimeHandle rt = JS_GetRuntime(g_js_context);
        int ret;
        while ((ret = JS_ExecutePendingJob(rt, &pctx)) > 0) {
            jobs++;
        }
        (void)ret;
        if (processed == 0 && jobs == 0) break;
        iterations++;
    }
}

// Some third-party polyfills are not safe to execute inside this emulator.
// We let the ShadyDOM (webcomponents-sd) and ShadyCSS polyfills run unmodified
// so they can own shadow DOM.  Only skip polyfills that are genuinely
// incompatible with the engine itself.
static bool is_unsafe_external_script(const char *url) {
    if (!url) return false;
    /* custom-elements-es5-adapter is required by Polymer ES5 elements to
     * construct/stamp correctly.  It was previously skipped because the
     * engine's closure resolution for class constructors inside functions
     * was broken (js_op_define_class var-refs type confusion); that bug is
     * fixed, so allow the adapter to load.  CYBER_SKIP_ES5_ADAPTER=1
     * restores the old behavior. */
    static const char *skip_patterns[] = {
        NULL
    };
    if (cyber_getenv("CYBER_SKIP_ES5_ADAPTER")) {
        static const char *es5[] = { "custom-elements-es5-adapter", NULL };
        skip_patterns[0] = es5[0];
    }
    for (const char **p = skip_patterns; *p; p++) {
        if (strstr(url, *p)) return true;
    }
    return false;
}

// Execute all page scripts (inline + external) in document order.
// Fetches external scripts, runs everything through js_quickjs_exec_scripts,
// and pumps timers/microtasks after each network response.

/* ============================================================================
 * StrBuf - minimal growable string used by the script patching code
 * ============================================================================
 * Always NUL-terminated. Positions are byte offsets; SB_NPOS means "not found"
 * (like SB_NPOS). Declare locals with SB_AUTO to free them at scope
 * exit. Allocation failure aborts (as std::string would throw). */
typedef struct StrBuf {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;
#define SB_NPOS ((size_t)-1)
#define SB_AUTO __attribute__((cleanup(sb_free)))

static void sb_free(StrBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}
static void sb_reserve(StrBuf *b, size_t need) {
    if (need + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 16;
    while (cap < need + 1) cap *= 2;
    char *d = (char *)realloc(b->data, cap);
    if (!d) abort();
    b->data = d;
    b->cap = cap;
}
static StrBuf sb_from(const char *data, size_t len) {
    StrBuf b = {0};
    sb_reserve(&b, len);
    memcpy(b.data, data, len);
    b.data[len] = '\0';
    b.len = len;
    return b;
}
static StrBuf sb_from_cstr(const char *str) { return sb_from(str, strlen(str)); }
/* Position of needle at or after from, or SB_NPOS. */
static size_t sb_find(const StrBuf *b, const char *needle, size_t from) {
    size_t n = strlen(needle);
    if (from > b->len || n > b->len - from) return SB_NPOS;
    for (size_t i = from; i + n <= b->len; i++) {
        if (memcmp(b->data + i, needle, n) == 0) return i;
    }
    return SB_NPOS;
}
static size_t sb_find_char(const StrBuf *b, char c, size_t from) {
    if (from >= b->len) return SB_NPOS;
    const char *q = (const char *)memchr(b->data + from, c, b->len - from);
    return q ? (size_t)(q - b->data) : SB_NPOS;
}
/* Replace count bytes at pos with str (count is clamped to the end). */
static void sb_replace(StrBuf *b, size_t pos, size_t count, const char *str) {
    if (pos > b->len) abort();
    if (count > b->len - pos) count = b->len - pos;
    size_t n = strlen(str);
    sb_reserve(b, b->len - count + n);
    memmove(b->data + pos + n, b->data + pos + count, b->len - pos - count + 1);
    memcpy(b->data + pos, str, n);
    b->len = b->len - count + n;
}
static void sb_insert(StrBuf *b, size_t pos, const char *str) { sb_replace(b, pos, 0, str); }
static StrBuf sb_substr(const StrBuf *b, size_t pos, size_t count) {
    if (pos > b->len) abort();
    if (count > b->len - pos) count = b->len - pos;
    return sb_from(b->data + pos, count);
}
/* Concatenation of a NULL-terminated list of C strings. */
static StrBuf sb_join(const char *first, ...) {
    StrBuf b = sb_from("", 0);
    va_list ap;
    va_start(ap, first);
    for (const char *p = first; p; p = va_arg(ap, const char *)) {
        size_t n = strlen(p);
        sb_reserve(&b, b.len + n);
        memcpy(b.data + b.len, p, n + 1);
        b.len += n;
    }
    va_end(ap);
    return b;
}

/* The inline player bootstrap sets window.ytplayer.config without width/height,
 * so base.js's player creation helper throws "missing height argument".
 * Inject sensible defaults so the player bootstrap completes.
 */
static void apply_youtube_player_bootstrap_patch(StrBuf *src) {
    const char *pat = "window.ytplayer.config={args:{raw_player_response:window.ytplayer.bootstrapPlayerResponse}};";
    size_t pos = sb_find(src, pat, 0);
    if (pos != SB_NPOS) {
        const char *repl = "window.ytplayer.config={args:{raw_player_response:window.ytplayer.bootstrapPlayerResponse},width:640,height:360};";
        sb_replace(src, pos, strlen(pat), repl);
        fprintf(stderr, "[PLAYER-BOOTSTRAP-PATCH] added width/height to ytplayer.config\n");
        fflush(stderr);
    }
}

bool html_execute_page_scripts(const char *html, JsExecResult *out_result) {
    if (!html || !out_result) return false;
    memset(out_result, 0, sizeof(JsExecResult));
    
    ScriptInfo scripts[MAX_SCRIPTS];
    memset(scripts, 0, sizeof(scripts));
    int script_count = 0;
    {
        CP_SCOPE("extract-scripts");
        script_count = extract_scripts_in_order(html, scripts, MAX_SCRIPTS);
    }
    if (script_count == 0) {
        LOG_ERROR("No scripts found in HTML");
        return false;
    }
    
    LOG_INFO("Found %d scripts to execute", script_count);
    
    // External scripts up to ~64 MiB are now safe to fetch.  The parser
    // nesting-state buffer has been moved from the C stack to the GC heap,
    // and cyberbrowser.exe is linked with a 64 MiB C stack, so large
    // ~10 MiB application bundles no longer overflow during JS_Eval.
    const size_t MAX_EXTERNAL_SCRIPT_SIZE = 64 * 1024 * 1024;
    
    // Fetch external scripts (skipped when MAX_EXTERNAL_SCRIPT_SIZE == 0)
    if (MAX_EXTERNAL_SCRIPT_SIZE == 0) {
        LOG_INFO("Skipping external script fetches (MAX_EXTERNAL_SCRIPT_SIZE == 0)");
    }
    /* Start every external fetch now so the downloads overlap (they are
     * still consumed, and executed, in document order). */
    if (MAX_EXTERNAL_SCRIPT_SIZE != 0) {
        for (int i = 0; i < script_count; i++)
            if (scripts[i].type == SCRIPT_TYPE_EXTERNAL && scripts[i].url[0] &&
                !is_unsafe_external_script(scripts[i].url))
                dom_fetch_prefetch(scripts[i].url);
    }
    for (int i = 0; i < script_count; i++) {
        if (scripts[i].type == SCRIPT_TYPE_EXTERNAL) {
            if (MAX_EXTERNAL_SCRIPT_SIZE == 0) {
                scripts[i].url[0] = '\0';
                continue;
            }

            /* Skip known-unsafe / unnecessarily large bundles before fetching. */
            if (is_unsafe_external_script(scripts[i].url)) {
                LOG_WARN("Skipping unsafe external script [%d]: %.200s",
                         scripts[i].parse_order, scripts[i].url);
                scripts[i].url[0] = '\0';
                continue;
            }

            HttpBuffer buffer = {0};
            char error[256] = {0};
            LOG_INFO("Fetching external script [%d]: %.80s",
                     scripts[i].parse_order, scripts[i].url);
            fprintf(stderr, "[html_extract] FETCH external script [%d]: %.200s\n",
                    scripts[i].parse_order, scripts[i].url);
            fflush(stderr);

            bool result = false;
            {
                CP_SCOPE_CAT("fetch-external-script", "net");
                result = dom_fetch_script(scripts[i].url, &buffer, error, sizeof(error));
            }
            fprintf(stderr, "[html_extract] FETCH external script [%d] result=%s size=%zu\n",
                    scripts[i].parse_order, result ? "ok" : "fail", result ? buffer.size : 0);
            fflush(stderr);
            if (result && buffer.data && buffer.size > 0) {
                const char *content = buffer.data;
                while (*content && (isspace((unsigned char)*content) ||
                       (unsigned char)*content == 0xEF ||
                       (unsigned char)*content == 0xBB ||
                       (unsigned char)*content == 0xBF)) {
                    content++;
                }

                bool is_html = (strncasecmp(content, "<!doctype", 9) == 0 ||
                               strncasecmp(content, "<html", 5) == 0 ||
                               strncasecmp(content, "<?xml", 5) == 0);

                if (is_html) {
                    LOG_WARN("Script [%d] is HTML not JS, skipping", scripts[i].parse_order);
                    http_free_buffer(&buffer);
                    scripts[i].url[0] = '\0';
                } else if (buffer.size > MAX_EXTERNAL_SCRIPT_SIZE) {
                    LOG_WARN("Script [%d] is %zu bytes, skipping external script execution",
                             scripts[i].parse_order, buffer.size);
                    http_free_buffer(&buffer);
                    scripts[i].url[0] = '\0';
                } else {
                    LOG_INFO("Loaded external script [%d]: %zu bytes URL=%.200s",
                             scripts[i].parse_order, buffer.size, scripts[i].url);

                    scripts[i].content = buffer.data;
                    scripts[i].content_len = buffer.size;

                    /* Diagnostic: trace the monomer lifecycle state-machine
                     * transitions in the desktop_polymer bundle.  CYBER_TRACE_LIFECYCLE=1. */
                    if (CYBER_ENV(CYBER_T_LIFECYCLE) && scripts[i].content_len > 1000000 &&
                        strstr(scripts[i].content, "prototype.transition=function(")) {
                        StrBuf patched SB_AUTO = sb_from(scripts[i].content, scripts[i].content_len);
                        size_t fpos = sb_find(&patched, "prototype.transition=function(", 0);
                        size_t bpos = sb_find_char(&patched, '{', fpos);
                        if (fpos != SB_NPOS && bpos != SB_NPOS) {
                            const char *inj = "try{console.error('[TR] '+this.state+'->'+arguments[0])}catch(e){}";
                            sb_insert(&patched, bpos + 1, inj);
                        }
                        size_t fpos2 = sb_find(&patched, "prototype.parkOrScheduleJob=function(", 0);
                        size_t bpos2 = fpos2 == SB_NPOS ? fpos2 : sb_find_char(&patched, '{', fpos2);
                        if (fpos2 != SB_NPOS && bpos2 != SB_NPOS) {
                            const char *inj2 = "try{var s$=arguments[2];if(s$&&(''+s$).indexOf('eoir')>=0){window.__eoirC=(window.__eoirC||0)+1;if(window.__eoirC<4||window.__eoirC===1000)console.error('[PARK-EOIR #'+window.__eoirC+'] V.name='+(arguments[0]&&arguments[0].name)+' V.body='+(''+arguments[0]).slice(0,60)+' al=='+(typeof appLoad)+' al===sal:'+(typeof appLoad!=='undefined'&&typeof scheduleAppLoad!=='undefined'?appLoad===scheduleAppLoad:'?')+' '+(((new Error('x')).stack||'').split(String.fromCharCode(10)).slice(1,6).join(' <- ')))}else console.error('[PARK] sig='+s$+' prio='+arguments[1])}catch(e){}";
                            sb_insert(&patched, bpos2 + 1, inj2);
                        }
                        /* Log when the lifecycle jobSet resolves (init completes).
                         * The call sits in expression position (after &&), so wrap
                         * it in a comma expression rather than inserting a stmt. */
                        const char *res_anchor = ".completedResolver.resolve()";
                        size_t fpos3 = sb_find(&patched, res_anchor, 0);
                        if (fpos3 != SB_NPOS) {
                            /* Back up over the receiver identifier (minified name
                             * shifts between fetches) and wrap the whole call in a
                             * comma expression — valid in the && chain it sits in. */
                            size_t id_start = fpos3;
                            while (id_start > 0 && (isalnum((unsigned char)patched.data[id_start-1]) ||
                                   patched.data[id_start-1]=='_' || patched.data[id_start-1]=='$'))
                                id_start--;
                            StrBuf recv SB_AUTO = sb_substr(&patched, id_start, fpos3 - id_start);
                            StrBuf call SB_AUTO = sb_join(recv.data, res_anchor, NULL);
                            StrBuf wrapped SB_AUTO = sb_join("(console.error('[JOBSET] resolved'),", call.data, ")", NULL);
                            sb_replace(&patched, id_start, call.len, wrapped.data);
                        }
                        /* Log a jobSet payload job that throws (which would leave the
                         * jobSet's completedResolver permanently unresolved). */
                        size_t fpos4 = sb_find(&patched, "function(){J.payload$jscomp$20.job();", 0);
                        if (fpos4 != SB_NPOS) {
                            /* Find the jobSet `this` capture by locating the receiver
                             * of .scheduledPayloads[ after the wrapper start. */
                            StrBuf js_this SB_AUTO = sb_from_cstr("y");
                            size_t sp = sb_find(&patched, ".scheduledPayloads[", fpos4);
                            if (sp != SB_NPOS) {
                                size_t id_start = sp;
                                while (id_start > 0 && (isalnum((unsigned char)patched.data[id_start-1]) ||
                                       patched.data[id_start-1]=='_' || patched.data[id_start-1]=='$'))
                                    id_start--;
                                sb_free(&js_this);
                                js_this = sb_substr(&patched, id_start, sp - id_start);
                            }
                            StrBuf to4 SB_AUTO = sb_join("function(){console.error('[JOBSET-JOB-RUN] i='+J.i$jscomp$549+' n='+",
                                js_this.data, ".scheduledPayloads.length);try{J.payload$jscomp$20.job()}catch(e$jscomp$1){console.error('[JOBSET-ERR] '+(e$jscomp$1&&e$jscomp$1.message)+' | '+((e$jscomp$1&&e$jscomp$1.stack)||'').split(String.fromCharCode(10))[0]);throw e$jscomp$1};", NULL);
                            const char *from4 = "function(){J.payload$jscomp$20.job();";
                            sb_replace(&patched, fpos4, strlen(from4), to4.data);
                        }
                        /* Trace gkY cancel/flushJobs — a cancel would explain jobSet
                         * payload jobs never running. */
                        size_t fpos6 = sb_find(&patched, "prototype.cancel=function(){for(var ", 0);
                        if (fpos6 != SB_NPOS) {
                            size_t b6 = sb_find_char(&patched, '{', fpos6 + 10);
                            if (b6 != SB_NPOS)
                                sb_insert(&patched, b6 + 1, "try{console.error('[JOBSET-CANCEL]')}catch(e){}");
                        }
                        size_t fpos7 = sb_find(&patched, "prototype.flushJobs=function(){", 0);
                        if (fpos7 != SB_NPOS) {
                            size_t b7 = sb_find_char(&patched, '{', fpos7);
                            if (b7 != SB_NPOS)
                                sb_insert(&patched, b7 + 1, "try{console.error('[JOBSET-FLUSH]')}catch(e){}");
                        }
                        /* Log each jobSet payload's scheduled priority. */
                        size_t fpos8 = sb_find(&patched, "this.scheduler.addJob(Q,i1A(this,V.payload$jscomp$20))", 0);
                        if (fpos8 != SB_NPOS) {
                            const char *a8 = "this.scheduler.addJob(Q,i1A(this,V.payload$jscomp$20))";
                            StrBuf r8 SB_AUTO = sb_join("(console.error('[JOBSET-ADD] i='+V.i$jscomp$549+' prio='+i1A(this,V.payload$jscomp$20)),", a8, ")", NULL);
                            sb_replace(&patched, fpos8, strlen(a8), r8.data);
                        }
                        /* Trace jK.addJob: does the jobSet scheduler delegate to a
                         * registered service or run synchronously? */
                        size_t fpos5 = sb_find(&patched, "jK.prototype.addJob=function(V,y,Q){", 0);
                        if (fpos5 != SB_NPOS) {
                            const char *from5 = "jK.prototype.addJob=function(V,y,Q){";
                            const char *to5 = "jK.prototype.addJob=function(V,y,Q){try{console.error('[JK-ADDJOB] prio='+y)}catch(e){}try{";
                            sb_replace(&patched, fpos5, strlen(from5), to5);
                            const char *tail5 = "Q===void 0?(V(),NaN):_.Mk(V,Q||0)}";
                            size_t tpos5 = sb_find(&patched, tail5, fpos5);
                            if (tpos5 != SB_NPOS) {
                                sb_replace(&patched, tpos5, strlen(tail5), "Q===void 0?(V(),NaN):_.Mk(V,Q||0)}catch(e$2){console.error('[JK-ADDJOB-ERR] '+(e$2&&e$2.message)+' | '+((e$2&&e$2.stack)||'').split(String.fromCharCode(10))[0]);throw e$2}}");
                            }
                        }
                        /* Trace the initial-data consumption (SDs) and ytd-app's
                         * updatePageData (which fires the 'cr' signal). */
                        size_t fpos9 = sb_find(&patched, "SDs=function(V,y){", 0);
                        if (fpos9 != SB_NPOS) {
                            size_t b9 = sb_find_char(&patched, '{', fpos9);
                            if (b9 != SB_NPOS)
                                sb_insert(&patched, b9 + 1, "try{console.error('[SDS] initial data consumed')}catch(e){}");
                        }
                        size_t fposRM = sb_find(&patched, "RM=function(V,y){", 0);
                        if (fposRM != SB_NPOS) {
                            size_t brm = sb_find_char(&patched, '{', fposRM);
                            if (brm != SB_NPOS)
                                sb_insert(&patched, brm + 1, "try{console.error('[RM] run root.loadData='+(V&&V.root&&typeof V.root.loadData))}catch(e){}");
                        }
                        size_t fpos10 = sb_find(&patched, ".updatePageData=function(V){", 0);
                        if (fpos10 != SB_NPOS) {
                            size_t b10 = sb_find_char(&patched, '{', fpos10);
                            if (b10 != SB_NPOS)
                                sb_insert(&patched, b10 + 1, "try{console.error('[UPD] updatePageData called page='+(V&&V.page)+' keys='+(V?Object.keys(V).slice(0,10).join(','):'null')+' pdu='+typeof this.performDataUpdate)}catch(e){}");
                        }
                        size_t fposJ = sb_find(&patched, ".updatePageDataJobId=_.MQ(_.Jv,J)", 0);
                        if (fposJ != SB_NPOS) {
                            const char *aJ = ".updatePageDataJobId=_.MQ(_.Jv,J)";
                            sb_replace(&patched, fposJ, strlen(aJ), ".updatePageDataJobId=(console.error('[UPDJ] scheduling J'),_.MQ(_.Jv,J))");
                        }
                        /* Trace the deferred J body (performDataUpdate + cr fire). */
                        size_t fposJB = sb_find(&patched, "var J=function(){V.filler?y.performDataUpdate(V,Q):zUD(", 0);
                        if (fposJB != SB_NPOS) {
                            size_t bJB = sb_find_char(&patched, '{', fposJB);
                            if (bJB != SB_NPOS)
                                sb_insert(&patched, bJB + 1, "try{console.error('[J] running page='+(V&&V.page))}catch(e){}");
                        }
                        /* Trace fireEvent detail integrity. */
                        size_t fposFE = sb_find(&patched, "V=new CustomEvent(V,{bubbles:!0,cancelable:!1,composed:!0,detail:y});this.dispatchEvent(V)", 0);
                        if (fposFE != SB_NPOS) {
                            const char *aFE = "V=new CustomEvent(V,{bubbles:!0,cancelable:!1,composed:!0,detail:y});this.dispatchEvent(V)";
                            const char *rFE = "V=new CustomEvent(V,{bubbles:!0,cancelable:!1,composed:!0,detail:y});console.error('[FE] fireEvent type='+V.type+' yType='+typeof y+' yStr='+(function(){try{return JSON.stringify(y).slice(0,80)}catch(e){return 'err:'+e.message}})());this.dispatchEvent(V)";
                            sb_replace(&patched, fposFE, strlen(aFE), rFE);
                        }
                        size_t fposPDU = sb_find(&patched, "performDataUpdate=function(V,y){", 0);
                        if (fposPDU != SB_NPOS) {
                            size_t bpdu = sb_find_char(&patched, '{', fposPDU);
                            if (bpdu != SB_NPOS)
                                sb_insert(&patched, bpdu + 1, "try{console.error('[PDU] performDataUpdate')}catch(e){}");
                        }
                        /* Trace zUD invoking the deferred performDataUpdate fn. */
                        size_t fposZ = sb_find(&patched, "try{_.Kp(\"fr_s\"),V()}catch(y){_.NR(y)}", 0);
                        if (fposZ != SB_NPOS) {
                            const char *aZ = "try{_.Kp(\"fr_s\"),V()}catch(y){_.NR(y)}";
                            const char *rZ = "try{_.Kp(\"fr_s\");console.error('[ZUD] invoking fn');V()}catch(y){console.error('[ZUD] fn caught: '+(y&&y.message)+' (skipping NR)')}";
                            sb_replace(&patched, fposZ, strlen(aZ), rZ);
                        }
                        size_t fpos11 = sb_find(&patched, "processSignal(\"cr\")", 0);
                        if (fpos11 != SB_NPOS) {
                            /* Back up over the receiver identifier (e.g. `t.`) and wrap
                             * the WHOLE call in a comma expression — inserting before
                             * `processSignal` alone would produce `t.console.error(...)`,
                             * which throws (t.console is undefined) and aborts the
                             * updatePageData continuation before "cr" fires. */
                            size_t id_start = fpos11;
                            while (id_start > 0 && (isalnum((unsigned char)patched.data[id_start-1]) ||
                                   patched.data[id_start-1]=='_' || patched.data[id_start-1]=='$' ||
                                   patched.data[id_start-1]=='.'))
                                id_start--;
                            StrBuf recv SB_AUTO = sb_substr(&patched, id_start, fpos11 - id_start);
                            StrBuf call SB_AUTO = sb_join(recv.data, "processSignal(\"cr\")", NULL);
                            StrBuf wrapped SB_AUTO = sb_join("(console.error('[CR] firing cr'),", call.data, ")", NULL);
                            sb_replace(&patched, id_start, call.len, wrapped.data);
                        }
                        /* Guard the MiniplayerService PIP check: our DI does not inject
                         * pipController, so `this.pipController.pictureInPictureSupported()`
                         * throws mid-performDataUpdate and aborts the "cr" signal (which
                         * gates the whole "rendered" transition and the metadata commit).
                         * Returning false (no PIP support) is correct here. */
                        size_t fposB3 = sb_find(&patched, "_.L.updateComputedBadges=function(V,y,Q){var v=this,J;", 0);
                        if (fposB3 != SB_NPOS) {
                            const char *aB3 = "_.L.updateComputedBadges=function(V,y,Q){var v=this,J;";
                            const char *rB3 = "_.L.updateComputedBadges=function(V,y,Q){console.error('[UCB] V='+typeof V+' isArr='+Array.isArray(V)+' filter='+typeof (V&&V.filter)+' this='+(this&&this.hostElement&&this.hostElement.tagName));var v=this,J;";
                            sb_replace(&patched, fposB3, strlen(aB3), rB3);
                        }
                        size_t fposPip = sb_find(&patched, "im.prototype.pictureInPictureSupported=function(){return this.pipController.pictureInPictureSupported()}", 0);
                        if (fposPip != SB_NPOS) {
                            const char *aP = "im.prototype.pictureInPictureSupported=function(){return this.pipController.pictureInPictureSupported()}";
                            const char *rP = "im.prototype.pictureInPictureSupported=function(){return this.pipController?this.pipController.pictureInPictureSupported():false}";
                            sb_replace(&patched, fposPip, strlen(aP), rP);
                            fprintf(stderr, "[LC-PATCH] pipController guard injected\n");
                        }
                        /* Trace the J continuation: does it reach the cr fire? */
                        size_t fposCR2 = sb_find(&patched, "var t=_.n_();_.Sy(t,\"cr\")||t.processSignal(\"cr\");v()", 0);
                        if (fposCR2 != SB_NPOS) {
                            const char *aC = "var t=_.n_();_.Sy(t,\"cr\")||t.processSignal(\"cr\");v()";
                            const char *rC = "console.error('[CR2] J-continuation filler='+V.filler);var t=_.n_();console.error('[CR2] Sy(cr)='+_.Sy(t,\"cr\"));_.Sy(t,\"cr\")||t.processSignal(\"cr\");console.error('[CR2] cr fired');v()";
                            sb_replace(&patched, fposCR2, strlen(aC), rC);
                        }
                        /* Guard badge-observer calls that run with a wrong `this` (the
                         * monomer fires some Polymer property observers on the host
                         * element instead of the controller, so `this.updateComputedBadges`
                         * is undefined and throws mid-performDataUpdate, aborting "cr"). */
                        size_t fposB1 = sb_find(&patched, "this.updateComputedBadges(this.badges,this.topStandaloneBadge)", 0);
                        if (fposB1 != SB_NPOS) {
                            const char *aB = "this.updateComputedBadges(this.badges,this.topStandaloneBadge)";
                            const char *rB = "this.updateComputedBadges&&this.updateComputedBadges(this.badges,this.topStandaloneBadge)";
                            size_t p = fposB1;
                            while (p != SB_NPOS) {
                                sb_replace(&patched, p, strlen(aB), rB);
                                p = sb_find(&patched, aB, p + strlen(rB));
                            }
                        }
                        size_t fposB2 = sb_find(&patched, "this.updateComputedBadges(this.badges,this.topStandaloneBadge,this.bottomStandaloneBadge)", 0);
                        if (fposB2 != SB_NPOS) {
                            const char *aB2 = "this.updateComputedBadges(this.badges,this.topStandaloneBadge,this.bottomStandaloneBadge)";
                            const char *rB2 = "this.updateComputedBadges&&this.updateComputedBadges(this.badges,this.topStandaloneBadge,this.bottomStandaloneBadge)";
                            size_t p2 = fposB2;
                            while (p2 != SB_NPOS) {
                                sb_replace(&patched, p2, strlen(aB2), rB2);
                                p2 = sb_find(&patched, aB2, p2 + strlen(rB2));
                            }
                        }
                        size_t fpos12 = sb_find(&patched, "onYtPageManagerAttached=function(V){", 0);
                        if (fpos12 != SB_NPOS) {
                            size_t b12 = sb_find_char(&patched, '{', fpos12);
                            if (b12 != SB_NPOS)
                                sb_insert(&patched, b12 + 1, "try{var g$=_.gU(V);console.error('[PMA] onYtPageManagerAttached gU='+(g$&&g$.tagName)+' id='+(g$&&g$.id)+' target='+(V&&V.target&&V.target.tagName))}catch(e){console.error('[PMA] err '+e.message)}");
                        }
                        size_t fpos12b = sb_find(&patched, "this.pageManagerAttachedPromise.resolve()", 0);
                        if (fpos12b != SB_NPOS) {
                            sb_replace(&patched, fpos12b, strlen("this.pageManagerAttachedPromise.resolve()"), "(console.error('[PMA-RESOLVE] typeof='+typeof this.pageManagerAttachedPromise),this.pageManagerAttachedPromise.resolve())");
                        }
                        /* Trace the page-data-fetched handler (leads to updatePageData). */
                        size_t fpos14 = sb_find(&patched, "onYtPageDataFetched=function(V,y){", 0);
                        if (fpos14 != SB_NPOS) {
                            size_t b14 = sb_find_char(&patched, '{', fpos14);
                            if (b14 != SB_NPOS)
                                sb_insert(&patched, b14 + 1, "try{console.error('[PDF] onYtPageDataFetched y='+(y?Object.keys(y).slice(0,6).join(','):'null')+' pd='+(y&&y.pageData?'y':'n'))}catch(e){}");
                        }
                        size_t fposPPD = sb_find(&patched, "publishPageData=function(V){", 0);
                        if (fposPPD != SB_NPOS) {
                            size_t bppd = sb_find_char(&patched, '{', fposPPD);
                            if (bppd != SB_NPOS)
                                sb_insert(&patched, bppd + 1, "try{var lit$={pageData:V};var d$=Object.getOwnPropertyDescriptor(lit$,'pageData');console.error('[PPD] page='+(V&&V.page)+' keys='+Object.keys(lit$).length+' desc='+JSON.stringify(d$&&{e:d$.enumerable,w:d$.writable,c:d$.configurable})+' vKeys='+(V?Object.keys(V).length:'-'))}catch(e){}");
                        }
                        /* Trace loadData's deferred navigation callback. */
                        size_t fpos15 = sb_find(&patched, "this.loadDepsPromise.then(function(){", 0);
                        if (fpos15 != SB_NPOS) {
                            size_t b15 = sb_find_char(&patched, '{', fpos15);
                            if (b15 != SB_NPOS)
                                sb_insert(&patched, b15 + 1, "try{console.error('[LDD] loadDeps then runs')}catch(e){}");
                        }
                        size_t fpos16 = sb_find(&patched, ".loadData=function(V){var y=this;", 0);
                        if (fpos16 != SB_NPOS) {
                            const char *a16 = ".loadData=function(V){var y=this;";
                            StrBuf r16 SB_AUTO = sb_join(a16, "try{console.error('[LD] loadData entry deps='+(this.loadDepsPromise?'y':'n'))}catch(e){}", NULL);
                            sb_replace(&patched, fpos16, strlen(a16), r16.data);
                        }
                        /* Trace the _.mm custom-promise async pump: EQY schedules,
                         * UR$ drains via a native Promise job. */
                        size_t fpos17 = sb_find(&patched, "EQY=function(V){V.executing_||(V.executing_=!0,_.WV(", 0);
                        if (fpos17 != SB_NPOS) {
                            size_t b17 = sb_find_char(&patched, '{', fpos17);
                            if (b17 != SB_NPOS)
                                sb_insert(&patched, b17 + 1, "try{window.__eqy=(window.__eqy||0)+1;if(window.__eqy<6)console.error('[EQY] schedule')}catch(e){}");
                        }
                        size_t fpos18 = sb_find(&patched, "UR$=function(){for(var V;V=AvU.remove();)", 0);
                        if (fpos18 != SB_NPOS) {
                            size_t b18 = sb_find_char(&patched, '{', fpos18);
                            if (b18 != SB_NPOS)
                                sb_insert(&patched, b18 + 1, "try{window.__ur=(window.__ur||0)+1;if(window.__ur<6)console.error('[UR] drain')}catch(e){}");
                        }
                        /* Trace _.mm callback invocation. */
                        size_t fpos19 = sb_find(&patched, "nQN=function(V,y,Q){y==2?V.JSC$9850_onFulfilled.call(V.context,Q)", 0);
                        if (fpos19 != SB_NPOS) {
                            size_t b19 = sb_find_char(&patched, '{', fpos19);
                            if (b19 != SB_NPOS)
                                sb_insert(&patched, b19 + 1, "try{window.__nqn=(window.__nqn||0)+1;if(window.__nqn<15)console.error('[NQN] state='+y)}catch(e){}");
                        }
                        /* Trace _.bQ event dispatches named 'attached'. */
                        size_t fpos13 = sb_find(&patched, "bQ=function(V,y,Q,v){", 0);
                        if (fpos13 != SB_NPOS) {
                            size_t b13 = sb_find_char(&patched, '{', fpos13);
                            if (b13 != SB_NPOS)
                                sb_insert(&patched, b13 + 1, "try{if(y==='attached'){window.__bqa=(window.__bqa||0)+1;if(window.__bqa<8)console.error('[BQ] attached fired on '+(V&&V.tagName))}}catch(e){}");
                        }
                        if (patched.len != scripts[i].content_len) {
                            char *nd = (char *)malloc(patched.len + 1);
                            if (nd) {
                                memcpy(nd, patched.data, patched.len);
                                nd[patched.len] = '\0';
                                scripts[i].content = nd;
                                scripts[i].content_len = patched.len;
                                fprintf(stderr, "[LC-PATCH] transition trace injected\n");
                            }
                        }
                    }
                    /* Log when the scheduler's O() executor runs jobs and when the
                     * web-animations polyfill's RAF queue schedules/runs its driver. */
                    if (CYBER_ENV(CYBER_T_LIFECYCLE) && scripts[i].content_len > 1000 &&
                        strstr(scripts[i].content, "m.length==0&&e(d);m.push([C,q]);return C")) {
                        StrBuf patched SB_AUTO = sb_from(scripts[i].content, scripts[i].content_len);
                        const char *from = "m.length==0&&e(d);m.push([C,q]);return C";
                        const char *to = "if(m.length==0){console.error('[WADRV] schedule driver');e(d)}m.push([C,q]);return C";
                        size_t pos = sb_find(&patched, from, 0);
                        if (pos != SB_NPOS) {
                            sb_replace(&patched, pos, strlen(from), to);
                        }
                        const char *fromD = "function d(q){var C=m;m=[];";
                        const char *toD = "function d(q){console.error('[WADRV] driver runs qlen='+m.length);var C=m;m=[];";
                        size_t posD = sb_find(&patched, fromD, 0);
                        if (posD != SB_NPOS) {
                            sb_replace(&patched, posD, strlen(fromD), toD);
                        }
                        if (patched.len != scripts[i].content_len) {
                            char *nd = (char *)malloc(patched.len + 1);
                            if (nd) {
                                memcpy(nd, patched.data, patched.len);
                                nd[patched.len] = '\0';
                                scripts[i].content = nd;
                                scripts[i].content_len = patched.len;
                                fprintf(stderr, "[LC-PATCH] wa-driver trace injected\n");
                            }
                        }
                    }
                    /* Log jobs that error inside the scheduler's O() executor so we can
                     * see which scheduled job hangs the lifecycle jobSet. */
                    if (CYBER_ENV(CYBER_T_LIFECYCLE) && scripts[i].content_len > 1000 &&
                        strstr(scripts[i].content, "function O(a){try{a()}catch(b){T(b)}}")) {
                        StrBuf patched SB_AUTO = sb_from(scripts[i].content, scripts[i].content_len);
                        const char *from = "function O(a){try{a()}catch(b){T(b)}}";
                        const char *to = "function O(a){try{if(!window.__schedRun)window.__schedRun=1;window.__schedRunCount=(window.__schedRunCount||0)+1;if(window.__schedRunCount<20||window.__schedRunCount%500===0)console.error('[SCHED-RUN #'+window.__schedRunCount+']');a()}catch(b){console.error('[SCHED-JOB-ERR] '+(b&&b.message)+' | '+((b&&b.stack)||'').split(String.fromCharCode(10))[0]);T(b)}}";
                        size_t pos = sb_find(&patched, from, 0);
                        if (pos != SB_NPOS) {
                            sb_replace(&patched, pos, strlen(from), to);
                        }
                        /* Trace who keeps re-queueing scheduleAppLoad (the eoir park
                         * flood that starves priority-0 lifecycle jobs). */
                        const char *fromP = "function P(a,b,c,d){++a.F;";
                        const char *toP = "function P(a,b,c,d){try{if(b&&b.name==='scheduleAppLoad'){window.__salC=(window.__salC||0)+1;if(window.__salC<4)console.error('[ADD-SAL] prio='+c+' body='+(''+b).slice(0,90)+' STACK '+((new Error()).stack||'').split(String.fromCharCode(10)).slice(1,7).join(' <- '))}}catch(e){}++a.F;";
                        size_t posP = sb_find(&patched, fromP, 0);
                        if (posP != SB_NPOS) {
                            sb_replace(&patched, posP, strlen(fromP), toP);
                        }
                        /* Trace prio-2 job adds (updatePageData's deferred J). */
                        const char *fromP2 = "var e=a.F;a.h[e]=b;a.l&&!d?a.v.push({id:e,priority:c}):(a.i[c].push(e),";
                        const char *toP2 = "var e=a.F;a.h[e]=b;try{if(c===2)console.error('[SCHED-P2] add id='+e)}catch(e$7){}a.l&&!d?a.v.push({id:e,priority:c}):(a.i[c].push(e),";
                        size_t posP2 = sb_find(&patched, fromP2, 0);
                        if (posP2 != SB_NPOS) {
                            sb_replace(&patched, posP2, strlen(fromP2), toP2);
                        }
                        /* Trace V drains when queue 2 is non-empty. */
                        const char *fromV = "function V(a,b,c){a.C&&a.m===4&&a.g||R(a);";
                        const char *toV = "function V(a,b,c){try{if(a.i[2].length>0)console.error('[SCHED-V] q2='+a.i[2].length+' q3='+a.i[3].length+' m='+a.m)}catch(e$8){}a.C&&a.m===4&&a.g||R(a);";
                        size_t posV = sb_find(&patched, fromV, 0);
                        if (posV != SB_NPOS) {
                            sb_replace(&patched, posV, strlen(fromV), toV);
                        }
                        /* Trace the RAF/timeout callbacks that reset the pump handle. */
                        const char *fromT = "g.T=function(a){this.C=!0;";
                        const char *toT = "g.T=function(a){window.__tCnt=(window.__tCnt||0)+1;if(window.__tCnt<10)console.error('[SCHED-CB] T(RAF) g='+this.g);this.C=!0;";
                        size_t posT = sb_find(&patched, fromT, 0);
                        if (posT != SB_NPOS) {
                            sb_replace(&patched, posT, strlen(fromT), toT);
                        }
                        const char *fromUU = "g.U=function(){V(this)}";
                        const char *toUU = "g.U=function(){window.__u2Cnt=(window.__u2Cnt||0)+1;if(window.__u2Cnt<10)console.error('[SCHED-CB] U(timeout) g='+this.g);V(this)}";
                        size_t posUU = sb_find(&patched, fromUU, 0);
                        if (posUU != SB_NPOS) {
                            sb_replace(&patched, posUU, strlen(fromUU), toUU);
                        }
                        const char *fromU = "function U(a){for(var b=r(J),c=b.next();!c.done;c=b.next())if(a.i[c.value].length)return!0;return!1}";
                        const char *toU = "function U(a){var rr=(function(){for(var b=r(J),c=b.next();!c.done;c=b.next())if(a.i[c.value].length)return!0;return!1})();window.__uCnt=(window.__uCnt||0)+1;if(window.__uCnt<25||window.__uCnt%200===0)console.error('[SCHED-U] ret='+rr+' g='+a.g+' m='+a.m);return rr}";
                        size_t posU = sb_find(&patched, fromU, 0);
                        if (posU != SB_NPOS) {
                            sb_replace(&patched, posU, strlen(fromU), toU);
                        }
                        const char *fromSt = "g.start=function(){this.D=!1;if(this.g===0)switch(this.m=Q(this),this.m){";
                        const char *toSt = "g.start=function(){window.__stCnt=(window.__stCnt||0)+1;if(window.__stCnt<25||window.__stCnt%200===0)console.error('[SCHED-START] g='+this.g+' m0='+this.m);this.D=!1;if(this.g===0)switch(this.m=Q(this),this.m){";
                        size_t posSt = sb_find(&patched, fromSt, 0);
                        if (posSt != SB_NPOS) {
                            sb_replace(&patched, posSt, strlen(fromSt), toSt);
                        }
                        const char *fromRaf = "case 3:this.g=window.requestAnimationFrame(this.M);break;";
                        const char *toRaf = "case 3:this.g=(console.error('[SCHED-ARM] raf m='+this.m),window.requestAnimationFrame(this.M));console.error('[SCHED-ARMED] id='+this.g);break;";
                        size_t posRaf = sb_find(&patched, fromRaf, 0);
                        if (posRaf != SB_NPOS) {
                            sb_replace(&patched, posRaf, strlen(fromRaf), toRaf);
                        }
                        /* Trace queue-0 state: pushes and drain opportunities. */
                        const char *fromQ = "function Q(a){if(a.i[8].length){if(a.C)return 4;if(!document.hidden&&a.B)return 3}for(var b=5;b>=a.j;b--)if(a.i[b].length>0)return b>0?!document.hidden&&a.B?3:2:1;return 0}";
                        const char *toQ = "function Q(a){var r=(function(){if(a.i[8].length){if(a.C)return 4;if(!document.hidden&&a.B)return 3}for(var b=5;b>=a.j;b--)if(a.i[b].length>0)return b>0?!document.hidden&&a.B?3:2:1;return 0})();if(a.i[0].length>0){window.__q0c=(window.__q0c||0)+1;if(window.__q0c<30)console.error('[SCHED-Q] ret='+r+' q0='+a.i[0].length+' q8='+(a.i[8]&&a.i[8].length)+' C='+a.C+' G='+a.G+' m='+a.m)}return r}";
                        size_t posQ = sb_find(&patched, fromQ, 0);
                        if (posQ != SB_NPOS) {
                            sb_replace(&patched, posQ, strlen(fromQ), toQ);
                        }
                        if (patched.len != scripts[i].content_len) {
                            char *nd = (char *)malloc(patched.len + 1);
                            if (nd) {
                                memcpy(nd, patched.data, patched.len);
                                nd[patched.len] = '\0';
                                scripts[i].content = nd;
                                scripts[i].content_len = patched.len;
                                fprintf(stderr, "[LC-PATCH] sched-job-err trace injected\n");
                            }
                        }
                    }
                    /* DIAGNOSTIC: force the lifecycle init to skip the jobSet wait, to
                     * test whether the "rendering" phase (and metadata) runs once the
                     * jobSet hang is bypassed.  CYBER_FORCE_RENDER=1. */
                    if (cyber_getenv("CYBER_FORCE_RENDER") && scripts[i].content_len > 1000000 &&
                        strstr(scripts[i].content, ".jobSet?v.yield(y.jobSet.completedResolver.promise,4):v.jumpTo(4)")) {
                        StrBuf patched SB_AUTO = sb_from(scripts[i].content, scripts[i].content_len);
                        const char *from = ".jobSet?v.yield(y.jobSet.completedResolver.promise,4):v.jumpTo(4)";
                        const char *to = ".jobSet?v.jumpTo(4):v.jumpTo(4)";
                        size_t pos = sb_find(&patched, from, 0);
                        if (pos != SB_NPOS) {
                            sb_replace(&patched, pos, strlen(from), to);
                            char *nd = (char *)malloc(patched.len + 1);
                            if (nd) {
                                memcpy(nd, patched.data, patched.len);
                                nd[patched.len] = '\0';
                                scripts[i].content = nd;
                                scripts[i].content_len = patched.len;
                                fprintf(stderr, "[LC-PATCH] force-render (skip jobSet wait) injected\n");
                            }
                        }
                    }
                }
            } else {
                LOG_WARN("Failed to fetch script [%d]: %s", scripts[i].parse_order, error);
                if (buffer.data) http_free_buffer(&buffer);
                scripts[i].url[0] = '\0';
            }

            // Pump timers and microtasks after each network response
            pump_timers_and_jobs_after_fetch();
        }
    }

    // Patch inline scripts as well (e.g. the player bootstrap that sets
    // window.ytplayer.config without width/height).
    for (int i = 0; i < script_count; i++) {
        if (scripts[i].type == SCRIPT_TYPE_INLINE && scripts[i].content && scripts[i].content_len > 0) {
            StrBuf patched SB_AUTO = sb_from(scripts[i].content, scripts[i].content_len);
            apply_youtube_player_bootstrap_patch(&patched);
            if (patched.len != scripts[i].content_len ||
                memcmp(patched.data, scripts[i].content, scripts[i].content_len) != 0) {
                char *new_data = (char *)malloc(patched.len + 1);
                if (new_data) {
                    memcpy(new_data, patched.data, patched.len);
                    new_data[patched.len] = '\0';
                    free((void *)scripts[i].content);
                    scripts[i].content = new_data;
                    scripts[i].content_len = patched.len;
                }
            }
        }
    }
    
    // Build execution arrays in parse order
    const char *exec_scripts[MAX_SCRIPTS];
    size_t exec_script_lens[MAX_SCRIPTS];
    int exec_count = 0;
    
    for (int i = 0; i < script_count && exec_count < MAX_SCRIPTS; i++) {
        for (int j = 0; j < script_count; j++) {
            if (scripts[j].parse_order == i) {
                if (scripts[j].type == SCRIPT_TYPE_EXTERNAL && scripts[j].url[0] == '\0') {
                    break;
                }
                if (scripts[j].type == SCRIPT_TYPE_INLINE && 
                    (!scripts[j].content || scripts[j].content_len == 0)) {
                    break;
                }
                if (scripts[j].type == SCRIPT_TYPE_JSON_LD) {
                    break;
                }
                exec_scripts[exec_count] = scripts[j].content;
                exec_script_lens[exec_count] = scripts[j].content_len;
                exec_count++;
                break;
            }
        }
    }
    
    if (exec_count == 0) {
        LOG_ERROR("No valid scripts to execute");
        free_script_infos(scripts, script_count);
        return false;
    }
    
    printf("Executing %d page scripts in document order...\n", exec_count);
    LOG_INFO("Executing %d scripts...", exec_count);
    log_to_file("html_media", "Executing %d scripts...", exec_count);
    for (int i = 0; i < exec_count; i++) {
        fprintf(stderr, "[html_extract] EXEC queue script %d/%d size=%zu\n",
                i, exec_count, exec_script_lens[i]);
    }
    fflush(stderr);

    fprintf(stderr, "[html_extract] CALL js_quickjs_exec_scripts exec_count=%d\n", exec_count);
    fflush(stderr);

    // Pass the original HTML so QuickJS can parse and populate the JS DOM.
    // Without this the page scripts see only the hardcoded skeleton document,
    // which is why the page rendered as a blank white screen.
    bool js_success = js_quickjs_exec_scripts(
        exec_scripts, exec_script_lens, exec_count,
        html, out_result);

    fprintf(stderr, "[html_extract] RETURN js_quickjs_exec_scripts success=%d\n", js_success);
    fflush(stderr);

    log_to_file("html_media", "js_quickjs_exec_scripts returned, success=%d", js_success);
    LOG_INFO("js_quickjs_exec_scripts returned, success=%d", js_success);
    
    free_script_infos(scripts, script_count);
    
    return js_success;
}






/* Domain-specific visitorData extraction removed. */
bool html_extract_visitor_data(const char *html, char *out_vd, size_t out_len) {
    (void)html;
    if (out_vd && out_len > 0) out_vd[0] = '\0';
    return false;
}

/* Domain-specific ytInitialPlayerResponse media extraction removed. */
bool html_extract_yt_player_response_media(const char *html, bool prefer_video,
                                                       char *out_url, size_t out_url_len,
                                                       char *out_mime, size_t out_mime_len,
                                                       char *out_title, size_t out_title_len,
                                                       char *out_thumbnail, size_t out_thumbnail_len) {
    (void)html; (void)prefer_video;
    if (out_url && out_url_len > 0) out_url[0] = '\0';
    if (out_mime && out_mime_len > 0) out_mime[0] = '\0';
    if (out_title && out_title_len > 0) out_title[0] = '\0';
    if (out_thumbnail && out_thumbnail_len > 0) out_thumbnail[0] = '\0';
    return false;
}

/* Domain-specific media URL extraction removed. */
bool html_extract_media_url(const char *html, HtmlMediaCandidate *outCandidate,
                            char *err, size_t errLen) {
    (void)html;
    if (outCandidate) memset(outCandidate, 0, sizeof(HtmlMediaCandidate));
    if (err && errLen > 0) {
        strncpy(err, "Media extraction disabled", errLen - 1);
        err[errLen - 1] = '\0';
    }
    return false;
}
