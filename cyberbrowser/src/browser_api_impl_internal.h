/*
 * Browser API Implementation - Internal declarations
 *
 * These symbols are shared between the split browser API source files.
 * They are NOT part of the public API.
 */
#ifndef BROWSER_API_IMPL_INTERNAL_H
#define BROWSER_API_IMPL_INTERNAL_H

#include "browser_api_impl.h"
#include "dom_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compatibility shim: this QuickJS fork does not expose JS_FreeCString, but
 * several browser API files use it after JS_ToCString.  Freeing the raw C
 * string properly would require internal string-handle bookkeeping, so we
 * leave it as a no-op for now; the strings are short-lived. */
static inline void JS_FreeCString(JSContextHandle ctx, const char *s) { (void)ctx; (void)s; }

/* Logging macros shared by all API files */
#define LOG_TAG "browser_api_impl"
#define LOG_ERROR(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)
#define LOG_INFO(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOG_WARN(...) platform_log(LOG_LEVEL_WARN, LOG_TAG, __VA_ARGS__)

/* Macros and constants referenced from the shared init file */
#define DOM_EXCEPTION_LOG_TAG "DOMException"
#define DOM_EX_LOGD(...) platform_log(LOG_LEVEL_INFO, DOM_EXCEPTION_LOG_TAG, __VA_ARGS__)

#define DOM_EXCEPTION_INDEX_SIZE_ERR 1
#define DOM_EXCEPTION_HIERARCHY_REQUEST_ERR 3
#define DOM_EXCEPTION_WRONG_DOCUMENT_ERR 4
#define DOM_EXCEPTION_INVALID_CHARACTER_ERR 5
#define DOM_EXCEPTION_NO_MODIFICATION_ALLOWED_ERR 7
#define DOM_EXCEPTION_NOT_FOUND_ERR 8
#define DOM_EXCEPTION_NOT_SUPPORTED_ERR 9
#define DOM_EXCEPTION_INVALID_STATE_ERR 11
#define DOM_EXCEPTION_SYNTAX_ERR 12
#define DOM_EXCEPTION_INVALID_MODIFICATION_ERR 13
#define DOM_EXCEPTION_NAMESPACE_ERR 14
#define DOM_EXCEPTION_INVALID_ACCESS_ERR 15
#define DOM_EXCEPTION_TYPE_MISMATCH_ERR 17
#define DOM_EXCEPTION_SECURITY_ERR 18
#define DOM_EXCEPTION_NETWORK_ERR 19
#define DOM_EXCEPTION_ABORT_ERR 20
#define DOM_EXCEPTION_URL_MISMATCH_ERR 21
#define DOM_EXCEPTION_QUOTA_EXCEEDED_ERR 22
#define DOM_EXCEPTION_TIMEOUT_ERR 23
#define DOM_EXCEPTION_INVALID_NODE_TYPE_ERR 24
#define DOM_EXCEPTION_DATA_CLONE_ERR 25
#define DEF_FUNC(ctx, parent, name, func, argc) \
    JS_SetPropertyStr(ctx, parent, name, JS_NewCFunction(ctx, func, name, argc))

#define DEF_PROP_STR(ctx, obj, name, value) \
    JS_SetPropertyStr(ctx, obj, name, JS_NewString(ctx, value))

#define DEF_PROP_INT(ctx, obj, name, value) \
    JS_SetPropertyStr(ctx, obj, name, JS_NewInt32(ctx, value))

#define DEF_PROP_BOOL(ctx, obj, name, value) \
    JS_SetPropertyStr(ctx, obj, name, JS_NewBool(ctx, value))

#define DEF_PROP_FLOAT(ctx, obj, name, value) \
    JS_SetPropertyStr(ctx, obj, name, JS_NewFloat64(ctx, value))

#define DEF_PROP_UNDEFINED(ctx, obj, name) \
    JS_SetPropertyStr(ctx, obj, name, JS_UNDEFINED)


