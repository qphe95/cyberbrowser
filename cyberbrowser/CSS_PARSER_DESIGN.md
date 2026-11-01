# CSS parsing: a CSS Syntax 3 tokenizer and parser

Status: step 1 done (css/CSS2 5218 -> 5234, none lost: the WHATWG single-byte encodings and
Big5 decoded, tables from tools/gen_charset_tables.js; a sheet whose Content-Type is not text/css
applies nothing).  Not done in it: the referring document's encoding as a sheet's fallback
(at-charset-012), which needs the HTML parser to decode non-UTF-8 documents first.  Step 2 done
(src/css_tokenizer.c, tests/test_css_tokenizer.c; not used by the parser yet).  Steps 3-6 planned.

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
