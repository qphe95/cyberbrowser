#include <string.h>
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
// Handles \xNN format escape sequences commonly found in YouTube's JSON
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
#define MAX_HTML_SIZE (20 * 1024 * 1024)  // 20MB max for large YouTube pages with big JSON payloads

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

// Media stream structure
typedef struct MediaStream {
    char url[2048];
    char mime_type[128];
    char quality[32];
    int width;
    int height;
    int itag;
    bool has_cipher;  // True if URL needs signature decryption
} MediaStream;

typedef struct {
    const char *html;
    size_t html_len;
    MediaStream *streams;
    int max_streams;
    int stream_count;
} ExtractContext;

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
static char *url_decode(const char *src) {
    if (!src) return NULL;
    
    size_t len = strlen(src);
    char *decoded = (char*)malloc(len + 1);
    if (!decoded) return NULL;
    
    char *dst = decoded;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '%' && i + 2 < len) {
            int hex1 = tolower(src[i + 1]);
            int hex2 = tolower(src[i + 2]);
            int val1 = (hex1 >= 'a') ? (hex1 - 'a' + 10) : (hex1 - '0');
            int val2 = (hex2 >= 'a') ? (hex2 - 'a' + 10) : (hex2 - '0');
            if (val1 >= 0 && val1 < 16 && val2 >= 0 && val2 < 16) {
                *dst++ = (char)((val1 << 4) | val2);
                i += 2;
                continue;
            }
        }
        *dst++ = src[i];
    }
    *dst = '\0';
    return decoded;
}

// Parse signatureCipher and extract URL + signature
static bool parse_signature_cipher(const char *cipher_text, char *out_url, size_t out_url_len, 
                                    char *out_sig, size_t out_sig_len) {
    if (!cipher_text || !out_url) return false;
    
    out_url[0] = '\0';
    if (out_sig) out_sig[0] = '\0';
    
    // signatureCipher format: url=XXX&sig=YYY or url=XXX&s=YYY
    const char *url_start = strstr(cipher_text, "url=");
    if (!url_start) return false;
    
    url_start += 4;
    const char *url_end = strchr(url_start, '&');
    
    char *decoded_url;
    if (url_end) {
        char encoded[1024];
        size_t len = url_end - url_start;
        if (len >= sizeof(encoded)) len = sizeof(encoded) - 1;
        strncpy(encoded, url_start, len);
        encoded[len] = '\0';
        decoded_url = url_decode(encoded);
    } else {
        char encoded[1024];
        strncpy(encoded, url_start, sizeof(encoded) - 1);
        encoded[sizeof(encoded) - 1] = '\0';
        decoded_url = url_decode(encoded);
    }
    
    if (!decoded_url) return false;
    
    strncpy(out_url, decoded_url, out_url_len - 1);
    out_url[out_url_len - 1] = '\0';
    free(decoded_url);
    
    // Extract signature if present
    if (out_sig) {
        const char *sig_start = strstr(cipher_text, "sig=");
        if (!sig_start) sig_start = strstr(cipher_text, "s=");
        if (sig_start) {
            sig_start = strchr(sig_start, '=');
            if (sig_start) {
                sig_start++;
                const char *sig_end = strchr(sig_start, '&');
                if (sig_end) {
                    size_t len = sig_end - sig_start;
                    if (len >= out_sig_len) len = out_sig_len - 1;
                    strncpy(out_sig, sig_start, len);
                    out_sig[len] = '\0';
                } else {
                    strncpy(out_sig, sig_start, out_sig_len - 1);
                    out_sig[out_sig_len - 1] = '\0';
                }
            }
        }
    }
    
    return out_url[0] != '\0';
}

