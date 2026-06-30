#!/usr/bin/env python3
"""
Extract Browser APIs from YouTube Data Files

Scans all JS/HTML/CSS/JSON files under youtube_data/ and writes a
straight, deduplicated, sorted list of browser API names to:
    youtube_data_analysis/extracted_browser_apis.txt

Usage:
    python scripts/extract_browser_apis.py
"""

import re
import sys
from pathlib import Path

# Root paths (relative to project root)
DATA_DIR = Path("youtube_data")
OUTPUT_DIR = Path("youtube_data_analysis")
OUTPUT_FILE = OUTPUT_DIR / "extracted_browser_apis.txt"

# Identifier regex piece.
IDENT = r'[a-zA-Z_$][a-zA-Z0-9_$]*'
# Property chain limited to 6 levels to avoid runaway matching on minified JS.
PROP_CHAIN = r'(?:\.' + IDENT + r'){0,6}'

# Combined regex patterns.  Each pattern extracts one or more API references.
API_PATTERNS = [
    # Global object property chains.
    re.compile(
        r'\b(?:window|document|navigator|location|console|localStorage|'
        r'sessionStorage|history|screen|performance|customElements|crypto|'
        r'visualViewport|devicePixelRatio|innerWidth|innerHeight|'
        r'outerWidth|outerHeight|screenX|screenY|pageXOffset|pageYOffset|'
        r'scrollX|scrollY|opener|parent|top|self|globalThis)\.' + IDENT + PROP_CHAIN + r'\b'
    ),

    # Static methods on built-in globals.
    re.compile(
        r'\b(?:URL|Promise|JSON|Object|Array|String|Number|Math|Date|RegExp|'
        r'Map|Set|WeakMap|WeakSet|Intl|Reflect|Symbol|Proxy|Error|'
        r'TextEncoder|TextDecoder|WebAssembly|DataView|ArrayBuffer|'
        r'SharedArrayBuffer|Atomics|BigInt|Iterator|AsyncIterator)\.' + IDENT + r'\b'
    ),

    # Global functions (called with parentheses).
    re.compile(
        r'\b(?:fetch|setTimeout|setInterval|clearTimeout|clearInterval|'
        r'requestAnimationFrame|cancelAnimationFrame|requestIdleCallback|'
        r'cancelIdleCallback|matchMedia|btoa|atob|encodeURIComponent|'
        r'decodeURIComponent|encodeURI|decodeURI|escape|unescape|isNaN|'
        r'isFinite|parseInt|parseFloat|queueMicrotask|structuredClone|'
        r'getComputedStyle|getSelection|open|close|alert|confirm|prompt|'
        r'scroll|scrollTo|scrollBy|moveTo|moveBy|resizeTo|resizeBy|print|'
        r'stop|focus|blur)\s*\('
    ),

    # Constructors used with `new`.
    re.compile(
        r'\bnew\s+(?:XMLHttpRequest|WebSocket|Worker|SharedWorker|ServiceWorker|'
        r'EventSource|MutationObserver|IntersectionObserver|ResizeObserver|'
        r'PerformanceObserver|ReportingObserver|MediaSource|ManagedMediaSource|'
        r'WebKitMediaSource|Event|CustomEvent|MouseEvent|KeyboardEvent|TouchEvent|'
        r'MessageEvent|FocusEvent|InputEvent|CompositionEvent|PointerEvent|'
        r'WheelEvent|DragEvent|AnimationEvent|TransitionEvent|Blob|File|FileReader|'
        r'FormData|Headers|Request|Response|URL|URLSearchParams|Image|ImageData|'
        r'ImageBitmap|OffscreenCanvas|DOMParser|XMLSerializer|XPathEvaluator|'
        r'AbortController|BroadcastChannel|Audio|AudioContext|AudioBuffer|'
        r'AudioBufferSourceNode|GainNode|AnalyserNode|OscillatorNode|'
        r'HTML[a-zA-Z]*Element|SVG[a-zA-Z]*Element|ShadowRoot|DocumentFragment|'
        r'Range|Selection|NodeIterator|TreeWalker|NamedNodeMap|DOMTokenList|'
        r'DOMRect|DOMRectList|DOMPoint|DOMMatrix|MediaQueryList|URLPattern|'
        r'TextEncoder|TextDecoder)\b'
    ),

    # Standalone constructor/global object names.
    re.compile(
        r'\b(?:XMLHttpRequest|WebSocket|Worker|SharedWorker|ServiceWorker|'
        r'EventSource|MutationObserver|IntersectionObserver|ResizeObserver|'
        r'PerformanceObserver|ReportingObserver|MediaSource|ManagedMediaSource|'
        r'WebKitMediaSource|Blob|File|FileReader|FormData|Headers|Request|Response|'
        r'URL|URLSearchParams|DOMParser|XMLSerializer|XPathEvaluator|AbortController|'
        r'AbortSignal|BroadcastChannel|HTML[a-zA-Z]*Element|SVG[a-zA-Z]*Element|'
        r'WebAssembly|AudioContext|OffscreenCanvas|ShadowRoot|DocumentFragment|'
        r'CustomEvent|MutationRecord|DOMTokenList|MediaQueryList|URLPattern|'
        r'TextEncoder|TextDecoder)\b'
    ),

    # Common DOM / EventTarget methods.
    re.compile(
        r'\b(?:addEventListener|removeEventListener|dispatchEvent|getAttribute|'
        r'setAttribute|removeAttribute|hasAttribute|getBoundingClientRect|'
        r'getClientRects|getComputedStyle|querySelector|querySelectorAll|'
        r'getElementById|getElementsByClassName|getElementsByTagName|'
        r'getElementsByName|createElement|createElementNS|createTextNode|'
        r'createDocumentFragment|importNode|adoptNode|appendChild|removeChild|'
        r'insertBefore|replaceChild|cloneNode|contains|closest|matches|'
        r'prepend|append|before|after|replaceWith|remove|scrollIntoView|'
        r'scrollTo|scrollBy|focus|blur|click|select|submit|reset|'
        r'preventDefault|stopPropagation|stopImmediatePropagation|composedPath|'
        r'attachShadow|requestFullscreen|exitFullscreen|getRootNode|normalize|'
        r'insertAdjacentElement|insertAdjacentHTML|insertAdjacentText|'
        r'toggleAttribute|setHTML|getHTML)\s*\('
    ),

    # Common built-in prototype methods called on instances.
    re.compile(
        r'\.(?:forEach|map|filter|reduce|reduceRight|find|findIndex|findLast|'
        r'findLastIndex|some|every|includes|indexOf|lastIndexOf|push|pop|shift|'
        r'unshift|splice|slice|concat|join|sort|reverse|flat|flatMap|fill|'
        r'copyWithin|entries|keys|values|from|of|at|with|padStart|padEnd|'
        r'trim|trimStart|trimEnd|replace|replaceAll|split|match|matchAll|'
        r'search|toLowerCase|toUpperCase|substring|substr|charAt|charCodeAt|'
        r'codePointAt|startsWith|endsWith|repeat|normalize|toFixed|toPrecision|'
        r'toExponential|toString|valueOf|hasOwnProperty|toLocaleString|'
        r'getTime|setTime|getFullYear|setFullYear|exec|test|then|catch|finally|'
        r'defineProperty|defineProperties|getOwnPropertyDescriptor|'
        r'getOwnPropertyDescriptors|getOwnPropertyNames|getOwnPropertySymbols|'
        r'getPrototypeOf|setPrototypeOf|preventExtensions|seal|freeze|'
        r'isSealed|isFrozen|isExtensible|assign|create|fromEntries|hasOwn|'
        r'groupBy|postMessage|terminate|send|open|abort|setRequestHeader|'
        r'getResponseHeader|getAllResponseHeaders|overrideMimeType|close|'
        r'play|pause|load|append|delete|get|getAll|has|set|json|text|blob|'
        r'arrayBuffer|formData|clone|getReader|cancel|read|releaseLock|'
        r'pipeTo|pipeThrough|tee|createObjectURL|revokeObjectURL|supportsType|'
        r'addSourceBuffer|removeSourceBuffer|endOfStream|appendBuffer)\s*\('
    ),
]

