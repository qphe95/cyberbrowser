# CSS parsing: a CSS Syntax 3 tokenizer and parser

Status: step 1 done (css/CSS2 5218 -> 5234, none lost: the WHATWG single-byte encodings and
Big5 decoded, tables from tools/gen_charset_tables.js; a sheet whose Content-Type is not text/css
applies nothing).  Not done in it: the referring document's encoding as a sheet's fallback
(at-charset-012), which needs the HTML parser to decode non-UTF-8 documents first.  Step 2 done
(src/css_tokenizer.c, tests/test_css_tokenizer.c).  Step 3 done (css/CSS2 5234 -> 5252, none
lost; parse time unchanged, below).  Step 4 done (css/CSS2 5252 -> 5271, none lost; parse
time below the old parser's, below).  Step 5 done (css/CSS2 5271 -> 5294, none lost; parse
time within noise of step 4's).  Step 6 done (css/CSS2 5294 -> 5321, none lost; YouTube's forced
layouts 217 -> 209 ms, noise).

As built, step 6: not a new set of checks but the grammar matcher css_props.c already had (for
@supports and the CSSOM), applied by layout: css_value_invalid(prop, value) is true for a value
outside the grammar of a property checked (the color properties, background, background-image,
border-color, font-family, box sizes and offsets, font-size, line-height, text-indent, letter- and
word-spacing, margins and paddings), and layout_apply_declaration then ignores the declaration, so
the one before it stays.  Unchecked properties, custom properties, wide keywords and values with
var() are never invalid; a bare number stays a length (no quirks mode here to tell when it is one).
The matcher costs microseconds, so the common values are recognised before it (a color
css_color_parse reads, plain lengths, none / normal / auto, one calc() / min() / max() / clamp(),
a plain font-family list), and the answer is cached on the declaration (CssDeclaration.invalid;
the cascade's copies take it from the rule's declaration unless var() was substituted).  Checking
all 69,280 declarations of YouTube's main sheet takes 7 ms; only applied ones are checked, once.
On YouTube 6 declarations are dropped (min-width: none, margin: 10 0 -6, height: none, ...), as
in Chrome.  Rebuilt values write a name's non-name characters as hex escapes (`test\f oo`), so
they read back as the same token.

Left: invalid values of the properties not checked (keywords of display, float, position and the
rest) are still read by their layout parsers as before.

As built, step 5: a prelude with an escape (or a comment) is read from tokens and its selector text
rebuilt: identifiers with escapes resolved, any character that is not a name character (and a digit
that would start a name) written `\X`, strings re-quoted, white space one space.  The selector
parser reads `\X` in tag, class, id and attribute names as the character X, and the comma splitters
skip it.  A selector whose `.` or `#` is not followed by an identifier (`.6x`, `#-1x`), whose `[`
is not followed by an attribute name (`[1digit]`), or with a number outside a function's argument,
voids its rule, on the text path and the token path alike.  With these: a url token written
`\url(` / `U\Rl(` is rebuilt as `url("...")`; font-family keeps the whole list (the font registry
takes the first family it has: before, only the first name was kept, so a missing one fell back to
sans-serif), unquoted names with their white space collapsed, `\"` inside a quoted name; and the
test server's script injection, which used an index into the lower-cased text (`İ` lower-cases to
two code units), no longer cuts an XHTML page with such characters.  YouTube's replay screenshot
changes in one place: a heading whose first family is missing now uses the next one in its list.

Left: case-sensitive-003 / 004 need `:lang()` (not supported) and case-insensitive pseudo-element
names in the places that look for `:before` / `:after` as text; case-sensitive-007 needs XHTML's
case-sensitive type selectors; escapes-002 / 009 / 011 / 014 - 016 are invalid values (step 6).