/* Forward declarations for shared helper functions, proto arrays, class defs, and globals */
GCValue js_crypto_get_random_values(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_subtle_digest(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_subtle_encrypt(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_subtle_decrypt(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_storage_get_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_storage_set_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_storage_remove_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_storage_clear(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_storage_key(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_storage_get_length(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_supports(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_escape(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_sheet_insert_rule(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_sheet_delete_rule(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_sheet_add_rule(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_sheet_remove_rule(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_sheet_replace(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_sheet_replace_sync(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_sheet_get_css_text(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_sheet_get_css_rules(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_sheet_get_rules(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_sheet_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_document_get_style_sheets(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_get_adopted_style_sheets(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_set_adopted_style_sheets(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_cookie_getter(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_cookie_setter(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_decl_set_property(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_decl_remove_property(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_decl_get_property_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_decl_get_property_priority(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_decl_get_css_text(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_decl_set_css_text(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_decl_get_length(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_css_style_decl_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
/* DOMTokenList.prototype.values / [Symbol.iterator]: the current tokens. */
/* The Symbol.iterator atom (well known, never freed). */
JSAtom dom_iterator_atom(JSContextHandle ctx);
/* DOMTokenList (browser/dom_token_list.c): the interface and element lists. */
void dom_token_list_install(JSContextHandle ctx, GCValue global, GCValue window);
/* URL, URLSearchParams (browser/url_classes.c); <a>/<area> URL attributes and baseURI. */
void url_classes_install(JSContextHandle ctx, GCValue global);
/* CharacterData and its node interfaces, DocumentType (character_data.c). */
void character_data_install(JSContextHandle ctx, GCValue global, GCValue character_data_proto, GCValue node_proto);
/* DOM "adopt": node's subtree becomes document doc's (dom_api.c). */
void dom_api_adopt(JSContextHandle ctx, NodeId node, NodeId doc);
/* Document.prototype's members, XMLDocument, DOMImplementation (document_api.c). */
void document_api_install(JSContextHandle ctx, GCValue global);
/* Node's namespace lookups, getElementsByTagName(NS) (node_namespaces.c). */
void node_namespaces_install(JSContextHandle ctx, GCValue global);
/* An XML document parsed from s (a parsererror one if not well-formed). */
GCValue dom_new_xml_document(JSContextHandle ctx, const char *s, size_t n, const char *content_type);
/* Does s match XML's Name production? */
bool dom_xml_is_name(const char *s, size_t n);
GCValue js_document_create_comment(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
/* TextEncoder and TextDecoder (encoding.c). */
/* The name of the encoding a label names (Encoding Standard 4.2), or NULL. */
const char *encoding_name_for_label(const char *s, size_t n);
void encoding_install(JSContextHandle ctx, GCValue global);
void url_hyperlinks_install(JSContextHandle ctx, GCValue global);
/* __cyberEventPath for dom_events.js (browser/event_path.c). */
void event_path_install(JSContextHandle ctx, GCValue global);
/* NodeIterator, TreeWalker and document.create* (browser/traversal.c). */
void traversal_install(JSContextHandle ctx, GCValue global);
void dom_element_set_attr(JSContextHandle ctx, NodeId n, const char *name, const char *value);
GCValue js_service_worker_register(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_service_worker_get_registration(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_service_worker_get_registrations(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_service_worker_add_event_listener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_geolocation_position_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_geolocation_position_error_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_geolocation_get_current_position(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_geolocation_watch_position(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_user_agent_data_get_high_entropy_values(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_navigator_get_battery(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_history_push_state(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_history_replace_state(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_media_metadata_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_console_log(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_warn(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_error(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_info(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_debug(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_trace(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_dir(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_dirxml(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_group(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_groupCollapsed(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_groupEnd(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_time(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_timeEnd(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_timeLog(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_count(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_countReset(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_assert(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_console_clear(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_get_computed_style(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_appendChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_removeChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_insertBefore_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_replaceChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_normalize_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_compareDocumentPosition_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_cloneNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_contains_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_getRootNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_get_ownerDocument(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_is_connected_getter(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
void dom_node_set_owner_document(JSContextHandle ctx, GCValue node, GCValue doc);
GCValue js_element_querySelector_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_querySelectorAll_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
bool matches_selector(JSContextHandle ctx, GCValue elem, const char* selector);
extern JSClassDef js_dom_node_class_def;
void capture_url(const char *url);
void capture_url_debug(const char *url, const char *source);

#ifdef __cplusplus
}
#endif

// Helper exposed by js_quickjs.c
void record_captured_url(const char *url);

#ifdef __cplusplus
extern "C" {
#endif
GCValue throw_dom_exception(JSContextHandle ctx, const char* name, const char* message);
GCValue js_create_resolved_promise(JSContextHandle ctx, GCValue value);
GCValue js_create_empty_resolved_promise(JSContextHandle ctx);
extern JSClassDef js_shadow_root_class_def;
GCValue js_shadow_root_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_node_get_firstChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_get_lastChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_get_nextSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_get_previousSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_get_parentNode(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_get_parentElement(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_get_childNodes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_get_nodeType(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_get_nodeName(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_tagName(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_localName(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_namespaceURI(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_prefix(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_firstElementChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_lastElementChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_nextElementSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_previousElementSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_childElementCount(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_children(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_append(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_prepend(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_replaceChildren(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_before(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_after(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_replaceWith(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_style(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_set_style(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_elements_by_tag_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_get_elements_by_tag_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_getElementsByClassName(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_set_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_id(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_set_id(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_class_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_set_class_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_remove_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_has_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_toggle_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_set_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_has_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_remove_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_getElementById(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_querySelector(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_querySelectorAll(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_click(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_animations(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_dataset(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_inner_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_set_inner_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_outer_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_set_outer_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_get_text_content(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_set_text_content(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_get_node_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_set_node_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_get_data(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_set_data(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_create_range(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_range_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern const JSCFunctionListEntry js_range_proto_funcs[];
extern const size_t js_range_proto_funcs_count;
GCValue js_document_create_tree_walker(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_tree_walker_next_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_tree_walker_previous_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_tree_walker_first_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_tree_walker_last_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_tree_walker_next_sibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_tree_walker_previous_sibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_tree_walker_parent_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_create_event(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_slot_assigned_nodes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_slot_assigned_elements(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_slot_get_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_getRootNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_mutation_observer_observe(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_mutation_observer_takeRecords(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_mutation_observer_disconnect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
/* A childList change of target: added and removed are arrays; prev and next
 * the siblings around them (null for none). */
void mo_notify_child_list(JSContextHandle ctx, GCValue target, GCValue added, GCValue removed, GCValue prev,
                          GCValue next);
bool mo_child_list_observed(JSContextHandle ctx, GCValue target);
bool mo_any_observing(JSContextHandle ctx);
void mo_notify_character_data(JSContextHandle ctx, GCValue target, const char *old_value);
/* old_value: NULL when the attribute was absent. */
void mo_notify_attribute(JSContextHandle ctx, GCValue target, const char *name, const char *old_value);
GCValue js_queue_microtask(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_create_document_fragment(JSContextHandle ctx);
GCValue js_document_import_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_element_from_point(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_attributes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_has_attributes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_attribute_names(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_matches(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_closest(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_cyber_upgrade_element(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
int js_cyber_ce_upgrade_depth(void);
GCValue js_cyber_ce_enqueue_upgrade(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
void js_cyber_ce_enqueue_upgrade_subtree(JSContextHandle ctx, GCValue root);
void js_cyber_ce_enqueue_callback(JSContextHandle ctx, GCValue elem, const char *name);
GCValue js_cyber_ce_flush_reactions(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
void js_cyber_ce_push_stack(JSContextHandle ctx);
void js_cyber_ce_pop_stack(JSContextHandle ctx);
void js_cyber_ce_schedule_flush(JSContextHandle ctx);
GCValue js_cyber_ce_schedule_flush_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
extern JSClassDef js_event_class_def;
GCValue js_event_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_event_preventDefault(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_event_stopPropagation(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_event_stopImmediatePropagation(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_event_get_eventPhase_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_event_composedPath(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_event_get_type_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_event_get_bubbles_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_event_get_cancelable_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_event_get_composed_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_event_get_defaultPrevented_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_event_get_target_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_event_get_currentTarget_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
extern JSClassDef js_custom_event_class_def;
GCValue js_custom_event_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern JSClassDef js_mouse_event_class_def;
GCValue js_mouse_event_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_custom_event_get_detail_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_mouse_event_get_clientX_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_mouse_event_get_clientY_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
extern JSClassDef js_focus_event_class_def;
extern JSClassDef js_service_worker_container_class_def;
extern JSClassDef js_service_worker_registration_class_def;
extern JSClassDef js_service_worker_class_def;
GCValue js_focus_event_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_focus_event_get_relatedTarget(JSContextHandle ctx, GCValue this_val);
GCValue js_event_target_addEventListener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_event_target_removeEventListener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_event_target_dispatchEvent(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_focus_event_get_relatedTarget_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_shadow_root_get_host_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_shadow_root_get_first_child(JSContextHandle ctx, GCValue this_val);
GCValue js_shadow_root_get_last_child(JSContextHandle ctx, GCValue this_val);
GCValue js_shadow_root_get_child_nodes(JSContextHandle ctx, GCValue this_val);
GCValue js_shadow_root_get_children(JSContextHandle ctx, GCValue this_val);
GCValue js_shadow_root_get_child_element_count(JSContextHandle ctx, GCValue this_val);
extern const JSCFunctionListEntry js_shadow_root_proto_funcs[];
extern const size_t js_shadow_root_proto_funcs_count;
GCValue js_element_attach_shadow(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_shadow_root(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_shadow_root_get_owner_document_wrapper(JSContextHandle ctx, GCValue this_val);
GCValue js_shadow_root_get_parent_node_wrapper(JSContextHandle ctx, GCValue this_val);
GCValue js_shadow_root_get_elements_by_tag_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_shadow_root_get_adopted_style_sheets(JSContextHandle ctx, GCValue this_val);
GCValue js_shadow_root_set_adopted_style_sheets(JSContextHandle ctx, GCValue this_val, GCValue val);
GCValue js_shadow_root_get_style_sheets(JSContextHandle ctx, GCValue this_val);
GCValue js_dummy_function_true(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_dummy_function(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_message_channel_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_create_from_ctor_proto(JSContextHandle ctx, GCValue ctor);
GCValue js_element_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_html_element_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_event_target_observed_attributes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
extern JSClassDef js_dom_exception_class_def;
GCValue js_dom_exception_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern const JSCFunctionListEntry js_dom_exception_proto_funcs[];
extern const size_t js_dom_exception_proto_funcs_count;
GCValue js_object_get_prototype_of(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_object_define_property(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_object_get_own_property_descriptor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_object_set_prototype_of(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_object_get_own_property_symbols(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_object_assign(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_reflect_construct(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_reflect_apply(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_reflect_has(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_promise_finally(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_string_includes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_array_includes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_array_from(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
extern JSClassDef js_map_class_def;
GCValue js_map_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern const JSCFunctionListEntry js_map_proto_funcs[];
extern const size_t js_map_proto_funcs_count;
extern JSClassDef js_custom_element_registry_class_def;
GCValue js_custom_elements_define(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_custom_elements_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_custom_elements_when_defined(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
extern JSClassDef js_animation_class_def;
extern JSClassDef js_keyframe_effect_class_def;
GCValue js_animation_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern const JSCFunctionListEntry js_animation_proto_funcs[];
extern const size_t js_animation_proto_funcs_count;
GCValue js_keyframe_effect_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern const JSCFunctionListEntry js_keyframe_effect_proto_funcs[];
extern const size_t js_keyframe_effect_proto_funcs_count;
GCValue js_element_animate(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
extern JSClassDef js_font_face_class_def;
extern JSClassDef js_font_face_set_class_def;
GCValue js_font_face_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern const JSCFunctionListEntry js_font_face_proto_funcs[];
extern const size_t js_font_face_proto_funcs_count;
extern const JSCFunctionListEntry js_font_face_set_proto_funcs[];
extern const size_t js_font_face_set_proto_funcs_count;
extern JSClassDef js_mutation_observer_class_def;
GCValue js_mutation_observer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern const JSCFunctionListEntry js_mutation_observer_proto_funcs[];
extern const size_t js_mutation_observer_proto_funcs_count;
extern JSClassDef js_resize_observer_class_def;
GCValue js_resize_observer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern const JSCFunctionListEntry js_resize_observer_proto_funcs[];
extern const size_t js_resize_observer_proto_funcs_count;
extern JSClassDef js_intersection_observer_class_def;
GCValue js_intersection_observer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern const JSCFunctionListEntry js_intersection_observer_proto_funcs[];
extern const size_t js_intersection_observer_proto_funcs_count;
extern JSClassDef js_performance_class_def;
extern JSClassDef js_performance_entry_class_def;
extern JSClassDef js_performance_observer_class_def;
double performance_get_time_ms(void);
GCValue js_performance_now(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
extern JSClassDef js_performance_timing_class_def;
extern const JSCFunctionListEntry js_performance_proto_funcs[];
extern const size_t js_performance_proto_funcs_count;
extern const JSCFunctionListEntry js_performance_entry_proto_funcs[];
extern const size_t js_performance_entry_proto_funcs_count;
GCValue js_performance_observer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern const JSCFunctionListEntry js_performance_observer_proto_funcs[];
extern const size_t js_performance_observer_proto_funcs_count;
extern JSClassDef js_dom_rect_class_def;
extern JSClassDef js_dom_rect_read_only_class_def;
GCValue js_element_getBoundingClientRect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_dom_rect_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_dom_rect_read_only_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_dom_rect_from_rect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_dom_rect_read_only_from_rect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
extern const JSCFunctionListEntry js_dom_rect_proto_funcs[];
extern const size_t js_dom_rect_proto_funcs_count;
extern const JSCFunctionListEntry js_dom_rect_read_only_proto_funcs[];
extern const size_t js_dom_rect_read_only_proto_funcs_count;
extern JSClassDef js_date_class_def;
GCValue js_date_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern const JSCFunctionListEntry js_date_proto_funcs[];
extern const size_t js_date_proto_funcs_count;
extern const JSCFunctionListEntry js_date_static_funcs[];
extern const size_t js_date_static_funcs_count;
extern JSClassDef js_media_source_class_def;
extern JSClassDef js_source_buffer_class_def;
GCValue js_media_source_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_source_buffer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_media_source_is_type_supported(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
/* Give `obj` the EventTarget methods (lists such as SourceBufferList and
 * TextTrackList are plain objects here). */
void js_media_make_event_target(JSContextHandle ctx, GCValue obj);

/* Media element playback (media_source_api.c).  A video element whose src is
 * a MediaSource's object URL is attached to it; while attached its
 * currentTime, readyState, buffered, play()/pause() come from here. */
bool media_element_attach(JSContextHandle ctx, GCValue video, GCValue media_source);
void media_element_detach(JSContextHandle ctx, GCValue video);
bool media_element_active(GCValue video);
double media_element_current_time(GCValue video);
void media_element_seek(GCValue video, double t);
double media_element_duration(GCValue video);
int media_element_ready_state(GCValue video);
bool media_element_paused(GCValue video);
bool media_element_ended(GCValue video);
GCValue media_element_buffered(JSContextHandle ctx, GCValue video);
void media_element_play(GCValue video);
void media_element_pause(GCValue video);
void media_element_set_rate(GCValue video, double rate);
void media_element_video_size(JSContextHandle ctx, GCValue video, int *w, int *h);
/* Fire queued media events and advance playback (each event-loop turn). */
int media_tick(JSContextHandle ctx);   /* returns the events fired */
bool media_tick_pending(void);
/* The frame a video element shows now (for painting). */
bool media_video_frame_for_node(NodeId node, const uint8_t **rgba,
                                int *w, int *h, uint64_t *frame_id);
/* Each video's frame for now into cache, as the display list draws it
 * ("cyber-video-frame:<node>"), when it changed: true if any did.  Showing
 * it then takes a repaint, not a layout. */
struct ImageCache;
bool media_refresh_video_frames(struct ImageCache *cache);
/* Is a video playing (frames due)? */
bool media_video_playing(void);
extern const JSCFunctionListEntry js_media_source_proto_funcs[];
extern const size_t js_media_source_proto_funcs_count;
extern const JSCFunctionListEntry js_source_buffer_proto_funcs[];
extern const size_t js_source_buffer_proto_funcs_count;
GCValue js_url_create_object_url(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_url_revoke_object_url(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
void url_api_install(JSContextHandle ctx, GCValue global);
void platform_api_install(JSContextHandle ctx, GCValue global, GCValue window);
void webgpu_api_install(JSContextHandle ctx, GCValue global, GCValue window);
void frame_api_install(JSContextHandle ctx, GCValue global);
void frame_api_on_connect(JSContextHandle ctx, NodeId subtree);
/* While set (a script's insertion), an iframe without src that connects
 * gets its about:blank document and load event at once (HTML 4.8.5). */
extern bool g_frame_blank_now;
extern const JSCFunctionListEntry js_shadow_root_native_funcs[];
extern const size_t js_shadow_root_native_funcs_count;
void timer_install_rejection_tracker(JSRuntimeHandle rt);
int frame_api_process(void);
bool frame_api_pending(void);
GCValue js_request_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_response_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_response_json(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_navigator_send_beacon(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_set_timeout(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_clear_timeout(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_set_interval(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_clear_interval(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_request_animation_frame(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_cancel_animation_frame(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_request_idle_callback(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_cancel_idle_callback(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_promise_resolve_empty_array(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_match_media(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_storage_estimate(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_null(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_media_capabilities_decoding_info(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_atob(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_false(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_btoa(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_permissions_query(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_empty_string(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_abort_controller_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_create_element(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_create_element_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_create_text_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_create_document_fragment(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_abort_signal_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_media_capabilities_encoding_info(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
int safe_set_property_str(JSContextHandle ctx, GCValue obj, const char *key, GCValue val);
GCValue js_audio_context_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_false_promise(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_blob_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_readable_stream_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_promise_reject(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_file_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_pressure_observer_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_form_data_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_zero(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_undefined(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_profiler_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_permission_status_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_worker_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
int is_obj_usable(JSContextHandle ctx, GCValue obj);
GCValue js_true(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_empty_array(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_dom_parser_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_promise_resolve_undefined(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_promise_resolve_empty_string(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_get_selection(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_dom_parser_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
GCValue js_xml_serializer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern double g_performance_time_origin;
extern JSClassID js_service_worker_container_class_id;
extern JSClassID js_service_worker_registration_class_id;
extern JSClassID js_service_worker_class_id;
#ifdef __cplusplus
extern "C" {
#endif
extern JSClassID js_xhr_class_id;
extern JSClassID js_video_class_id;
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
}
#endif

/* These functions are defined in js_quickjs.c */
extern GCValue js_xhr_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern GCValue js_video_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern GCValue js_fetch(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
extern "C" {
#endif
extern const JSCFunctionListEntry js_xhr_proto_funcs[];
extern const JSCFunctionListEntry js_video_proto_funcs[];
extern const size_t js_xhr_proto_funcs_count;
extern const size_t js_video_proto_funcs_count;
void timer_api_reset(void);
int timer_process_due(JSContextHandle ctx);
/* IntersectionObserver per-frame re-evaluation (misc_api.c). */
int intersection_observers_tick(JSContextHandle ctx);
/* Run every queued promise job (HTML microtask checkpoint). */
void timer_microtask_checkpoint(JSContextHandle ctx);
int timer_has_pending(void);
void timer_set_idle_deadline(unsigned long long deadline_ms);
int scheduler_process_tasks(JSContextHandle ctx);
void scheduler_reset(void);
void dom_enable_dynamic_script_loading(void);
#ifdef __cplusplus
}
#endif
GCValue js_scheduler_post_task(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_scheduler_yield(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
extern GCHandle service_worker_handle;

#ifdef __cplusplus
}
#endif

#endif // BROWSER_API_IMPL_INTERNAL_H
