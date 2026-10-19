/* Layout snapshots of the node store.  See dom_snapshot.h. */
#include "dom_snapshot.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct HtmlArenaBlock {
    struct HtmlArenaBlock *next;
    size_t used, cap;
    char data[];
} HtmlArenaBlock;

static void *arena_alloc(HtmlDocument *doc, size_t n) {
    n = (n + 7) & ~(size_t)7;
    HtmlArenaBlock *b = doc->arena;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > 32 * 1024 ? n : 32 * 1024;
        HtmlArenaBlock *nb = malloc(sizeof(HtmlArenaBlock) + cap);
        nb->next = b;
        nb->used = 0;
        nb->cap = cap;
        doc->arena = nb;
        b = nb;
    }
    void *p = b->data + b->used;
    b->used += n;
    return p;
}

void html_document_arena_free(HtmlDocument *doc) {
    HtmlArenaBlock *b = doc->arena;
    while (b) { HtmlArenaBlock *n = b->next; free(b); b = n; }
    doc->arena = NULL;
}

const char *html_document_strdup(HtmlDocument *doc, const char *s, size_t n) {
    if (!s || n == 0) return "";
    char *p = arena_alloc(doc, n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

HtmlDocument *html_document_new(void) {
    HtmlDocument *doc = calloc(1, sizeof(HtmlDocument));
    if (!doc) return NULL;
    if (!po_array_init(&doc->array, sizeof(HtmlNode), 64)) { free(doc); return NULL; }
    doc->root_idx = doc->head_idx = doc->body_idx = -1;
    doc->current_parent_idx = -1;
    return doc;
}

int html_document_add(HtmlDocument *doc, int parent, HtmlNodeType type) {
    int idx = po_array_add(&doc->array, parent);
    if (idx < 0) return -1;
    HtmlNode *n = po_array_payload(&doc->array, idx);
    n->type = type;
    n->tag_name = "";
    n->js_object = JS_UNDEFINED;
    return idx;
}

void html_document_add_attr(HtmlDocument *doc, int idx, const char *name, const char *value, size_t vlen) {
    HtmlAttribute *a = arena_alloc(doc, sizeof(HtmlAttribute));
    a->name = name;
    a->value = html_document_strdup(doc, value, vlen);
    a->next = NULL;
    HtmlNode *n = po_array_payload(&doc->array, idx);
    HtmlAttribute **tail = &n->attributes;
    while (*tail) tail = &(*tail)->next;
    *tail = a;
}

void html_document_find_roots(HtmlDocument *doc) {
    doc->root_idx = doc->head_idx = doc->body_idx = -1;
    for (int r = doc->array.first_root; r >= 0; r = po_array_next_sibling(&doc->array, r)) {
        HtmlNode *node = po_array_payload(&doc->array, r);
        if (node->type != HTML_NODE_ELEMENT) continue;
        if (doc->root_idx < 0) doc->root_idx = r;
        if (strcasecmp(node->tag_name, "head") == 0) doc->head_idx = r;
        else if (strcasecmp(node->tag_name, "body") == 0) doc->body_idx = r;
        else if (strcasecmp(node->tag_name, "html") == 0) {
            for (int c = po_array_first_child(&doc->array, r); c >= 0; c = po_array_next_sibling(&doc->array, c)) {
                HtmlNode *ch = po_array_payload(&doc->array, c);
                if (ch->type != HTML_NODE_ELEMENT) continue;
                if (strcasecmp(ch->tag_name, "head") == 0) doc->head_idx = c;
                else if (strcasecmp(ch->tag_name, "body") == 0) doc->body_idx = c;
            }
        }
    }
}

/* Concatenated text of a raw-text element's text children (malloc'd). */
static char *raw_text_of(uint32_t s, size_t *len_out) {
    size_t n = 0;
    for (uint32_t c = g_dom.first[s]; c; c = g_dom.next[c])
        if (g_dom.kind[c] == DOM_K_TEXT) n += g_dom.text[c].len;
    char *out = malloc(n + 1);
    size_t o = 0;
    for (uint32_t c = g_dom.first[s]; c; c = g_dom.next[c])
        if (g_dom.kind[c] == DOM_K_TEXT && g_dom.text[c].len) {
            memcpy(out + o, g_dom.text[c].s, g_dom.text[c].len);
            o += g_dom.text[c].len;
        }
    out[o] = '\0';
    *len_out = strlen(out);
    return out;
}

static int snap_node(HtmlDocument *doc, uint32_t s, int parent) {
    if (g_dom.kind[s] == DOM_K_TEXT) {
        int idx = html_document_add(doc, parent, HTML_NODE_TEXT);
        HtmlNode *n = po_array_payload(&doc->array, idx);
        DomStr *t = &g_dom.text[s];
        n->text_content = malloc(t->len + 1);
        if (t->len) memcpy(n->text_content, t->s, t->len);
        n->text_content[t->len] = '\0';
        n->text_len = t->len;
        n->store_id = dom_id_of(s);
        return idx;
    }
    int idx = html_document_add(doc, parent, HTML_NODE_ELEMENT);
    HtmlNode *n = po_array_payload(&doc->array, idx);
    const char *tn = css_atom_str(g_dom.tag[s]);
    n->tag_name = tn ? tn : "";
    n->store_id = dom_id_of(s);
    DomAttrList *l = &g_dom.attrs[s];
    for (int i = 0; i < l->n; i++) {
        const char *an = css_atom_str(l->a[i].name);
        html_document_add_attr(doc, idx, an ? an : "", l->a[i].value.s, l->a[i].value.len);
    }
    if ((g_dom.flags[s] & DOMF_RAW_TEXT) && !(g_dom.flags[s] & DOMF_SELF_CLOSED)) {
        n = po_array_payload(&doc->array, idx);
        n->text_content = raw_text_of(s, &n->text_len);
    }
    return idx;
}

HtmlDocument *dom_snapshot_parsed(NodeId container) {
    if (!dom_valid(container)) return NULL;
    HtmlDocument *doc = html_document_new();
    if (!doc) return NULL;
    uint32_t root = dom_slot(container);
    /* Iterative preorder with the HtmlDocument parent of each open level. */
    uint32_t cap = 64, sp = 0;
    int *pstack = malloc(cap * sizeof(int));
    uint32_t s = g_dom.first[root];
    int parent = -1;
    while (s) {
        int k = g_dom.kind[s];
        bool descend = false;
        int idx = -1;
        if (k == DOM_K_ELEMENT || k == DOM_K_TEXT) {
            idx = snap_node(doc, s, parent);
            descend = k == DOM_K_ELEMENT && !(g_dom.flags[s] & DOMF_RAW_TEXT) && g_dom.first[s];
        }
        if (descend) {
            if (sp == cap) { cap *= 2; pstack = realloc(pstack, cap * sizeof(int)); }
            pstack[sp++] = parent;
            parent = idx;
            s = g_dom.first[s];
            continue;
        }
        while (s != root && !g_dom.next[s]) {
            s = g_dom.parent[s];
            if (s == root) break;
            parent = pstack[--sp];
        }
        if (s == root) break;
        s = g_dom.next[s];
    }
    free(pstack);
    html_document_find_roots(doc);
    return doc;
}