As built, step 4: rule lists (css_rule_list, tests/test_css_rules.c) by CSS Syntax 5.4.1 - 5.4.3.
A qualified rule's prelude runs to its `{` with brackets and strings nested (a `;` or a top-level
`}` is part of it, voiding the rule); `@` without an identifier after it starts a qualified rule;
an at-rule ends at its `;` or block, or at the `}` closing the block it is in; @media, @supports,
@layer, @container, @scope, @starting-style and @document blocks are rule lists (@media's condition
attached as before), any other at-rule's block is skipped (so `@badAtRule screen { div {...} }`
applies nothing); `<!--`, `-->`, and `<![CDATA[` / `]]>` (which an XML parser would have taken out
of an XHTML document's style; ours parses it as HTML) are ignored at the top level only.  A
selector list with an empty selector or one starting with a digit is dropped.  The @import scan
(css_layout.c) passes over invalid rules and unknown at-rules (css_sheet_item_skip) as browsers do.
In declaration lists: junk that is not a declaration ends at its `;` or with its `{}` block (read as
a nested rule, not kept), a declaration with a bad string or url is dropped, and the text fast path
leaves a property that is not one identifier to the tokens.  Preludes and at-rules are read as
text when they have none of what only the tokens read right (escapes, comments, unbalanced
brackets, `} ; @ <`, `-->`, a newline in a string); on YouTube's main sheet 2 of 21,400 preludes
take the token path.  The text scan checks the selector starts itself, so the old second pass over
each selector (css_prelude_invalid) runs only on the token path.  Same session, alternating runs:

| sheet | old parser (HEAD) | step 4 |
|---|---|---|
| YouTube main (3.2 MB) | 10.95 - 11.31 ms | 10.10 - 10.41 ms |
| YouTube second (550 KB) | 1.82 - 2.00 ms | 1.66 - 1.83 ms |
| Wikipedia (210 KB) | 0.59 - 0.60 ms | 0.55 - 0.58 ms |

Left for later steps: escapes in selectors (strings-000's `\` newline in an attribute value), and
an invalid value falling back to the earlier declaration (matching-brackets-002, declarations-009
#f, malformed-decl-007: step 6).  declarations-009 #c (`@media {...} color: red;` in a block,
expected green) contradicts malformed-decl-block-001 #p3 (`@foo {color: red} color: green`,
expected green) under any one rule for at-rules in declaration lists; CSS Syntax today ends the
at-rule at its block, which #p3 follows.

As built, step 3: the tokenizer also runs as a stream (css_tz_next, one token per call into the
caller's token, no array; tested to give the array's tokens).  Declaration lists (rule blocks and
style attributes) are read from it by css_consume_declarations (section 5.4.5: declarations to their
semicolon at their own nesting level, at-rules in a declaration list ignored with their block,
anything else skipped to its semicolon, !important as the last ! and important tokens; values the
source text, or rebuilt from the tokens when an escape or a comment is in them).  A rule block goes
through the old text scanner first (css_decl_block_fast), which gives up, adding nothing, on
anything only the token path reads right: an escape, `{`, `@`, `[`, `]`, `<`, a comment, a newline
in a string, a `!` that is not a final !important, a declaration without a colon; then the block
is read from tokens.  On YouTube's main sheet 142 blocks take the token path.  A table of the
characters the scanner looks at keeps its loop at one check for any other.  Same build session,
best of 5 per run, three runs each:

| sheet | old parser | step 3 |
|---|---|---|
| YouTube main (3.2 MB) | 10.90 - 11.16 ms | 10.91 - 11.04 ms |
| YouTube second (550 KB) | 1.87 - 1.97 ms | 1.84 - 1.94 ms |
| Wikipedia (210 KB) | 0.59 ms | 0.62 - 0.63 ms |

Without the fast path (every block from tokens) the YouTube sheet took 15 ms.

Step 2's measurement (best of 5, one thread; CSS_TOK_BENCH=<files> with the css_tokenizer tests):

| sheet | bytes | tokens | tokenize | today's whole parse |
|---|---|---|---|---|
| YouTube main | 3,213,960 | 516,153 | 8.77 ms | 10.97 ms |
| YouTube second | 550,424 | 107,837 | 1.87 ms | 1.92 ms |
| Wikipedia | 209,585 | 35,653 | 0.74 ms | 0.60 ms |

Tokenizing into an array costs about what today's parse does in all, so a parser on top of the
array would come out near twice today's time.  Most of it is writing the tokens (40 bytes each,
whitespace included).  So steps 3 and 4 use the tokenizer as a stream: the parser pulls one token
at a time (css_next_token on a small state, the arena kept for escaped values only), with no array,
and the array API stays for tests.  The target stays: parse time within noise of today's on these
sheets, measured with the same benchmark before step 3 lands.