# Standard browser globals / roots.  Used to filter out YouTube-specific
# internals such as window.yt, window.ytcfg, window.WIZ_global_data, etc.
STANDARD_BROWSER_ROOTS = {
    # Global objects
    'window', 'document', 'navigator', 'location', 'console', 'localStorage',
    'sessionStorage', 'history', 'screen', 'performance', 'customElements',
    'crypto', 'visualViewport', 'devicePixelRatio', 'innerWidth', 'innerHeight',
    'outerWidth', 'outerHeight', 'screenX', 'screenY', 'pageXOffset',
    'pageYOffset', 'scrollX', 'scrollY', 'opener', 'parent', 'top', 'self',
    'globalThis',

    # Built-in constructors / globals
    'URL', 'URLSearchParams', 'Promise', 'JSON', 'Object', 'Array', 'String',
    'Number', 'Math', 'Date', 'RegExp', 'Map', 'Set', 'WeakMap', 'WeakSet',
    'Intl', 'Reflect', 'Symbol', 'Proxy', 'Error', 'TypeError', 'ReferenceError',
    'SyntaxError', 'RangeError', 'URIError', 'EvalError', 'AggregateError',
    'TextEncoder', 'TextDecoder', 'WebAssembly', 'DataView', 'ArrayBuffer',
    'SharedArrayBuffer', 'Atomics', 'BigInt', 'Iterator', 'AsyncIterator',
    'Generator', 'AsyncGenerator', 'Iterator', 'AsyncIterator',

    # Networking / async
    'fetch', 'XMLHttpRequest', 'WebSocket', 'Worker', 'SharedWorker',
    'ServiceWorker', 'EventSource', 'AbortController', 'AbortSignal',
    'BroadcastChannel',

    # Observers
    'MutationObserver', 'IntersectionObserver', 'ResizeObserver',
    'PerformanceObserver', 'ReportingObserver',

    # Media
    'MediaSource', 'ManagedMediaSource', 'WebKitMediaSource', 'Audio',
    'AudioContext', 'AudioBuffer', 'AudioBufferSourceNode', 'GainNode',
    'AnalyserNode', 'OscillatorNode', 'HTMLAudioElement', 'HTMLVideoElement',
    'HTMLMediaElement', 'Image', 'ImageData', 'ImageBitmap', 'OffscreenCanvas',

    # DOM parsing / serialization
    'DOMParser', 'XMLSerializer', 'XMLHttpRequest', 'XPathEvaluator',
    'Blob', 'File', 'FileReader', 'FormData', 'Headers', 'Request', 'Response',

    # Events
    'Event', 'CustomEvent', 'MouseEvent', 'KeyboardEvent', 'TouchEvent',
    'MessageEvent', 'FocusEvent', 'InputEvent', 'CompositionEvent',
    'PointerEvent', 'WheelEvent', 'DragEvent', 'AnimationEvent',
    'TransitionEvent', 'MutationRecord',

    # DOM interfaces
    'ShadowRoot', 'DocumentFragment', 'Range', 'Selection', 'NodeIterator',
    'TreeWalker', 'NamedNodeMap', 'DOMTokenList', 'DOMRect', 'DOMRectList',
    'DOMPoint', 'DOMMatrix', 'MediaQueryList', 'URLPattern',

    # Timers / animation / global functions
    'setTimeout', 'setInterval', 'clearTimeout', 'clearInterval',
    'requestAnimationFrame', 'cancelAnimationFrame', 'requestIdleCallback',
    'cancelIdleCallback', 'matchMedia', 'btoa', 'atob', 'encodeURIComponent',
    'decodeURIComponent', 'encodeURI', 'decodeURI', 'escape', 'unescape',
    'isNaN', 'isFinite', 'parseInt', 'parseFloat', 'queueMicrotask',
    'structuredClone', 'getComputedStyle', 'getSelection', 'open', 'close',
    'alert', 'confirm', 'prompt', 'scroll', 'scrollTo', 'scrollBy', 'moveTo',
    'moveBy', 'resizeTo', 'resizeBy', 'print', 'stop', 'focus', 'blur',

    # DOM / EventTarget methods
    'addEventListener', 'removeEventListener', 'dispatchEvent', 'getAttribute',
    'setAttribute', 'removeAttribute', 'hasAttribute', 'getBoundingClientRect',
    'getClientRects', 'querySelector', 'querySelectorAll', 'getElementById',
    'getElementsByClassName', 'getElementsByTagName', 'getElementsByName',
    'createElement', 'createElementNS', 'createTextNode',
    'createDocumentFragment', 'importNode', 'adoptNode', 'appendChild',
    'removeChild', 'insertBefore', 'replaceChild', 'cloneNode', 'contains',
    'closest', 'matches', 'prepend', 'append', 'before', 'after', 'replaceWith',
    'remove', 'scrollIntoView', 'focus', 'blur', 'click', 'select', 'submit',
    'reset', 'preventDefault', 'stopPropagation', 'stopImmediatePropagation',
    'composedPath', 'attachShadow', 'requestFullscreen', 'exitFullscreen',
    'getRootNode', 'normalize', 'insertAdjacentElement', 'insertAdjacentHTML',
    'insertAdjacentText', 'toggleAttribute', 'setHTML', 'getHTML',

    # Common prototype methods
    'forEach', 'map', 'filter', 'reduce', 'reduceRight', 'find', 'findIndex',
    'findLast', 'findLastIndex', 'some', 'every', 'includes', 'indexOf',
    'lastIndexOf', 'push', 'pop', 'shift', 'unshift', 'splice', 'slice',
    'concat', 'join', 'sort', 'reverse', 'flat', 'flatMap', 'fill',
    'copyWithin', 'entries', 'keys', 'values', 'from', 'of', 'at', 'with',
    'padStart', 'padEnd', 'trim', 'trimStart', 'trimEnd', 'replace',
    'replaceAll', 'split', 'match', 'matchAll', 'search', 'toLowerCase',
    'toUpperCase', 'substring', 'substr', 'charAt', 'charCodeAt',
    'codePointAt', 'startsWith', 'endsWith', 'repeat', 'normalize', 'toFixed',
    'toPrecision', 'toExponential', 'toString', 'valueOf', 'hasOwnProperty',
    'toLocaleString', 'getTime', 'setTime', 'getFullYear', 'setFullYear',
    'exec', 'test', 'then', 'catch', 'finally', 'defineProperty',
    'defineProperties', 'getOwnPropertyDescriptor', 'getOwnPropertyDescriptors',
    'getOwnPropertyNames', 'getOwnPropertySymbols', 'getPrototypeOf',
    'setPrototypeOf', 'preventExtensions', 'seal', 'freeze', 'isSealed',
    'isFrozen', 'isExtensible', 'assign', 'create', 'fromEntries', 'hasOwn',
    'groupBy', 'postMessage', 'terminate', 'send', 'open', 'abort',
    'setRequestHeader', 'getResponseHeader', 'getAllResponseHeaders',
    'overrideMimeType', 'close', 'play', 'pause', 'load', 'append', 'delete',
    'get', 'getAll', 'has', 'set', 'json', 'text', 'blob', 'arrayBuffer',
    'formData', 'clone', 'getReader', 'cancel', 'read', 'releaseLock',
    'pipeTo', 'pipeThrough', 'tee', 'createObjectURL', 'revokeObjectURL',
    'supportsType', 'addSourceBuffer', 'removeSourceBuffer', 'endOfStream',
    'appendBuffer',
}


