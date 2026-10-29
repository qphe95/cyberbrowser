/* HTMLElement basics: innerText, focus/blur, reflected global attributes, ... */
#ifndef HTML_ELEMENT_API_H
#define HTML_ELEMENT_API_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs on Element.prototype (only where the property is not defined):
 * innerText/outerText, focus()/blur(), hidden/title/lang/dir/accessKey/
 * draggable/translate/inert/tabIndex/contentEditable/isContentEditable,
 * insertAdjacentHTML/Element/Text, getClientRects, checkVisibility and
 * no-op scrollIntoView/scroll/scrollTo/scrollBy. */
void html_element_api_install(JSContextHandle ctx, GCValue element_proto);

/* CharacterData members on the Text/Comment prototype: length, appendData,
 * insertData, deleteData, replaceData, substringData, splitText, wholeText. */
void html_character_data_api_install(JSContextHandle ctx, GCValue character_data_proto);

/* HTMLLinkElement: href (resolved), rel, media, type, as, crossOrigin,
 * integrity, hreflang, referrerPolicy reflect their attributes. */
void html_link_api_install(JSContextHandle ctx, GCValue link_proto);
void html_script_api_install(JSContextHandle ctx, GCValue script_proto);

#ifdef __cplusplus
}
#endif

#endif