// Decode a YouTube signatureCipher/s+sp+url string into a playable URL.
// Input format:  s=<sig>&sp=<sig_param>&url=<base_url>
// Output format: <base_url>&<sig_param>=<sig>
// Returns true if decoding succeeded and out_playable is populated.
static bool decode_signature_cipher(const char *cipher_text, char *out_playable, size_t out_len) {
    if (!cipher_text || !out_playable || out_len == 0) return false;
    out_playable[0] = '\0';

    // Must contain both s= and url=
    const char *s_start = strstr(cipher_text, "s=");
    const char *url_start = strstr(cipher_text, "url=");
    if (!s_start || !url_start) return false;

    // Extract and URL-decode the base URL
    url_start += 4;
    const char *url_end = strchr(url_start, '&');
    char *decoded_url = NULL;
    if (url_end) {
        char tmp[2048];
        size_t len = (size_t)(url_end - url_start);
        if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
        memcpy(tmp, url_start, len);
        tmp[len] = '\0';
        decoded_url = url_decode(tmp);
    } else {
        decoded_url = url_decode(url_start);
    }
    if (!decoded_url || decoded_url[0] == '\0') {
        free(decoded_url);
        return false;
    }

    // Extract and URL-decode the signature value
    s_start += 2;
    const char *s_end = strchr(s_start, '&');
    char *decoded_sig = NULL;
    if (s_end) {
        char tmp[1024];
        size_t len = (size_t)(s_end - s_start);
        if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
        memcpy(tmp, s_start, len);
        tmp[len] = '\0';
        decoded_sig = url_decode(tmp);
    } else {
        decoded_sig = url_decode(s_start);
    }
    if (!decoded_sig || decoded_sig[0] == '\0') {
        free(decoded_url);
        free(decoded_sig);
        return false;
    }

    // Extract the signature parameter name (sp=); default to "sig"
    const char *sp_param = "sig";
    const char *sp_start = strstr(cipher_text, "sp=");
    if (sp_start) {
        sp_start += 3;
        const char *sp_end = strchr(sp_start, '&');
        static char sp_buf[32];
        size_t sp_len = sp_end ? (size_t)(sp_end - sp_start) : strlen(sp_start);
        if (sp_len > 0 && sp_len < sizeof(sp_buf)) {
            memcpy(sp_buf, sp_start, sp_len);
            sp_buf[sp_len] = '\0';
            sp_param = sp_buf;
        }
    }

    // URL-encode the signature value so that =, +, &, % inside it don't break the query string
    char encoded_sig[2048];
    size_t es = 0;
    for (const char *p = decoded_sig; *p && es < sizeof(encoded_sig) - 4; p++) {
        if (*p == '=') {
            encoded_sig[es++] = '%'; encoded_sig[es++] = '3'; encoded_sig[es++] = 'D';
        } else if (*p == '+') {
            encoded_sig[es++] = '%'; encoded_sig[es++] = '2'; encoded_sig[es++] = 'B';
        } else if (*p == '&') {
            encoded_sig[es++] = '%'; encoded_sig[es++] = '2'; encoded_sig[es++] = '6';
        } else if (*p == '%') {
            encoded_sig[es++] = '%'; encoded_sig[es++] = '2'; encoded_sig[es++] = '5';
        } else {
            encoded_sig[es++] = *p;
        }
    }
    encoded_sig[es] = '\0';

    // Build the playable URL: <decoded_url>&<sp_param>=<encoded_sig>
    size_t needed = strlen(decoded_url) + 1 + strlen(sp_param) + 1 + strlen(encoded_sig) + 1;
    if (needed > out_len) {
        free(decoded_url);
        free(decoded_sig);
        return false;
    }

    // Use & or ? depending on whether decoded_url already has query params
    const char *sep = strchr(decoded_url, '?') ? "&" : "?";
    snprintf(out_playable, out_len, "%s%s%s=%s", decoded_url, sep, sp_param, encoded_sig);

    free(decoded_url);
    free(decoded_sig);
    return true;
}



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
// These contain initialization code like ytcfg, ytInitialData, etc.
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
                snprintf(temp, sizeof(temp), "https://www.youtube.com%s", scripts[count].url);
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
// known-bad polyfills; the large YouTube application bundles (kevlar_base,
// ytmainappweb) must be allowed to run because they carry player bootstrap
// logic that the page expects.
static bool is_unsafe_external_script(const char *url) {
    if (!url) return false;
    static const char *skip_patterns[] = {
        // Polyfills that monkey-patch native DOM prototypes and corrupt state.
        // webcomponents-sd is provided natively by the emulator, so the external
        // bundle is not fetched.
        "webcomponents-sd",
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
    // and cyberbrowser.exe is linked with a 64 MiB C stack, so YouTube's
    // ~10 MiB kevlar_base bundle no longer overflows during JS_Eval.
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
    // which is why YouTube rendered as a blank white screen.
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

// Extract YouTube video ID from URL
static bool extract_yt_video_id(const char *url, char *out_id, size_t out_len) {
    if (!url || !out_id || out_len == 0) return false;
    
    const char *patterns[] = {
        "?v=",
        "&v=",
        "/v/",
        "/embed/",
        ".be/",
        NULL
    };
    
    for (int i = 0; patterns[i]; i++) {
        const char *p = strstr(url, patterns[i]);
        if (p) {
            p += strlen(patterns[i]);
            size_t len = 0;
            while (p[len] && (isalnum((unsigned char)p[len]) || p[len] == '_' || p[len] == '-')) {
                len++;
            }
            if (len >= 11 && len < out_len) {
                strncpy(out_id, p, len);
                out_id[len] = '\0';
                return true;
            }
        }
    }
    
    return false;
}

// Execute player scripts and get captured URLs
// Returns number of URLs captured, fills urls array
/* Extract visitorData from HTML by executing inline scripts in QuickJS.
 * This performs true browser emulation: the watch page scripts run and populate
 * window.ytcfg, then we read ytcfg.get('VISITOR_DATA') from the live JS objects.
 * 
 * To avoid hanging on huge data payloads or player scripts, we only execute
 * scripts that contain "ytcfg" and are under a size threshold. This typically
 * reduces execution from 20+ scripts to 1-3 small initialization scripts.
 */
bool html_extract_visitor_data(const char *html, char *out_vd, size_t out_len) {
    if (!html || !out_vd || out_len == 0) {
        return false;
    }
    
    if (!g_js_context) {
        LOG_INFO("QuickJS context not available, cannot execute scripts");
        return false;
    }
    
    char *scripts[MAX_SCRIPTS];
    int script_count = html_extract_inline_scripts(html, scripts, MAX_SCRIPTS);
    if (script_count == 0) {
        LOG_INFO("No inline scripts found in HTML");
        return false;
    }
    
    /* Filter: only execute scripts that mention ytcfg and are reasonably small.
     * YouTube watch pages have many inline scripts: config init (small),
     * data payloads like ytInitialPlayerResponse (huge, 100KB+), and player
     * code (medium-to-large). We only need the config init scripts. */
    const char *ytcfg_scripts[MAX_SCRIPTS];
    size_t ytcfg_script_lens[MAX_SCRIPTS];
    int ytcfg_count = 0;
    const size_t MAX_SCRIPT_SIZE = 2000000; /* 2MB threshold - ytcfg.set({...}) data payloads can be large */
    
    for (int i = 0; i < script_count && ytcfg_count < MAX_SCRIPTS; i++) {
        size_t len = strlen(scripts[i]);
        if (len < MAX_SCRIPT_SIZE && strstr(scripts[i], "ytcfg") != NULL) {
            ytcfg_scripts[ytcfg_count] = scripts[i];
            ytcfg_script_lens[ytcfg_count] = len;
            ytcfg_count++;
        }
    }
    
    LOG_INFO("Extracted %d inline scripts, filtered to %d ytcfg scripts for execution",
             script_count, ytcfg_count);
    
    if (ytcfg_count == 0) {
        for (int i = 0; i < script_count; i++) {
            free(scripts[i]);
        }
        LOG_INFO("No ytcfg-related scripts found, skipping JS execution");
        return false;
    }
    
    static const char *visitor_data_expr =
        "(function() {"
        "  if (typeof ytcfg !== 'undefined' && ytcfg) {"
        "    if (typeof ytcfg.get === 'function') {"
        "      var v = ytcfg.get('VISITOR_DATA');"
        "      if (v) return v;"
        "    }"
        "    if (typeof ytcfg.d === 'function') {"
        "      var d = ytcfg.d();"
        "      if (d && d.VISITOR_DATA) return d.VISITOR_DATA;"
        "    }"
        "  }"
        "  if (typeof window !== 'undefined' && window.ytcfg) {"
        "    if (typeof window.ytcfg.get === 'function') {"
        "      var v = window.ytcfg.get('VISITOR_DATA');"
        "      if (v) return v;"
        "    }"
        "  }"
        "  return null;"
        "})()";
    
    bool found = js_quickjs_extract_value(ytcfg_scripts, ytcfg_script_lens, ytcfg_count,
                                          visitor_data_expr, out_vd, out_len);
    
    for (int i = 0; i < script_count; i++) {
        free(scripts[i]);
    }
    
    if (found) {
        LOG_INFO("Extracted visitorData via JS execution (len=%zu)", strlen(out_vd));
    } else {
        LOG_INFO("JS execution did not yield visitorData");
    }
    
    return found;
}

static bool is_audio_itag_url(const char *url) {
    return strstr(url, "itag=140") || strstr(url, "itag=139") ||
           strstr(url, "itag=251") || strstr(url, "itag=250") ||
           strstr(url, "itag=249");
}

static void derive_mime_from_url(const char *url, char *out_mime, size_t out_mime_len) {
    if (!out_mime || out_mime_len == 0) return;
    if (strstr(url, "mime=audio/webm") || strstr(url, "mime=video/webm")) {
        if (strstr(url, "mime=audio/webm") || is_audio_itag_url(url)) {
            strncpy(out_mime, "audio/webm", out_mime_len - 1);
        } else {
            strncpy(out_mime, "video/webm", out_mime_len - 1);
        }
    } else if (strstr(url, "mime=audio")) {
        strncpy(out_mime, "audio/mp4", out_mime_len - 1);
    } else if (strstr(url, "mime=video")) {
        strncpy(out_mime, "video/mp4", out_mime_len - 1);
    } else if (is_audio_itag_url(url)) {
        strncpy(out_mime, "audio/mp4", out_mime_len - 1);
    } else {
        strncpy(out_mime, "video/mp4", out_mime_len - 1);
    }
    out_mime[out_mime_len - 1] = '\0';
}

extern "C" bool html_select_best_media_url(const JsExecResult *result,
                                           bool prefer_video,
                                           char *out_url, size_t out_url_len,
                                           char *out_mime, size_t out_mime_len) {
    if (!result || !out_url || out_url_len == 0) return false;
    out_url[0] = '\0';
    if (out_mime && out_mime_len > 0) out_mime[0] = '\0';

    const char *urls[JS_MAX_CAPTURED_URLS];
    int url_count = 0;
    for (int i = 0; i < result->captured_url_count && url_count < JS_MAX_CAPTURED_URLS; i++) {
        if (strstr(result->captured_urls[i], "googlevideo.com")) {
            urls[url_count++] = result->captured_urls[i];
        }
    }
    if (url_count == 0) return false;

    auto is_preferred_itag = [&](const char *url) -> bool {
        if (prefer_video) {
            return strstr(url, "itag=22") || strstr(url, "itag=78") ||
                   strstr(url, "itag=59") || strstr(url, "itag=18");
        } else {
            return strstr(url, "itag=140") || strstr(url, "itag=139") ||
                   strstr(url, "itag=251");
        }
    };

    const char *best = NULL;

    // 1. Preferred non-SABR itag= URL
    for (int i = 0; i < url_count && !best; i++) {
        if (strstr(urls[i], "itag=") && !strstr(urls[i], "sabr=1") &&
            is_preferred_itag(urls[i])) {
            best = urls[i];
        }
    }
    // 2. Any non-SABR itag= URL matching the audio/video preference
    for (int i = 0; i < url_count && !best; i++) {
        if (strstr(urls[i], "itag=") && !strstr(urls[i], "sabr=1")) {
            bool audio = is_audio_itag_url(urls[i]);
            if (prefer_video ? !audio : audio) {
                best = urls[i];
            }
        }
    }
    // 3. Fallback to any non-SABR itag= URL
    for (int i = 0; i < url_count && !best; i++) {
        if (strstr(urls[i], "itag=") && !strstr(urls[i], "sabr=1")) {
            best = urls[i];
        }
    }
    // 4. Decrypted sig= URL without sabr=1/initplayback
    for (int i = 0; i < url_count && !best; i++) {
        if ((strstr(urls[i], "sig=") || strstr(urls[i], "signature=")) &&
            !strstr(urls[i], "sabr=1") && !strstr(urls[i], "initplayback")) {
            best = urls[i];
        }
    }
    // 5. Decodable signatureCipher
    for (int i = 0; i < url_count && !best; i++) {
        char decoded[2048];
        if (decode_signature_cipher(urls[i], decoded, sizeof(decoded))) {
            best = urls[i];
        }
    }
    // 6. First googlevideo URL
    if (!best) best = urls[0];

    char selected_url[2048];
    if (decode_signature_cipher(best, selected_url, sizeof(selected_url))) {
        LOG_INFO("Decoded signatureCipher to playable URL");
    } else {
        strncpy(selected_url, best, sizeof(selected_url) - 1);
        selected_url[sizeof(selected_url) - 1] = '\0';
    }

    strncpy(out_url, selected_url, out_url_len - 1);
    out_url[out_url_len - 1] = '\0';

    derive_mime_from_url(selected_url, out_mime, out_mime_len);

    LOG_INFO("Selected URL: %.50s...", out_url);
    return true;
}

/* Find the URL of YouTube's base.js player bundle in the HTML.
 * Returns true if a base.js URL was found and written to out_url. */
static bool find_base_js_url(const char *html, char *out_url, size_t out_len) {
    if (!html || !out_url || out_len == 0) return false;
    out_url[0] = '\0';

    const char *needle = "/base.js";
    const char *p = html;
    while ((p = strstr(p, needle)) != NULL) {
        const char *end = p + strlen(needle);
        const char *start = p;
        while (start > html && start[-1] != '"' && start[-1] != '\'' && start[-1] != ' ') start--;
        size_t len = (size_t)(end - start);
        if (len > 0 && len < out_len) {
            memcpy(out_url, start, len);
            out_url[len] = '\0';
            /* Resolve relative URL */
            if (out_url[0] == '/' && out_url[1] != '/') {
                char tmp[2048];
                snprintf(tmp, sizeof(tmp), "https://www.youtube.com%s", out_url);
                strncpy(out_url, tmp, out_len - 1);
                out_url[out_len - 1] = '\0';
            }
            return true;
        }
        p = end;
    }
    return false;
}

/* Execute YouTube's base.js in the current JS context so that _yt_player is
 * defined. Returns true if _yt_player is present after execution. */
static bool load_and_execute_base_js(JSContextHandle ctx, const char *html) {
    if (!ctx || !html) return false;

    char base_js_url[2048] = {0};
    if (!find_base_js_url(html, base_js_url, sizeof(base_js_url))) {
        LOG_WARN("base.js URL not found in HTML");
        return false;
    }
    LOG_INFO("Fetching base.js from %s", base_js_url);

    HttpBuffer buf = {0};
    char err[512] = {0};
    if (!http_get_to_memory(base_js_url, &buf, err, sizeof(err))) {
        LOG_WARN("Failed to fetch base.js: %s", err);
        return false;
    }

    GCValue r = JS_Eval(ctx, buf.data, buf.size, "<base.js>", JS_EVAL_TYPE_GLOBAL);
    http_free_buffer(&buf);
    if (JS_IsException(r)) {
        LOG_WARN("base.js execution threw exception");
        return false;
    }

    GCValue global = JS_GetGlobalObject(ctx);
    GCValue ytp = JS_GetPropertyStr(ctx, global, "_yt_player");
    if (JS_IsUndefined(ytp) || JS_IsNull(ytp)) {
        LOG_WARN("_yt_player not defined after base.js execution");
        return false;
    }
    return true;
}

/* Decrypt YouTube signatureCipher strings using the _yt_player decryptor.
 * The input json_cstr is the stringified ytInitialPlayerResponse.
 * Decrypted URLs are appended to js_result. Returns true if at least one
 * decrypted URL was produced. */
static bool decrypt_youtube_streaming_urls(JSContextHandle ctx, const char *html,
                                           const char *json_cstr,
                                           JsExecResult *js_result) {
    if (!ctx || !json_cstr || !js_result) return false;

    /* If no signatureCiphers are present, there is nothing to decrypt. */
    if (!strstr(json_cstr, "signatureCipher")) {
        return false;
    }

    /* Ensure _yt_player is loaded. */
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue ytp = JS_GetPropertyStr(ctx, global, "_yt_player");
    if (JS_IsUndefined(ytp) || JS_IsNull(ytp)) {
        if (!load_and_execute_base_js(ctx, html)) {
            return false;
        }
    }

    const char *decrypt_js =
        "(function() {"
        "  var result = { urls: [], error: null, fnName: null };"
        "  try {"
        "    var decryptFn = null;"
        "    var fnName = null;"
        "    function isDecryptFn(f) {"
        "      if (typeof f !== 'function') return false;"
        "      var s = f.toString();"
        "      return (s.indexOf('split') > -1 || s.indexOf('reverse') > -1 || s.indexOf('slice') > -1 || s.indexOf('splice') > -1) &&"
        "             s.length > 30 && s.length < 3000 && s.indexOf('native code') === -1 &&"
        "             s.indexOf('class') !== 0 && s.indexOf('function') === 0;"
        "    }"
        "    if (typeof window._yt_player !== 'undefined') {"
        "      var keys1 = Object.keys(_yt_player);"
        "      for (var i = 0; i < keys1.length && i < 600 && !decryptFn; i++) {"
        "        try {"
        "          var v1 = _yt_player[keys1[i]];"
        "          if (isDecryptFn(v1)) { decryptFn = v1; fnName = '_yt_player.' + keys1[i]; break; }"
        "          if (typeof v1 === 'object' && v1 !== null && !decryptFn) {"
        "            var keys2 = Object.keys(v1);"
        "            for (var j = 0; j < keys2.length && j < 100 && !decryptFn; j++) {"
        "              try {"
        "                var v2 = v1[keys2[j]];"
        "                if (isDecryptFn(v2)) { decryptFn = v2; fnName = '_yt_player.' + keys1[i] + '.' + keys2[j]; break; }"
        "              } catch(e) {}"
        "            }"
        "          }"
        "        } catch(e) {}"
        "      }"
        "    }"
        "    if (!decryptFn) { result.error = 'No decrypt function found'; return result; }"
        "    result.fnName = fnName;"
        "    var ytip = window.ytInitialPlayerResponse;"
        "    if (!ytip || !ytip.streamingData) { result.error = 'No streamingData'; return result; }"
        "    var formats = ytip.streamingData.formats || [];"
        "    var adaptive = ytip.streamingData.adaptiveFormats || [];"
        "    function processFormat(fmt) {"
        "      if (fmt.signatureCipher) {"
        "        var params = {};"
        "        var parts = fmt.signatureCipher.split('&');"
        "        for (var j = 0; j < parts.length; j++) {"
        "          var kv = parts[j].split('=');"
        "          if (kv.length === 2) {"
        "            params[decodeURIComponent(kv[0])] = decodeURIComponent(kv[1]);"
        "          }"
        "        }"
        "        if (params.s && params.url) {"
        "          try {"
        "            var sig = decryptFn(params.s);"
        "            var sp = params.sp || 'sig';"
        "            var url = params.url + '&' + sp + '=' + encodeURIComponent(sig);"
        "            result.urls.push(url);"
        "          } catch(e) { result.error = (result.error ? result.error + '; ' : '') + e.message; }"
        "        }"
        "      } else if (fmt.url) {"
        "        result.urls.push(fmt.url);"
        "      }"
        "    }"
        "    for (var i = 0; i < formats.length; i++) processFormat(formats[i]);"
        "    for (var i = 0; i < adaptive.length; i++) processFormat(adaptive[i]);"
        "  } catch(e) {"
        "    result.error = (e && e.message) ? e.message : String(e);"
        "  }"
        "  return result;"
        "})();";

    GCValue r = JS_Eval(ctx, decrypt_js, strlen(decrypt_js), "<yt_decrypt>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        LOG_WARN("Signature decryption JS threw exception");
        return false;
    }

    GCValue err_val = JS_GetPropertyStr(ctx, r, "error");
    const char *err_str = JS_ToCString(ctx, err_val);
    if (err_str && err_str[0]) {
        LOG_WARN("Signature decryption error: %s", err_str);
    }

    GCValue urls_val = JS_GetPropertyStr(ctx, r, "urls");
    GCValue len_val = JS_GetPropertyStr(ctx, urls_val, "length");
    int count = JS_VALUE_GET_INT(len_val);
    bool added = false;
    for (int i = 0; i < count && js_result->captured_url_count < JS_MAX_CAPTURED_URLS; i++) {
        GCValue v = JS_GetPropertyUint32(ctx, urls_val, i);
        const char *s = JS_ToCString(ctx, v);
        if (s && strstr(s, "googlevideo.com")) {
            strncpy(js_result->captured_urls[js_result->captured_url_count], s, JS_MAX_URL_LEN - 1);
            js_result->captured_urls[js_result->captured_url_count][JS_MAX_URL_LEN - 1] = '\0';
            js_result->captured_url_count++;
            added = true;
        }
    }

    GCValue fn_val = JS_GetPropertyStr(ctx, r, "fnName");
    const char *fn_str = JS_ToCString(ctx, fn_val);
    if (fn_str && fn_str[0]) {
        LOG_INFO("YouTube decryptor: %s", fn_str);
    }

    return added;
}

/* Lightweight watch-page emulation: extract ytInitialPlayerResponse from the
 * HTML, evaluate the inline data script, and pick the best streaming URL from
 * streamingData.formats / adaptiveFormats. This avoids fetching and executing
 * the multi-megabyte external player bundle, which can hang or crash the
 * emulator. */
extern "C" bool html_extract_yt_player_response_media(const char *html, bool prefer_video,
                                                       char *out_url, size_t out_url_len,
                                                       char *out_mime, size_t out_mime_len,
                                                       char *out_title, size_t out_title_len,
                                                       char *out_thumbnail, size_t out_thumbnail_len) {
    if (!html || !g_js_context || !out_url || out_url_len == 0) return false;
    out_url[0] = '\0';
    if (out_mime && out_mime_len > 0) out_mime[0] = '\0';
    if (out_title && out_title_len > 0) out_title[0] = '\0';
    if (out_thumbnail && out_thumbnail_len > 0) out_thumbnail[0] = '\0';

    const char *patterns[] = {
        "var ytInitialPlayerResponse = ",
        "window.ytInitialPlayerResponse = ",
        "ytInitialPlayerResponse = ",
        NULL
    };
    const char *start = NULL;
    const char *prefix = NULL;
    for (int i = 0; patterns[i] && !start; i++) {
        start = strstr(html, patterns[i]);
        if (start) {
            prefix = patterns[i];
            start += strlen(patterns[i]);
        }
    }
    if (!start) {
        LOG_WARN("ytInitialPlayerResponse not found in HTML");
        return false;
    }

    while (*start && isspace((unsigned char)*start)) start++;
    if (*start != '{') {
        LOG_WARN("ytInitialPlayerResponse does not start with '{'");
        return false;
    }

    int brace_count = 0;
    bool in_string = false;
    bool escape = false;
    const char *p = start;
    while (*p) {
        if (escape) {
            escape = false;
        } else if (*p == '\\') {
            escape = true;
        } else if (*p == '"' && !in_string) {
            in_string = true;
        } else if (*p == '"' && in_string) {
            in_string = false;
        } else if (!in_string) {
            if (*p == '{') brace_count++;
            else if (*p == '}') {
                brace_count--;
                if (brace_count == 0) {
                    p++;
                    break;
                }
            }
        }
        p++;
    }
    if (brace_count != 0) {
        LOG_WARN("ytInitialPlayerResponse brace mismatch");
        return false;
    }

    size_t json_len = (size_t)(p - start);
    size_t script_size = json_len + (prefix ? strlen(prefix) : 0) + 16;
    char *script = (char*)malloc(script_size);
    if (!script) return false;
    snprintf(script, script_size, "%s%.*s;", prefix ? prefix : "", (int)json_len, start);

    LOG_INFO("Evaluating ytInitialPlayerResponse (%zu bytes)", json_len);
    GCValue result = JS_Eval(g_js_context, script, strlen(script), "<ytInitialPlayerResponse>", JS_EVAL_TYPE_GLOBAL);
    free(script);
    if (JS_IsException(result)) {
        LOG_WARN("ytInitialPlayerResponse eval failed");
        return false;
    }
    (void)result;

    GCValue global = JS_GetGlobalObject(g_js_context);
    GCValue ytip = JS_GetPropertyStr(g_js_context, global, "ytInitialPlayerResponse");
    if (JS_IsUndefined(ytip) || JS_IsNull(ytip)) {
        LOG_WARN("ytInitialPlayerResponse not defined after eval");
        return false;
    }

    GCValue json_str = JS_JSONStringify(g_js_context, ytip, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsUndefined(json_str) || JS_IsException(json_str)) {
        LOG_WARN("Failed to stringify ytInitialPlayerResponse");
        return false;
    }
    const char *json_cstr = JS_ToCString(g_js_context, json_str);
    if (!json_cstr) {
        LOG_WARN("JS_ToCString failed for ytInitialPlayerResponse");
        return false;
    }

    /* Extract title */
    if (out_title && out_title_len > 0) {
        const char *vd = strstr(json_cstr, "\"videoDetails\"");
        if (vd) {
            const char *t = strstr(vd, "\"title\":\"");
            if (t) {
                t += 9;
                const char *end = strchr(t, '"');
                if (end) {
                    size_t len = (size_t)(end - t);
                    if (len > 0 && len < out_title_len) {
                        memcpy(out_title, t, len);
                        out_title[len] = '\0';
                    }
                }
            }
        }
    }

    /* Extract thumbnail (last thumbnail URL inside the thumbnails array).
     * We bound the scan to the thumbnails array so we don't accidentally pick
     * up a streamingData URL that appears later in the JSON. */
    if (out_thumbnail && out_thumbnail_len > 0) {
        const char *vd = strstr(json_cstr, "\"videoDetails\"");
        if (vd) {
            const char *th = strstr(vd, "\"thumbnails\"");
            if (th) {
                /* Walk to the matching ']' of the thumbnails array. */
                const char *arr_start = strchr(th, '[');
                if (arr_start) {
                    int bracket_count = 0;
                    bool in_string = false;
                    bool escape = false;
                    const char *q = arr_start;
                    while (*q) {
                        if (escape) {
                            escape = false;
                        } else if (*q == '\\') {
                            escape = true;
                        } else if (*q == '"' && !in_string) {
                            in_string = true;
                        } else if (*q == '"' && in_string) {
                            in_string = false;
                        } else if (!in_string) {
                            if (*q == '[') bracket_count++;
                            else if (*q == ']') {
                                bracket_count--;
                                if (bracket_count == 0) break;
                            }
                        }
                        q++;
                    }
                    const char *last_url = NULL;
                    const char *scan = arr_start;
                    while (scan < q && (scan = strstr(scan, "\"url\":\"")) != NULL) {
                        if (scan >= q) break;
                        last_url = scan + 7;
                        scan++;
                    }
                    if (last_url && last_url < q) {
                        const char *end = strchr(last_url, '"');
                        if (end && end <= q) {
                            size_t len = (size_t)(end - last_url);
                            if (len > 0 && len < out_thumbnail_len) {
                                memcpy(out_thumbnail, last_url, len);
                                out_thumbnail[len] = '\0';
                            }
                        }
                    }
                }
            }
        }
    }

    /* Collect URLs from streamingData */
    JsExecResult js_result;
    memset(&js_result, 0, sizeof(js_result));

    const char *scan = json_cstr;
    while ((scan = strstr(scan, "\"url\":\"")) != NULL) {
        const char *url_start = scan + 7;
        const char *end = strchr(url_start, '"');
        if (end) {
            size_t len = (size_t)(end - url_start);
            if (len > 0 && len < JS_MAX_URL_LEN && js_result.captured_url_count < JS_MAX_CAPTURED_URLS &&
                strstr(url_start, "googlevideo.com")) {
                memcpy(js_result.captured_urls[js_result.captured_url_count], url_start, len);
                js_result.captured_urls[js_result.captured_url_count][len] = '\0';
                js_result.captured_url_count++;
            }
        }
        scan = url_start;
    }

    /* If encrypted signatureCipher streams are present, load base.js and
     * decrypt them with the _yt_player decryptor. This produces real
     * googlevideo.com URLs with a valid signature parameter. */
    if (strstr(json_cstr, "\"signatureCipher\":\"")) {
        decrypt_youtube_streaming_urls(g_js_context, html, json_cstr, &js_result);
    }

    if (js_result.captured_url_count == 0) {
        LOG_WARN("No googlevideo URLs found in ytInitialPlayerResponse");
        return false;
    }

    bool ok = html_select_best_media_url(&js_result, prefer_video,
                                         out_url, out_url_len,
                                         out_mime, out_mime_len);
    if (ok) {
        LOG_INFO("Selected media URL from ytInitialPlayerResponse: %.50s...", out_url);
    }
    return ok;
}

// Backward compatibility wrapper for html_extract_media_url.
// Uses the guarded html_execute_page_scripts path so the unprotected
// execute_scripts_and_get_urls fallback cannot be forced to run the large
// player bundle.
bool html_extract_media_url(const char *html, HtmlMediaCandidate *outCandidate,
                            char *err, size_t errLen) {
    if (!html || !outCandidate) {
        if (err && errLen > 0) {
            strncpy(err, "Invalid arguments", errLen - 1);
            err[errLen - 1] = '\0';
        }
        return false;
    }

    // Clear output
    memset(outCandidate, 0, sizeof(HtmlMediaCandidate));

    JsExecResult js_result;
    memset(&js_result, 0, sizeof(JsExecResult));
    if (!html_execute_page_scripts(html, &js_result)) {
        LOG_WARN("Page script execution failed or produced no URLs");
        if (err && errLen > 0) {
            strncpy(err, "No media URLs found", errLen - 1);
            err[errLen - 1] = '\0';
        }
        return false;
    }

    if (!html_select_best_media_url(&js_result, false,
                                    outCandidate->url, sizeof(outCandidate->url),
                                    outCandidate->mime, sizeof(outCandidate->mime))) {
        if (err && errLen > 0) {
            strncpy(err, "No media URLs found", errLen - 1);
            err[errLen - 1] = '\0';
        }
        return false;
    }

    // Copy title and thumbnail
    if (js_result.title[0]) {
        strncpy(outCandidate->title, js_result.title, sizeof(outCandidate->title) - 1);
        outCandidate->title[sizeof(outCandidate->title) - 1] = '\0';
    }
    if (js_result.thumbnailUrl[0]) {
        strncpy(outCandidate->thumbnailUrl, js_result.thumbnailUrl, sizeof(outCandidate->thumbnailUrl) - 1);
        outCandidate->thumbnailUrl[sizeof(outCandidate->thumbnailUrl) - 1] = '\0';
    }

    LOG_INFO("Selected URL: %.50s...", outCandidate->url);

    return true;
}
