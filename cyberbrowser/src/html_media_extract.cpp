#include <string.h>
#include <string>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include "html_media_extract.h"
#include "http_download.h"
#include "js_quickjs.h"
#include "platform.h"
#include "url_utils.h"

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

// Some third-party polyfills are not safe to execute inside this emulator:
// they monkey-patch native DOM prototypes and corrupt state.  Skip only those
// known-bad polyfills; large application bundles must be allowed to run
// because they carry bootstrap logic that the page expects.
static bool is_unsafe_external_script(const char *url) {
    if (!url) return false;
    static const char *skip_patterns[] = {
        // Skip the Polymer ES5 adapter. The adapter's wrapper class corrupts
        // the QuickJS bytecode when combined with our native HTMLElement
        // upgrade path.  The ShadyDOM polyfill is now allowed to run because
        // the missing Node type constants that made it throw "not a function"
        // have been added.
        "custom-elements-es5-adapter",
        "webcomponents-sd-shadycss",
        "webcomponents-all-noPatch",
        NULL
    };
    for (const char **p = skip_patterns; *p; p++) {
        if (strstr(url, *p)) return true;
    }
    return false;
}

// Execute all page scripts (inline + external) in document order.
// Fetches external scripts, runs everything through js_quickjs_exec_scripts,
// and pumps timers/microtasks after each network response.

/* Patch YouTube's dependency-injector source so that resolving PAGE_TOKEN
 * before ytd-page-manager has registered its provider returns a safe proxy
 * instead of throwing.  This removes a hard failure in ytd-app's
 * connectedCallback.  The patch is intentionally conservative: it only touches
 * scripts that contain the exact minified patterns used by the current
 * kevlar runtime/base module.
 */
static void apply_youtube_page_token_patch(std::string &src) {
    // (1) Capture the real page-manager value when the provider is added.
    const char *set_pat = "this.providers.set(c.provide,c);";
    const char *throw_pat = "throw Error(\"Zc`\"+k);";
    bool found_set = (src.find(set_pat) != std::string::npos);
    bool found_throw = (src.find(throw_pat) != std::string::npos);
    fprintf(stderr, "[PAGE-TOKEN-PATCH] set=%d throw=%d size=%zu\n", found_set?1:0, found_throw?1:0, src.size());
    fflush(stderr);

    const char *set_repl =
        "if(c&&c.provide){var pt=c.provide;"
        "if((pt.key&&pt.key.name===\"PAGE_TOKEN\")||pt.name===\"PAGE_TOKEN\"){"
        "var __pmv=c.useValue;"
        "if(__pmv===undefined&&c.useClass)try{__pmv=new c.useClass();}catch(_e){__pmv=null;}"
        "if(__pmv===undefined&&c.useFactory)try{__pmv=c.useFactory();}catch(_e){__pmv=null;}"
        "this.__cyber_page_manager=__pmv||null;}"
        "this.providers.set(c.provide,c);}";
    size_t pos = 0;
    while ((pos = src.find(set_pat, pos)) != std::string::npos) {
        src.replace(pos, strlen(set_pat), set_repl);
        pos += strlen(set_repl);
    }

    // (2) Return a proxy for PAGE_TOKEN when no provider exists yet.
    const char *throw_repl =
        "if(k&&k.name===\"PAGE_TOKEN\"){"
        "var __pm=c.__cyber_page_manager;"
        "if(__pm)return __pm;"
        "return c.__cyber_pm_proxy||(c.__cyber_pm_proxy=new Proxy({},"
        "{get:function(t,p){var r=c.__cyber_page_manager;"
        "return r&&p in r?r[p]:function(){return null;}}}));}"
        "throw Error(\"Zc`\"+k);";
    pos = src.find(throw_pat);
    if (pos != std::string::npos) {
        src.replace(pos, strlen(throw_pat), throw_repl);
    }
}

/* YouTube's scoped DOM wrapper (e.g. e19/iaD, used by _.WJ) is sometimes
 * constructed with a null node in our ShadyDOM environment.  Reading
 * __shady_native_children on a wrapper whose node is null aborts
 * ytd-app.connectedCallback.  Guard the constructor so a null/undefined node
 * falls back to an empty DocumentFragment; this keeps the accessor reads from
 * throwing and lets stamping continue.  The variable name is minified, so a
 * regex matches the pattern.
 */
