#include <string.h>
#include <string>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include "html_media_extract.h"
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



// Find the true end of a script tag, handling strings and comments
// This prevents premature termination when </script> appears inside JS strings
static const char* find_script_end(const char *content_start) {
    if (!content_start) return NULL;
    
    const char *p = content_start;
    bool in_string = false;
    char string_char = 0;
    bool escape = false;
    int comment_state = 0;  // 0=none, 1=maybe single-line, 2=single-line, 3=maybe multi, 4=multi
    
    while (*p) {
        // Handle comments
        if (!in_string) {
            if (comment_state == 0) {
                if (*p == '/') {
                    comment_state = 1;  // Maybe starting comment
                }
            } else if (comment_state == 1) {
                if (*p == '/') {
                    comment_state = 2;  // Single-line comment started
                } else if (*p == '*') {
                    comment_state = 4;  // Multi-line comment started
                } else {
                    comment_state = 0;  // Not a comment
                }
            } else if (comment_state == 2) {
                if (*p == '\n') {
                    comment_state = 0;  // End single-line comment
                }
            } else if (comment_state == 4) {
                if (*p == '*') {
                    comment_state = 3;  // Maybe ending multi-line
                }
            } else if (comment_state == 3) {
                if (*p == '/') {
                    comment_state = 0;  // End multi-line comment
                } else if (*p != '*') {
                    comment_state = 4;  // Still in multi-line comment
                }
            }
            
            // Check for </script> when not in string and not in comment
            if (comment_state == 0 && *p == '<') {
                if (strncasecmp(p, "</script>", 9) == 0) {
                    return p;  // Found actual script end
                }
            }
        }
        
        // Handle strings
        if (comment_state == 0) {
            if (escape) {
                escape = false;
            } else if (*p == '\\') {
                escape = true;
            } else if (!in_string) {
                if (*p == '"' || *p == '\'' || *p == '`') {
                    in_string = true;
                    string_char = *p;
                }
            } else {
                if (*p == string_char) {
                    in_string = false;
                    string_char = 0;
                }
            }
        }
        
        p++;
    }
    
    return NULL;  // No closing tag found
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
            p = script_end + 9;
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
        
        p = script_end + 9;
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
                p = script_end + 9;
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
                    p = script_end + 9;
                    continue;
                }
            } else {
                script_content = (char*)malloc(content_len + 1);
                if (!script_content) {
                    LOG_ERROR("Failed to allocate %zu bytes for script content", content_len + 1);
                    p = script_end + 9;
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
            
            p = script_end + 9;
        }
    }
    
    LOG_INFO("Extracted %d scripts in parse order", count);
    return count;
}

extern "C" int timer_process_due(JSContextHandle ctx);

