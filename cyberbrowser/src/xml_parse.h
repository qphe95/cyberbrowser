/*
 * xml_parse.h - XML documents into the node store (xml_parse.c).
 */
#ifndef XML_PARSE_H
#define XML_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include "dom_store.h"

/* Parse n bytes of XML (UTF-8) into doc, an empty document node.  False on
 * a well-formedness error, with its message (and position) in err; what was
 * built before it stays. */
bool dom_parse_xml(const char *s, size_t n, NodeId doc, char *err, size_t errlen);

#endif