def is_standard_browser_api(api: str) -> bool:
    """Return True if the API does not look like a YouTube-specific internal."""
    # Blacklist known YouTube/obfuscated internal namespaces.
    non_browser_prefixes = (
        'window.yt.',
        'window.yt ',
        'window.YT.',
        'window.YT ',
        'window.ytcfg',
        'window.ytcsi',
        'window.WIZ_',
        'window.ytInitial',
        'window.ytplayer',
        'window.youtubewebview',
        'window.V6V',
        'window.ytAt',
    )
    return not api.startswith(non_browser_prefixes)


def normalize_api(raw: str) -> str:
    """Normalize a raw regex match into a clean API name."""
    s = raw.strip()
    # Strip leading 'new ' from constructor matches.
    if s.startswith('new '):
        s = s[4:].strip()
    # Strip trailing '(' and whitespace from function-style matches.
    s = s.rstrip().rstrip('(').rstrip()
    # Strip leading dot from prototype-method matches.
    if s.startswith('.'):
        s = s[1:].strip()
    return s


def extract_apis_from_text(text: str) -> set[str]:
    """Return a set of browser API names found in the given text."""
    found: set[str] = set()
    for pattern in API_PATTERNS:
        for match in pattern.finditer(text):
            api = normalize_api(match.group(0))
            if api:
                found.add(api)
    return found