static void apply_youtube_e19_null_guard_patch(std::string &src) {
    fprintf(stderr, "[E19-PATCH] scan source size=%zu\n", src.size());
    fflush(stderr);
    const char *pat = "this.node=";
    size_t pos = 0;
    bool applied = false;
    int candidates = 0;
    while ((pos = src.find(pat, pos)) != std::string::npos) {
        size_t start = pos;
        pos += strlen(pat);
        size_t var_start = pos;
        while (pos < src.size() && (isalnum((unsigned char)src[pos]) || src[pos] == '_' || src[pos] == '$')) {
            pos++;
        }
        std::string var(src, var_start, pos - var_start);
        candidates++;
        std::string expected = var + " instanceof ShadowRoot?" + var + ".host:" + var;
        if (src.compare(var_start, expected.size(), expected) == 0) {
            std::string repl = "this.node=" + var + " instanceof ShadowRoot?" + var +
                               ".host:(" + var + "==null?(console.error('e19 wrapper fallback used',new Error().stack),document.createDocumentFragment()):" + var + ")";
            src.replace(start, (var_start - start) + expected.size(), repl);
            applied = true;
            break;
        }
        pos = start + 1;
    }
    fprintf(stderr, "[E19-PATCH] candidates=%d applied=%d\n", candidates, applied?1:0);
    fflush(stderr);
    if (applied) {
        fprintf(stderr, "[E19-PATCH] applied scoped DOM wrapper null guard\n");
        fflush(stderr);
    }
}

/* YouTube's scoped DOM wrapper (hp / t$) treats a plain DocumentFragment as a
 * shadow root if it walks like one.  Polymer stamps templates into fragments
 * before attachShadow is called; without this branch _.Z1(fragment) wraps the
 * host element instead and querySelector("#button") misses the stamped
 * content, leaving e19 wrappers with a null node.
 */
static void apply_youtube_scoped_root_patch(std::string &src) {
    // The ShadyDOM scoped wrapper constructor (t$/hp) only recognises a real
    // ShadowRoot.  Polymer stamps templates into plain DocumentFragments before
    // attachShadow runs, so _.Z1(fragment) wrongly wraps the host element and
    // querySelector("#button") misses the stamped content.  Treat a fragment
    // like a shadow root, and fall back to the fragment itself when host is
    // missing.
    const char *pat = "instanceof ShadowRoot)this.host=";
    size_t pos = src.find(pat);
    if (pos == std::string::npos) {
        fprintf(stderr, "[SCOPED-ROOT-PATCH] pattern not found\n");
        fflush(stderr);
        return;
    }
    // Extract the variable name, e.g. "if(d instanceof ShadowRoot"
    size_t if_start = pos;
    while (if_start > 0 && src[if_start - 1] != '(') if_start--;
    std::string var(src, if_start, pos - if_start);

    std::string old_cond = "if(" + var + " instanceof ShadowRoot)";
    std::string new_cond = "if(" + var + " instanceof ShadowRoot||" + var + " instanceof DocumentFragment)";
    size_t cond_pos = src.find(old_cond, if_start > 3 ? if_start - 3 : 0);
    if (cond_pos != std::string::npos) {
        src.replace(cond_pos, old_cond.size(), new_cond);
    }

    // Insert a fallback for the host expression:  ...(_.X6(d.host),this.root=d;
    // becomes  ...(_.X6(d.host)||d,this.root=d;
    std::string host_tail = ".host),this.root=" + var + ";";
    size_t host_pos = src.find(host_tail, pos);
    if (host_pos != std::string::npos) {
        std::string new_host_tail = ".host)||" + var + ",this.root=" + var + ";";
        src.replace(host_pos, host_tail.size(), new_host_tail);
    }

    fprintf(stderr, "[SCOPED-ROOT-PATCH] applied for var=%s\n", var.c_str());
    fflush(stderr);
}

/* Polymer's ShadyDOM helpers save/wrap Node.prototype mutation methods.
 * Later module bundles re-run these helpers after earlier bundles have already
 * wrapped Node.prototype, so the captured "native" reference becomes a wrapper
 * that calls an undefined deeper native.  This makes appendChild/insertBefore
 * throw "cannot read property 'call' of undefined".  Guard both helpers so
 * they only execute once, on the first bundle, when the real C functions are
 * still exposed on Node.prototype.
 */
static void apply_youtube_dom_patch_patch(std::string &src) {
    size_t count = 0;

    /* Guard b6B (lifecycle wrapper that captures insertBefore/removeChild/etc).
     * Inject a global flag check at the very start of the function body. */
    const char *b6b_pat = "b6B=function(){";
    size_t b6b_pos = src.find(b6b_pat);
    while (b6b_pos != std::string::npos) {
        const char *b6b_repl = "b6B=function(){if(window.__cyber_b6b_done)return;window.__cyber_b6b_done=!0;";
        src.replace(b6b_pos, strlen(b6b_pat), b6b_repl);
        fprintf(stderr, "[DOM-PATCH] guarded b6B at %zu\n", b6b_pos);
        count++;
        b6b_pos = src.find(b6b_pat, b6b_pos + strlen(b6b_repl));
    }

    /* Guard qVB (descriptor saver that writes __shady_native_*). */
    const char *qvb_pat = "qVB=function(){";
    size_t qvb_pos = src.find(qvb_pat);
    while (qvb_pos != std::string::npos) {
        const char *qvb_repl = "qVB=function(){if(window.__cyber_qvb_done)return;window.__cyber_qvb_done=!0;";
        src.replace(qvb_pos, strlen(qvb_pat), qvb_repl);
        fprintf(stderr, "[DOM-PATCH] guarded qVB at %zu\n", qvb_pos);
        count++;
        qvb_pos = src.find(qvb_pat, qvb_pos + strlen(qvb_repl));
    }

    fprintf(stderr, "[DOM-PATCH] scan size=%zu guards=%zu\n", src.size(), count);
    fflush(stderr);
}