// Pump timers and microtasks after a network response.
static void pump_timers_and_jobs_after_fetch(void) {
    if (!g_js_context) return;
    
    int iterations = 0;
    while (iterations < 20) {
        int processed = timer_process_due(g_js_context);
        int jobs = 0;
        JSContextHandle pctx;
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
    if (getenv("CYBER_SKIP_ES5_ADAPTER")) {
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

/* The inline player bootstrap sets window.ytplayer.config without width/height,
 * so base.js's player creation helper throws "missing height argument".
 * Inject sensible defaults so the player bootstrap completes.
 */
static void apply_youtube_player_bootstrap_patch(std::string &src) {
    const char *pat = "window.ytplayer.config={args:{raw_player_response:window.ytplayer.bootstrapPlayerResponse}};";
    size_t pos = src.find(pat);
    if (pos != std::string::npos) {
        const char *repl = "window.ytplayer.config={args:{raw_player_response:window.ytplayer.bootstrapPlayerResponse},width:640,height:360};";
        src.replace(pos, strlen(pat), repl);
        fprintf(stderr, "[PLAYER-BOOTSTRAP-PATCH] added width/height to ytplayer.config\n");
        fflush(stderr);
    }
}

extern "C" bool html_execute_page_scripts(const char *html, JsExecResult *out_result) {
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
                result = http_get_to_memory(scripts[i].url, &buffer, error, sizeof(error));
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
                    if (getenv("CYBER_TRACE_LIFECYCLE") && scripts[i].content_len > 1000000 &&
                        strstr(scripts[i].content, "prototype.transition=function(")) {
                        std::string patched(scripts[i].content, scripts[i].content_len);
                        size_t fpos = patched.find("prototype.transition=function(");
                        size_t bpos = patched.find('{', fpos);
                        if (fpos != std::string::npos && bpos != std::string::npos) {
                            const char *inj = "try{console.error('[TR] '+this.state+'->'+arguments[0])}catch(e){}";
                            patched.insert(bpos + 1, inj);
                        }
                        size_t fpos2 = patched.find("prototype.parkOrScheduleJob=function(");
                        size_t bpos2 = fpos2 == std::string::npos ? fpos2 : patched.find('{', fpos2);
                        if (fpos2 != std::string::npos && bpos2 != std::string::npos) {
                            const char *inj2 = "try{var s$=arguments[2];if(s$&&(''+s$).indexOf('eoir')>=0){window.__eoirC=(window.__eoirC||0)+1;if(window.__eoirC<4||window.__eoirC===1000)console.error('[PARK-EOIR #'+window.__eoirC+'] V.name='+(arguments[0]&&arguments[0].name)+' V.body='+(''+arguments[0]).slice(0,60)+' al=='+(typeof appLoad)+' al===sal:'+(typeof appLoad!=='undefined'&&typeof scheduleAppLoad!=='undefined'?appLoad===scheduleAppLoad:'?')+' '+(((new Error('x')).stack||'').split(String.fromCharCode(10)).slice(1,6).join(' <- ')))}else console.error('[PARK] sig='+s$+' prio='+arguments[1])}catch(e){}";
                            patched.insert(bpos2 + 1, inj2);
                        }
                        /* Log when the lifecycle jobSet resolves (init completes).
                         * The call sits in expression position (after &&), so wrap
                         * it in a comma expression rather than inserting a stmt. */
                        const char *res_anchor = ".completedResolver.resolve()";
                        size_t fpos3 = patched.find(res_anchor);
                        if (fpos3 != std::string::npos) {
                            /* Back up over the receiver identifier (minified name
                             * shifts between fetches) and wrap the whole call in a
                             * comma expression — valid in the && chain it sits in. */
                            size_t id_start = fpos3;
                            while (id_start > 0 && (isalnum((unsigned char)patched[id_start-1]) ||
                                   patched[id_start-1]=='_' || patched[id_start-1]=='$'))
                                id_start--;
                            std::string call = patched.substr(id_start, fpos3 - id_start) + res_anchor;
                            patched.replace(id_start, call.size(),
                                "(console.error('[JOBSET] resolved')," + call + ")");
                        }
                        /* Log a jobSet payload job that throws (which would leave the
                         * jobSet's completedResolver permanently unresolved). */
                        size_t fpos4 = patched.find("function(){J.payload$jscomp$20.job();");
                        if (fpos4 != std::string::npos) {
                            /* Find the jobSet `this` capture by locating the receiver
                             * of .scheduledPayloads[ after the wrapper start. */
                            std::string js_this = "y";
                            size_t sp = patched.find(".scheduledPayloads[", fpos4);
                            if (sp != std::string::npos) {
                                size_t id_start = sp;
                                while (id_start > 0 && (isalnum((unsigned char)patched[id_start-1]) ||
                                       patched[id_start-1]=='_' || patched[id_start-1]=='$'))
                                    id_start--;
                                js_this = patched.substr(id_start, sp - id_start);
                            }
                            std::string to4 = std::string("function(){console.error('[JOBSET-JOB-RUN] i='+J.i$jscomp$549+' n='+") +
                                js_this + ".scheduledPayloads.length);try{J.payload$jscomp$20.job()}catch(e$jscomp$1){console.error('[JOBSET-ERR] '+(e$jscomp$1&&e$jscomp$1.message)+' | '+((e$jscomp$1&&e$jscomp$1.stack)||'').split(String.fromCharCode(10))[0]);throw e$jscomp$1};";
                            const char *from4 = "function(){J.payload$jscomp$20.job();";
                            patched.replace(fpos4, strlen(from4), to4);
                        }
                        /* Trace gkY cancel/flushJobs — a cancel would explain jobSet
                         * payload jobs never running. */
                        size_t fpos6 = patched.find("prototype.cancel=function(){for(var ");
                        if (fpos6 != std::string::npos) {
                            size_t b6 = patched.find('{', fpos6 + 10);
                            if (b6 != std::string::npos)
                                patched.insert(b6 + 1, "try{console.error('[JOBSET-CANCEL]')}catch(e){}");
                        }
                        size_t fpos7 = patched.find("prototype.flushJobs=function(){");
                        if (fpos7 != std::string::npos) {
                            size_t b7 = patched.find('{', fpos7);
                            if (b7 != std::string::npos)
                                patched.insert(b7 + 1, "try{console.error('[JOBSET-FLUSH]')}catch(e){}");
                        }
                        /* Log each jobSet payload's scheduled priority. */
                        size_t fpos8 = patched.find("this.scheduler.addJob(Q,i1A(this,V.payload$jscomp$20))");
                        if (fpos8 != std::string::npos) {
                            const char *a8 = "this.scheduler.addJob(Q,i1A(this,V.payload$jscomp$20))";
                            std::string r8 = std::string("(console.error('[JOBSET-ADD] i='+V.i$jscomp$549+' prio='+i1A(this,V.payload$jscomp$20)),") + a8 + ")";
                            patched.replace(fpos8, strlen(a8), r8);
                        }
                        /* Trace jK.addJob: does the jobSet scheduler delegate to a
                         * registered service or run synchronously? */
                        size_t fpos5 = patched.find("jK.prototype.addJob=function(V,y,Q){");
                        if (fpos5 != std::string::npos) {
                            const char *from5 = "jK.prototype.addJob=function(V,y,Q){";
                            const char *to5 = "jK.prototype.addJob=function(V,y,Q){try{console.error('[JK-ADDJOB] prio='+y)}catch(e){}try{";
                            patched.replace(fpos5, strlen(from5), to5);
                            const char *tail5 = "Q===void 0?(V(),NaN):_.Mk(V,Q||0)}";
                            size_t tpos5 = patched.find(tail5, fpos5);
                            if (tpos5 != std::string::npos) {
                                patched.replace(tpos5, strlen(tail5),
                                    "Q===void 0?(V(),NaN):_.Mk(V,Q||0)}catch(e$2){console.error('[JK-ADDJOB-ERR] '+(e$2&&e$2.message)+' | '+((e$2&&e$2.stack)||'').split(String.fromCharCode(10))[0]);throw e$2}}");
                            }
                        }
                        /* Trace the initial-data consumption (SDs) and ytd-app's
                         * updatePageData (which fires the 'cr' signal). */
                        size_t fpos9 = patched.find("SDs=function(V,y){");
                        if (fpos9 != std::string::npos) {
                            size_t b9 = patched.find('{', fpos9);
                            if (b9 != std::string::npos)
                                patched.insert(b9 + 1, "try{console.error('[SDS] initial data consumed')}catch(e){}");
                        }
                        size_t fposRM = patched.find("RM=function(V,y){");
                        if (fposRM != std::string::npos) {
                            size_t brm = patched.find('{', fposRM);
                            if (brm != std::string::npos)
                                patched.insert(brm + 1, "try{console.error('[RM] run root.loadData='+(V&&V.root&&typeof V.root.loadData))}catch(e){}");
                        }
                        size_t fpos10 = patched.find(".updatePageData=function(V){");
                        if (fpos10 != std::string::npos) {
                            size_t b10 = patched.find('{', fpos10);
                            if (b10 != std::string::npos)
                                patched.insert(b10 + 1, "try{console.error('[UPD] updatePageData called page='+(V&&V.page)+' keys='+(V?Object.keys(V).slice(0,10).join(','):'null')+' pdu='+typeof this.performDataUpdate)}catch(e){}");
                        }
                        size_t fposJ = patched.find(".updatePageDataJobId=_.MQ(_.Jv,J)");
                        if (fposJ != std::string::npos) {
                            const char *aJ = ".updatePageDataJobId=_.MQ(_.Jv,J)";
                            patched.replace(fposJ, strlen(aJ),
                                ".updatePageDataJobId=(console.error('[UPDJ] scheduling J'),_.MQ(_.Jv,J))");
                        }
                        /* Trace the deferred J body (performDataUpdate + cr fire). */
                        size_t fposJB = patched.find("var J=function(){V.filler?y.performDataUpdate(V,Q):zUD(");
                        if (fposJB != std::string::npos) {
                            size_t bJB = patched.find('{', fposJB);
                            if (bJB != std::string::npos)
                                patched.insert(bJB + 1, "try{console.error('[J] running page='+(V&&V.page))}catch(e){}");
                        }
                        /* Trace fireEvent detail integrity. */
                        size_t fposFE = patched.find("V=new CustomEvent(V,{bubbles:!0,cancelable:!1,composed:!0,detail:y});this.dispatchEvent(V)");
                        if (fposFE != std::string::npos) {
                            const char *aFE = "V=new CustomEvent(V,{bubbles:!0,cancelable:!1,composed:!0,detail:y});this.dispatchEvent(V)";
                            const char *rFE = "V=new CustomEvent(V,{bubbles:!0,cancelable:!1,composed:!0,detail:y});console.error('[FE] fireEvent type='+V.type+' yType='+typeof y+' yStr='+(function(){try{return JSON.stringify(y).slice(0,80)}catch(e){return 'err:'+e.message}})());this.dispatchEvent(V)";
                            patched.replace(fposFE, strlen(aFE), rFE);
                        }
                        size_t fposPDU = patched.find("performDataUpdate=function(V,y){");
                        if (fposPDU != std::string::npos) {
                            size_t bpdu = patched.find('{', fposPDU);
                            if (bpdu != std::string::npos)
                                patched.insert(bpdu + 1, "try{console.error('[PDU] performDataUpdate')}catch(e){}");
                        }
                        /* Trace zUD invoking the deferred performDataUpdate fn. */
                        size_t fposZ = patched.find("try{_.Kp(\"fr_s\"),V()}catch(y){_.NR(y)}");
                        if (fposZ != std::string::npos) {
                            const char *aZ = "try{_.Kp(\"fr_s\"),V()}catch(y){_.NR(y)}";
                            const char *rZ = "try{_.Kp(\"fr_s\");console.error('[ZUD] invoking fn');V()}catch(y){console.error('[ZUD] fn caught: '+(y&&y.message)+' (skipping NR)')}";
                            patched.replace(fposZ, strlen(aZ), rZ);
                        }
                        size_t fpos11 = patched.find("processSignal(\"cr\")");
                        if (fpos11 != std::string::npos) {
                            /* Back up over the receiver identifier (e.g. `t.`) and wrap
                             * the WHOLE call in a comma expression — inserting before
                             * `processSignal` alone would produce `t.console.error(...)`,
                             * which throws (t.console is undefined) and aborts the
                             * updatePageData continuation before "cr" fires. */
                            size_t id_start = fpos11;
                            while (id_start > 0 && (isalnum((unsigned char)patched[id_start-1]) ||
                                   patched[id_start-1]=='_' || patched[id_start-1]=='$' ||
                                   patched[id_start-1]=='.'))
                                id_start--;
                            std::string call = patched.substr(id_start, fpos11 - id_start) + "processSignal(\"cr\")";
                            patched.replace(id_start, call.size(),
                                "(console.error('[CR] firing cr')," + call + ")");
                        }
                        /* Guard the MiniplayerService PIP check: our DI does not inject
                         * pipController, so `this.pipController.pictureInPictureSupported()`
                         * throws mid-performDataUpdate and aborts the "cr" signal (which
                         * gates the whole "rendered" transition and the metadata commit).
                         * Returning false (no PIP support) is correct here. */
                        size_t fposB3 = patched.find("_.L.updateComputedBadges=function(V,y,Q){var v=this,J;");
                        if (fposB3 != std::string::npos) {
                            const char *aB3 = "_.L.updateComputedBadges=function(V,y,Q){var v=this,J;";
                            const char *rB3 = "_.L.updateComputedBadges=function(V,y,Q){console.error('[UCB] V='+typeof V+' isArr='+Array.isArray(V)+' filter='+typeof (V&&V.filter)+' this='+(this&&this.hostElement&&this.hostElement.tagName));var v=this,J;";
                            patched.replace(fposB3, strlen(aB3), rB3);
                        }
                        size_t fposPip = patched.find("im.prototype.pictureInPictureSupported=function(){return this.pipController.pictureInPictureSupported()}");
                        if (fposPip != std::string::npos) {
                            const char *aP = "im.prototype.pictureInPictureSupported=function(){return this.pipController.pictureInPictureSupported()}";
                            const char *rP = "im.prototype.pictureInPictureSupported=function(){return this.pipController?this.pipController.pictureInPictureSupported():false}";
                            patched.replace(fposPip, strlen(aP), rP);
                            fprintf(stderr, "[LC-PATCH] pipController guard injected\n");
                        }
                        /* Trace the J continuation: does it reach the cr fire? */
                        size_t fposCR2 = patched.find("var t=_.n_();_.Sy(t,\"cr\")||t.processSignal(\"cr\");v()");
                        if (fposCR2 != std::string::npos) {
                            const char *aC = "var t=_.n_();_.Sy(t,\"cr\")||t.processSignal(\"cr\");v()";
                            const char *rC = "console.error('[CR2] J-continuation filler='+V.filler);var t=_.n_();console.error('[CR2] Sy(cr)='+_.Sy(t,\"cr\"));_.Sy(t,\"cr\")||t.processSignal(\"cr\");console.error('[CR2] cr fired');v()";
                            patched.replace(fposCR2, strlen(aC), rC);
                        }
                        /* Guard badge-observer calls that run with a wrong `this` (the
                         * monomer fires some Polymer property observers on the host
                         * element instead of the controller, so `this.updateComputedBadges`
                         * is undefined and throws mid-performDataUpdate, aborting "cr"). */
                        size_t fposB1 = patched.find("this.updateComputedBadges(this.badges,this.topStandaloneBadge)");
                        if (fposB1 != std::string::npos) {
                            const char *aB = "this.updateComputedBadges(this.badges,this.topStandaloneBadge)";
                            const char *rB = "this.updateComputedBadges&&this.updateComputedBadges(this.badges,this.topStandaloneBadge)";
                            size_t p = fposB1;
                            while (p != std::string::npos) {
                                patched.replace(p, strlen(aB), rB);
                                p = patched.find(aB, p + strlen(rB));
                            }
                        }
                        size_t fposB2 = patched.find("this.updateComputedBadges(this.badges,this.topStandaloneBadge,this.bottomStandaloneBadge)");
                        if (fposB2 != std::string::npos) {
                            const char *aB2 = "this.updateComputedBadges(this.badges,this.topStandaloneBadge,this.bottomStandaloneBadge)";
                            const char *rB2 = "this.updateComputedBadges&&this.updateComputedBadges(this.badges,this.topStandaloneBadge,this.bottomStandaloneBadge)";
                            size_t p2 = fposB2;
                            while (p2 != std::string::npos) {
                                patched.replace(p2, strlen(aB2), rB2);
                                p2 = patched.find(aB2, p2 + strlen(rB2));
                            }
                        }
                        size_t fpos12 = patched.find("onYtPageManagerAttached=function(V){");
                        if (fpos12 != std::string::npos) {
                            size_t b12 = patched.find('{', fpos12);
                            if (b12 != std::string::npos)
                                patched.insert(b12 + 1, "try{var g$=_.gU(V);console.error('[PMA] onYtPageManagerAttached gU='+(g$&&g$.tagName)+' id='+(g$&&g$.id)+' target='+(V&&V.target&&V.target.tagName))}catch(e){console.error('[PMA] err '+e.message)}");
                        }
                        size_t fpos12b = patched.find("this.pageManagerAttachedPromise.resolve()");
                        if (fpos12b != std::string::npos) {
                            patched.replace(fpos12b, strlen("this.pageManagerAttachedPromise.resolve()"),
                                "(console.error('[PMA-RESOLVE] typeof='+typeof this.pageManagerAttachedPromise),this.pageManagerAttachedPromise.resolve())");
                        }
                        /* Trace the page-data-fetched handler (leads to updatePageData). */
                        size_t fpos14 = patched.find("onYtPageDataFetched=function(V,y){");
                        if (fpos14 != std::string::npos) {
                            size_t b14 = patched.find('{', fpos14);
                            if (b14 != std::string::npos)
                                patched.insert(b14 + 1, "try{console.error('[PDF] onYtPageDataFetched y='+(y?Object.keys(y).slice(0,6).join(','):'null')+' pd='+(y&&y.pageData?'y':'n'))}catch(e){}");
                        }
                        size_t fposPPD = patched.find("publishPageData=function(V){");
                        if (fposPPD != std::string::npos) {
                            size_t bppd = patched.find('{', fposPPD);
                            if (bppd != std::string::npos)
                                patched.insert(bppd + 1, "try{var lit$={pageData:V};var d$=Object.getOwnPropertyDescriptor(lit$,'pageData');console.error('[PPD] page='+(V&&V.page)+' keys='+Object.keys(lit$).length+' desc='+JSON.stringify(d$&&{e:d$.enumerable,w:d$.writable,c:d$.configurable})+' vKeys='+(V?Object.keys(V).length:'-'))}catch(e){}");
                        }
                        /* Trace loadData's deferred navigation callback. */
                        size_t fpos15 = patched.find("this.loadDepsPromise.then(function(){");
                        if (fpos15 != std::string::npos) {
                            size_t b15 = patched.find('{', fpos15);
                            if (b15 != std::string::npos)
                                patched.insert(b15 + 1, "try{console.error('[LDD] loadDeps then runs')}catch(e){}");
                        }
                        size_t fpos16 = patched.find(".loadData=function(V){var y=this;");
                        if (fpos16 != std::string::npos) {
                            const char *a16 = ".loadData=function(V){var y=this;";
                            std::string r16 = std::string(a16) + "try{console.error('[LD] loadData entry deps='+(this.loadDepsPromise?'y':'n'))}catch(e){}";
                            patched.replace(fpos16, strlen(a16), r16);
                        }
                        /* Trace the _.mm custom-promise async pump: EQY schedules,
                         * UR$ drains via a native Promise job. */
                        size_t fpos17 = patched.find("EQY=function(V){V.executing_||(V.executing_=!0,_.WV(");
                        if (fpos17 != std::string::npos) {
                            size_t b17 = patched.find('{', fpos17);
                            if (b17 != std::string::npos)
                                patched.insert(b17 + 1, "try{window.__eqy=(window.__eqy||0)+1;if(window.__eqy<6)console.error('[EQY] schedule')}catch(e){}");
                        }
                        size_t fpos18 = patched.find("UR$=function(){for(var V;V=AvU.remove();)");
                        if (fpos18 != std::string::npos) {
                            size_t b18 = patched.find('{', fpos18);
                            if (b18 != std::string::npos)
                                patched.insert(b18 + 1, "try{window.__ur=(window.__ur||0)+1;if(window.__ur<6)console.error('[UR] drain')}catch(e){}");
                        }
                        /* Trace _.mm callback invocation. */
                        size_t fpos19 = patched.find("nQN=function(V,y,Q){y==2?V.JSC$9850_onFulfilled.call(V.context,Q)");
                        if (fpos19 != std::string::npos) {
                            size_t b19 = patched.find('{', fpos19);
                            if (b19 != std::string::npos)
                                patched.insert(b19 + 1, "try{window.__nqn=(window.__nqn||0)+1;if(window.__nqn<15)console.error('[NQN] state='+y)}catch(e){}");
                        }
                        /* Trace _.bQ event dispatches named 'attached'. */
                        size_t fpos13 = patched.find("bQ=function(V,y,Q,v){");
                        if (fpos13 != std::string::npos) {
                            size_t b13 = patched.find('{', fpos13);
                            if (b13 != std::string::npos)
                                patched.insert(b13 + 1, "try{if(y==='attached'){window.__bqa=(window.__bqa||0)+1;if(window.__bqa<8)console.error('[BQ] attached fired on '+(V&&V.tagName))}}catch(e){}");
                        }
                        if (patched.size() != scripts[i].content_len) {
                            char *nd = (char *)malloc(patched.size() + 1);
                            if (nd) {
                                memcpy(nd, patched.data(), patched.size());
                                nd[patched.size()] = '\0';
                                scripts[i].content = nd;
                                scripts[i].content_len = patched.size();
                                fprintf(stderr, "[LC-PATCH] transition trace injected\n");
                            }
                        }
                    }
                    /* Log when the scheduler's O() executor runs jobs and when the
                     * web-animations polyfill's RAF queue schedules/runs its driver. */
                    if (getenv("CYBER_TRACE_LIFECYCLE") && scripts[i].content_len > 1000 &&
                        strstr(scripts[i].content, "m.length==0&&e(d);m.push([C,q]);return C")) {
                        std::string patched(scripts[i].content, scripts[i].content_len);
                        const char *from = "m.length==0&&e(d);m.push([C,q]);return C";
                        const char *to = "if(m.length==0){console.error('[WADRV] schedule driver');e(d)}m.push([C,q]);return C";
                        size_t pos = patched.find(from);
                        if (pos != std::string::npos) {
                            patched.replace(pos, strlen(from), to);
                        }
                        const char *fromD = "function d(q){var C=m;m=[];";
                        const char *toD = "function d(q){console.error('[WADRV] driver runs qlen='+m.length);var C=m;m=[];";
                        size_t posD = patched.find(fromD);
                        if (posD != std::string::npos) {
                            patched.replace(posD, strlen(fromD), toD);
                        }
                        if (patched.size() != scripts[i].content_len) {
                            char *nd = (char *)malloc(patched.size() + 1);
                            if (nd) {
                                memcpy(nd, patched.data(), patched.size());
                                nd[patched.size()] = '\0';
                                scripts[i].content = nd;
                                scripts[i].content_len = patched.size();
                                fprintf(stderr, "[LC-PATCH] wa-driver trace injected\n");
                            }
                        }
                    }
                    /* Log jobs that error inside the scheduler's O() executor so we can
                     * see which scheduled job hangs the lifecycle jobSet. */
                    if (getenv("CYBER_TRACE_LIFECYCLE") && scripts[i].content_len > 1000 &&
                        strstr(scripts[i].content, "function O(a){try{a()}catch(b){T(b)}}")) {
                        std::string patched(scripts[i].content, scripts[i].content_len);
                        const char *from = "function O(a){try{a()}catch(b){T(b)}}";
                        const char *to = "function O(a){try{if(!window.__schedRun)window.__schedRun=1;window.__schedRunCount=(window.__schedRunCount||0)+1;if(window.__schedRunCount<20||window.__schedRunCount%500===0)console.error('[SCHED-RUN #'+window.__schedRunCount+']');a()}catch(b){console.error('[SCHED-JOB-ERR] '+(b&&b.message)+' | '+((b&&b.stack)||'').split(String.fromCharCode(10))[0]);T(b)}}";
                        size_t pos = patched.find(from);
                        if (pos != std::string::npos) {
                            patched.replace(pos, strlen(from), to);
                        }
                        /* Trace who keeps re-queueing scheduleAppLoad (the eoir park
                         * flood that starves priority-0 lifecycle jobs). */
                        const char *fromP = "function P(a,b,c,d){++a.F;";
                        const char *toP = "function P(a,b,c,d){try{if(b&&b.name==='scheduleAppLoad'){window.__salC=(window.__salC||0)+1;if(window.__salC<4)console.error('[ADD-SAL] prio='+c+' body='+(''+b).slice(0,90)+' STACK '+((new Error()).stack||'').split(String.fromCharCode(10)).slice(1,7).join(' <- '))}}catch(e){}++a.F;";
                        size_t posP = patched.find(fromP);
                        if (posP != std::string::npos) {
                            patched.replace(posP, strlen(fromP), toP);
                        }
                        /* Trace prio-2 job adds (updatePageData's deferred J). */
                        const char *fromP2 = "var e=a.F;a.h[e]=b;a.l&&!d?a.v.push({id:e,priority:c}):(a.i[c].push(e),";
                        const char *toP2 = "var e=a.F;a.h[e]=b;try{if(c===2)console.error('[SCHED-P2] add id='+e)}catch(e$7){}a.l&&!d?a.v.push({id:e,priority:c}):(a.i[c].push(e),";
                        size_t posP2 = patched.find(fromP2);
                        if (posP2 != std::string::npos) {
                            patched.replace(posP2, strlen(fromP2), toP2);
                        }
                        /* Trace V drains when queue 2 is non-empty. */
                        const char *fromV = "function V(a,b,c){a.C&&a.m===4&&a.g||R(a);";
                        const char *toV = "function V(a,b,c){try{if(a.i[2].length>0)console.error('[SCHED-V] q2='+a.i[2].length+' q3='+a.i[3].length+' m='+a.m)}catch(e$8){}a.C&&a.m===4&&a.g||R(a);";
                        size_t posV = patched.find(fromV);
                        if (posV != std::string::npos) {
                            patched.replace(posV, strlen(fromV), toV);
                        }
                        /* Trace the RAF/timeout callbacks that reset the pump handle. */
                        const char *fromT = "g.T=function(a){this.C=!0;";
                        const char *toT = "g.T=function(a){window.__tCnt=(window.__tCnt||0)+1;if(window.__tCnt<10)console.error('[SCHED-CB] T(RAF) g='+this.g);this.C=!0;";
                        size_t posT = patched.find(fromT);
                        if (posT != std::string::npos) {
                            patched.replace(posT, strlen(fromT), toT);
                        }
                        const char *fromUU = "g.U=function(){V(this)}";
                        const char *toUU = "g.U=function(){window.__u2Cnt=(window.__u2Cnt||0)+1;if(window.__u2Cnt<10)console.error('[SCHED-CB] U(timeout) g='+this.g);V(this)}";
                        size_t posUU = patched.find(fromUU);
                        if (posUU != std::string::npos) {
                            patched.replace(posUU, strlen(fromUU), toUU);
                        }
                        const char *fromU = "function U(a){for(var b=r(J),c=b.next();!c.done;c=b.next())if(a.i[c.value].length)return!0;return!1}";
                        const char *toU = "function U(a){var rr=(function(){for(var b=r(J),c=b.next();!c.done;c=b.next())if(a.i[c.value].length)return!0;return!1})();window.__uCnt=(window.__uCnt||0)+1;if(window.__uCnt<25||window.__uCnt%200===0)console.error('[SCHED-U] ret='+rr+' g='+a.g+' m='+a.m);return rr}";
                        size_t posU = patched.find(fromU);
                        if (posU != std::string::npos) {
                            patched.replace(posU, strlen(fromU), toU);
                        }
                        const char *fromSt = "g.start=function(){this.D=!1;if(this.g===0)switch(this.m=Q(this),this.m){";
                        const char *toSt = "g.start=function(){window.__stCnt=(window.__stCnt||0)+1;if(window.__stCnt<25||window.__stCnt%200===0)console.error('[SCHED-START] g='+this.g+' m0='+this.m);this.D=!1;if(this.g===0)switch(this.m=Q(this),this.m){";
                        size_t posSt = patched.find(fromSt);
                        if (posSt != std::string::npos) {
                            patched.replace(posSt, strlen(fromSt), toSt);
                        }
                        const char *fromRaf = "case 3:this.g=window.requestAnimationFrame(this.M);break;";
                        const char *toRaf = "case 3:this.g=(console.error('[SCHED-ARM] raf m='+this.m),window.requestAnimationFrame(this.M));console.error('[SCHED-ARMED] id='+this.g);break;";
                        size_t posRaf = patched.find(fromRaf);
                        if (posRaf != std::string::npos) {
                            patched.replace(posRaf, strlen(fromRaf), toRaf);
                        }
                        /* Trace queue-0 state: pushes and drain opportunities. */
                        const char *fromQ = "function Q(a){if(a.i[8].length){if(a.C)return 4;if(!document.hidden&&a.B)return 3}for(var b=5;b>=a.j;b--)if(a.i[b].length>0)return b>0?!document.hidden&&a.B?3:2:1;return 0}";
                        const char *toQ = "function Q(a){var r=(function(){if(a.i[8].length){if(a.C)return 4;if(!document.hidden&&a.B)return 3}for(var b=5;b>=a.j;b--)if(a.i[b].length>0)return b>0?!document.hidden&&a.B?3:2:1;return 0})();if(a.i[0].length>0){window.__q0c=(window.__q0c||0)+1;if(window.__q0c<30)console.error('[SCHED-Q] ret='+r+' q0='+a.i[0].length+' q8='+(a.i[8]&&a.i[8].length)+' C='+a.C+' G='+a.G+' m='+a.m)}return r}";
                        size_t posQ = patched.find(fromQ);
                        if (posQ != std::string::npos) {
                            patched.replace(posQ, strlen(fromQ), toQ);
                        }
                        if (patched.size() != scripts[i].content_len) {
                            char *nd = (char *)malloc(patched.size() + 1);
                            if (nd) {
                                memcpy(nd, patched.data(), patched.size());
                                nd[patched.size()] = '\0';
                                scripts[i].content = nd;
                                scripts[i].content_len = patched.size();
                                fprintf(stderr, "[LC-PATCH] sched-job-err trace injected\n");
                            }
                        }
                    }
                    /* DIAGNOSTIC: force the lifecycle init to skip the jobSet wait, to
                     * test whether the "rendering" phase (and metadata) runs once the
                     * jobSet hang is bypassed.  CYBER_FORCE_RENDER=1. */
                    if (getenv("CYBER_FORCE_RENDER") && scripts[i].content_len > 1000000 &&
                        strstr(scripts[i].content, ".jobSet?v.yield(y.jobSet.completedResolver.promise,4):v.jumpTo(4)")) {
                        std::string patched(scripts[i].content, scripts[i].content_len);
                        const char *from = ".jobSet?v.yield(y.jobSet.completedResolver.promise,4):v.jumpTo(4)";
                        const char *to = ".jobSet?v.jumpTo(4):v.jumpTo(4)";
                        size_t pos = patched.find(from);
                        if (pos != std::string::npos) {
                            patched.replace(pos, strlen(from), to);
                            char *nd = (char *)malloc(patched.size() + 1);
                            if (nd) {
                                memcpy(nd, patched.data(), patched.size());
                                nd[patched.size()] = '\0';
                                scripts[i].content = nd;
                                scripts[i].content_len = patched.size();
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
            std::string patched(scripts[i].content, scripts[i].content_len);
            apply_youtube_player_bootstrap_patch(patched);
            if (patched.size() != scripts[i].content_len ||
                memcmp(patched.data(), scripts[i].content, scripts[i].content_len) != 0) {
                char *new_data = (char *)malloc(patched.size() + 1);
                if (new_data) {
                    memcpy(new_data, patched.data(), patched.size());
                    new_data[patched.size()] = '\0';
                    free((void *)scripts[i].content);
                    scripts[i].content = new_data;
                    scripts[i].content_len = patched.size();
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
extern "C" bool html_extract_visitor_data(const char *html, char *out_vd, size_t out_len) {
    (void)html;
    if (out_vd && out_len > 0) out_vd[0] = '\0';
    return false;
}

/* Domain-specific ytInitialPlayerResponse media extraction removed. */
extern "C" bool html_extract_yt_player_response_media(const char *html, bool prefer_video,
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
extern "C" bool html_extract_media_url(const char *html, HtmlMediaCandidate *outCandidate,
                            char *err, size_t errLen) {
    (void)html;
    if (outCandidate) memset(outCandidate, 0, sizeof(HtmlMediaCandidate));
    if (err && errLen > 0) {
        strncpy(err, "Media extraction disabled", errLen - 1);
        err[errLen - 1] = '\0';
    }
    return false;
}