## Why

src/css_parser.c reads stylesheets by scanning characters: a selector is everything up to `{`, a
property everything up to `:`, a value everything up to `;` or `}` outside quotes and parentheses,
an at-rule a keyword then up to `{` or `;`.  That works for well-formed CSS and fails where CSS
Syntax says otherwise:

- **Escapes** (about 30 pages): `\64\69\76` is `div`, `\31 23` is "123", `\"` inside a string is a
  quote, `\:` in a selector is a literal colon.  We keep escapes as text, so escaped selectors,
  properties and keywords match nothing.
- **Error recovery** (about 25): a declaration that hits a `{` swallows the block; `@foo ] } )`
  must skip to the matching `;` or block, counting brackets and skipping strings; an at-rule inside
  a declaration block is ignored up to its own end; `<!--` / `-->` are ignored only at the top
  level; an unterminated string ends at the newline and voids its declaration; end of file closes
  open blocks.
- **Value validation** (about 12): `width: "10px"` (a string, not a length), `color: rgb(0, 128 ,0)`
  with odd spacing, `+10px` / `-.5em`, `!` not followed by `important`, values that start valid then
  carry junk.
- **Selectors** (about 8): pseudo-class case insensitivity, identifiers starting with `-` or `\31`,
  attribute values with digits.
- **Encodings** (16): stylesheets in ISO-8859-5/6/7/8/11, KOI8-R and Big5, and the referring
  document's encoding (css_charset.c decodes UTF-8, UTF-16, windows-1252 and Shift_JIS only).

These matter beyond the tests: minified and generated CSS uses escapes (Tailwind's `.sm\:p-4`,
`.w-1\/2`), and a parser that recovers differently from browsers applies or drops rules they do
not.

## How browsers do it

CSS Syntax Level 3: a **tokenizer** (section 4) turns the stylesheet into tokens (ident, function,
at-keyword, hash, string, bad-string, url, bad-url, delim, number, percentage, dimension,
whitespace, CDO, CDC, colon, semicolon, comma, brackets) with escapes already resolved and comments
dropped; the **parser** (section 5) consumes a list of rules (qualified rules and at-rules, each
with a prelude of component values and a block), and a block's contents as a list of declarations,
each "consume a declaration" with exact error recovery (a bad one is skipped up to the next `;` at
its own nesting level).  Then each property's grammar validates its value; an invalid one is
dropped (the declaration does not exist).

## Design

### Tokenizer (new src/css_tokenizer.c)

- Spec algorithm over the UTF-8 text (after css_charset.c decoded it), producing tokens into a
  growable array per sheet (or per parse chunk): type, flags (ident-like "id" hash, number integer
  or not), the source span, and for idents / strings / urls / functions / at-keywords / hashes /
  dimensions' units their **value with escapes resolved** (an arena per parse, so no allocation per
  token).
- Preprocessing: CR LF / CR / FF as LF, NUL and surrogates as U+FFFD, done in the tokenizer's
  input reader rather than a copy.