def main() -> int:
    if not DATA_DIR.is_dir():
        print(f"Error: data directory not found: {DATA_DIR.resolve()}", file=sys.stderr)
        return 1

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    # Find all relevant files, including captured JSON responses.
    files = sorted(
        p
        for p in DATA_DIR.rglob('*')
        if p.is_file() and p.suffix.lower() in {'.js', '.html', '.htm', '.css', '.json'}
    )

    print(f"Scanning {len(files)} files in {DATA_DIR} ...")

    all_apis: set[str] = set()
    per_file: dict[Path, set[str]] = {}

    for path in files:
        try:
            text = path.read_text(encoding='utf-8', errors='ignore')
        except Exception as e:
            print(f"  Warning: could not read {path}: {e}", file=sys.stderr)
            continue

        apis = extract_apis_from_text(text)
        if apis:
            per_file[path] = apis
            all_apis.update(apis)

    sorted_apis = sorted(all_apis, key=str.lower)

    # Write straight list (everything).
    OUTPUT_FILE.write_text('\n'.join(sorted_apis) + '\n', encoding='utf-8')

    # Write filtered list of standard browser APIs only.
    standard_apis = sorted(
        {api for api in all_apis if is_standard_browser_api(api)},
        key=str.lower,
    )
    standard_file = OUTPUT_DIR / 'extracted_browser_apis_standard.txt'
    standard_file.write_text('\n'.join(standard_apis) + '\n', encoding='utf-8')

    # Also write a detailed report with per-file breakdown.
    detail_path = OUTPUT_DIR / 'extracted_browser_apis_detailed.txt'
    detail_lines = [
        f"Browser API extraction from {DATA_DIR}",
        f"Files scanned: {len(files)}",
        f"Files with APIs: {len(per_file)}",
        f"Total unique APIs: {len(sorted_apis)}",
        "",
        "=" * 80,
        "COMBINED API LIST",
        "=" * 80,
        "",
    ]
    detail_lines.extend(sorted_apis)
    detail_lines.extend([
        "",
        "=" * 80,
        "PER-FILE API COUNTS",
        "=" * 80,
        "",
    ])
    for path in sorted(per_file):
        apis = sorted(per_file[path], key=str.lower)
        detail_lines.append(f"{path.name}: {len(apis)} APIs")
        for api in apis:
            detail_lines.append(f"  {api}")
        detail_lines.append("")
    detail_path.write_text('\n'.join(detail_lines) + '\n', encoding='utf-8')

    print(f"Done.")
    print(f"  Straight list:   {OUTPUT_FILE} ({len(sorted_apis)} APIs)")
    print(f"  Standard list:   {standard_file} ({len(standard_apis)} APIs)")
    print(f"  Detailed list:   {detail_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
