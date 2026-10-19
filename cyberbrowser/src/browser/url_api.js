/*
 * url_api.js - URL, URLSearchParams and HTMLHyperlinkElementUtils
 * (WHATWG URL Standard and HTML Standard), on top of the C URL parser.
 *
 * This file is compiled into the browser as browser/url_api_js.inc
 * (regenerate with tools/gen_js_inc.py after editing).  It evaluates to a
 * function called once with (global, natives); `natives` provides:
 *   parse(input [, baseRecord])     -> record string | null
 *   components(record)              -> [href, origin, protocol, username,
 *                                       password, host, hostname, port,
 *                                       pathname, search, hash]
 *   set(record, setter, value)      -> record | null (invalid href)
 *   formEncode(s), formDecode(s)    application/x-www-form-urlencoded
 *   createObjectURL, revokeObjectURL
 */
(function (global, N) {
    'use strict';

    const parse = N.parse, components = N.components, setPart = N.set;
    const formEncode = N.formEncode, formDecode = N.formDecode;

    const HREF = 0, ORIGIN = 1, PROTOCOL = 2, USERNAME = 3, PASSWORD = 4,
          HOST = 5, HOSTNAME = 6, PORT = 7, PATHNAME = 8, SEARCH = 9, HASH = 10;
    const SETTER = { href: 0, protocol: 1, username: 2, password: 3, host: 4,
                     hostname: 5, port: 6, pathname: 7, search: 8, hash: 9 };

    const urls = new WeakMap();     /* URL -> { rec, c, query } */
    const params = new WeakMap();   /* URLSearchParams -> { list, url } */

    /* WebIDL USVString conversion: ToString (throws for symbols), then
       lone surrogates become U+FFFD */
    const toWellFormed = String.prototype.toWellFormed;
    function usv(v) { return toWellFormed.call(`${v}`); }

    function needArgs(n, got, op, iface) {
        if (got < n)
            throw new TypeError(`Failed to execute '${op}' on '${iface}': ${n} argument${n > 1 ? 's' : ''} required, but only ${got} present.`);
    }

    /* application/x-www-form-urlencoded parser / serializer */
    function parseForm(s) {
        const out = [];
        if (s === '')
            return out;
        for (const seq of s.split('&')) {
            if (seq === '')
                continue;
            const i = seq.indexOf('=');
            const name = i < 0 ? seq : seq.slice(0, i);
            const value = i < 0 ? '' : seq.slice(i + 1);
            out.push([formDecode(name), formDecode(value)]);
        }
        return out;
    }

    function serializeForm(list) {
        let s = '';
        for (let i = 0; i < list.length; i++) {
            if (i)
                s += '&';
            s += formEncode(list[i][0]) + '=' + formEncode(list[i][1]);
        }
        return s;
    }

    function urlState(o) {
        const s = urls.get(o);
        if (!s)
            throw new TypeError('Illegal invocation');
        return s;
    }

    function paramsState(o) {
        const s = params.get(o);
        if (!s)
            throw new TypeError('Illegal invocation');
        return s;
    }

    /* the query string of a URL record, as the search params list sees it */
    function queryList(c) {
        return parseForm(c[SEARCH].slice(1));
    }

    function makeParams(list, url) {
        const o = Object.create(URLSearchParams.prototype);
        params.set(o, { list, url });
        return o;
    }

    /* URLSearchParams "update steps" */
    function updateParams(ps) {
        if (!ps.url)
            return;
        const us = urls.get(ps.url);
        /* "" makes the query null (the search setter does the same) */
        us.rec = setPart(us.rec, SETTER.search, serializeForm(ps.list));
        us.c = components(us.rec);
    }

    function setUrlPart(o, name, v) {
        const s = urlState(o);
        s.rec = setPart(s.rec, SETTER[name], usv(v));
        s.c = components(s.rec);
    }

    class URL {
        constructor(url, base = undefined) {
            needArgs(1, arguments.length, 'constructor', 'URL');
            url = usv(url);
            let baseRec;
            if (base !== undefined) {
                baseRec = parse(usv(base));
                if (baseRec === null)
                    throw new TypeError(`Failed to construct 'URL': Invalid base URL`);
            }
            const rec = parse(url, baseRec);
            if (rec === null)
                throw new TypeError(`Failed to construct 'URL': Invalid URL`);
            const c = components(rec);
            const st = { rec, c, query: null };
            urls.set(this, st);
            st.query = makeParams(queryList(c), this);
        }

        static canParse(url, base = undefined) {
            needArgs(1, arguments.length, 'canParse', 'URL');
            let baseRec;
            if (base !== undefined) {
                baseRec = parse(usv(base));
                if (baseRec === null)
                    return false;
            }
            return parse(usv(url), baseRec) !== null;
        }

        static parse(url, base = undefined) {
            needArgs(1, arguments.length, 'parse', 'URL');
            try {
                return base === undefined ? new URL(url) : new URL(url, base);
            } catch (e) {
                return null;
            }
        }

        get href() { return urlState(this).c[HREF]; }
        set href(v) {
            const s = urlState(this);
            const rec = parse(usv(v));
            if (rec === null)
                throw new TypeError(`Failed to set the 'href' property on 'URL': Invalid URL`);
            s.rec = rec;
            s.c = components(rec);
            params.get(s.query).list = queryList(s.c);
        }
        get origin() { return urlState(this).c[ORIGIN]; }
        get protocol() { return urlState(this).c[PROTOCOL]; }
        set protocol(v) { setUrlPart(this, 'protocol', v); }
        get username() { return urlState(this).c[USERNAME]; }
        set username(v) { setUrlPart(this, 'username', v); }
        get password() { return urlState(this).c[PASSWORD]; }
        set password(v) { setUrlPart(this, 'password', v); }
        get host() { return urlState(this).c[HOST]; }
        set host(v) { setUrlPart(this, 'host', v); }
        get hostname() { return urlState(this).c[HOSTNAME]; }
        set hostname(v) { setUrlPart(this, 'hostname', v); }
        get port() { return urlState(this).c[PORT]; }
        set port(v) { setUrlPart(this, 'port', v); }
        get pathname() { return urlState(this).c[PATHNAME]; }
        set pathname(v) { setUrlPart(this, 'pathname', v); }
        get search() { return urlState(this).c[SEARCH]; }
        set search(v) {
            setUrlPart(this, 'search', v);
            const s = urls.get(this);
            params.get(s.query).list = queryList(s.c);
        }
        get searchParams() { return urlState(this).query; }
        get hash() { return urlState(this).c[HASH]; }
        set hash(v) { setUrlPart(this, 'hash', v); }
        toString() { return urlState(this).c[HREF]; }
        toJSON() { return urlState(this).c[HREF]; }
    }

    class URLSearchParams {
        constructor(init = '') {
            let list = [];
            if (init !== null && (typeof init === 'object' || typeof init === 'function')) {
                const it = init[Symbol.iterator];
                if (it !== undefined && it !== null) {
                    if (typeof it !== 'function')
                        throw new TypeError(`Failed to construct 'URLSearchParams': The object must have a callable @@iterator property.`);
                    for (const pair of init) {
                        if (pair === null || (typeof pair !== 'object' && typeof pair !== 'function'))
                            throw new TypeError(`Failed to construct 'URLSearchParams': The provided value cannot be converted to a sequence.`);
                        const arr = Array.from(pair);
                        if (arr.length !== 2)
                            throw new TypeError(`Failed to construct 'URLSearchParams': Sequence initializer must only contain pair elements`);
                        list.push([usv(arr[0]), usv(arr[1])]);
                    }
                } else {
                    /* record<USVString, USVString> */
                    const index = new Map();
                    for (const key of Reflect.ownKeys(init)) {
                        const desc = Reflect.getOwnPropertyDescriptor(init, key);
                        if (desc === undefined || !desc.enumerable)
                            continue;
                        const k = usv(key);
                        const v = usv(init[key]);
                        if (index.has(k)) {
                            list[index.get(k)][1] = v;
                        } else {
                            index.set(k, list.length);
                            list.push([k, v]);
                        }
                    }
                }
            } else {
                let s = usv(init);
                if (s.charCodeAt(0) === 0x3F)  /* '?' */
                    s = s.slice(1);
                list = parseForm(s);
            }
            params.set(this, { list, url: null });
        }

        get size() { return paramsState(this).list.length; }

        append(name, value) {
            needArgs(2, arguments.length, 'append', 'URLSearchParams');
            const s = paramsState(this);
            s.list.push([usv(name), usv(value)]);
            updateParams(s);
        }

        delete(name, value = undefined) {
            needArgs(1, arguments.length, 'delete', 'URLSearchParams');
            const s = paramsState(this);
            name = usv(name);
            if (value !== undefined) {
                value = usv(value);
                s.list = s.list.filter(p => !(p[0] === name && p[1] === value));
            } else {
                s.list = s.list.filter(p => p[0] !== name);
            }
            updateParams(s);
        }

        get(name) {
            needArgs(1, arguments.length, 'get', 'URLSearchParams');
            name = usv(name);
            for (const p of paramsState(this).list)
                if (p[0] === name)
                    return p[1];
            return null;
        }

        getAll(name) {
            needArgs(1, arguments.length, 'getAll', 'URLSearchParams');
            name = usv(name);
            const out = [];
            for (const p of paramsState(this).list)
                if (p[0] === name)
                    out.push(p[1]);
            return out;
        }

        has(name, value = undefined) {
            needArgs(1, arguments.length, 'has', 'URLSearchParams');
            name = usv(name);
            if (value !== undefined)
                value = usv(value);
            for (const p of paramsState(this).list)
                if (p[0] === name && (value === undefined || p[1] === value))
                    return true;
            return false;
        }

        set(name, value) {
            needArgs(2, arguments.length, 'set', 'URLSearchParams');
            const s = paramsState(this);
            name = usv(name);
            value = usv(value);
            let found = false;
            const out = [];
            for (const p of s.list) {
                if (p[0] === name) {
                    if (found)
                        continue;
                    found = true;
                    out.push([name, value]);
                } else {
                    out.push(p);
                }
            }
            if (!found)
                out.push([name, value]);
            s.list = out;
            updateParams(s);
        }

        sort() {
            const s = paramsState(this);
            /* stable, by UTF-16 code units of the names */
            const keyed = s.list.map((p, i) => [p, i]);
            keyed.sort((a, b) => a[0][0] < b[0][0] ? -1 : a[0][0] > b[0][0] ? 1 : a[1] - b[1]);
            s.list = keyed.map(k => k[0]);
            updateParams(s);
        }

        forEach(callback, thisArg = undefined) {
            needArgs(1, arguments.length, 'forEach', 'URLSearchParams');
            if (typeof callback !== 'function')
                throw new TypeError(`Failed to execute 'forEach' on 'URLSearchParams': The callback provided as parameter 1 is not a function.`);
            const s = paramsState(this);
            for (let i = 0; i < s.list.length; i++) {
                const p = s.list[i];
                callback.call(thisArg, p[1], p[0], this);
            }
        }

        entries() { return makeIterator(this, 'key+value'); }
        keys() { return makeIterator(this, 'key'); }
        values() { return makeIterator(this, 'value'); }
        toString() { return serializeForm(paramsState(this).list); }
    }

    /* URLSearchParams Iterator: iterates the live list */
    const IteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));
    const iterators = new WeakMap();
    const ParamsIteratorPrototype = Object.create(IteratorPrototype, {
        next: {
            value: function next() {
                const it = iterators.get(this);
                if (!it)
                    throw new TypeError('Illegal invocation');
                const list = params.get(it.target).list;
                if (it.index >= list.length)
                    return { value: undefined, done: true };
                const p = list[it.index++];
                const value = it.kind === 'key' ? p[0] : it.kind === 'value' ? p[1] : [p[0], p[1]];
                return { value, done: false };
            },
            writable: true, enumerable: true, configurable: true
        },
        [Symbol.toStringTag]: { value: 'URLSearchParams Iterator', configurable: true }
    });

    function makeIterator(target, kind) {
        paramsState(target);
        const it = Object.create(ParamsIteratorPrototype);
        iterators.set(it, { target, kind, index: 0 });
        return it;
    }

    /* WebIDL: attributes and operations are enumerable */
    function enumerable(proto, names) {
        for (const n of names) {
            const d = Object.getOwnPropertyDescriptor(proto, n);
            if (d) {
                d.enumerable = true;
                Object.defineProperty(proto, n, d);
            }
        }
    }

    enumerable(URL.prototype, ['href', 'origin', 'protocol', 'username', 'password', 'host',
        'hostname', 'port', 'pathname', 'search', 'searchParams', 'hash', 'toString', 'toJSON']);
    enumerable(URLSearchParams.prototype, ['size', 'append', 'delete', 'get', 'getAll', 'has',
        'set', 'sort', 'forEach', 'entries', 'keys', 'values', 'toString']);
    Object.defineProperty(URLSearchParams.prototype, Symbol.iterator, {
        value: URLSearchParams.prototype.entries, writable: true, configurable: true
    });
    Object.defineProperty(URL.prototype, Symbol.toStringTag, { value: 'URL', configurable: true });
    Object.defineProperty(URLSearchParams.prototype, Symbol.toStringTag,
        { value: 'URLSearchParams', configurable: true });
    for (const name of ['createObjectURL', 'revokeObjectURL']) {
        Object.defineProperty(URL, name, {
            value: N[name], writable: true, enumerable: true, configurable: true
        });
    }

    for (const [name, value] of [['URL', URL], ['URLSearchParams', URLSearchParams]]) {
        Object.defineProperty(global, name, { value, writable: true, configurable: true });
    }
    if (global.window && global.window !== global) {
        global.window.URL = URL;
        global.window.URLSearchParams = URLSearchParams;
    }
    if (typeof global.webkitURL === 'undefined')
        Object.defineProperty(global, 'webkitURL', { value: URL, writable: true, configurable: true });

    /* ------------------------------------------------------------------
     * Document base URL and HTMLHyperlinkElementUtils (<a>, <area>)
     * ------------------------------------------------------------------ */

    function documentURLRecord() {
        let href = '';
        try {
            href = String(global.location && global.location.href);
        } catch (e) {
        }
        const rec = parse(href);
        return rec !== null ? rec : parse('about:blank');
    }

    /* the document base URL: the first <base href> resolved against the
       document URL, else the document URL */
    function baseURLRecord() {
        const docRec = documentURLRecord();
        let base = null;
        try {
            const doc = global.document;
            base = doc && doc.querySelector ? doc.querySelector('base[href]') : null;
        } catch (e) {
        }
        if (base) {
            const rec = parse(String(base.getAttribute('href')), docRec);
            if (rec !== null)
                return rec;
        }
        return docRec;
    }

    try {
        const doc = global.document;
        if (doc) {
            Object.defineProperty(doc, 'baseURI', {
                get() { return components(baseURLRecord())[HREF]; },
                set(v) { /* read-only; tolerated for older embedder code */ },
                enumerable: true, configurable: true
            });
        }
    } catch (e) {
    }

    function linkRecord(el) {
        const h = el.getAttribute('href');
        if (h === null)
            return null;
        return parse(String(h), baseURLRecord());
    }

    function defineHyperlinkUtils(proto) {
        const get = (index, nullValue) => function () {
            const rec = linkRecord(this);
            return rec === null ? nullValue : components(rec)[index];
        };
        const set = (name) => function (v) {
            const rec = linkRecord(this);
            if (rec === null)
                return;
            const nrec = setPart(rec, SETTER[name], usv(v));
            if (nrec !== null)
                this.setAttribute('href', components(nrec)[HREF]);
        };
        const props = {
            href: {
                get() {
                    const rec = linkRecord(this);
                    if (rec !== null)
                        return components(rec)[HREF];
                    const h = this.getAttribute('href');
                    return h === null ? '' : String(h);
                },
                set(v) { this.setAttribute('href', usv(v)); }
            },
            origin: { get: get(ORIGIN, '') },
            protocol: { get: get(PROTOCOL, ':'), set: set('protocol') },
            username: { get: get(USERNAME, ''), set: set('username') },
            password: { get: get(PASSWORD, ''), set: set('password') },
            host: { get: get(HOST, ''), set: set('host') },
            hostname: { get: get(HOSTNAME, ''), set: set('hostname') },
            port: { get: get(PORT, ''), set: set('port') },
            pathname: { get: get(PATHNAME, ''), set: set('pathname') },
            search: { get: get(SEARCH, ''), set: set('search') },
            hash: { get: get(HASH, ''), set: set('hash') },
        };
        for (const name of Object.keys(props))
            Object.defineProperty(proto, name, Object.assign({ enumerable: true, configurable: true }, props[name]));
        Object.defineProperty(proto, 'toString', {
            value: function toString() { return this.href; },
            writable: true, enumerable: true, configurable: true
        });
    }

    const HTMLElementCtor = global.HTMLElement;
    const elementProto = HTMLElementCtor && HTMLElementCtor.prototype;
    for (const name of ['HTMLAnchorElement', 'HTMLAreaElement']) {
        let ctor = global[name];
        if (typeof ctor !== 'function') {
            ctor = function () { throw new TypeError('Illegal constructor'); };
            Object.defineProperty(ctor, 'name', { value: name });
            Object.defineProperty(global, name, { value: ctor, writable: true, configurable: true });
        }
        let proto = ctor.prototype;
        if (!proto || typeof proto !== 'object' || proto === elementProto) {
            proto = Object.create(elementProto || Object.prototype);
            Object.defineProperty(proto, 'constructor', { value: ctor, writable: true, configurable: true });
            try {
                Object.defineProperty(ctor, 'prototype', { value: proto });
            } catch (e) {
                ctor.prototype = proto;
            }
        } else if (elementProto && Object.getPrototypeOf(proto) !== elementProto) {
            Object.setPrototypeOf(proto, elementProto);
        }
        defineHyperlinkUtils(proto);
        if (global.window && global.window !== global)
            global.window[name] = ctor;
    }
})