- Fast path: the common token kinds of a minified sheet (idents, `{`, `}`, `:`, `;`, numbers, hashes)
  in a tight loop; measured with the replay gate (YouTube's sheets are about 1 MB) against today's
  parser.

### Parser (src/css_parser.c, rewritten front end)

- Consume a list of rules (top level: CDO / CDC ignored), qualified rules with their prelude and
  block, at-rules with their prelude and block or `;`, blocks and functions as nested component
  values, all by the spec.
- @media, @supports, @layer, @container blocks: nested rule lists as today (media conditions
  attached as now); @font-face, @keyframes, @page, @counter-style skipped as today; @import and
  @charset honoured only where valid (top, before other rules); any other at-rule ignored with its
  block.
- Declaration lists (rule blocks and style attributes, one function): consume a declaration;
  `!important` recognised only as `!` then the ident `important` at the end; at-rules inside are
  ignored; a bad declaration skipped to its `;`.
- **Output unchanged**: CssStylesheet / CssRule / CssDeclaration with selector_text, property and
  value strings, so the cascade, selector compiler, matcher and the property parsers in
  css_layout.c keep working.  The strings are re-serialized from the tokens:
  - identifiers with escapes resolved, except where the character would mean something else in
    the string the consumers re-read (a selector's `.`, `:`, `#`, `[`, whitespace, quotes; a
    leading digit or `-` digit): those are written back as CSS escapes (`\:`), which the selector
    parser learns to read as literal characters;
  - strings re-quoted with `"` and only `"` / `\` / newline escaped, so value consumers that read
    strings (content, font-family, quotes) see one canonical form;
  - comments gone, whitespace runs one space.
- The parallel parse (sheet_split chunks, css_stylesheet_parse_range) stays: the split already
  respects strings and comments; it also learns escapes (a `\}` is not a block end).

### Values

The property parsers in css_layout.c validate more of their grammar, so an invalid value is
dropped instead of half applied: a string where a length / keyword is expected, junk after a valid
value, numbers with signs and exponents (`+.5e1px`), `rgb()` with any whitespace.  A small shared
helper over the declaration's tokens (re-tokenizing the value string, or keeping the tokens on the
declaration) gives them "one length", "one keyword of this set", "a color" checks.

### Encodings (css_charset.c)

All WHATWG single-byte encodings (ISO-8859-2..16, windows-125x, KOI8-R/U, macintosh, ...) from
tables generated with Node's TextDecoder (as tools/gen_sjis_index.js does for Shift_JIS), and
Big5 / EUC-JP / EUC-KR / GBK if the table size is acceptable; the referring document's encoding
passed down as the fallback (today NULL: UTF-8).

## Steps (each lands on its own: full css/CSS2 run, no page lost, replay gate)

1. **Encodings**: single-byte tables (and Big5), the document's encoding as the fallback.  Expected:
   the at-charset / character-encoding / content-type pages (about 16).  Independent of the rest.
2. **Tokenizer** with unit tests (the spec's examples, escapes, bad strings / urls, numbers),
   unused by the parser yet; its speed measured on YouTube's and Wikipedia's sheets.
3. **Declaration lists from tokens** (rule blocks and style attributes): error recovery, `!important`,
   escapes in properties and values, canonical strings.  Expected: malformed-decl, declaration,
   core-syntax, escapes in values, strings, eof.
4. **Rule lists from tokens**: qualified rules, at-rules, nested blocks, CDO / CDC, bracket matching.
   Expected: at-rule, blocks, matching-brackets, comments, sgml-comments, square-brackets.
5. **Escapes in selectors**: the selector parser reads `\X` as a literal; identifiers starting with
   `-` / escaped digits; case-insensitive pseudo-classes.  Expected: escaped-ident, escapes,
   ident, case-sensitive.
6. **Value validation**: the shared checks and the properties that need them.  Expected:
   ignored-rules, keywords, colors, signed-numbers, uri.

## Checks

- Unit tests for the tokenizer (spec examples) and for declaration / rule parsing (each recovery
  case of CSS Syntax section 5).
- Full css/CSS2 runs after each step (no page lost), and the other CSS suites the run covers.
- Replay gate A/B (YouTube and Wikipedia load times; the parse stage's own time with CSS_LOG=1):
  stylesheet parsing must not get slower in step 3 and 4 by more than noise; the keep-tree check.
- The selector compiler's output on YouTube's sheets compared before and after (same rules, same
  selectors, same declarations, apart from escapes now resolved).

## Risks

- Speed: a token array per sheet is more work than scanning; the arena and the fast path keep it to
  one pass, measured before steps 3 and 4 land.
- Behaviour on real pages: rules browsers drop that we applied (and the reverse) will change pages;
  the replay screenshots are compared at each step.
- Re-serialized strings: any consumer that relied on the raw text (an escape it handled itself, a
  comment it skipped) is found by the full runs and the screenshot comparison.