/* The webcomponents-sd polyfill captures Element.prototype.matches into a local
 * variable `ma` at load time.  In our engine the property may not be a function
 * at that exact moment, so `ma` ends up undefined and ShadyDOM's scoped
 * querySelector throws "not a function".  Patch the captured usages to fall
 * back to the live Element.prototype.matches.
 */
static void apply_shadydom_matches_patch(std::string &src) {
    const char *pat1 = "ma.call(e,a)";
    size_t pos = src.find(pat1);
    if (pos != std::string::npos) {
        src.replace(pos, strlen(pat1), "(function(){try{return ma.call(e,a)}catch(__ex){console.error(\"matches-throw\",String(__ex),__ex&&__ex.stack);return false}})()");
        fprintf(stderr, "[MATCHES-PATCH] patched ma.call(e,a)\n");
        fflush(stderr);
        FILE *f = fopen("webcomponents_patched.js", "wb");
        if (f) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
    }
    const char *pat2 = "return ma.call(d,a)";
    pos = src.find(pat2);
    if (pos != std::string::npos) {
        src.replace(pos, strlen(pat2), "return (function(){try{return ma.call(d,a)}catch(__ex){console.error(\"matches-throw-d\",String(__ex),__ex&&__ex.stack);return false}})()");
        fprintf(stderr, "[MATCHES-PATCH] patched ma.call(d,a)\n");
        fflush(stderr);
    }
    // Wrap rc.querySelector in a diagnostic try/catch so we can see exactly
    // which sub-expression throws "not a function".
    const char *qs_start = "querySelector:function(a){";
    pos = src.find(qs_start);
    if (pos != std::string::npos) {
        size_t body_start = pos + strlen(qs_start);
        size_t body_end = src.find(",querySelectorAll:function", body_start);
        if (body_end != std::string::npos) {
            std::string wrapped = "try{" + src.substr(body_start, body_end - body_start) + "}catch(__ex){console.error('qs-throw',String(__ex),'K=',K,'this=',this&&this.nodeName,'sel=',a,'ma=',typeof ma,'nc=',typeof nc,'p=',typeof p,'slice=',typeof Array.prototype.slice);throw __ex;}";
            src.replace(body_start, body_end - body_start, wrapped);
            fprintf(stderr, "[MATCHES-PATCH] wrapped querySelector\n");
            fflush(stderr);
        }
    }
}

extern "C" bool html_execute_page_scripts(const char *html, JsExecResult *out_result) {
    if (!html || !out_result) return false;
    memset(out_result, 0, sizeof(JsExecResult));
    
    ScriptInfo scripts[MAX_SCRIPTS];
    memset(scripts, 0, sizeof(scripts));
    int script_count = extract_scripts_in_order(html, scripts, MAX_SCRIPTS);
    if (script_count == 0) {
        LOG_ERROR("No scripts found in HTML");
        return false;
    }
    
    LOG_INFO("Found %d scripts to execute", script_count);
    
    // External scripts up to ~64 MiB are now safe to fetch.  The parser
    // nesting-state buffer has been moved from the C stack to the GC heap,
    // and cyberbrowser.exe is linked with a 64 MiB C stack, so large
    // ~10 MiB application bundles no longer overflow during JS_Eval.
    // Polyfills that are known to leave the JS heap in a corrupted state in our
    // emulator (e.g. the ShadyDOM webcomponents-sd polyfill) are still skipped
    // by URL.
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

            bool result = http_get_to_memory(scripts[i].url, &buffer, error, sizeof(error));
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

                    // Apply a source-level PAGE_TOKEN fallback patch to the
                    // YouTube runtime/base module before execution.
                    std::string patched(buffer.data, buffer.size);
                    apply_youtube_page_token_patch(patched);
                    apply_youtube_e19_null_guard_patch(patched);
                    apply_youtube_scoped_root_patch(patched);
                    apply_shadydom_matches_patch(patched);
                    apply_youtube_dom_patch_patch(patched);
                    if (patched.size() != buffer.size) {
                        char *new_data = (char *)malloc(patched.size() + 1);
                        if (new_data) {
                            memcpy(new_data, patched.data(), patched.size());
                            new_data[patched.size()] = '\0';
                            http_free_buffer(&buffer);
                            buffer.data = new_data;
                            buffer.size = patched.size();
                        }
                    } else {
                        // The patch function may have rewritten the content in
                        // place without changing the size; copy it back.
                        memcpy(buffer.data, patched.data(), patched.size());
                    }

                    scripts[i].content = buffer.data;
                    scripts[i].content_len = buffer.size;
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
