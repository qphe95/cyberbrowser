/*
 * dom_events.js - DOM events (DOM Standard section 2, UI Events), installed
 * in each realm by platform_api_install (browser/platform_api.c):
 *   - Event and its interfaces (CustomEvent, UIEvent, FocusEvent,
 *     MouseEvent, PointerEvent, WheelEvent, KeyboardEvent, InputEvent,
 *     CompositionEvent, ErrorEvent, PromiseRejectionEvent, ...);
 *   - EventTarget: listeners (capture, once, passive, signal, handleEvent
 *     objects) and dispatch through the event path (capture, target and
 *     bubble phases, shadow-tree retargeting, on* handler properties);
 *   - HTMLElement.prototype.click() with the activation behavior of
 *     checkboxes, radio buttons, labels, submit buttons and links;
 *   - the event handler IDL attributes (onclick, oninput, ...; HTML
 *     8.1.8), with on* content attributes compiled when first read;
 *   - MessageChannel / MessagePort and window.postMessage (HTML 9.4-9.5):
 *     each message is delivered as a task of its own;
 *   - session history within the document (HTML 7.4): history.pushState,
 *     replaceState, back/forward/go, and fragment navigation (location.hash,
 *     same-document links), with popstate and hashchange.
 * Called as fn(global, window).  Listener lists are kept under a symbol
 * shared by all realms (Symbol.for), so a target's listeners are the same
 * whichever realm's methods add or dispatch.
 */
(function (global, win) {
    'use strict';
    var LISTENERS = Symbol.for('cyber.listeners');
    var STATE = Symbol.for('cyber.eventState');
    var NONE = 0, CAPTURING_PHASE = 1, AT_TARGET = 2, BUBBLING_PHASE = 3;

    function define(obj, name, value) {
        Object.defineProperty(obj, name, { value: value, writable: true, configurable: true });
    }
    function expose(name, value) {
        define(global, name, value);
        if (win && win !== global) define(win, name, value);
    }
    function getter(proto, name, fn) {
        Object.defineProperty(proto, name, { get: fn, configurable: true, enumerable: true });
    }
    function now() { return global.performance ? global.performance.now() : Date.now(); }
    function st(ev) {
        var s = ev && ev[STATE];
        if (!s) throw new TypeError('Illegal invocation');
        return s;
    }
    function dict(init) {
        if (init === undefined || init === null) return {};
        if (typeof init !== 'object' && typeof init !== 'function') throw new TypeError('The event init is not an object');
        return init;
    }

    /* ---- Event ---- */
    function Event(type, init) {
        if (!new.target) throw new TypeError("Failed to construct 'Event': Please use the 'new' operator");
        if (arguments.length < 1) throw new TypeError("Failed to construct 'Event': 1 argument required");
        init = dict(init);
        Object.defineProperty(this, STATE, { value: {
            type: String(type), bubbles: !!init.bubbles, cancelable: !!init.cancelable,
            composed: !!init.composed, target: null, currentTarget: null, relatedTarget: null,
            phase: NONE, canceled: false, stop: false, stopImmediate: false, inPassive: false,
            dispatching: false, initialized: true, isTrusted: false, timeStamp: now(), path: [] } });
        /* ([LegacyUnforgeable]: each event's own) */
        Object.defineProperty(this, 'isTrusted', { get: isTrustedGetter, enumerable: true });
    }
    var isTrustedGetter = function isTrusted() { return st(this).isTrusted; };
    Object.defineProperty(isTrustedGetter, 'name', { value: 'get isTrusted' });
    var EP = Event.prototype;
    [['NONE', NONE], ['CAPTURING_PHASE', CAPTURING_PHASE], ['AT_TARGET', AT_TARGET],
     ['BUBBLING_PHASE', BUBBLING_PHASE]].forEach(function (c) {
        Object.defineProperty(Event, c[0], { value: c[1], enumerable: true });
        Object.defineProperty(EP, c[0], { value: c[1], enumerable: true });
    });
    getter(EP, 'type', function () { return st(this).type; });
    getter(EP, 'target', function () { return st(this).target; });
    getter(EP, 'srcElement', function () { return st(this).target; });
    getter(EP, 'currentTarget', function () { return st(this).currentTarget; });
    getter(EP, 'eventPhase', function () { return st(this).phase; });
    getter(EP, 'bubbles', function () { return st(this).bubbles; });
    getter(EP, 'cancelable', function () { return st(this).cancelable; });
    getter(EP, 'composed', function () { return st(this).composed; });
    getter(EP, 'defaultPrevented', function () { return st(this).canceled; });
    /* (coarsened to 5 microseconds, as High Resolution Time asks) */
    getter(EP, 'timeStamp', function () { return Math.floor(st(this).timeStamp * 200) / 200; });
    Object.defineProperty(EP, 'returnValue', { configurable: true, enumerable: true,
        get: function () { return !st(this).canceled; },
        set: function (v) { if (!v) cancel(st(this)); } });
    Object.defineProperty(EP, 'cancelBubble', { configurable: true, enumerable: true,
        get: function () { return st(this).stop; },
        set: function (v) { var s = st(this); if (v) s.stop = true; } });
    function cancel(s) { if (s.cancelable && !s.inPassive) s.canceled = true; }
    define(EP, 'preventDefault', function preventDefault() { cancel(st(this)); });
    define(EP, 'stopPropagation', function stopPropagation() { st(this).stop = true; });
    define(EP, 'stopImmediatePropagation', function stopImmediatePropagation() {
        var s = st(this); s.stop = true; s.stopImmediate = true;
    });
    define(EP, 'composedPath', function composedPath() {
        var s = st(this);
        if (!s.dispatching) return [];
        /* the path the current target sees: without the shadow trees closed to it */
        if (s.flat) { var out = []; for (var i = 0; i < s.flat.length; i += 4) out.push(s.flat[i]); return out; }
        return s.path.map(function (p) { return p.item; });
    });
    define(EP, 'initEvent', function initEvent(type, bubbles, cancelable) {
        var s = st(this);
        if (arguments.length < 1) throw new TypeError("Failed to execute 'initEvent' on 'Event': 1 argument required, but only 0 present.");
        if (s.dispatching) return;
        s.type = String(type); s.bubbles = !!bubbles; s.cancelable = !!cancelable;
        s.stop = s.stopImmediate = s.canceled = false; s.target = null;
        s.initialized = true;
    });
    Object.defineProperty(EP, Symbol.toStringTag, { value: 'Event', configurable: true });

    /* An event interface: a constructor taking (type, init) whose own
     * attributes are the `fields` of init with their defaults. */
    function eventClass(name, Parent, fields, setup) {
        var C = function (type, init) {
            if (!new.target) throw new TypeError("Failed to construct '" + name + "': Please use the 'new' operator");
            if (arguments.length < 1) throw new TypeError("Failed to construct '" + name + "': 1 argument required");
            var self = Reflect.construct(Parent, [type, init], new.target);
            init = dict(init);
            var s = self[STATE];
            for (var k in fields) s[k] = init[k] !== undefined ? fields[k](init[k]) : fields[k]();
            if (setup) setup(s, init);
            return self;
        };
        Object.defineProperty(C, 'name', { value: name });
        C.prototype = Object.create(Parent.prototype);
        define(C.prototype, 'constructor', C);
        Object.setPrototypeOf(C, Parent);
        Object.keys(fields).forEach(function (k) {
            getter(C.prototype, k, function () { return st(this)[k]; });
        });
        Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
        expose(name, C);
        return C;
    }
    function any(d) { return function (v) { return arguments.length ? v : d; }; }
    function num(d) { return function (v) { return arguments.length ? Number(v) || 0 : d; }; }
    /* (a double: a number that is finite, else a TypeError) */
    function dbl(d) { return function (v) {
        if (!arguments.length) return d;
        var n = Number(v);
        if (!isFinite(n)) throw new TypeError('The provided double value is non-finite.');
        return n;
    }; }
    function int(d) { return function (v) { return arguments.length ? (Number(v) | 0) : d; }; }
    function bool(d) { return function (v) { return arguments.length ? !!v : d; }; }
    function str(d) { return function (v) { return arguments.length ? String(v) : d; }; }
    function nul() { return function (v) { return arguments.length ? v : null; }; }

    var CustomEvent = eventClass('CustomEvent', Event, { detail: nul() });
    define(CustomEvent.prototype, 'initCustomEvent', function initCustomEvent(type, bubbles, cancelable, detail) {
        if (st(this).dispatching) return;    /* (init* do nothing while it is dispatched) */
        if (arguments.length < 1) throw new TypeError("Failed to execute 'initCustomEvent' on 'CustomEvent': 1 argument required, but only 0 present.");
        this.initEvent(type, bubbles, cancelable);
        st(this).detail = detail === undefined ? null : detail;
    });
    var UIEvent = eventClass('UIEvent', Event, { view: function (v) {
        if (v === undefined || v === null) return null;
        if (typeof v !== 'object' || !('window' in v))
            throw new TypeError("Failed to construct 'UIEvent': Failed to read the 'view' property: not of type 'Window'.");
        return v;
    }, detail: int(0), which: int(0) });
    define(UIEvent.prototype, 'initUIEvent', function initUIEvent(type, bubbles, cancelable, view, detail) {
        if (st(this).dispatching) return;
        if (arguments.length < 1) throw new TypeError("Failed to execute 'initUIEvent' on 'UIEvent': 1 argument required, but only 0 present.");
        this.initEvent(type, bubbles, cancelable);
        var s = st(this);
        s.view = view === undefined ? null : view;
        s.detail = detail | 0;
    });
    var FocusEvent = eventClass('FocusEvent', UIEvent, { relatedTarget: nul() });
    var modifiers = { ctrlKey: bool(false), shiftKey: bool(false), altKey: bool(false), metaKey: bool(false) };
    function withModifiers(o) { for (var k in modifiers) o[k] = modifiers[k]; return o; }
    var MouseEvent = eventClass('MouseEvent', UIEvent, withModifiers({
        screenX: num(0), screenY: num(0), clientX: num(0), clientY: num(0),
        button: int(0), buttons: int(0), relatedTarget: nul(), movementX: num(0), movementY: num(0)
    }), function (s) { if (!s.which) s.which = s.button + 1 > 0 && s.type.indexOf('mouse') === 0 ? s.button + 1 : 0; });
    var MEP = MouseEvent.prototype;
    getter(MEP, 'x', function () { return st(this).clientX; });
    getter(MEP, 'y', function () { return st(this).clientY; });
    getter(MEP, 'pageX', function () { return st(this).clientX + (global.scrollX || 0); });
    getter(MEP, 'pageY', function () { return st(this).clientY + (global.scrollY || 0); });
    function offset(ev, axis) {
        var s = st(ev), t = s.target;
        if (!t || typeof t.getBoundingClientRect !== 'function') return axis ? s.clientY : s.clientX;
        var r = t.getBoundingClientRect();
        return axis ? s.clientY - r.top : s.clientX - r.left;
    }
    getter(MEP, 'offsetX', function () { return offset(this, 0); });
    getter(MEP, 'offsetY', function () { return offset(this, 1); });
    function getModifierState(key) {
        var s = st(this);
        switch (String(key)) {
        case 'Control': return !!s.ctrlKey;
        case 'Shift': return !!s.shiftKey;
        case 'Alt': return !!s.altKey;
        case 'Meta': return !!s.metaKey;
        default: return false;
        }
    }
    define(MEP, 'getModifierState', getModifierState);
    define(MEP, 'initMouseEvent', function initMouseEvent(type, bubbles, cancelable, view, detail, sx, sy, cx, cy,
                                                          ctrl, alt, shift, meta, button, related) {
        if (st(this).dispatching) return;
        this.initEvent(type, bubbles, cancelable);
        var s = st(this);
        s.view = view === undefined ? null : view; s.detail = detail | 0;
        s.screenX = +sx || 0; s.screenY = +sy || 0; s.clientX = +cx || 0; s.clientY = +cy || 0;
        s.ctrlKey = !!ctrl; s.altKey = !!alt; s.shiftKey = !!shift; s.metaKey = !!meta;
        s.button = button | 0; s.relatedTarget = related === undefined ? null : related;
    });
    eventClass('PointerEvent', MouseEvent, {
        pointerId: int(0), width: num(1), height: num(1), pressure: num(0), tangentialPressure: num(0),
        tiltX: int(0), tiltY: int(0), twist: int(0), pointerType: str(''), isPrimary: bool(false)
    });
    define(global.PointerEvent.prototype, 'getCoalescedEvents', function () { return []; });
    define(global.PointerEvent.prototype, 'getPredictedEvents', function () { return []; });
    var WheelEvent = eventClass('WheelEvent', MouseEvent, { deltaX: num(0), deltaY: num(0), deltaZ: num(0), deltaMode: int(0) });
    [['DOM_DELTA_PIXEL', 0], ['DOM_DELTA_LINE', 1], ['DOM_DELTA_PAGE', 2]].forEach(function (c) {
        Object.defineProperty(WheelEvent, c[0], { value: c[1], enumerable: true });
        Object.defineProperty(WheelEvent.prototype, c[0], { value: c[1], enumerable: true });
    });
    eventClass('DragEvent', MouseEvent, { dataTransfer: nul() });
    var KeyboardEvent = eventClass('KeyboardEvent', UIEvent, withModifiers({
        key: str(''), code: str(''), location: int(0), repeat: bool(false), isComposing: bool(false),
        charCode: int(0), keyCode: int(0)
    }), function (s, init) { if (init.which === undefined) s.which = s.keyCode || s.charCode; });
    [['DOM_KEY_LOCATION_STANDARD', 0], ['DOM_KEY_LOCATION_LEFT', 1], ['DOM_KEY_LOCATION_RIGHT', 2],
     ['DOM_KEY_LOCATION_NUMPAD', 3]].forEach(function (c) {
        Object.defineProperty(KeyboardEvent, c[0], { value: c[1], enumerable: true });
        Object.defineProperty(KeyboardEvent.prototype, c[0], { value: c[1], enumerable: true });
    });
    define(KeyboardEvent.prototype, 'getModifierState', getModifierState);
    define(KeyboardEvent.prototype, 'initKeyboardEvent', function initKeyboardEvent(type, bubbles, cancelable, view, key,
                                                                                     location, ctrl, alt, shift, meta) {
        if (st(this).dispatching) return;
        if (arguments.length < 1) throw new TypeError("Failed to execute 'initKeyboardEvent' on 'KeyboardEvent': 1 argument required, but only 0 present.");
        this.initEvent(type, bubbles, cancelable);
        var s = st(this);
        s.view = view === undefined ? null : view;
        s.key = key === undefined ? '' : String(key);
        s.location = location >>> 0;
        s.ctrlKey = !!ctrl; s.altKey = !!alt; s.shiftKey = !!shift; s.metaKey = !!meta;
    });
    eventClass('InputEvent', UIEvent, { data: nul(), inputType: str(''), isComposing: bool(false),
                                         dataTransfer: nul() });
    define(global.InputEvent.prototype, 'getTargetRanges', function () { return []; });
    eventClass('CompositionEvent', UIEvent, { data: str('') });
    eventClass('TouchEvent', UIEvent, withModifiers({ touches: function (v) { return arguments.length ? v : []; },
        targetTouches: function (v) { return arguments.length ? v : []; },
        changedTouches: function (v) { return arguments.length ? v : []; } }));
    eventClass('ErrorEvent', Event, { message: str(''), filename: str(''), lineno: int(0), colno: int(0),
                                       error: any(undefined) });
    /* (InterestEvent, tentative: its source an Element or null) */
    eventClass('InterestEvent', Event, { source: any(null) }, function (s, init) {
        if (init.source !== undefined && init.source !== null && !(global.Element && init.source instanceof global.Element)) throw new TypeError("Failed to construct 'InterestEvent': Failed to read the 'source' property: Failed to convert value to 'Element'.");
        if (init.source === undefined) s.source = null;
    });
    eventClass('PromiseRejectionEvent', Event, { promise: any(undefined), reason: any(undefined) }, function (s, init) {
        /* (promise: a required member of its init) */
        if (init.promise === undefined) throw new TypeError("Failed to construct 'PromiseRejectionEvent': required member promise is undefined.");
    });
    eventClass('HashChangeEvent', Event, { oldURL: str(''), newURL: str('') });
    eventClass('MediaQueryListEvent', Event, { media: str(''), matches: bool(false) });
    eventClass('NavigateEvent', Event, { navigationType: str('push'), destination: nul(), canIntercept: bool(false),
        userInitiated: bool(false), hashChange: bool(false), signal: nul(), formData: nul(), downloadRequest: nul(), sourceElement: nul(),
        info: nul(), hasUAVisualTransition: bool(false) });
    eventClass('PopStateEvent', Event, { state: nul(), hasUAVisualTransition: bool(false) });
    eventClass('PageTransitionEvent', Event, { persisted: bool(false) });
    eventClass('PageSwapEvent', Event, { viewTransition: nul(), activation: nul() });
    eventClass('PageRevealEvent', Event, { viewTransition: nul() });
    eventClass('ProgressEvent', Event, { lengthComputable: bool(false), loaded: num(0), total: num(0) });
    eventClass('AnimationEvent', Event, { animationName: str(''), elapsedTime: dbl(0), pseudoElement: str(''), animation: nul() });
    eventClass('TransitionEvent', Event, { propertyName: str(''), elapsedTime: dbl(0), pseudoElement: str(''), animation: nul() });
    eventClass('SubmitEvent', Event, { submitter: nul() });
    eventClass('FormDataEvent', Event, { formData: nul() });
    eventClass('ToggleEvent', Event, { oldState: str(''), newState: str(''), source: nul() });
    eventClass('FontFaceSetLoadEvent', Event, { fontfaces: function (v) { return Object.freeze(arguments.length && v != null ? Array.prototype.slice.call(v) : []); } });
    eventClass('StorageEvent', Event, { key: nul(), oldValue: nul(), newValue: nul(), url: str(''),
                                         storageArea: nul() });
    /* (ports: a sequence, null not one; a frozen array) */
    var portsOf = function (v, what) {
        if (v === null || (typeof v !== 'object' && typeof v !== 'function') || typeof v[Symbol.iterator] !== 'function')
            throw new TypeError(what + ": The provided value cannot be converted to a sequence.");
        return Object.freeze(Array.from(v));
    };
    var MessageEventC = eventClass('MessageEvent', Event, { data: nul(), origin: str(''), lastEventId: str(''),
                                             source: nul(), ports: function (v) { return arguments.length ? portsOf(v, "Failed to construct 'MessageEvent'") : Object.freeze([]); } });
    define(MessageEventC.prototype, 'initMessageEvent', function initMessageEvent(type, bubbles, cancelable, data, origin, lastEventId, source, ports) {
        if (!(this instanceof MessageEventC)) throw new TypeError('Illegal invocation');
        if (arguments.length < 1) throw new TypeError("Failed to execute 'initMessageEvent' on 'MessageEvent': 1 argument required, but only 0 present.");
        var p = ports === undefined ? Object.freeze([]) : portsOf(ports, "Failed to execute 'initMessageEvent' on 'MessageEvent'");
        if (st(this).dispatching) return;
        this.initEvent(type, !!bubbles, !!cancelable);
        var x = st(this);
        x.data = data === undefined ? null : data;
        x.origin = origin === undefined ? '' : String(origin);
        x.lastEventId = lastEventId === undefined ? '' : String(lastEventId);
        x.source = source === undefined ? null : source;
        x.ports = p;
    });
    Object.defineProperty(MessageEventC.prototype.initMessageEvent, 'length', { value: 1 });
    eventClass('DeviceMotionEvent', Event, { acceleration: nul(), accelerationIncludingGravity: nul(), rotationRate: nul(),
                                             interval: num(0) });
    eventClass('DeviceOrientationEvent', Event, { alpha: nul(), beta: nul(), gamma: nul(), absolute: bool(false) });
    /* (made by createEvent only, in browsers; a constructor here) */
    (function () {
        var BUE = eventClass('BeforeUnloadEvent', Event, { returnValue: str('') });
        /* (no constructor of its own in the IDL: length 0; returnValue settable) */
        Object.defineProperty(BUE, 'length', { value: 0 });
        var g0 = Object.getOwnPropertyDescriptor(BUE.prototype, 'returnValue').get;
        var s0 = { returnValue: function (v) { if (!(this instanceof BUE)) throw new TypeError('Illegal invocation'); st(this).returnValue = String(v); } }.returnValue;
        Object.defineProperty(s0, 'name', { value: 'set returnValue' });
        Object.defineProperty(BUE.prototype, 'returnValue', { get: g0, set: s0, enumerable: true, configurable: true });
    })();
    eventClass('GamepadEvent', Event, { gamepad: nul() });
    var TextEvent = eventClass('TextEvent', UIEvent, { data: str('') });
    define(TextEvent.prototype, 'initTextEvent', function (type, bubbles, cancelable, view, data) {
        this.initEvent(type, bubbles, cancelable);
        st(this).view = view === undefined ? null : view;
        st(this).data = data === undefined ? '' : String(data);
    });
    expose('Event', Event);
    /* (the classes platform_api made from the Event there was then) */
    ['SnapEvent', 'ContentVisibilityAutoStateChangeEvent', 'PageSwapEvent', 'PageRevealEvent'].forEach(function (n) {
        var C = global[n];
        if (!C) return;
        Object.setPrototypeOf(C.prototype, EP);
        Object.setPrototypeOf(C, Event);
    });

    /* ---- EventTarget ---- */
    function flatten(options, adding) {
        /* (any other value: it's the capture flag, as a boolean) */
        if (!options || (typeof options !== 'object' && typeof options !== 'function')) return { capture: !!options };
        /* (removeEventListener reads capture alone) */
        if (!adding) return { capture: !!options.capture };
        /* (signal: an AbortSignal when given; null is a TypeError) */
        if (adding && options.signal !== undefined && (options.signal === null || typeof options.signal !== 'object'))
            throw new TypeError("Failed to read the 'signal' property from 'AddEventListenerOptions': Failed to convert value to 'AbortSignal'.");
        return { capture: !!options.capture, once: !!options.once, passive: options.passive,
                 signal: options.signal };
    }
    function listenersOf(target, create) {
        var map = target[LISTENERS];
        if (!map) {
            if (!create) return null;
            map = Object.create(null);
            Object.defineProperty(target, LISTENERS, { value: map, configurable: true });
        }
        return map;
    }
    function defaultPassive(target, type) {
        if (type !== 'touchstart' && type !== 'touchmove' && type !== 'wheel' && type !== 'mousewheel')
            return false;
        if (target === global || (target && target.nodeType === 9)) return true;
        var d = target && target.ownerDocument;
        return !!d && (target === d.documentElement || target === d.body);
    }
    /* (WebIDL: this an EventTarget; a plain object or a primitive is not) */
    function etThis(t, method, argc) {
        var target = t == null ? global : t;
        if ((typeof target !== 'object' && typeof target !== 'function') || Object.getPrototypeOf(target) === Object.prototype)
            throw new TypeError("Failed to execute '" + method + "' on 'EventTarget': Illegal invocation");
        if (argc < 2) throw new TypeError("Failed to execute '" + method + "' on 'EventTarget': 2 arguments required, but only " + argc + " present.");
        return target;
    }
    function addEventListener(type, callback, options) {
        var target = etThis(this, 'addEventListener', arguments.length);
        listenerChanges++;
        var o = flatten(options, true);
        if (callback === null || callback === undefined) return;
        if (typeof callback !== 'function' && typeof callback !== 'object')
            throw new TypeError("Failed to execute 'addEventListener': parameter 2 is not of type 'Object'.");
        if (o.signal && o.signal.aborted) return;
        type = String(type);
        var map = listenersOf(target, true);
        var list = map[type] || (map[type] = []);
        for (var i = 0; i < list.length; i++)
            if (list[i].callback === callback && list[i].capture === o.capture) return;
        var l = { seq: ++listenerSeq, callback: callback, capture: o.capture, once: !!o.once,
                  passive: o.passive === undefined ? defaultPassive(target, type) : !!o.passive,
                  removed: false };
        list.push(l);
        if (o.signal && typeof o.signal.addEventListener === 'function')
            o.signal.addEventListener('abort', function () { removeListener(target, type, l); });
    }
    function removeListener(target, type, l) {
        var map = listenersOf(target, false), list = map && map[type];
        l.removed = true;
        if (!list) return;
        var i = list.indexOf(l);
        if (i >= 0) {
            /* a new array: a dispatch iterating the old one is unaffected */
            map[type] = list.slice(0, i).concat(list.slice(i + 1));
        }
    }
    function removeEventListener(type, callback, options) {
        var target = etThis(this, 'removeEventListener', arguments.length);
        var capture = flatten(options).capture;
        var map = listenersOf(target, false), list = map && map[String(type)];
        if (!list) return;
        for (var i = 0; i < list.length; i++)
            if (list[i].callback === callback && list[i].capture === capture) {
                removeListener(target, String(type), list[i]);
                return;
            }
    }

    function isNode(x) { return x !== null && typeof x === 'object' && typeof x.nodeType === 'number'; }
    function isShadowRoot(x) { return isNode(x) && x.nodeType === 11 && x.host !== undefined && x.host !== null && 'mode' in x; }
    function rootOf(n) {
        while (n) {
            var p = n.parentNode;
            if (!p) return n;
            n = p;
        }
        return n;
    }
    /* The shadow-including root chain test: whether a's root is a shadow
     * root that is not an inclusive ancestor (across hosts) of b. */
    function isShadowIncludingInclusiveAncestor(a, b) {
        while (b) {
            if (a === b) return true;
            var r = rootOf(b);
            b = isShadowRoot(r) ? r.host : null;
            if (r === a) return true;
        }
        return false;
    }
    function retarget(a, b) {
        while (isNode(a)) {
            var r = rootOf(a);
            if (!isShadowRoot(r)) return a;
            if (isNode(b) && isShadowIncludingInclusiveAncestor(r, b)) return a;
            a = r.host;
        }
        return a;
    }
    /* "Get the parent" (DOM 4.2.? / 2.7): a node's parent (an assigned slot
     * first), a shadow root's host (for a composed event), a document's
     * window (except for load). */
    function parentOf(t, ev, s) {
        if (isNode(t)) {
            if (t.nodeType === 9) return s.type === 'load' ? null : (t.defaultView || null);
            if (isShadowRoot(t)) return s.composed || !rootIsTarget(t, s) ? t.host : null;
            var slot = t.assignedSlot;
            if (slot) return slot;
            return t.parentNode || null;
        }
        return typeof t.__cyberParentTarget === 'function' ? t.__cyberParentTarget(ev) : null;
    }
    function rootIsTarget(shadowRoot, s) {
        /* an uncomposed event leaves its shadow tree only when dispatched
         * outside it: the first path item's root is not this shadow root */
        var first = s.path.length ? s.path[0].item : null;
        return first && rootOf(first) === shadowRoot;
    }

    function report(err, g) {
        try {
            /* (in the listener's global, when it's another) */
            if (g && g !== global && typeof g.__cyberReportError === 'function') g.__cyberReportError(err);
            else if (g && g !== global && typeof g.reportError === 'function') g.reportError(err);
            else if (global.reportError === publicReportError) reportError(err);
            else if (typeof global.reportError === 'function' && global.reportError !== report) global.reportError(err);
            else if (global.console) global.console.error('Uncaught ' + (err && err.stack ? err + '\n' + err.stack : err));
        } catch (e) { /* the report itself failed */ }
    }

    /* reportError(e), HTML "report an exception" (scripts, timers and
     * listeners that throw come here too): an ErrorEvent at the window, its
     * onerror called with (message, source, lineno, colno, error); the
     * console when nothing cancels it.  (An error while one is reported is
     * not reported again.) */
    var reporting = false;
    /* (where script called from: the first frame of a page's script on the stack, its URL, line and column) */
    function callerLocation() {
        var st = '';
        try { st = String(new Error().stack || ''); } catch (e) { return null; }
        var lines = st.split('\n');
        for (var i = 0; i < lines.length; i++) {
            var m = /\(?((?:https?|file|blob|data):[^\s()]*?|<script_\d+>):(\d+)(?::(\d+))?\)?\s*$/.exec(lines[i]);
            if (!m) continue;
            var file = m[1], line = +m[2] || 0, col = +m[3] || 1;
            try {
                var smap = global.__cyberScriptMap, sm = /^<script_\d+>$/.test(file) && smap ? smap[file] : null;
                if (sm) { file = sm[0]; line += sm[1]; }
            } catch (e) { }
            if (/^<script_/.test(file)) continue;
            return { file: file, line: line, col: col };
        }
        return null;
    }
    function isErrorObject(e) {
        try { return !!e && typeof e === 'object' && Object.prototype.toString.call(e) === '[object Error]'; } catch (x) { return false; }
    }
    function reportError(err, where) {
        if (reporting) return;
        reporting = true;
        try {
            var msg, isErr = isErrorObject(err);
            try { msg = 'Uncaught ' + (isErr ? (err.name ? err.name + ': ' : '') + err.message : typeof err === 'object' && err !== null ? 'exception' : String(err)); }
            catch (e) { msg = 'Uncaught exception'; }
            var line = 0, file = '', col = 0;
            try { if (isErr && !where) { line = +err.lineNumber || 0; file = String(err.fileName || ''); } } catch (e) { /* (getters) */ }
            if (where) { file = where.file; line = where.line; col = where.col; }
            /* (a parser-run script's name: its URL, or the document's and where its text starts) */
            try {
                var smap = global.__cyberScriptMap, sm = /^<script_\d+>$/.test(file) && smap ? smap[file] : null;
                if (sm) { file = sm[0]; line += sm[1]; }
            } catch (e) { }
            var ev = new global.ErrorEvent('error', { message: msg, filename: file, lineno: line, colno: col || (line ? 1 : 0),
                                                       error: err, cancelable: true });
            if (global.dispatchEvent(ev) && global.console)
                global.console.error(msg + (err && err.stack ? '\n' + err.stack : ''));
        } catch (e) { /* the report itself failed */ }
        reporting = false;
    }
    /* (self.reportError(e): reported as from where it was called) */
    var publicReportError = { reportError: function (e) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'reportError' on 'Window': 1 argument required, but only 0 present.");
        reportError(e, callerLocation());
    } }.reportError;
    Object.defineProperty(publicReportError, 'length', { value: 1 });
    Object.defineProperty(global, 'reportError', { value: publicReportError, writable: true, configurable: true, enumerable: true });
    Object.defineProperty(global, '__cyberReportError', { value: function (e) { reportError(e); }, configurable: true });

    /* The legacy (prefixed) types of a trusted event that has no listener of
     * its own type on an object (DOM 2.9 "invoke"), and their handlers'
     * names (lower-case: onwebkitanimationend). */
    var LEGACY = { animationend: 'webkitAnimationEnd', animationiteration: 'webkitAnimationIteration',
                   animationstart: 'webkitAnimationStart', transitionend: 'webkitTransitionEnd' };
    var HANDLER_NAME = { webkitAnimationEnd: 'webkitanimationend', webkitAnimationIteration: 'webkitanimationiteration',
                         webkitAnimationStart: 'webkitanimationstart', webkitTransitionEnd: 'webkittransitionend' };

    function handlerProp(item, type) {
        /* an on<type> event handler: an own or inherited data/accessor value */
        var h;
        try { h = item['on' + (HANDLER_NAME[type] || type)]; } catch (e) { return null; }
        return typeof h === 'function' ? h : null;
    }

    /* Run entry's listeners of one pass: capture listeners in the capture
     * pass, the others in the bubble pass (both at the target, where the
     * phase reads AT_TARGET). */
    function invoke(ev, s, entry, phase, capturePass) {
        var item = entry.item;
        s.target = entry.target;
        s.relatedTarget = entry.relatedTarget;
        s.currentTarget = item;
        s.phase = phase;
        if (s.stop) return;
        if (innerInvoke(ev, s, entry, item, capturePass) || !s.isTrusted || !LEGACY[s.type]) return;
        /* (none of its own type here: the legacy type's, under that type) */
        var type = s.type;
        s.type = LEGACY[type];
        try { innerInvoke(ev, s, entry, item, capturePass); } finally { s.type = type; }
    }

    /* window.event (HTML 8.1.8 "current event"): the event whose listener
     * runs, unless its object is in a shadow tree (DOM 2.10 "inner invoke") */
    var currentEvent;
    var CURRENT_EVENT = Symbol.for('cyber.currentEvent');
    Object.defineProperty(global, CURRENT_EVENT, { configurable: true,
        get: function () { return currentEvent; }, set: function (v) { currentEvent = v; } });
    function inShadowTree(item) {
        if (!item || typeof item.getRootNode !== 'function') return false;
        try {
            var r = item.getRootNode();
            return r.nodeType === 11 && typeof r.mode === 'string';
        } catch (e) { return false; }
    }
    /* The global of a listener: a function's realm's; an object's, by its
     * prototype's constructor. */
    var functionGlobal = global.__cyberFunctionGlobal;
    var scriptFrames = global.__cyberScriptFrames, microtasks = global.__cyberMicrotasks;
    /* (the frames of a dispatch nothing called: dispatchEvent0, dispatch, its entry) */
    var DISPATCH_FRAMES = 3;
    function listenerGlobal(cb) {
        if (!functionGlobal) return global;
        try {
            var made = functionGlobal(cb);
            if (made) return made;
            if (typeof cb !== 'function') cb = Object.getPrototypeOf(cb).constructor;
            return typeof cb === 'function' ? functionGlobal(cb) || global : global;
        } catch (e) { return global; }
    }

    /* (no listener runs for a target, or from a global, not fully active) */
    function inactive(item, g) {
        var f = global.__cyberInactive;
        return f ? f(item, g) : false;
    }

    /* The listeners of s.type at item (and its handler); whether it has any. */
    var listenerSeq = 0;
    function innerInvoke(ev, s, entry, item, capturePass) {
        /* (where the object was when the path was made) */
        var shadow = entry.flags === undefined ? entry.shadow : (entry.flags & EP_SHADOW) !== 0;
        var map = item[LISTENERS];
        var list = map && map[s.type];
        var found = false;
        if (list) for (var k = 0; k < list.length && !found; k++) found = !list[k].removed;
        /* (the handler's place among the non-capture listeners: where it was first set) */
        var hq = !capturePass && item[HANDLER_SEQ], hseq = hq ? hq[s.type] : undefined, handlerRun = false;
        if (list) {
            /* (the listeners there now: DOM 2.10 clones the list) */
            for (var i = 0, end = list.length; i < end; i++) {
                var l = list[i];
                if (l.removed) continue;
                if (l.capture !== capturePass) continue;
                if (hseq !== undefined && !handlerRun && l.seq > hseq) {
                    handlerRun = true;
                    if (runHandler(ev, s, entry, item, shadow)) found = true;
                    if (s.stopImmediate) return true;
                }
                if (l.once) removeListener(item, s.type, l);
                var fn = l.callback, thisArg = item;
                var g = listenerGlobal(fn), here = g === global;
                if (inactive(item, g)) continue;
                var prevEvent = here ? currentEvent : g[CURRENT_EVENT];
                if (!shadow) { if (here) currentEvent = ev; else g[CURRENT_EVENT] = ev; }
                try {
                    if (typeof fn !== 'function') {
                        thisArg = fn;
                        try { fn = here ? fn.handleEvent : g.__cyberRealmHandleEvent(fn); } catch (e) { report(e, g); continue; }
                        if (typeof fn !== 'function') {
                            report(new (g.TypeError || TypeError)("'handleEvent' is not a function"), g);
                            continue;
                        }
                    }
                    if (l.passive) s.inPassive = true;
                    try {
                        if (here) fn.call(thisArg, ev);
                        else g.__cyberRealmCall(fn, thisArg, ev);
                    } catch (e) { report(e, g); }
                } finally {
                    s.inPassive = false;
                    /* (with the event still current: the callback's clean-up) */
                    if (s.alone) microtasks();
                    if (here) currentEvent = prevEvent; else g[CURRENT_EVENT] = prevEvent;
                }
                if (s.stopImmediate) return true;
            }
        }
        if (!capturePass && !handlerRun && runHandler(ev, s, entry, item, shadow)) found = true;
        return found;
    }
    /* the on<type> handler, in the non-capture pass: read through its
     * property unless the native path says there is no content attribute
     * and none was set (nor forwarded from <body> to the window) */
    function runHandler(ev, s, entry, item, shadow) {
        var found = false;
        /* (a native path's flags say whether a handler may be set) */
        var h = (entry.flags === undefined || (entry.flags & (EP_ON_ATTR | EP_HANDLER | EP_ALWAYS | EP_ON_PROP))) &&
                !inactive(item, null) ? handlerProp(item, s.type) : null;
        if (h && inactive(item, listenerGlobal(h))) h = null;
        if (h) found = true;
        {
            if (h) {
                var r, hg = listenerGlobal(h), hhere = hg === global;
                var prevEv = hhere ? currentEvent : hg[CURRENT_EVENT];
                if (!shadow) { if (hhere) currentEvent = ev; else hg[CURRENT_EVENT] = ev; }
                try {
                    r = s.type === 'error' && ev instanceof global.ErrorEvent && item === global
                        ? h.call(item, ev.message, ev.filename, ev.lineno, ev.colno, ev.error)
                        : h.call(item, ev);
                    /* (Web IDL: the return value converts as the callback's finishing) */
                    if (s.type === 'beforeunload' && r != null) r = String(r);
                } catch (e) { report(e, hg); r = undefined; }
                finally {
                    if (s.alone) microtasks();
                    if (hhere) currentEvent = prevEv; else hg[CURRENT_EVENT] = prevEv;
                }
                if (s.type === 'error' && item === global ? r === true : r === false) cancel(s);
                /* (beforeunload: a value cancels a BeforeUnloadEvent, and is its returnValue if it had none) */
                else if (s.type === 'beforeunload' && r != null && global.BeforeUnloadEvent && ev instanceof global.BeforeUnloadEvent) {
                    cancel(s);
                    try { if (ev.returnValue === '') ev.returnValue = r; } catch (e) { }
                }
            }
        }
        return found;
    }

    /* DOM 2.9 "dispatch" with activation: a click MouseEvent's activation
     * target (the target, else, as it bubbles, the nearest ancestor with
     * activation behavior) gets its legacy-pre-activation behavior first,
     * then its activation behavior, or (canceled) its legacy-canceled one. */
    function dispatch(target, ev) {
        var s = st(ev), act = null, saved, isClick = false;
        if (s.type === 'click' && !s.dispatching && s.initialized && global.MouseEvent && ev instanceof global.MouseEvent) {
            isClick = true;
            act = activationTargetOf(target, s.bubbles);
            /* (the path as it is now: listeners may take the target out) */
            var clickPath = [];
            for (var cp = target; cp && clickPath.length < 4096; cp = s.bubbles ? (cp.parentNode || cp.host) : null) clickPath.push(cp);
            if (act) saved = preActivate(act);
        }
        var r = dispatchEvent0(target, ev);
        if (isClick) {
            if (s.canceled) { if (act) canceledActivate(act, saved); }
            else {
                /* (who handles it, decided after the listeners, as engines do (whatwg/html#10032): from the target up,
                 * the first that does something now; a button or input that does nothing passes it on to a link) */
                var pre = act && act.localName === 'input' && saved && (saved.type === 'checkbox' || saved.type === 'radio') ? act : null;
                var inner = false;
                for (var ci = 0, up; ci < clickPath.length && (up = clickPath[ci]); ci++) {
                    if (up === pre) { runActivation(up, saved); break; }
                    /* (a label: nothing when the click came from interactive content in it, or from its own control) */
                    if (up.nodeType === 1 && up.localName === 'label' && up.namespaceURI === HTML_NS && up !== target) {
                        var lc = null;
                        try { lc = up.control; } catch (e) { }
                        if (inner || (lc && (lc === target || (lc.contains && lc.contains(target))))) break;
                    }
                    var wasInner = inner;
                    if (interactiveContent(up)) inner = true;
                    if (!hasActivationNow(up)) continue;
                    /* (past interactive content that did nothing: only a link it is in goes on) */
                    if (wasInner && !((up.localName === 'a' || up.localName === 'area') && up.hasAttribute('href'))) continue;
                    if (runActivation(up, up === act ? saved : null) !== false) break;
                }
            }
        }
        return r;
    }
    function dispatchEvent0(target, ev) {
        var s = st(ev);
        if (s.dispatching || !s.initialized)
            throw new DOMExceptionOr("Failed to execute 'dispatchEvent': The event is already being dispatched.", 'InvalidStateError');
        s.dispatching = true;
        s.alone = scriptFrames && scriptFrames() === DISPATCH_FRAMES;
        var origRelated = s.relatedTarget;
        var path = [];
        var t = target, related = retarget(origRelated, target);
        var guard = 0;
        /* (its related target retargets to itself: no one to tell) */
        if (origRelated && related === target && origRelated !== target) {
            s.relatedTarget = null;
            s.dispatching = false;
            s.stop = s.stopImmediate = false;
            return !s.canceled;
        }
        s.path = path;
        /* a node's path comes from the node store in one call (event_path.c) */
        /* (a trusted event with a legacy type: every object, for its listeners of either type) */
        var flat = nativePath && !(s.isTrusted && LEGACY[s.type]) && !HANDLER_NAME[s.type]
                   ? nativePath(target, s.type, s.composed, origRelated || null, HANDLER_TYPE[s.type] === true) : null;
        if (flat) return dispatchFlat(ev, s, target, origRelated, flat);
        {
            while (t && guard++ < 4096) {
                path.push({ item: t, target: retarget(target, t), shadow: inShadowTree(t),
                            relatedTarget: origRelated ? retarget(origRelated, t) : null });
                t = parentOf(t, ev, s);
            }
        }
        /* The targets it ends with: the last entry's that is at its target,
         * none out of a shadow tree (DOM 2.9 "clearTargets"). */
        var last = path.length ? path[0] : null;
        for (var q = path.length - 1; q > 0; q--) if (path[q].item === path[q].target) { last = path[q]; break; }
        var clear = last && (inShadowTree(last.target) || (last.relatedTarget && inShadowTree(last.relatedTarget)));
        /* capture: from the outermost to the target */
        for (var i = path.length - 1; i >= 0; i--) {
            var e = path[i];
            invoke(ev, s, e, i === 0 || e.item === e.target ? AT_TARGET : CAPTURING_PHASE, true);
        }
        /* target and bubble: from the target outward */
        for (var j = 0; j < path.length; j++) {
            var f = path[j];
            if (j === 0 || f.item === f.target) invoke(ev, s, f, AT_TARGET, false);
            else if (s.bubbles) invoke(ev, s, f, BUBBLING_PHASE, false);
        }
        s.phase = NONE;
        s.currentTarget = null;
        s.target = clear ? null : last ? last.target : target;
        s.relatedTarget = clear || !last ? null : last.relatedTarget;
        s.dispatching = false;
        s.stop = s.stopImmediate = false;
        return !s.canceled;
    }
    /* Dispatch over a native path ([item, target, relatedTarget, flags] per
     * entry, event_path.c): only the entries where something may run are
     * visited; composedPath() reads the items from s.flat. */
    var EP_LISTENERS = 1, EP_ON_ATTR = 2, EP_HANDLER = 4, EP_ALWAYS = 8, EP_ON_PROP = 16, EP_ALL = 31, EP_SHADOW = 32;
    /* addEventListener and handler sets: once a listener runs that adds one,
     * the flags may be stale, and every later entry is visited */
    var listenerChanges = 0;
    function dispatchFlat(ev, s, target, origRelated, flat) {
        s.flat = flat;
        var n = flat.length >> 2, changes = listenerChanges;
        /* (as dispatchEvent0: the targets it ends with) */
        var last = 0;
        for (var q = n - 1; q > 0; q--) if (flat[4 * q] === flat[4 * q + 1]) { last = q; break; }
        var endTarget = flat[4 * last + 1], endRelated = flat[4 * last + 2];
        var clear = (flat[4 * last + 3] & EP_SHADOW) !== 0 || (endRelated && inShadowTree(endRelated));
        for (var i = n - 1; i >= 0; i--) {
            var fl = changes === listenerChanges ? flat[4 * i + 3] : EP_ALL | (flat[4 * i + 3] & EP_SHADOW);
            if (!(fl & EP_LISTENERS)) continue;
            var e = { item: flat[4 * i], target: flat[4 * i + 1], relatedTarget: flat[4 * i + 2], flags: fl };
            invoke(ev, s, e, i === 0 || e.item === e.target ? AT_TARGET : CAPTURING_PHASE, true);
        }
        for (var j = 0; j < n; j++) {
            var f = changes === listenerChanges ? flat[4 * j + 3] : EP_ALL | (flat[4 * j + 3] & EP_SHADOW);
            if (!(f & EP_ALL)) continue;
            var item = flat[4 * j], tgt = flat[4 * j + 1];
            var atTarget = j === 0 || item === tgt;
            if (!atTarget && !s.bubbles) continue;
            invoke(ev, s, { item: item, target: tgt, relatedTarget: flat[4 * j + 2], flags: f },
                   atTarget ? AT_TARGET : BUBBLING_PHASE, false);
        }
        s.phase = NONE;
        s.currentTarget = null;
        s.target = clear ? null : endTarget;
        s.relatedTarget = clear ? null : endRelated;
        s.dispatching = false;
        s.stop = s.stopImmediate = false;
        s.flat = null;
        return !s.canceled;
    }
    var nativePath = typeof global.__cyberEventPath === 'function' ? global.__cyberEventPath : null;
    function DOMExceptionOr(msg, name) {
        if (typeof global.DOMException === 'function') return new global.DOMException(msg, name);
        var e = new Error(msg); e.name = name; return e;
    }
    /* window.event: [Replaceable], the current event or undefined */
    if (global.document) [global, win].forEach(function (o) {
        if (!o || Object.getOwnPropertyDescriptor(o, 'event')) return;
        var ed = Object.getOwnPropertyDescriptor({
            get event() {
                if (this !== undefined && this !== null && this !== global && this !== win) throw new TypeError('Illegal invocation');
                return currentEvent;
            },
            set event(v) { Object.defineProperty(this, 'event', { value: v, writable: true, configurable: true, enumerable: true }); }
        }, 'event');
        Object.defineProperty(o, 'event', { configurable: true, enumerable: true, get: ed.get, set: ed.set });
    });

    /* (the engine's events: trusted ones, anim_api.c) */
    Object.defineProperty(global, '__cyberDispatchTrusted', { configurable: true, value: function (target, event) {
        st(event).isTrusted = true;
        return dispatch(target, event);
    } });
    function dispatchEvent(event) {
        var target = this == null ? global : this;
        if (!event || !event[STATE])
            throw new TypeError("Failed to execute 'dispatchEvent': parameter 1 is not of type 'Event'.");
        /* (one being dispatched throws, and stays as it is) */
        var es = st(event);
        if (!es.dispatching && es.initialized) es.isTrusted = false;
        return dispatch(target, event);
    }

    function EventTarget() {
        if (!new.target) throw new TypeError("Failed to construct 'EventTarget': Please use the 'new' operator");
    }
    var ETP = global.EventTarget && global.EventTarget.prototype ? global.EventTarget.prototype : EventTarget.prototype;
    if (ETP !== EventTarget.prototype) {
        EventTarget.prototype = ETP;
        define(ETP, 'constructor', EventTarget);
    }
    define(ETP, 'addEventListener', addEventListener);
    define(ETP, 'removeEventListener', removeEventListener);
    define(ETP, 'dispatchEvent', dispatchEvent);
    Object.defineProperty(ETP, Symbol.toStringTag, { value: 'EventTarget', configurable: true });
    expose('EventTarget', EventTarget);

    /* window and document carried their own copies of the old methods */
    var WP = Object.getPrototypeOf(global);
    if (WP && WP !== Object.prototype && WP !== ETP) {
        ['addEventListener', 'removeEventListener', 'dispatchEvent'].forEach(function (k) {
            if (Object.prototype.hasOwnProperty.call(WP, k)) delete WP[k];
        });
        /* (through WindowProperties, the named-access object, when there is one:
         * Window.prototype -> WindowProperties -> EventTarget.prototype) */
        var top = WP;
        while (Object.getPrototypeOf(top) && Object.getPrototypeOf(top) !== Object.prototype && Object.getPrototypeOf(top) !== ETP)
            top = Object.getPrototypeOf(top);
        if (Object.getPrototypeOf(top) === Object.prototype) Object.setPrototypeOf(top, ETP);
    }
    [global, win, global.document].forEach(function (o) {
        if (!o) return;
        ['addEventListener', 'removeEventListener', 'dispatchEvent'].forEach(function (k) {
            if (Object.prototype.hasOwnProperty.call(o, k)) delete o[k];
        });
        if (!(o instanceof EventTarget) && o !== global)
            for (var p = o; p; p = Object.getPrototypeOf(p))
                if (Object.getPrototypeOf(p) === Object.prototype || Object.getPrototypeOf(p) === null) {
                    if (p !== ETP && p !== Object.prototype) Object.setPrototypeOf(p, ETP);
                    break;
                }
    });
    if (!(global instanceof EventTarget)) {
        define(global, 'addEventListener', addEventListener);
        define(global, 'removeEventListener', removeEventListener);
        define(global, 'dispatchEvent', dispatchEvent);
    }

    /* document.createEvent (DOM 4.5) */
    var createEventMap = {
        event: Event, events: Event, htmlevents: Event, svgevents: Event, customevent: CustomEvent,
        uievent: UIEvent, uievents: UIEvent, focusevent: FocusEvent, mouseevent: MouseEvent,
        mouseevents: MouseEvent, keyboardevent: KeyboardEvent,
        compositionevent: global.CompositionEvent, touchevent: global.TouchEvent,
        hashchangeevent: global.HashChangeEvent, messageevent: global.MessageEvent,
        storageevent: global.StorageEvent, dragevent: global.DragEvent,
        beforeunloadevent: global.BeforeUnloadEvent, devicemotionevent: global.DeviceMotionEvent,
        deviceorientationevent: global.DeviceOrientationEvent, textevent: global.TextEvent
    };
    function createEvent(iface) {
        if (!this || this.nodeType !== 9) throw new TypeError('Illegal invocation');
        if (arguments.length < 1) throw new TypeError("Failed to execute 'createEvent' on 'Document': 1 argument required.");
        var C = createEventMap[String(iface).toLowerCase()];
        if (!C) throw DOMExceptionOr("Failed to execute 'createEvent': The provided event type ('" + iface + "') is invalid.", 'NotSupportedError');
        var e = new C('');
        st(e).initialized = false;
        return e;
    }
    var DP = global.Document && global.Document.prototype;
    if (DP) define(DP, 'createEvent', createEvent);
    if (global.document) {
        if (Object.prototype.hasOwnProperty.call(global.document, 'createEvent')) delete global.document.createEvent;
        if (typeof global.document.createEvent !== 'function') define(global.document, 'createEvent', createEvent);
    }

    /* ---- click() and activation behavior (HTML 6.5.3) ---- */
    function fire(target, type, Ctor, init) {
        var e = new (Ctor || Event)(type, init || { bubbles: true });
        return target.dispatchEvent(e);
    }
    function disabledControl(el) {
        var n = el.localName;
        return (n === 'button' || n === 'input' || n === 'select' || n === 'textarea' ||
                n === 'optgroup' || n === 'option' || n === 'fieldset') && el.hasAttribute('disabled');
    }
    /* a control's form owner: its form="" element, else its nearest form ancestor */
    function formOf(el) {
        var id = el.getAttribute && el.getAttribute('form');
        if (id !== null && id !== undefined && el.isConnected) {
            var f = el.ownerDocument ? el.ownerDocument.getElementById(id) : null;
            return f && f.localName === 'form' ? f : null;
        }
        for (var n = el.parentNode; n; n = n.parentNode) if (n.localName === 'form') return n;
        return null;
    }
    function inputType(el) { return String(el.getAttributeNS(null, 'type') || 'text').toLowerCase(); }
    function radioGroup(el) {
        var name = el.getAttribute('name'), root = formOf(el) || rootOf(el);
        if (!name || !root || !root.querySelectorAll) return [];
        return Array.prototype.filter.call(root.querySelectorAll('input[type=radio]'), function (r) {
            return r !== el && r.getAttribute('name') === name && formOf(r) === formOf(el);
        });
    }
    /* click() (HTML 6.5.3): a synthetic click at the element, unless it is a
     * disabled form control or its click is in progress */
    var CLICKING = Symbol('clicking');
    function click() {
        if (disabledControl(this) || this[CLICKING]) return;
        var view = this.ownerDocument && this.ownerDocument.defaultView || global;
        Object.defineProperty(this, CLICKING, { value: true, configurable: true });
        try {
            this.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, composed: true,
                                                         view: view, detail: 1 }));
        } finally { delete this[CLICKING]; }
    }
    /* checkedness as the user agent changes it: the control's state, not
     * its `checked` setter (browser/form_controls.js) */
    function getChecked(el) {
        var fi = global[Symbol.for('cyber.formInternals')];
        return fi ? fi.getChecked(el) : !!el.checked;
    }
    function setChecked(el, v) {
        var fi = global[Symbol.for('cyber.formInternals')];
        if (fi) fi.setChecked(el, v); else el.checked = v;
    }
    var HTML_NS = 'http://www.w3.org/1999/xhtml';
    var ACTIVATING_INPUT = { checkbox: 1, radio: 1, submit: 1, image: 1, reset: 1, button: 1 };
    /* (a button's type state: its keyword; none or another, Auto: a button with command or commandfor, else a submit) */
    function buttonType(el) {
        var t = String(el.getAttribute('type') || '').toLowerCase();
        if (t === 'reset' || t === 'button' || t === 'submit') return t;
        return el.hasAttribute('commandfor') || el.hasAttribute('command') ? 'button' : 'submit';
    }
    /* Whether a node has activation behavior (HTML: input, button, a and
     * area with href, label, a details element's summary). */
    function hasActivation(n) {
        if (!n || n.nodeType !== 1 || n.namespaceURI !== HTML_NS) return false;
        switch (n.localName) {
        case 'input': return ACTIVATING_INPUT[inputType(n)] === 1;
        case 'button': case 'label': return true;
        case 'a': case 'area': return n.hasAttribute('href');
        case 'summary': return summaryOf(n) !== null;
        }
        return false;
    }
    /* the details element a summary is the summary for (its first summary child), or null */
    function summaryOf(n) {
        var d = n.parentNode;
        if (!d || d.localName !== 'details' || d.namespaceURI !== HTML_NS) return null;
        for (var c = d.firstElementChild; c; c = c.nextElementSibling)
            if (c.localName === 'summary' && c.namespaceURI === HTML_NS) return c === n ? d : null;
        return null;
    }
    /* (what can handle a click as it is now: any input or button (one that does nothing passes it on), a link with an href, a
     * label, a details element's summary) */
    /* (HTML 3.2.5.2.7 interactive content) */
    function interactiveContent(n) {
        if (!n || n.nodeType !== 1 || n.namespaceURI !== HTML_NS) return false;
        switch (n.localName) {
        case 'a': return n.hasAttribute('href');
        case 'audio': case 'video': return n.hasAttribute('controls');
        case 'img': return n.hasAttribute('usemap');
        case 'input': return inputType(n) !== 'hidden';
        case 'button': case 'details': case 'embed': case 'iframe': case 'label': case 'select': case 'textarea': return true;
        }
        return false;
    }
    function hasActivationNow(n) {
        if (!n || n.nodeType !== 1 || n.namespaceURI !== HTML_NS) return false;
        return n.localName === 'input' || hasActivation(n);
    }
    function activationTargetOf(target, bubbles) {
        if (hasActivation(target)) return target;
        if (!bubbles) return null;
        for (var n = target.parentNode || target.host; n; n = n.parentNode || n.host)
            if (hasActivation(n)) return n;
        return null;
    }
    /* legacy-pre-activation behavior: a checkbox toggles, a radio is checked */
    function preActivate(el) {
        if (el.localName !== 'input') return null;
        var type = inputType(el), saved = { type: type, checked: getChecked(el), radio: null, indeterminate: !!el.indeterminate };
        /* (a checkbox: toggled, no longer indeterminate) */
        if (type === 'checkbox') { setChecked(el, !saved.checked); if (saved.indeterminate) el.indeterminate = false; }
        else if (type === 'radio') {
            radioGroup(el).forEach(function (r) { if (getChecked(r)) saved.radio = r; });
            setChecked(el, true);
        }
        return saved;
    }
    function canceledActivate(el, saved) {
        if (!saved) return;
        if (saved.type === 'checkbox') { setChecked(el, saved.checked); el.indeterminate = saved.indeterminate; }
        else if (saved.type === 'radio') { setChecked(el, saved.checked); if (saved.radio) setChecked(saved.radio, true); }
    }
    function runActivation(el, saved) {
        var view = el.ownerDocument && el.ownerDocument.defaultView || global;
        var isInput = el.localName === 'input', type = isInput ? inputType(el) : '';
        if (type === 'checkbox' || type === 'radio') {
            /* (a disconnected one changes, with no events) */
            if (!el.isConnected) return;
            if (type === 'checkbox' || (saved && !saved.checked)) {
                /* (the user agent's: trusted) */
                var tr = typeof global.__cyberDispatchTrusted === 'function' ? global.__cyberDispatchTrusted : null;
                var ie = new Event('input', { bubbles: true, composed: true }), ce = new Event('change', { bubbles: true });
                if (tr) tr(el, ie); else el.dispatchEvent(ie);
                if (tr) tr(el, ce); else el.dispatchEvent(ce);
            }
            return;
        }
        /* (a button that shows, hides or toggles a popover) */
        if ((el.localName === 'button' || (isInput && /^(button|submit|reset|image)$/.test(type))) && el.hasAttribute('popovertarget') &&
            !disabledControl(el) && !((type === 'submit' || type === 'image' || (el.localName === 'button' && buttonType(el) === 'submit')) && formOf(el))) {
            var pt = 'popoverTargetElement' in el ? el.popoverTargetElement : el.ownerDocument.getElementById(el.getAttribute('popovertarget'));
            if (pt && pt.hasAttribute('popover') && typeof pt.togglePopover === 'function') {
                var act = String(el.getAttribute('popovertargetaction') || 'toggle').toLowerCase();
                /* (the invoker: the popover's implicit anchor, for layout) */
                try {
                    var ik = el.getAttribute('data-cyber-iasrc') || String(++global.__cyberIaSeq || (global.__cyberIaSeq = 1));
                    el.setAttribute('data-cyber-iasrc', ik);
                    pt.setAttribute('data-cyber-ia', ik);
                } catch (e) { }
                try {
                    if (typeof global.__cyberPopoverInvoke === 'function') global.__cyberPopoverInvoke(pt, el, act);
                    else if (act === 'show') pt.showPopover(); else if (act === 'hide') pt.hidePopover(); else pt.togglePopover();
                } catch (e) { /* (not shown: as if not asked) */ }
            }
            /* (a popover's: not handling the click, a link it is in goes on, as engines do) */
            return false;
        }
        if ((isInput && (type === 'submit' || type === 'image')) || (el.localName === 'button' && buttonType(el) === 'submit')) {
            var form = disabledControl(el) ? null : formOf(el);
            if (form && form.isConnected) { requestSubmit(form, el); return; }
            return false;
        }
        if ((isInput && type === 'reset') || (el.localName === 'button' && buttonType(el) === 'reset')) {
            var f2 = disabledControl(el) ? null : formOf(el);
            if (f2 && f2.isConnected && typeof f2.reset === 'function') { f2.reset(); return; }
            return false;
        }
        /* (what does nothing on its own: false, so a link it is in goes on) */
        if (el.localName === 'button' && (el.hasAttribute('command') || el.hasAttribute('commandfor'))) return false;
        if (isInput && type === 'file') return;
        if (isInput && /^(text|search|tel|url|email|password|number|range|color|date|month|week|time|datetime-local|hidden)$/.test(type)) return false;
        if (isInput || el.localName === 'button') return false;
        if (el.localName === 'label') {
            var control = el.control !== undefined ? el.control
                : (el.htmlFor ? (el.ownerDocument.getElementById(el.htmlFor)) : el.querySelector('input,button,select,textarea'));
            if (control && control !== el && typeof control.click === 'function') { control.click(); return; }
            return false;
        }
        if (el.localName === 'summary') {
            var d = summaryOf(el);
            if (d) {
                var was = d.hasAttribute('open');
                /* (its toggle event: the attribute's change queues it, __cyberDetailsOpen) */
                if (was) d.removeAttribute('open'); else d.setAttribute('open', '');
            }
            return;
        }
        followHyperlink(el, view);
    }
    var FIRING_SUBMIT = Symbol('firingSubmit');
    function requestSubmit(form, submitter) {
        /* (HTML 4.10.21.3: while its submission events fire, a form is not submitted again) */
        if (form[FIRING_SUBMIT]) return;
        form[FIRING_SUBMIT] = true;
        try { requestSubmit0(form, submitter); } finally { form[FIRING_SUBMIT] = false; }
    }
    function requestSubmit0(form, submitter) {
        /* (interactive validation: an invalid form isn't sent, unless told not to check) */
        if (!form.hasAttribute('novalidate') && !(submitter && submitter.hasAttribute && submitter.hasAttribute('formnovalidate')) &&
            typeof form.checkValidity === 'function' && !form.checkValidity()) return;
        var ok = form.dispatchEvent(new global.SubmitEvent('submit', { bubbles: true, cancelable: true,
                                                                       submitter: submitter || null }));
        form[FIRING_SUBMIT] = false;
        if (ok && typeof form.__cyberSubmit === 'function') form.__cyberSubmit(submitter);
    }
    function followHyperlink(a, view) {
        var href = a.getAttribute('href');
        if (href === null) return;
        /* (a javascript: URL runs, in a task of its own) */
        if (/^\s*javascript:/i.test(href)) {
            var code = href.replace(/^\s*javascript:/i, '');
            try { code = decodeURIComponent(code); } catch (e) { /* (as it is) */ }
            global.setTimeout(function () { (0, global.eval)(code); }, 0);
            return;
        }
        var loc = view.location;
        if (!loc) return;
        var url;
        try { url = new global.URL(href, a.baseURI || loc.href); } catch (e) { return; }
        /* (hyperlink auditing: each ping URL, parsed as the document's, sent "PING" as it goes) */
        var ping = a.getAttribute('ping');
        if (ping && typeof global.fetch === 'function' && /^https?:$/.test(url.protocol)) {
            String(ping).split(/[\t\n\f\r ]+/).forEach(function (p) {
                if (!p) return;
                var pa = a.ownerDocument.createElement('a'), pu;
                pa.setAttribute('href', p);
                try { pu = pa.href; } catch (e) { pu = ''; }
                if (!/^https?:/.test(pu)) return;
                try { global.fetch(pu, { method: 'POST', body: 'PING', headers: { 'Content-Type': 'text/ping' }, keepalive: true, credentials: 'include' }).catch(function () { }); } catch (e) { }
            });
        }
        /* (target: another browsing context, the link's own or its document's <base>) */
        var target = a.getAttribute('target');
        if (target === null) {
            var bt = a.ownerDocument && a.ownerDocument.querySelector('base[target]');
            if (bt) target = bt.getAttribute('target');
        }
        target = target === null ? '' : String(target).replace(/[\t\n]/g, '');
        var tl = target.toLowerCase();
        if (tl && tl !== '_self') {
            if (tl === '_parent' || tl === '_top') {
                var up = tl === '_top' ? view.top : view.parent;
                if (up && up !== view) { up.location.href = url.href; return; }
            } else if (typeof view.open === 'function') {
                var rel = ' ' + String(a.getAttribute('rel') || '').toLowerCase().replace(/\s+/g, ' ') + ' ';
                var noopen = rel.indexOf(' noopener ') >= 0 || rel.indexOf(' noreferrer ') >= 0 ||
                             (tl === '_blank' && rel.indexOf(' opener ') < 0);
                view.open(url.href, target, noopen ? 'noopener' : '');
                return;
            }
        }
        var cur;
        try { cur = new global.URL(loc.href); } catch (e) { return; }
        /* same-document navigation: a fragment change */
        if (url.origin === cur.origin && url.pathname === cur.pathname && url.search === cur.search) {
            if (url.hash !== cur.hash) loc.hash = url.hash;
            else scrollToFragment(view, url.hash);
            return;
        }
        if (typeof view.__cyberNavigate === 'function') view.__cyberNavigate(url.href, a);
        else loc.href = url.href;
    }
    /* HTML "form submission": where it goes, from the form and its submitter. */
    function submitForm(form, submitter) {
        var d = form.ownerDocument, view = d.defaultView;
        if (!view || !form.isConnected) return;
        var attr = function (name, over) {
            return submitter && submitter.hasAttribute(over) ? submitter.getAttribute(over) : form.getAttribute(name);
        };
        var method = String(attr('method', 'formmethod') || 'get').toLowerCase();
        if (method === 'dialog') {
            var dlg = form.closest('dialog');
            if (dlg && dlg.open) dlg.close(submitter ? submitter.value : undefined);
            return;
        }
        var action = attr('action', 'formaction'), url;
        /* (its query in the document's encoding, URL parsing's encoding override) */
        if (action && /[^\x00-\x7f]/.test(action) && d.characterSet && !/^utf-8$/i.test(d.characterSet) && typeof global.__cyberLegacyQuery === 'function') {
            var lqa = global.__cyberLegacyQuery(action, d.characterSet);
            if (lqa) action = lqa;
        }
        try { url = new global.URL(action || d.URL, form.baseURI); } catch (e) { return; }
        var post = method === 'post', data = new global.FormData(form, submitter || undefined);
        var nl = function (s) { return String(s).replace(/\r\n|\r|\n/g, '\r\n'); };
        var pairs = function () {
            var o = [];
            data.forEach(function (v, n) { o.push([nl(n), nl(global.File && v instanceof global.File ? v.name : v)]); });
            return o;
        };
        /* (the form's encoding: accept-charset's first known label, else its document's; UTF-16 as UTF-8) */
        var formCs = (function () {
            var ls = String(form.getAttribute('accept-charset') || '').trim().split(/[\t\n\f\r ]+/);
            for (var li = 0; li < ls.length; li++) {
                if (!ls[li]) continue;
                try { var e0 = new global.TextDecoder(ls[li]).encoding; if (e0) return /^utf-16/i.test(e0) ? 'UTF-8' : e0; } catch (x) { }
            }
            var c0 = d.characterSet || 'UTF-8';
            return /^utf-16/i.test(c0) ? 'UTF-8' : c0;
        })();
        /* (a string's bytes in it: a character it lacks as &#N;, a lone surrogate first U+FFFD) */
        var usv = function (s) { return String(s).replace(/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(^|[^\uD800-\uDBFF])[\uDC00-\uDFFF]/g, function (m, pre) { return (pre || '') + '\uFFFD'; }); };
        var bytesOf = function (s) {
            s = usv(s);
            if (!/^utf-8$/i.test(formCs) && typeof global.__cyberLegacyQuery === 'function') {
                var q = global.__cyberLegacyQuery('http://x/?' + s.replace(/%/g, '%25').replace(/#/g, '%23'), formCs);
                if (q !== null && q !== undefined) {
                    q = q.slice(10);
                    var out = [];
                    for (var k = 0; k < q.length; k++) {
                        var c = q.charCodeAt(k);
                        if (c === 37 && /^[0-9A-Fa-f]{2}$/.test(q.substr(k + 1, 2))) { out.push(parseInt(q.substr(k + 1, 2), 16)); k += 2; }
                        else out.push(c & 0xFF);
                    }
                    return new Uint8Array(out);
                }
            }
            return new global.TextEncoder().encode(s);
        };
        /* (application/x-www-form-urlencoded: the bytes percent-encoded, a space a +) */
        var urlenc = function (s) {
            var b = bytesOf(s), o = '';
            for (var k = 0; k < b.length; k++) {
                var c = b[k];
                if ((c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c === 42 || c === 45 || c === 46 || c === 95) o += String.fromCharCode(c);
                else if (c === 32) o += '+';
                else o += '%' + (c < 16 ? '0' : '') + c.toString(16).toUpperCase();
            }
            return o;
        };
        var urlencoded = function () { return pairs().map(function (p) { return urlenc(p[0]) + '=' + urlenc(p[1]); }).join('&'); };
        if (!post) url.search = urlencoded();
        var target = attr('target', 'formtarget');
        /* (a POST into a frame: its body as the enctype has it, sent, and the response the frame's document; one from a frame
         * into itself the same, its own document replaced) */
        var selfPost = post && (!target || /^_self$/i.test(target)) && view.parent && view.parent !== view;
        if (post && ((target && !/^_(self|top|parent|blank)$/i.test(target)) || selfPost)) {
            var w = selfPost ? view : null;
            if (!w) {
                try { w = view.frames[target]; } catch (e) { w = null; }
                if (!w || w === view || !w.document) {
                    var fr = d.getElementsByName(target);
                    for (var fi = 0; fi < fr.length && !w; fi++) if (fr[fi].localName === 'iframe' || fr[fi].localName === 'frame') w = fr[fi].contentWindow;
                }
            }
            var doc2 = null;
            try { doc2 = w && w.document; } catch (e) { doc2 = null; }
            if (doc2 && typeof view.fetch === 'function') {
                var enc = String(attr('enctype', 'formenctype') || '').toLowerCase(), body, type;
                if (enc === 'multipart/form-data') {
                    var bd = '----cyberFormBoundary' + Math.random().toString(36).slice(2, 18), parts = [];
                    var esc = function (s) { return String(s).replace(/\n/g, '%0A').replace(/\r/g, '%0D').replace(/"/g, '%22'); };
                    data.forEach(function (v, n) {
                        var isFile = global.File && v instanceof global.File;
                        var h = '--' + bd + '\r\nContent-Disposition: form-data; name="' + esc(nl(n)) + '"';
                        if (isFile) h += '; filename="' + esc(v.name) + '"\r\nContent-Type: ' + (v.type || 'application/octet-stream');
                        parts.push(bytesOf(h + '\r\n\r\n'), isFile ? v : bytesOf(nl(v)), bytesOf('\r\n'));
                    });
                    parts.push(bytesOf('--' + bd + '--\r\n'));
                    /* (the files' bytes read first: the body goes as one array of bytes) */
                    body = Promise.all(parts.map(function (p) { return p instanceof Uint8Array ? p : p.arrayBuffer().then(function (ab) { return new Uint8Array(ab); }); })).then(function (a) {
                        var tot = 0;
                        a.forEach(function (x) { tot += x.length; });
                        var all = new Uint8Array(tot), at = 0;
                        a.forEach(function (x) { all.set(x, at); at += x.length; });
                        return all;
                    });
                    type = 'multipart/form-data; boundary=' + bd;
                } else if (enc === 'text/plain') {
                    body = bytesOf(pairs().map(function (p) { return p[0] + '=' + p[1] + '\r\n'; }).join(''));
                    type = 'text/plain';
                } else {
                    body = urlencoded();
                    type = 'application/x-www-form-urlencoded';
                }
                Promise.resolve(body).then(function (b) { return view.fetch(url.href, { method: 'POST', body: b, headers: { 'Content-Type': type } }); }).then(function (r) {
                    var ct = r.headers.get('content-type') || '';
                    return r.text().then(function (t) {
                        var html = /html|xml/i.test(ct) ? t : '<pre>' + t.replace(/&/g, '&amp;').replace(/</g, '&lt;') + '</pre>';
                        doc2.open(); doc2.write(html); doc2.close();
                    });
                }).catch(function () { });
                return;
            }
        }
        if (target && !/^_self$/i.test(target) && !post) {
            /* (the form's rel: noreferrer, noopener, opener; a new _blank one implies noopener) */
            var frel = ' ' + String(form.getAttribute('rel') || '').toLowerCase().replace(/\s+/g, ' ') + ' ';
            var fnoref = frel.indexOf(' noreferrer ') >= 0;
            var fnoopen = fnoref || frel.indexOf(' noopener ') >= 0 || (/^_blank$/i.test(target) && frel.indexOf(' opener ') < 0);
            view.open(url.href, target, [fnoopen ? 'noopener' : '', fnoref ? 'noreferrer' : ''].filter(Boolean).join(','));
            return;
        }
        if (typeof view.__cyberNavigate === 'function') view.__cyberNavigate(url.href, submitter || form, post ? data : null);
        else if (!post) view.location.href = url.href;
    }
    var HEP = global.HTMLElement && global.HTMLElement.prototype;
    if (HEP) define(HEP, 'click', click);
    var ELP = global.Element && global.Element.prototype;
    if (ELP && Object.prototype.hasOwnProperty.call(ELP, 'click')) define(ELP, 'click', click);
    var FP = global.HTMLFormElement && global.HTMLFormElement.prototype;
    if (FP) define(FP, '__cyberSubmit', function (submitter) { submitForm(this, submitter); });
    if (FP) {
        define(FP, 'requestSubmit', function (submitter) { requestSubmit(this, submitter); });
    }

    /* ---- event handler attributes (HTML 8.1.8.1) ---- */
    var HANDLERS = Symbol.for('cyber.eventHandlers');
    var GLOBAL_HANDLERS = ['abort', 'auxclick', 'beforeinput', 'beforematch', 'beforetoggle', 'blur',
        'cancel', 'canplay', 'canplaythrough', 'change', 'click', 'close', 'command', 'contextlost', 'contextmenu',
        'contextrestored', 'copy', 'cuechange', 'cut', 'dblclick', 'drag', 'dragend', 'dragenter',
        'dragleave', 'dragover', 'dragstart', 'drop', 'durationchange', 'emptied', 'ended', 'error',
        'focus', 'formdata', 'input', 'invalid', 'keydown', 'keypress', 'keyup', 'load', 'loadeddata',
        'loadedmetadata', 'loadstart', 'mousedown', 'mouseenter', 'mouseleave', 'mousemove', 'mouseout',
        'mouseover', 'mouseup', 'paste', 'pause', 'play', 'playing', 'progress', 'ratechange', 'reset',
        'resize', 'scroll', 'scrollend', 'securitypolicyviolation', 'seeked', 'seeking', 'select',
        'slotchange', 'stalled', 'submit', 'suspend', 'timeupdate', 'toggle', 'volumechange', 'waiting',
        'wheel', 'scrollsnapchange', 'scrollsnapchanging', 'contentvisibilityautostatechange',
        'animationstart', 'animationiteration', 'animationend', 'animationcancel',
        'transitionrun', 'transitionstart', 'transitionend', 'transitioncancel', 'pointerover',
        'pointerenter', 'pointerdown', 'pointermove', 'pointerup', 'pointercancel', 'pointerout',
        'pointerleave', 'gotpointercapture', 'lostpointercapture', 'touchstart', 'touchend',
        'touchmove', 'touchcancel', 'selectstart', 'selectionchange'];
    var WINDOW_HANDLERS = ['afterprint', 'beforeprint', 'beforeunload', 'hashchange', 'languagechange',
        'message', 'messageerror', 'offline', 'online', 'pagehide', 'pageshow', 'popstate',
        'rejectionhandled', 'storage', 'unhandledrejection', 'unload', 'pagereveal', 'pageswap'];
    var DOCUMENT_HANDLERS = ['readystatechange', 'visibilitychange', 'fullscreenchange', 'fullscreenerror'];
    /* the types whose on<type> properties are the handler accessors below */
    var HANDLER_TYPE = Object.create(null);
    GLOBAL_HANDLERS.concat(WINDOW_HANDLERS, DOCUMENT_HANDLERS).forEach(function (t) { HANDLER_TYPE[t] = true; });
    /* (an accessor's this: an object that inherits it; nothing, the global) */
    function handlerThis(t, P) {
        if (t === undefined || t === null) return global;
        if ((typeof t === 'object' || typeof t === 'function') && (t === global || t === win || P.isPrototypeOf(t))) return t;
        throw new TypeError('Illegal invocation');
    }
    function handlerAccessor(P, type) {
        var name = 'on' + type;
        var own = Object.getOwnPropertyDescriptor(P, name);
        if (own && (own.get || own.set)) return;    /* (a native one already) */
        Object.defineProperty(P, name, { configurable: true, enumerable: true,
            get: function () {
                var t = handlerThis(this, P);
                /* <body>'s window handlers are the window's (HTML 8.1.8.2); a body with no window, none */
                var fw = bodyForwards(t, type);
                if (fw && t.ownerDocument !== global.document) return null;
                var self = fw ? global : t;
                var h = self[HANDLERS];
                if (h && Object.prototype.hasOwnProperty.call(h, type)) return h[type];
                /* the content attribute, compiled on first use (the window's
                 * are <body>'s) */
                var holder = self === global && BODY_FORWARDED.indexOf(type) >= 0 && global.document ? global.document.body : self;
                if (holder && typeof holder.getAttribute === 'function') {
                    var code = holder.getAttribute(name);
                    /* (an element of a document with no browsing context: never compiled) */
                    if (code !== null && holder.ownerDocument && !holder.ownerDocument.defaultView) return null;
                    if (code !== null) {
                        var fn = null;
                        /* (its source text as HTML's "getting the current value of the event handler" makes it: function onX(params) { body }) */
                        var params = type === 'error' && (t === global || fw) ? 'event, source, lineno, colno, error' : 'event';
                        try {
                            fn = (0, eval)('(function ' + name + '(' + params + ') {\n' + code + '\n})');
                            if (typeof fn !== 'function') throw new SyntaxError('Invalid event handler');
                        } catch (e) {
                            fn = null;
                            /* (a body that only parses on its own lines: as the Function constructor takes it) */
                            if (!(e instanceof SyntaxError)) report(e);
                            else {
                                try { fn = params === 'event' ? new Function('event', code) : new Function('event', 'source', 'lineno', 'colno', 'error', code); }
                                catch (e2) { report(e2); }
                            }
                        }
                        /* (compiled: its value now; its place in the listeners as it was, even when it failed) */
                        var hh = self[HANDLERS];
                        if (!hh) { hh = Object.create(null); Object.defineProperty(self, HANDLERS, { value: hh, configurable: true }); }
                        hh[type] = fn;
                        var qq = self[HANDLER_SEQ];
                        if (!qq) { qq = Object.create(null); Object.defineProperty(self, HANDLER_SEQ, { value: qq, configurable: true }); }
                        if (!qq[type]) qq[type] = ++listenerSeq;
                        listenerChanges++;
                        return fn;
                    }
                }
                return null;
            },
            set: function (v) {
                var t = handlerThis(this, P);
                var fw = bodyForwards(t, type);
                if (fw && t.ownerDocument !== global.document) return;
                var self = fw ? global : t;
                setHandler(self, type, typeof v === 'function' || (v !== null && typeof v === 'object') ? v : null);
            } });
        /* (named as an attribute's accessors are) */
        var d = Object.getOwnPropertyDescriptor(P, name);
        Object.defineProperty(d.get, 'name', { configurable: true, value: 'get ' + name });
        Object.defineProperty(d.set, 'name', { configurable: true, value: 'set ' + name });
    }
    /* The handlers <body> (and <frameset>) forward to the window. */
    var BODY_FORWARDED = ['blur', 'error', 'focus', 'load', 'resize', 'scroll'].concat(WINDOW_HANDLERS);
    function bodyForwards(el, type) {
        return el !== global && el && (el.localName === 'body' || el.localName === 'frameset') &&
               el.namespaceURI === 'http://www.w3.org/1999/xhtml' && BODY_FORWARDED.indexOf(type) >= 0;
    }
    /* (document.open, HTML 8.4.x: every listener and handler of the document, its shadow-including descendants and its window, gone at once) */
    var clearTarget = function (t) {
        var map = t[LISTENERS];
        if (map) {
            for (var ty in map) (map[ty] || []).forEach(function (l) { l.removed = true; });
            delete t[LISTENERS];
        }
        var h = t[HANDLERS];
        if (h) { for (var k in h) h[k] = null; }
        listenerChanges++;
    };
    Object.defineProperty(global, '__cyberDocOpenClear', { configurable: true, value: function (doc) {
        var walk = function (n) {
            clearTarget(n);
            if (n.nodeType === 1 && n.shadowRoot) walk(n.shadowRoot);
            for (var c = n.firstChild; c; c = c.nextSibling) walk(c);
        };
        walk(doc);
        var w = doc.defaultView;
        if (w && w.document === doc) {
            try { if (typeof w.__cyberDocOpenClearWindow === 'function') w.__cyberDocOpenClearWindow(); else clearTarget(w); } catch (e) { }
        }
    } });
    Object.defineProperty(global, '__cyberDocOpenClearWindow', { configurable: true, value: function () { clearTarget(global); } });
    function setHandler(target, type, v) {
        listenerChanges++;
        var h = target[HANDLERS];
        if (!h) {
            h = Object.create(null);
            Object.defineProperty(target, HANDLERS, { value: h, configurable: true });
        }
        h[type] = v;
        /* (HTML "event handlers": its listener added, at the end of the list, when it is first set; kept where it is while
         * it changes; gone when it is set to null) */
        var q = target[HANDLER_SEQ];
        if (!q) { q = Object.create(null); Object.defineProperty(target, HANDLER_SEQ, { value: q, configurable: true }); }
        if (v === null || v === undefined) delete q[type];
        else if (!q[type]) q[type] = ++listenerSeq;
    }
    var HANDLER_SEQ = Symbol('cyber.handlerSeq');
    /* (a content attribute set: the handler's value its uncompiled script, compiled when read; activated if it wasn't;
     * removed: deactivated) */
    /* (a details element's open attribute added or removed: its toggle event in a task, one for the changes before it runs,
     * HTML's details notification task steps) */
    var DOPEN = Symbol('detailsOpen'), DPEND = Symbol('detailsToggle');
    /* (the details elements of el's name group: the same name, in the same tree, el left out) */
    function detailsGroup(el) {
        var nm = el.getAttribute('name');
        if (!nm) return [];
        /* (its tree: the document, a shadow root, or the top of a subtree not in either) */
        var root = el.getRootNode(), out = [];
        var all = root.querySelectorAll ? Array.prototype.slice.call(root.querySelectorAll('details[name]')) : [];
        if (root.nodeType === 1 && root.localName === 'details') all.unshift(root);
        for (var i = 0; i < all.length; i++) {
            var d = all[i];
            if (d !== el && d.namespaceURI === HTML_NS && d.getAttribute('name') === nm) out.push(d);
        }
        return out;
    }
    /* (inserted, or its name changed: closed if another of its group is open, HTML's ensure details exclusivity) */
    function detailsEnsure(el) {
        if (!el.hasAttribute('open')) return;
        var g = detailsGroup(el);
        for (var i = 0; i < g.length; i++) if (g[i].hasAttribute('open')) { el.removeAttribute('open'); return; }
    }
    Object.defineProperty(global, '__cyberDetailsInserted', { configurable: true, value: function (n) {
        if (!n || n.nodeType !== 1) return;
        var list = n.localName === 'details' ? [n] : [];
        if (n.querySelectorAll) list = list.concat(Array.prototype.slice.call(n.querySelectorAll('details[open]')));
        list.forEach(function (d) {
            if (d.namespaceURI !== HTML_NS) return;
            /* (open from the parser, never seen by the attribute hook: its toggle event all the same) */
            if (d[DOPEN] === undefined && d.hasAttribute('open')) global.__cyberDetailsOpen(d, true, true);
            if (!d.hasAttribute('name') || !d.hasAttribute('open')) return;
            /* (as if inserted one at a time, in tree order: the open ones outside, and those before it, count) */
            var at = list.indexOf(d), g = detailsGroup(d);
            for (var i = 0; i < g.length; i++) {
                var o = g[i], oi = list.indexOf(o);
                if (o.hasAttribute('open') && (oi < 0 ? !(n === o || n.contains(o)) : oi < at)) { d.removeAttribute('open'); return; }
            }
        });
    } });
    Object.defineProperty(global, '__cyberDetailsName', { configurable: true, value: function (el) { detailsEnsure(el); } });
    /* (a document the parser made: its open details' toggle events, the attribute never having been added by a hook) */
    Object.defineProperty(global, '__cyberDetailsParsed', { configurable: true, value: function (doc) {
        var all = doc && doc.querySelectorAll ? doc.querySelectorAll('details[open]') : [];
        for (var i = 0; i < all.length; i++) if (all[i][DOPEN] === undefined && all[i].namespaceURI === HTML_NS) global.__cyberDetailsOpen(all[i], true, true);
    } });
    Object.defineProperty(global, '__cyberDetailsOpen', { configurable: true, value: function (el, present, noClose) {
        present = !!present;
        /* (opened: the others of its group closed; not for one the parser made open, which its insertion decides) */
        if (present && !noClose && el.getAttribute('name')) {
            detailsGroup(el).forEach(function (d) { if (d.hasAttribute('open')) d.removeAttribute('open'); });
        }
        var prev = el[DOPEN];
        if (prev === undefined) prev = !present;
        if (prev === present) return;
        el[DOPEN] = present;
        var p = el[DPEND];
        if (p) { p.newState = present ? 'open' : 'closed'; return; }
        p = { oldState: prev ? 'open' : 'closed', newState: present ? 'open' : 'closed' };
        el[DPEND] = p;
        global.setTimeout(function () {
            el[DPEND] = null;
            /* (one for all the changes since it was queued, the same state or not; a user agent's: trusted) */
            var te = new (global.ToggleEvent || Event)('toggle', { oldState: p.oldState, newState: p.newState });
            if (typeof global.__cyberDispatchTrusted === 'function') global.__cyberDispatchTrusted(el, te); else el.dispatchEvent(te);
        }, 0);
    } });
    Object.defineProperty(global, '__cyberHandlerAttr', { configurable: true, value: function (el, name, present) {
        var type = String(name).slice(2).toLowerCase();
        if (!HANDLER_TYPE[type] || !el || el.nodeType !== 1) return;
        var self = bodyForwards(el, type) ? (el.ownerDocument === global.document ? global : null) : el;
        if (!self) return;
        listenerChanges++;
        var h = self[HANDLERS];
        if (h) delete h[type];
        var q = self[HANDLER_SEQ];
        if (!q) { q = Object.create(null); Object.defineProperty(self, HANDLER_SEQ, { value: q, configurable: true }); }
        if (!present) delete q[type];
        else if (!q[type]) q[type] = ++listenerSeq;
    } });
    var ELEMENT_P = global.HTMLElement && global.HTMLElement.prototype;
    var DOC_P = global.Document && global.Document.prototype;
    /* (MathML Core 2.1.x: MathMLElement, an Element with GlobalEventHandlers, HTMLOrSVGElement and ElementCSSInlineStyle) */
    if (typeof global.MathMLElement !== 'function' && global.Element) {
        var MC = function MathMLElement() { throw new TypeError('Illegal constructor'); };
        var MP = Object.create(global.Element.prototype);
        Object.defineProperty(MP, 'constructor', { value: MC, writable: true, configurable: true });
        Object.defineProperty(MP, Symbol.toStringTag, { value: 'MathMLElement', configurable: true });
        Object.defineProperty(MC, 'prototype', { value: MP, writable: false, enumerable: false, configurable: false });
        Object.setPrototypeOf(MC, global.Element);
        var SP_ = global.SVGElement && global.SVGElement.prototype;
        if (SP_) ['dataset', 'style', 'attributeStyleMap', 'tabIndex', 'autofocus', 'focus', 'blur'].forEach(function (k) {
            var d = Object.getOwnPropertyDescriptor(SP_, k);
            if (d) Object.defineProperty(MP, k, d);
        });
        Object.defineProperty(global, 'MathMLElement', { value: MC, writable: true, configurable: true, enumerable: false });
    }
    var MATH_P = global.MathMLElement && global.MathMLElement.prototype;
    [ELEMENT_P, global.SVGElement && global.SVGElement.prototype, MATH_P, DOC_P].forEach(function (P) {
        if (P) GLOBAL_HANDLERS.forEach(function (t) { handlerAccessor(P, t); });
    });
    if (DOC_P) DOCUMENT_HANDLERS.forEach(function (t) { handlerAccessor(DOC_P, t); });
    /* (the legacy prefixed handlers: GlobalEventHandlers' too) */
    [ELEMENT_P, global.SVGElement && global.SVGElement.prototype, MATH_P, DOC_P, Object.getPrototypeOf(global) !== Object.prototype ? Object.getPrototypeOf(global) : null].forEach(function (P) {
        if (P) Object.keys(HANDLER_NAME).forEach(function (k) { handlerAccessor(P, HANDLER_NAME[k]); });
    });
    /* (the Fullscreen API's on Element and Document; slotchange on ShadowRoot) */
    [global.Element && global.Element.prototype, DOC_P].forEach(function (P) {
        if (P) ['fullscreenchange', 'fullscreenerror'].forEach(function (t) { handlerAccessor(P, t); });
    });
    if (global.ShadowRoot) handlerAccessor(global.ShadowRoot.prototype, 'slotchange');
    var WINP = Object.getPrototypeOf(global);
    /* (Window is the [Global]: its handlers are the window's own properties; a plain value already there kept as the handler) */
    GLOBAL_HANDLERS.concat(WINDOW_HANDLERS).forEach(function (t) {
        var d = Object.getOwnPropertyDescriptor(global, 'on' + t);
        if (d && 'value' in d && d.configurable) {
            delete global['on' + t];
            if (typeof d.value === 'function') setHandler(global, t, d.value);
        }
        handlerAccessor(global, t);
        if (WINP && WINP !== Object.prototype) { var wd = Object.getOwnPropertyDescriptor(WINP, 'on' + t); if (wd && wd.configurable) delete WINP['on' + t]; }
    });
    /* (the window's own, as its native handlers are: the global's attributes) */
    ['transitionrun', 'transitionstart', 'transitionend', 'transitioncancel',
     'animationstart', 'animationiteration', 'animationend', 'animationcancel'].forEach(function (t) {
        if (!Object.getOwnPropertyDescriptor(global, 'on' + t)) handlerAccessor(global, t);
        if (WINP && WINP !== Object.prototype && Object.getOwnPropertyDescriptor(WINP, 'on' + t)) delete WINP['on' + t];
    });
    /* (WindowEventHandlers: <body> and <frameset>'s, forwarded to the window) */
    [global.HTMLBodyElement, global.HTMLFrameSetElement].forEach(function (C) {
        if (C && C.prototype) WINDOW_HANDLERS.forEach(function (t) { handlerAccessor(C.prototype, t); });
    });
    /* window and document data properties (set up earlier as plain values)
     * give way to the accessors, keeping a handler already stored there */
    [global, global.document].forEach(function (o) {
        if (!o) return;
        GLOBAL_HANDLERS.concat(WINDOW_HANDLERS, DOCUMENT_HANDLERS).forEach(function (t) {
            var d = Object.getOwnPropertyDescriptor(o, 'on' + t);
            if (!d || !('value' in d) || !d.configurable) return;
            delete o['on' + t];
            if (typeof d.value === 'function') setHandler(o, t, d.value);
        });
    });

    /* ---- messaging (HTML 9.4, 9.5) ---- */
    /* (StructuredSerialize: what it throws, a getter's exception too, goes to the caller) */
    function clone(v) {
        if (typeof global.structuredClone === 'function') return global.structuredClone(v);
        return v;
    }
    function task(fn) { global.setTimeout(fn, 0); }
    var PORT = Symbol('port');
    function MessagePort() { throw new TypeError('Illegal constructor'); }
    MessagePort.prototype = Object.create(ETP);
    define(MessagePort.prototype, 'constructor', MessagePort);
    function newPort() {
        var p = Object.create(MessagePort.prototype);
        Object.defineProperty(p, PORT, { value: { other: null, started: false, closed: false, queue: [] } });
        return p;
    }
    function port(p) { var s = p && p[PORT]; if (!s) throw new TypeError('Illegal invocation'); return s; }
    function deliver(target) {
        var s = target[PORT];
        if (!s.started || s.closed) return;
        while (s.queue.length) {
            var m = s.queue.shift();
            target.dispatchEvent(new global.MessageEvent('message', { data: m.data, ports: m.ports || [] }));
        }
    }
    /* (postMessage's transfer list: an array, or options.transfer) */
    function transferOf(x) {
        if (x === undefined || x === null) return [];
        if (typeof x === 'object' && typeof x[Symbol.iterator] === 'function' && !(typeof x.transfer === 'object')) return Array.from(x);
        if (typeof x === 'object' && x.transfer !== undefined) {
            if (x.transfer === null || typeof x.transfer !== 'object' || typeof x.transfer[Symbol.iterator] !== 'function') throw new TypeError("Failed to execute 'postMessage': transfer is not iterable.");
            return Array.from(x.transfer);
        }
        return [];
    }
    function cloneWith(message, tl) {
        if (!tl.length) return clone(message);
        return global.structuredClone(message, { transfer: tl });
    }
    function portsOf(tl) { return tl.filter(function (t) { return Object.prototype.toString.call(t) === '[object MessagePort]'; }); }
    define(MessagePort.prototype, 'postMessage', function postMessage(message) {
        var s = port(this), other = s.other;
        var tl = transferOf(arguments[1]);
        if (tl.indexOf(this) >= 0) throw new global.DOMException("Failed to execute 'postMessage' on 'MessagePort': Port at index is the source port.", 'DataCloneError');
        var data = cloneWith(message, tl), ports = portsOf(tl);
        if (s.closed || !other) return;
        task(function () {
            var o = other[PORT];
            if (o.closed) return;
            o.queue.push({ data: data, ports: ports });
            deliver(other);
        });
    });
    define(MessagePort.prototype, 'start', function start() {
        var s = port(this), self = this;
        if (s.started) return;
        s.started = true;
        if (s.queue.length) task(function () { deliver(self); });
    });
    define(MessagePort.prototype, 'close', function close() { port(this).closed = true; });
    /* setting onmessage starts the port (HTML 9.4.4) */
    Object.defineProperty(MessagePort.prototype, 'onmessage', { configurable: true, enumerable: true,
        get: function () { var h = this[HANDLERS]; return h && h.message || null; },
        set: function (v) { setHandler(this, 'message', typeof v === 'function' ? v : null); if (v) this.start(); } });
    handlerAccessor(MessagePort.prototype, 'messageerror');
    Object.defineProperty(MessagePort.prototype, Symbol.toStringTag, { value: 'MessagePort', configurable: true });
    function MessageChannel() {
        if (!new.target) throw new TypeError("Failed to construct 'MessageChannel': Please use the 'new' operator");
        var a = newPort(), b = newPort();
        a[PORT].other = b; b[PORT].other = a;
        Object.defineProperty(this, CHAN, { value: [a, b] });
    }
    /* (its ports: behind the prototype's getters) */
    var CHAN = Symbol('cyber.channel');
    ['port1', 'port2'].forEach(function (k, i) {
        var g = { [k]: function () { var s = this && this[CHAN]; if (!s) throw new TypeError('Illegal invocation'); return s[i]; } }[k];
        Object.defineProperty(g, 'name', { value: 'get ' + k });
        Object.defineProperty(MessageChannel.prototype, k, { get: g, enumerable: true, configurable: true });
    });
    Object.defineProperty(MessageChannel.prototype, Symbol.toStringTag, { value: 'MessageChannel', configurable: true });
    expose('MessageChannel', MessageChannel);
    expose('MessagePort', MessagePort);
    /* window.postMessage: a message event at the target window, from the
     * caller's window (the realm of the function called) */
    function originOf(w) { try { return w.location.origin; } catch (e) { return 'null'; } }
    define(global, 'postMessage', function postMessage(message, options) {
        var target = this == null ? global : this;
        /* (from who called: a parent posting to its frame calls the frame's own function) */
        var source = global.__cyberCallerGlobal() || global;
        var targetOrigin = typeof options === 'object' && options !== null ? options.targetOrigin : options;
        if (targetOrigin === undefined) targetOrigin = '/';
        var tl = typeof options === 'object' && options !== null ? transferOf(options) : transferOf(arguments[2]);
        var data = cloneWith(message, tl), ports = portsOf(tl);
        task(function () {
            if (targetOrigin !== '*' && targetOrigin !== '/' && targetOrigin !== originOf(target)) return;
            var ME = target.MessageEvent || global.MessageEvent;
            target.dispatchEvent(new ME('message', { data: data, origin: originOf(source), source: source, ports: ports }));
        });
    });

    /* ---- session history (HTML 7.2, 7.4) ---- */
    var loc = global.location;
    var hrefDesc = loc && Object.getOwnPropertyDescriptor(loc, 'href');
    var hashDesc = loc && Object.getOwnPropertyDescriptor(loc, 'hash');
    var hist = global.history;
    if (loc && hist && hrefDesc && hrefDesc.set && hashDesc && hashDesc.set) {
        var setHref = function (u) { hrefDesc.set.call(loc, u); };
        var getHref = function () { return hrefDesc.get.call(loc); };
        var entries = [{ url: getHref(), state: null }], index = 0;
        var stripHash = function (u) { var i = u.indexOf('#'); return i < 0 ? u : u.slice(0, i); };
        var resolve = function (u) {
            try { return new global.URL(String(u), getHref()).href; }
            catch (e) { throw new (global.DOMException || Error)("Failed to execute 'pushState': the URL '" + u + "' is invalid (" + e + ").", 'SecurityError'); }
        };
        var sameOrigin = function (a, b) {
            try { return new global.URL(a).origin === new global.URL(b).origin; } catch (e) { return false; }
        };
        /* (HTML "can have its URL rewritten": the same scheme, credentials, host and port; http(s): anything else may
         * change; file: not the path; the rest only the fragment) */
        var rewritable = function (from, to) {
            var a, b;
            try { a = new global.URL(from); b = new global.URL(to); } catch (e) { return false; }
            if (a.protocol !== b.protocol || a.username !== b.username || a.password !== b.password || a.host !== b.host) return false;
            if (/^https?:$/.test(b.protocol)) return true;
            if (b.protocol === 'file:') return a.pathname === b.pathname;
            return stripHash(a.href) === stripHash(b.href);
        };
        var fireHistoryEvents = function (oldURL, newURL, state, hashChanged) {
            task(function () {
                global.dispatchEvent(new global.PopStateEvent('popstate', { state: state }));
                if (hashChanged)
                    global.dispatchEvent(new global.HashChangeEvent('hashchange', { oldURL: oldURL, newURL: newURL }));
            });
        };
        var cloneState = function (s) { return s === undefined ? null : clone(s); };
        var rateStart = 0, rateCount = 0;
        var updateState = function (method, state, url, push) {
            var serialized = cloneState(state), cur = getHref(), target = cur;
            if (url !== undefined && url !== null) {
                target = resolve(url);
                if (!sameOrigin(target, cur) || !rewritable(cur, target))
                    throw new (global.DOMException || Error)("Failed to execute '" + method + "' on 'History': a history state object with URL '" + target + "' cannot be created in a document with origin '" + new global.URL(cur).origin + "'.", 'SecurityError');
            }
            /* (too many calls, as engines limit them (200 in 10 s): ignored, after the checks above) */
            var nowMs = Date.now();
            if (nowMs - rateStart > 10000) { rateStart = nowMs; rateCount = 0; }
            if (++rateCount > 200) return;
            if (push) { entries.length = index + 1; entries.push({ url: target, state: serialized }); index++; }
            else entries[index] = { url: target, state: serialized };
            if (target !== cur) setHref(target);
        };
        var traverse = function (delta) {
            var to = index + delta;
            /* (past this document's: another's, if the frame has one) */
            if (delta && (to < 0 || to >= entries.length) && global.__cyberFrameTraverse) {
                global.__cyberFrameTraverse(to < 0 ? to : to - entries.length + 1);
                return;
            }
            if (!delta || to < 0 || to >= entries.length) return;
            var from = entries[index], e = entries[to];
            index = to;
            /* entries of this document only (other documents' would navigate) */
            if (e.url !== getHref()) setHref(e.url);
            fireHistoryEvents(from.url, e.url, e.state,
                              stripHash(from.url) === stripHash(e.url) && from.url !== e.url);
        };
        /* a navigation to a fragment of this document (HTML 7.4.2.3.3) */
        var navigateToFragment = function (newURL) {
            var oldURL = getHref();
            if (newURL === oldURL) return;
            entries.length = index + 1;
            entries.push({ url: newURL, state: null });
            index++;
            setHref(newURL);
            scrollToFragment(global, new global.URL(newURL).hash);
            /* (a text directive in it: ::target-text over its text now) */
            if (newURL.indexOf(':~:') >= 0 && typeof global.__cyberSelectionChanged === 'function') global.__cyberSelectionChanged();
            fireHistoryEvents(oldURL, newURL, null, true);
        };
        /* (the URL's fragment once the document is parsed, and again as it finishes loading) */
        global.addEventListener('DOMContentLoaded', function () {
            var h = new global.URL(getHref()).hash;
            if (h) { try { scrollToFragment(global, h); } catch (e) { /* (at load) */ } }
        }, { once: true });
        global.addEventListener('load', function () {
            var h = new global.URL(getHref()).hash;
            if (h) scrollToFragment(global, h);
            if (global.__cyberFontsLoaded) global.__cyberFontsLoaded();
        }, { once: true });
        var HP = Object.getPrototypeOf(hist) && Object.getPrototypeOf(hist) !== Object.prototype ? Object.getPrototypeOf(hist) : hist;
        ['pushState', 'replaceState', 'back', 'forward', 'go'].forEach(function (k) {
            if (Object.prototype.hasOwnProperty.call(hist, k)) delete hist[k];
        });
        ['state', 'length'].forEach(function (k) {
            if (Object.prototype.hasOwnProperty.call(hist, k)) delete hist[k];
        });
        /* (the methods act on the History object they are called on: its
         * realm's session history, kept on it under a registered symbol, so
         * a frame's pushState called on the page's history is the page's) */
        var HIST = Symbol.for('cyber.history');
        Object.defineProperty(hist, HIST, { value: {
            update: updateState, traverse: traverse,
            state: function () { return entries[index].state; }, length: function () { return entries.length; } } });
        var histOf = function (h) {
            var impl = h !== null && (typeof h === 'object' || typeof h === 'function') ? h[HIST] : undefined;
            if (!impl) throw new TypeError('Illegal invocation');
            return impl;
        };
        define(HP, 'pushState', function pushState(state, unused, url) { var h = histOf(this); if (arguments.length < 2) throw new TypeError("Failed to execute 'pushState' on 'History': 2 arguments required, but only " + arguments.length + ' present.'); h.update('pushState', state, url, true); });
        define(HP, 'replaceState', function replaceState(state, unused, url) { var h = histOf(this); if (arguments.length < 2) throw new TypeError("Failed to execute 'replaceState' on 'History': 2 arguments required, but only " + arguments.length + ' present.'); h.update('replaceState', state, url, false); });
        define(HP, 'back', function back() { histOf(this).traverse(-1); });
        define(HP, 'forward', function forward() { histOf(this).traverse(1); });
        define(HP, 'go', function go(delta) { histOf(this).traverse(delta | 0); });
        Object.defineProperty(HP, 'state', { configurable: true, enumerable: true, get: function () { return histOf(this).state(); } });
        Object.defineProperty(HP, 'length', { configurable: true, enumerable: true, get: function () { return histOf(this).length(); } });
        /* location: hash, and href/assign/replace that only change the fragment */
        Object.defineProperty(loc, 'hash', { configurable: true, enumerable: true,
            get: hashDesc.get,
            set: function (v) {
                var u = new global.URL(getHref());
                v = String(v);
                /* (Location's: an empty one is an empty fragment, the '#' kept, not none as URL's) */
                var target;
                if (v.replace(/^#/, '') === '') {
                    if (getHref().indexOf('#') < 0) return;   /* (none before: none still) */
                    target = stripHash(getHref()) + '#';
                }
                else { u.hash = v; target = u.href; }
                if (target !== getHref()) navigateToFragment(target);
            } });
        /* (the page set elsewhere by script: the tab process has it go there) */
        var leave = function (v) {
            /* (the engine telling the page its own URL is no navigation) */
            if (global.__cyberLocationInit) return false;
            var abs;
            try { abs = new global.URL(String(v), getHref()).href; } catch (e) { return false; }
            return !!global.__cyberTabNavigate && global.__cyberTabNavigate(abs);
        };
        var fragmentOnly = function (u) {
            var target;
            try { target = new global.URL(String(u), getHref()).href; } catch (e) { return null; }
            return stripHash(target) === stripHash(getHref()) && target.indexOf('#') >= 0 ? target : null;
        };
        Object.defineProperty(loc, 'href', { configurable: true, enumerable: true,
            get: hrefDesc.get,
            set: function (v) {
                try { new global.URL(String(v), global.document.baseURI); }
                catch (e) { throw new global.DOMException("'" + v + "' is not a valid URL.", 'SyntaxError'); }
                var t = fragmentOnly(v);
                if (t) navigateToFragment(t);
                else if (!leave(v)) { setHref(String(v)); entries[index] = { url: getHref(), state: null }; }
            } });
        ['assign', 'replace'].forEach(function (k) {
            var orig = loc[k];
            if (typeof orig !== 'function') return;
            define(loc, k, function (v) {
                /* (one that doesn't parse: the document stays) */
                try { new global.URL(String(v), global.document.baseURI); }
                catch (e) { throw new global.DOMException("'" + v + "' is not a valid URL.", 'SyntaxError'); }
                var t = fragmentOnly(v);
                if (t) {
                    if (k === 'replace') { var old = getHref(); setHref(t); entries[index] = { url: t, state: null }; fireHistoryEvents(old, t, null, true); }
                    else navigateToFragment(t);
                    return;
                }
                if (leave(v)) return;
                return orig.call(loc, v);
            });
        });

        /* ---- the Navigation API (HTML 7.2), over this session history; only
         * this document's entries are known ---- */
        var navKeys = 0;
        var entryIds = function (e) {
            if (!e.key) { e.key = 'k' + (++navKeys); e.id = 'i' + (++navKeys); }
            return e;
        };
        function NavigationHistoryEntry() { throw new TypeError('Illegal constructor'); }
        NavigationHistoryEntry.prototype = Object.create(ETP);
        define(NavigationHistoryEntry.prototype, 'constructor', NavigationHistoryEntry);
        var ENTRY = Symbol('cyber.navEntry');
        var entryObject = function (e) {
            entryIds(e);
            if (!e.object) {
                e.object = Object.create(NavigationHistoryEntry.prototype);
                Object.defineProperty(e.object, ENTRY, { value: e });
            }
            return e.object;
        };
        [['key', function (e) { return e.key; }], ['id', function (e) { return e.id; }], ['url', function (e) { return e.url; }],
         ['index', function (e) { return entries.indexOf(e); }], ['sameDocument', function () { return true; }]].forEach(function (p) {
            Object.defineProperty(NavigationHistoryEntry.prototype, p[0], { configurable: true, enumerable: true,
                get: function () { return p[1](this[ENTRY]); } });
        });
        define(NavigationHistoryEntry.prototype, 'getState', function getState() {
            var s = this[ENTRY].navState;
            return s === undefined ? undefined : clone(s);
        });
        function Navigation() { throw new TypeError('Illegal constructor'); }
        Navigation.prototype = Object.create(ETP);
        define(Navigation.prototype, 'constructor', Navigation);
        Object.defineProperty(Navigation.prototype, Symbol.toStringTag, { value: 'Navigation', configurable: true });
        ['navigate', 'navigatesuccess', 'navigateerror', 'currententrychange'].forEach(function (t) {
            var slot = Symbol(t);
            Object.defineProperty(Navigation.prototype, 'on' + t, { configurable: true, enumerable: true,
                get: function () { return this[slot] || null; },
                set: function (f) { this[slot] = typeof f === 'function' ? f : null; } });
        });
        function NavigationDestination() { throw new TypeError('Illegal constructor'); }
        /* (a destination's and a navigate event's state: behind their prototypes' members) */
        var DEST = Symbol('cyber.navDest'), NAVEV = Symbol('cyber.navEvent');
        ['url', 'key', 'id', 'index', 'sameDocument'].forEach(function (k) {
            var g = { [k]: function () { var s = this && this[DEST]; if (!s) throw new TypeError('Illegal invocation'); return s[k]; } }[k];
            Object.defineProperty(g, 'name', { value: 'get ' + k });
            Object.defineProperty(NavigationDestination.prototype, k, { get: g, enumerable: true, configurable: true });
        });
        define(NavigationDestination.prototype, 'getState', function getState() {
            var s = this && this[DEST];
            if (!s) throw new TypeError('Illegal invocation');
            return s.state === undefined ? undefined : clone(s.state);
        });
        Object.defineProperty(NavigationDestination.prototype, Symbol.toStringTag, { value: 'NavigationDestination', configurable: true });
        if (global.NavigateEvent && global.NavigateEvent.prototype) {
            define(global.NavigateEvent.prototype, 'intercept', function intercept(options) {
                var s = this && this[NAVEV];
                if (!(this instanceof global.NavigateEvent)) throw new TypeError('Illegal invocation');
                if (s) s.intercept(options);
            });
            define(global.NavigateEvent.prototype, 'scroll', function scroll() { if (!(this instanceof global.NavigateEvent)) throw new TypeError('Illegal invocation'); });
        }
        var settled = function (d) {
            d.promise.catch(function () {});
            return d;
        };
        var deferred = function () {
            var d = {};
            d.promise = new Promise(function (res, rej) { d.resolve = res; d.reject = rej; });
            return settled(d);
        };
        var navAbort = function () { return new (global.DOMException)('The navigation was aborted', 'AbortError'); };
        var nav = Object.create(Navigation.prototype), navTransition = null;
        /* One navigation: its event, then what it does. commit(): the URL and
         * entry change, in this document; away(): it leaves the document. */
        var navigateSteps = function (type, url, state, info, commit, away, source, formData) {
            var committed = deferred(), finished = deferred();
            var sameDoc = stripHash(url) === stripHash(getHref());
            var hashChange = sameDoc && url !== getHref() && url.indexOf('#') >= 0;
            var dest = Object.create(NavigationDestination.prototype);
            Object.defineProperty(dest, DEST, { value: { url: url, key: '', id: '', index: -1, sameDocument: hashChange, state: state } });
            var handlers = null, precommits = null, controller = new global.AbortController();
            var ev = new global.NavigateEvent('navigate', { cancelable: true, navigationType: type, destination: dest,
                canIntercept: sameOrigin(url, getHref()), userInitiated: false, hashChange: hashChange,
                signal: controller.signal, formData: formData || null, downloadRequest: null, sourceElement: source || null, info: info, hasUAVisualTransition: false });
            Object.defineProperty(ev, NAVEV, { value: { intercept: function (options) {
                handlers = handlers || [];
                precommits = precommits || [];
                if (options && options.handler) handlers.push(options.handler);
                if (options && options.precommitHandler) precommits.push(options.precommitHandler);
            } } });
            var go = dispatch(nav, ev);
            if (!go) {
                var e = navAbort();
                committed.reject(e);
                finished.reject(e);
                return { committed: committed.promise, finished: finished.promise };
            }
            if (handlers) {
                var runAll = function (list) {
                    return Promise.all(list.map(function (h) { try { return h(); } catch (x) { return Promise.reject(x); } }));
                };
                navTransition = { navigationType: type, from: nav.currentEntry, committed: committed.promise, finished: finished.promise };
                var mine = navTransition;
                var failed = function (x) {
                    if (navTransition === mine) navTransition = null;
                    var f = new Event('navigateerror');
                    Object.defineProperty(f, 'error', { value: x, enumerable: true });
                    dispatch(nav, f);
                    committed.reject(x);
                    finished.reject(x);
                };
                /* (the entry changes once the precommit handlers are through) */
                runAll(precommits).then(function () {
                    commit();
                    committed.resolve(nav.currentEntry);
                    return runAll(handlers);
                }).then(function () {
                    if (navTransition === mine) navTransition = null;
                    dispatch(nav, new Event('navigatesuccess'));
                    finished.resolve(nav.currentEntry);
                }, failed);
            } else if (hashChange || type === 'traverse' || url === getHref() && type === 'replace') {
                commit();
                committed.resolve(nav.currentEntry);
                finished.resolve(nav.currentEntry);
            } else {
                away();
            }
            return { committed: committed.promise, finished: finished.promise };
        };
        var entryChanged = function (type, from) {
            var e = new Event('currententrychange');
            Object.defineProperty(e, 'navigationType', { value: type, enumerable: true });
            Object.defineProperty(e, 'from', { value: from, enumerable: true });
            dispatch(nav, e);
        };
        define(Navigation.prototype, 'entries', function entries_() { return entries.map(entryObject); });
        Object.defineProperty(Navigation.prototype.entries, 'name', { value: 'entries' });
        Object.defineProperty(Navigation.prototype, 'currentEntry', { configurable: true, enumerable: true, get: function () { return entryObject(entries[index]); } });
        Object.defineProperty(Navigation.prototype, 'transition', { configurable: true, enumerable: true, get: function () { return navTransition; } });
        Object.defineProperty(Navigation.prototype, 'activation', { configurable: true, enumerable: true, get: function () { return null; } });
        Object.defineProperty(Navigation.prototype, 'canGoBack', { configurable: true, enumerable: true, get: function () { return index > 0; } });
        Object.defineProperty(Navigation.prototype, 'canGoForward', { configurable: true, enumerable: true, get: function () { return index + 1 < entries.length; } });
        var navigateTo = function (type, url, state, info, source, formData) {
            return navigateSteps(type, url, state, info, function () {
                var from = nav.currentEntry, entry = { url: url, state: null, navState: state };
                if (type === 'push') { entries.length = index + 1; entries.push(entry); index++; }
                else entries[index] = entry;
                if (url !== getHref()) setHref(url);
                entryChanged(type, from);
            }, function () {
                if (!formData) loc.href = url;
            }, source, formData);
        };
        define(Navigation.prototype, 'navigate', function navigate(u, options) {
            var o = options || {}, url;
            try { url = new global.URL(String(u), getHref()).href; }
            catch (e) { var d = deferred(), x = new (global.DOMException)('Invalid URL', 'SyntaxError'); d.reject(x); return { committed: d.promise, finished: d.promise }; }
            var type = o.history === 'replace' ? 'replace' : 'push', state = o.state === undefined ? undefined : clone(o.state);
            return navigateTo(type, url, state, o.info);
        });
        /* A link followed or a form submitted, from its element. (A POST the
         * page doesn't take over has nowhere to go: loading is by URL here.) */
        Object.defineProperty(global, '__cyberNavigate', { configurable: true, value: function (url, source, formData) {
            navigateTo('push', url, undefined, undefined, source, formData);
        } });
        define(Navigation.prototype, 'reload', function reload(options) {
            var o = options || {}, url = getHref();
            return navigateSteps('reload', url, o.state, o.info, function () {}, function () { loc.reload(); });
        });
        var traverseBy = function (delta, info) {
            var to = index + delta, d = deferred();
            if (to < 0 || to >= entries.length) {
                var x = new (global.DOMException)('Cannot traverse there', 'InvalidStateError');
                d.reject(x);
                return { committed: d.promise, finished: d.promise };
            }
            return navigateSteps('traverse', entries[to].url, entries[to].navState, info, function () {
                var from = nav.currentEntry;
                traverse(delta);
                entryChanged('traverse', from);
            }, function () {});
        };
        define(Navigation.prototype, 'back', function back(o) { return traverseBy(-1, o && o.info); });
        define(Navigation.prototype, 'forward', function forward(o) { return traverseBy(1, o && o.info); });
        define(Navigation.prototype, 'traverseTo', function traverseTo(key, o) {
            for (var i = 0; i < entries.length; i++) if (entryIds(entries[i]).key === String(key)) return traverseBy(i - index, o && o.info);
            var d = deferred(), x = new (global.DOMException)('No such entry', 'InvalidStateError');
            d.reject(x);
            return { committed: d.promise, finished: d.promise };
        });
        define(Navigation.prototype, 'updateCurrentEntry', function updateCurrentEntry(o) {
            if (!o || !('state' in o)) throw new TypeError("Failed to execute 'updateCurrentEntry': required member state is undefined.");
            var from = nav.currentEntry;
            entries[index].navState = clone(o.state);
            entryChanged(null, from);
        });
        /* (Navigation's operations: this a Navigation) */
        ['navigate', 'reload', 'traverseTo', 'back', 'forward', 'entries', 'updateCurrentEntry'].forEach(function (k) {
            var f = Navigation.prototype[k];
            if (typeof f !== 'function') return;
            var w = { [k]: function () { if (!(this instanceof Navigation)) throw new TypeError('Illegal invocation'); return f.apply(this, arguments); } }[k];
            Object.defineProperty(w, 'length', { value: f.length });
            define(Navigation.prototype, k, w);
        });
        /* (NavigationHistoryEntry's ondispose) */
        (function () {
            var slot = Symbol('ondispose');
            Object.defineProperty(NavigationHistoryEntry.prototype, 'ondispose', { configurable: true, enumerable: true,
                get: function () { if (!(this instanceof NavigationHistoryEntry)) throw new TypeError('Illegal invocation'); return this[slot] || null; },
                set: function (f) { if (!(this instanceof NavigationHistoryEntry)) throw new TypeError('Illegal invocation'); this[slot] = typeof f === 'function' ? f : null; } });
        })();
        /* (NavigationTransition, NavigationActivation, NavigationPrecommitController: their interfaces) */
        var plainIface = function (name, attrs, ops) {
            var C = function () { throw new TypeError('Illegal constructor'); };
            Object.defineProperty(C, 'name', { value: name });
            var K = Symbol(name);
            attrs.forEach(function (k) {
                var g = { [k]: function () { var s = this && this[K]; if (!s) throw new TypeError('Illegal invocation'); return s[k]; } }[k];
                Object.defineProperty(g, 'name', { value: 'get ' + k });
                Object.defineProperty(C.prototype, k, { get: g, enumerable: true, configurable: true });
            });
            (ops || []).forEach(function (o) {
                var m = { [o[0]]: function () {
                    if (!this || !this[K]) throw new TypeError('Illegal invocation');
                    if (arguments.length < o[1]) throw new TypeError("Failed to execute '" + o[0] + "' on '" + name + "': " + o[1] + ' argument required, but only ' + arguments.length + ' present.');
                } }[o[0]];
                Object.defineProperty(m, 'length', { value: o[1] });
                define(C.prototype, o[0], m);
            });
            Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
            expose(name, C);
            return C;
        };
        plainIface('NavigationTransition', ['navigationType', 'from', 'to', 'committed', 'finished']);
        plainIface('NavigationActivation', ['from', 'entry', 'navigationType']);
        plainIface('NavigationPrecommitController', [], [['redirect', 1], ['addHandler', 1]]);
        if (typeof global.NavigationCurrentEntryChangeEvent !== 'function')
            eventClass('NavigationCurrentEntryChangeEvent', Event, { navigationType: nul(), from: nul() });
        expose('Navigation', Navigation);
        expose('NavigationHistoryEntry', NavigationHistoryEntry);
        expose('NavigationDestination', NavigationDestination);
        Object.defineProperty(global, 'navigation', { configurable: true, enumerable: true, get: function () { return nav; } });
    }

    /* ---- AbortController / AbortSignal (DOM 3.1-3.2) ---- */
    var ABORT = Symbol('cyber.abortSignal'), SIGNAL = Symbol('cyber.abortController');
    function abortState(s) {
        var a = s != null ? s[ABORT] : undefined;
        if (!a) throw new TypeError('Illegal invocation');
        return a;
    }
    function AbortSignal() { throw new TypeError("Failed to construct 'AbortSignal': Illegal constructor"); }
    AbortSignal.prototype = Object.create(ETP);
    define(AbortSignal.prototype, 'constructor', AbortSignal);
    Object.defineProperty(AbortSignal.prototype, Symbol.toStringTag, { value: 'AbortSignal', configurable: true });
    function newSignal() {
        var s = Object.create(AbortSignal.prototype);
        Object.defineProperty(s, ABORT, { value: { aborted: false, reason: undefined, algorithms: [],
            dependent: false, sources: [], dependents: [] } });
        return s;
    }
    function abortReason(reason) {
        return reason === undefined ? DOMExceptionOr('signal is aborted without reason', 'AbortError') : reason;
    }
    function runAbortSteps(s) {
        var a = s[ABORT], algs = a.algorithms;
        a.algorithms = [];
        algs.forEach(function (f) { try { f(); } catch (e) { report(e); } });
        var ev = new Event('abort');
        st(ev).isTrusted = true;
        dispatch(s, ev);
    }
    /* "signal abort": the signal, then its dependents, all aborted before any event */
    function signalAbort(s, reason) {
        var a = s[ABORT];
        if (a.aborted) return;
        a.aborted = true;
        a.reason = abortReason(reason);
        var deps = [];
        a.dependents.forEach(function (d) {
            var da = d[ABORT];
            if (da.aborted) return;
            da.aborted = true;
            da.reason = a.reason;
            deps.push(d);
        });
        runAbortSteps(s);
        deps.forEach(runAbortSteps);
    }
    getter(AbortSignal.prototype, 'aborted', function () { return abortState(this).aborted; });
    getter(AbortSignal.prototype, 'reason', function () { return abortState(this).reason; });
    define(AbortSignal.prototype, 'throwIfAborted', function throwIfAborted() {
        var a = abortState(this);
        if (a.aborted) throw a.reason;
    });
    handlerAccessor(AbortSignal.prototype, 'abort');
    (function () {
        /* (onabort: an AbortSignal's only) */
        var d = Object.getOwnPropertyDescriptor(AbortSignal.prototype, 'onabort');
        if (d && d.get) {
            var g = d.get, sset = d.set;
            Object.defineProperty(AbortSignal.prototype, 'onabort', { configurable: true, enumerable: true,
                get: function () { abortState(this); return g.call(this); },
                set: function (v) { abortState(this); return sset.call(this, v); } });
        }
    })();
    Object.setPrototypeOf(AbortSignal, EventTarget);
    define(AbortSignal, 'abort', function abort(reason) {
        var s = newSignal(), a = s[ABORT];
        a.aborted = true;
        a.reason = abortReason(reason);
        return s;
    });
    define(AbortSignal, 'timeout', function timeout(ms) {
        ms = Number(ms);
        if (!isFinite(ms) || ms < 0 || ms > Number.MAX_SAFE_INTEGER)
            throw new TypeError("Failed to execute 'timeout' on 'AbortSignal': Value is outside the 'unsigned long long' value range.");
        var s = newSignal();
        global.setTimeout(function () {
            signalAbort(s, DOMExceptionOr('signal timed out', 'TimeoutError'));
        }, Math.floor(ms));
        return s;
    });
    define(AbortSignal, 'any', function any(signals) {
        var list = Array.from(signals, function (s) { abortState(s); return s; });
        var r = newSignal(), ra = r[ABORT];
        for (var i = 0; i < list.length; i++) {
            if (list[i][ABORT].aborted) { ra.aborted = true; ra.reason = list[i][ABORT].reason; return r; }
        }
        ra.dependent = true;
        function follow(src) {
            if (ra.sources.indexOf(src) >= 0) return;
            ra.sources.push(src);
            src[ABORT].dependents.push(r);
        }
        list.forEach(function (s) {
            var sa = s[ABORT];
            if (!sa.dependent) follow(s);
            else sa.sources.forEach(follow);
        });
        return r;
    });
    expose('AbortSignal', AbortSignal);

    function AbortController() {
        if (!new.target) throw new TypeError("Failed to construct 'AbortController': Please use the 'new' operator");
        Object.defineProperty(this, SIGNAL, { value: newSignal() });
    }
    function controllerSignal(c) {
        var s = c != null ? c[SIGNAL] : undefined;
        if (!s) throw new TypeError('Illegal invocation');
        return s;
    }
    getter(AbortController.prototype, 'signal', function () { return controllerSignal(this); });
    define(AbortController.prototype, 'abort', function abort(reason) { signalAbort(controllerSignal(this), reason); });
    Object.defineProperty(AbortController.prototype, Symbol.toStringTag, { value: 'AbortController', configurable: true });
    expose('AbortController', AbortController);

    /* ---- input: pointer, wheel and keys as the browser gets them (the tab's
     * wheel and clicks, and testdriver in the WPT runs) ---- */
    var pointer = { over: null, pressed: null, buttons: 0, bar: null, touch: null, touchFrame: null, wheelTarget: null, wheelUntil: 0, wheelAt: 0 };
    /* (wheel events go on to the first one's target while they keep coming and
     * it stays where it was) */
    var WHEEL_TRANSACTION_MS = 500;
    function docTop(t) {
        if (global.getComputedStyle(t).display === 'contents') return NaN;
        var rects = t.getClientRects();
        return rects.length ? rects[0].top + global.scrollY : NaN;
    }
    /* A frame at a point: its document takes the input, at its coordinates. */
    function frameInput(t) {
        if (!t || !/^i?frame$/.test(t.localName || '') || !global.__cyberInputInto(t)) return null;
        var input = {};
        ['move', 'down', 'up', 'wheel', 'touchDown', 'touchMove', 'touchUp'].forEach(function (k) {
            input[k] = function (a, b, c, d) { global.__cyberInputInto(t, k, a, b, c === undefined ? 0 : c, d === undefined ? 0 : d); };
        });
        var r = t.getBoundingClientRect();
        return { input: input, left: r.left + t.clientLeft, top: r.top + t.clientTop };
    }
    /* The scrollbar of a box at a point: which way, and how far along. */
    function scrollbarAt(t, x, y) {
        for (var a = t; a; a = flatParent(a)) {
            if (a.nodeType !== 1 || a === a.ownerDocument.documentElement || a === a.ownerDocument.body) continue;
            var r = a.getBoundingClientRect(), cs = global.getComputedStyle(a);
            var left = r.left + a.clientLeft, top = r.top + a.clientTop;
            if (!/auto|scroll/.test(cs.overflow || cs.overflowY || '')) continue;
            /* (from its padding box's edge out: scrollbars take no room here,
             * so a box's scrollbar is at most its border) */
            var vbar = a.offsetWidth - a.clientWidth - a.clientLeft;
            var hbar = a.offsetHeight - a.clientHeight - a.clientTop;
            if (vbar > 0 && x >= left + a.clientWidth && x < left + a.clientWidth + vbar && y >= top && y < top + a.clientHeight)
                return { el: a, prop: 'scrollTop', at: y - top, track: a.clientHeight, size: a.scrollHeight, last: y };
            if (hbar > 0 && y >= top + a.clientHeight && y < top + a.clientHeight + hbar && x >= left && x < left + a.clientWidth)
                return { el: a, prop: 'scrollLeft', at: x - left, track: a.clientWidth, size: a.scrollWidth, last: x };
        }
        /* (the viewport's take no room either: just past its edges) */
        var root = t && t.ownerDocument && t.ownerDocument.scrollingElement;
        if (root && x >= global.innerWidth && y < global.innerHeight)
            return { el: root, prop: 'scrollTop', at: y, track: global.innerHeight, size: root.scrollHeight, last: y };
        if (root && y >= global.innerHeight && x < global.innerWidth)
            return { el: root, prop: 'scrollLeft', at: x, track: global.innerWidth, size: root.scrollWidth, last: x };
        return null;
    }
    /* A press on a scrollbar: on the thumb it drags; beside it, a page. */
    function barPress(bar) {
        if (bar.size <= bar.track) return;
        var from = bar.el[bar.prop] * bar.track / bar.size, len = bar.track * bar.track / bar.size;
        if (bar.at < from || bar.at >= from + len) {
            bar.el[bar.prop] += (bar.at < from ? -1 : 1) * bar.track * 0.875;
            return;
        }
        pointer.bar = bar;
        global.__cyberScrollHold(true);
    }
    /* A touch event at the finger's target: cancelable if a listener isn't passive. */
    function touchEvent(t, type, x, y, ended) {
        var touch = { identifier: 0, target: t, clientX: x, clientY: y, pageX: x + global.scrollX, pageY: y + global.scrollY,
                      screenX: x, screenY: y, radiusX: 1, radiusY: 1, rotationAngle: 0, force: 1 };
        return trusted(t, global.TouchEvent, type, { cancelable: listenerBlocks(t, [type]), view: global,
            touches: ended ? [] : [touch], targetTouches: ended ? [] : [touch], changedTouches: [touch] });
    }
    function touchInit(x, y, button) {
        var i = mouseInit(x, y, button);
        i.pointerId = 2; i.pointerType = 'touch'; i.isPrimary = true; i.pressure = 0.5;
        return i;
    }
    function trusted(target, Ctor, type, init) {
        init.bubbles = init.bubbles !== false;
        init.composed = init.composed !== false;
        return global.__cyberDispatchTrusted(target, new Ctor(type, init));
    }
    function hitAt(x, y) {
        var d = global.document;
        return d ? d.elementFromPoint(x, y) || d.documentElement : null;
    }
    function flatParent(n) { return n.assignedSlot || n.parentNode || n.host || null; }
    /* (a disabled form control gets no mouse events, HTML 4.10.18.5) */
    function controlDisabled(el) {
        return /^(button|input|select|textarea|optgroup|option|fieldset)$/.test(el.localName || '') && el.matches(':disabled');
    }
    function mouseInit(x, y, button, related) {
        return { cancelable: true, clientX: x, clientY: y, screenX: x, screenY: y, button: button,
                 buttons: pointer.buttons, view: global, relatedTarget: related || null };
    }
    function pointerInit(x, y, button, related) {
        var i = mouseInit(x, y, button, related);
        i.pointerId = 1; i.pointerType = 'mouse'; i.isPrimary = true; i.pressure = pointer.buttons ? 0.5 : 0;
        return i;
    }
    /* (in a disabled form control, its shadow tree or children included: no mousedown, mouseup or click) */
    function inDisabledControl(t) {
        for (var e = t, n = 0; e && n < 1000; n++) {
            if (e.nodeType === 1 && controlDisabled(e)) return true;
            e = e.parentNode || (e.host && e.nodeType === 11 ? e.host : null);
        }
        return false;
    }
    function bothEvents(t, x, y, button, ptype, mtype, related) {
        trusted(t, global.PointerEvent, ptype, pointerInit(x, y, button, related));
        if ((mtype === 'mousedown' || mtype === 'mouseup') && inDisabledControl(t)) return true;
        return trusted(t, MouseEvent, mtype, mouseInit(x, y, button, related));
    }
    /* pointerenter/leave, mouseenter/leave: at each element, not bubbling */
    function boundary(el, x, y, ptype, mtype, related) {
        if (el.nodeType !== 1) return;
        var pi = pointerInit(x, y, 0, related), mi = mouseInit(x, y, 0, related);
        pi.bubbles = mi.bubbles = false;
        pi.cancelable = mi.cancelable = false;
        trusted(el, global.PointerEvent, ptype, pi);
        trusted(el, MouseEvent, mtype, mi);
    }
    /* :hover and :active: the element and what it is in */
    function chainState(el, bit, on) {
        for (var a = el; a; a = flatParent(a)) if (a.nodeType === 1) global.__cyberElState(a, bit, on);
    }
    function focusTarget(t, canceled) {
        if (canceled) return;
        for (var a = t; a; a = flatParent(a)) {
            /* (a click focuses tabindex=-1 too: only Tab skips it) */
            if (a.nodeType === 1 && (a.tabIndex >= 0 || a.hasAttribute('tabindex') || a.isContentEditable)) {
                if (!controlDisabled(a) && typeof a.focus === 'function') a.focus();
                return;
            }
        }
        var ae = global.document && global.document.activeElement;
        if (ae && typeof ae.blur === 'function') ae.blur();
    }
    function commonAncestor(a, b) {
        for (var x = a; x; x = flatParent(x)) {
            for (var y = b; y; y = flatParent(y)) if (x === y) return x;
        }
        return null;
    }
    /* The wheel's event may be cancelled only when a listener for it isn't
     * passive (or there's a handler). */
    function wheelBlocked(t) { return listenerBlocks(t, ['wheel', 'mousewheel']); }
    function listenerBlocks(t, types) {
        var path = [];
        for (var a = t; a; a = flatParent(a)) path.push(a);
        path.push(global);
        return path.some(function (o) {
            var map = o[LISTENERS];
            return types.some(function (type) {
                if (o['on' + type]) return true;
                return map && map[type] && map[type].some(function (l) { return !l.removed && !l.passive; });
            });
        });
    }
    /* What the wheel moves: on each axis, the nearest box the user can scroll
     * that has room, going out to the viewport (overscroll-behavior stops it). */
    function wheelScroll(t, dx, dy, keyed) {
        /* (the user's scroll snaps as it ends, not step by step) */
        if (global.__cyberSnapHold) global.__cyberSnapHold(1);
        try { return wheelScrollNow(t, dx, dy, keyed); }
        finally { if (global.__cyberSnapHold) global.__cyberSnapHold(-1); }
    }
    function wheelScrollNow(t, dx, dy, keyed) {
        var aim = function (el) {
            if (keyed && global.__cyberSnapIntent)
                global.__cyberSnapIntent(el, el.scrollLeft, el.scrollTop, Math.sign(dx), Math.sign(dy));
        };
        for (var a = t; a && (dx || dy); a = flatParent(a)) {
            if (a.nodeType !== 1 || a === a.ownerDocument.documentElement) continue;
            /* (the viewport has a body's overflow when the root has none) */
            if (a === a.ownerDocument.body && global.getComputedStyle(a.parentNode).overflowY === 'visible') continue;
            var cs = global.getComputedStyle(a), byX = /auto|scroll/.test(cs.overflowX), byY = /auto|scroll/.test(cs.overflowY);
            if (dy && byY) {
                if (dy > 0 ? a.scrollTop < a.scrollHeight - a.clientHeight : a.scrollTop > 0) { aim(a); a.scrollTop += dy; dy = 0; }
                else if (/contain|none/.test(cs.overscrollBehaviorY)) dy = 0;
            }
            if (dx && byX) {
                if (dx > 0 ? a.scrollLeft < a.scrollWidth - a.clientWidth : a.scrollLeft > 0) { aim(a); a.scrollLeft += dx; dx = 0; }
                else if (/contain|none/.test(cs.overscrollBehaviorX)) dx = 0;
            }
        }
        if (!dx && !dy) return;
        var was = [global.scrollX, global.scrollY];
        if (global.document.documentElement) aim(global.document.documentElement);
        global.scrollBy(dx, dy);
        /* (nothing left to scroll here: the frame's container) */
        if (global.scrollX === was[0] && global.scrollY === was[1] && global.frameElement) {
            var outer = global.parent && global.parent.__cyberInput;
            if (outer) outer.chain(global.frameElement, dx, dy);
        }
    }
    /* (HTML: "scroll to the fragment") */
    function scrollToFragment(view, hash) {
        var name = String(hash || '').slice(1), d = view.document, el = null;
        try { name = decodeURIComponent(name); } catch (e) { /* (as it is) */ }
        if (name) {
            el = d.getElementById(name);
            if (!el) {
                var named = d.getElementsByName ? d.getElementsByName(name) : [];
                for (var i = 0; i < named.length && !el; i++) if (named[i].localName === 'a') el = named[i];
            }
        }
        if (el) {
            /* (its scroll-snap-align, where it has one: how it is aligned) */
            var blk = 'start', inl = 'nearest';
            try {
                var sa = String(view.getComputedStyle(el).scrollSnapAlign || '').trim().split(/\s+/);
                if (/^(start|end|center)$/.test(sa[0])) blk = sa[0];
                var si = sa.length > 1 ? sa[1] : sa[0];
                if (/^(start|end|center)$/.test(si)) inl = si;
            } catch (e) { /* (as start) */ }
            el.scrollIntoView({ block: blk, inline: inl });
        }
        else if (!name || name.toLowerCase() === 'top') view.scrollTo(0, 0);
    }
    var KEYS = { '\uE003': 'Backspace', '\uE004': 'Tab', '\uE006': 'Enter', '\uE007': 'Enter', '\uE008': 'Shift', '\uE009': 'Control',
                 '\uE00A': 'Alt', '\uE00C': 'Escape', '\uE00D': ' ', '\uE00E': 'PageUp', '\uE00F': 'PageDown', '\uE010': 'End',
                 '\uE011': 'Home', '\uE012': 'ArrowLeft', '\uE013': 'ArrowUp', '\uE014': 'ArrowRight', '\uE015': 'ArrowDown',
                 '\uE017': 'Delete', '\uE03D': 'Meta' };
    function keyOf(k) { return KEYS[k] || k; }
    /* (the modifier keys held down) */
    var held = { Shift: false, Control: false, Alt: false, Meta: false };
    /* Tab: the next element in the sequential focus order (positive tabindex
     * first, then tree order), round to the first; Shift+Tab the one before.
     * A focusgroup's items, a run of them in that order, are one stop: the one
     * focused last there (unless it has no memory), else its focusgroupstart one, else its first. */
    var FG_MEM = new WeakMap();
    function focusNext(back) {
        var d = global.document, all = d.querySelectorAll('*'), list = [];
        for (var i = 0; i < all.length; i++) {
            var e = all[i];
            if (typeof e.tabIndex === 'number' && e.tabIndex >= 0 && !controlDisabled(e) && e.getClientRects().length) list.push(e);
        }
        list = list.filter(function (e) { return e.tabIndex > 0; }).sort(function (a, b) { return a.tabIndex - b.tabIndex; })
            .concat(list.filter(function (e) { return e.tabIndex === 0; }));
        if (!list.length) return;
        /* (the stops: each a run of one focusgroup's items, or an element alone; a native directional input handler, or what is in the top layer, alone) */
        var stops = [], cur = d.activeElement;
        for (var j = 0; j < list.length; j++) {
            var el = list[j], tk = fgTokens(el), owner = tk && tk[0] === 'none' ? null : fgScope(el);
            if (owner && (fgDirectional(el) || fgInTopLayer(el, owner))) owner = null;
            var last = stops[stops.length - 1];
            if (owner && last && last.owner === owner) last.items.push(el);
            else stops.push({ owner: owner, items: [el] });
        }
        var at = -1;
        for (var q = 0; q < stops.length && at < 0; q++) if (stops[q].items.indexOf(cur) >= 0) at = q;
        var n = stops.length, to;
        if (at < 0 && cur && cur !== d.body && cur.compareDocumentPosition) {
            /* (focused but not a stop (tabindex -1): the next stop after it in the tree, or the one before; not its own focusgroup's) */
            var curOwner = fgScope(cur);
            to = -1;
            if (!back) { for (var q2 = 0; q2 < n && to < 0; q2++) if ((cur.compareDocumentPosition(stops[q2].items[0]) & 4) && !(curOwner && stops[q2].owner === curOwner)) to = q2; }
            else { for (var q3 = n - 1; q3 >= 0 && to < 0; q3--) if ((cur.compareDocumentPosition(stops[q3].items[0]) & 2) && !(curOwner && stops[q3].owner === curOwner)) to = q3; }
            if (to < 0) to = back ? n - 1 : 0;
        } else to = at < 0 ? (back ? n - 1 : 0) : (at + (back ? -1 : 1) + n) % n;
        var st = stops[to], pick = st.items[0];
        if (st.owner) {
            var otk = fgTokens(st.owner) || [], mem = otk.indexOf('nomemory') < 0 ? FG_MEM.get(st.owner) : null;
            var start = null;
            for (var r = 0; r < st.items.length && !start; r++) if (st.items[r].hasAttribute('focusgroupstart')) start = st.items[r];
            pick = mem && st.items.indexOf(mem) >= 0 ? mem : start || st.items[0];
        }
        pick.focus();
    }
    /* (an element that takes the arrow keys itself: a text field, a select, an editing host, media with controls) */
    function fgDirectional(e) {
        var ln = e.localName;
        if (ln === 'textarea' || ln === 'select') return true;
        if (ln === 'input') return !/^(button|submit|reset|image|checkbox|radio|color|file|hidden)$/.test(String(e.getAttribute('type') || 'text').toLowerCase());
        if ((ln === 'video' || ln === 'audio') && e.hasAttribute('controls')) return true;
        return !!e.isContentEditable;
    }
    /* (inside a showing popover or modal dialog that is inside the focusgroup: not its item) */
    function fgInTopLayer(e, owner) {
        for (var p = e; p && p !== owner; p = flatParent(p)) {
            if (p.nodeType !== 1) continue;
            try { if (p.matches(':popover-open') || p.matches(':modal')) return true; } catch (x) { }
        }
        return false;
    }
    /* (a focusgroup's item focused: what it remembers) */
    if (global.document && global.document.addEventListener) global.document.addEventListener('focusin', function (ev) {
        var t = ev.target;
        if (!t || t.nodeType !== 1) return;
        var tk = fgTokens(t);
        if (tk && tk[0] === 'none') return;
        var o = fgScope(t);
        if (o) FG_MEM.set(o, t);
    }, true);
    /* ---- scoped focusgroup (Open UI): arrow keys, Home and End move focus among a focusgroup's items ---- */
    /* (its tokens, when the first names a behavior: else no focusgroup at all) */
    var FG_BEHAVIORS = { toolbar: 1, tablist: 1, radiogroup: 1, listbox: 1, menu: 1, menubar: 1, grid: 1, feed: 1, none: 1 };
    function fgTokens(e) {
        var v = e.getAttribute && e.getAttribute('focusgroup');
        if (v === null || v === undefined) return null;
        var t = String(v).toLowerCase().trim().split(/\s+/).filter(Boolean);
        /* (the first behavior token, wherever it is: first in the list handed back) */
        for (var i = 0; i < t.length; i++) if (FG_BEHAVIORS[t[i]]) return [t[i]].concat(t.slice(0, i), t.slice(i + 1));
        return null;
    }
    /* (the focusgroup an element is an item of: its nearest ancestor with the attribute, unless that opts out) */
    function fgScope(e) {
        for (var p = e && flatParent(e); p && p.nodeType === 1; p = flatParent(p)) {
            var t = fgTokens(p);
            if (!t) continue;
            return t[0] === 'none' ? null : p;
        }
        return null;
    }
    function fgFocusable(e) {
        if (e.nodeType !== 1 || typeof e.tabIndex !== 'number') return false;
        if (typeof global.__cyberFocusableArea === 'function') { try { if (!global.__cyberFocusableArea(e)) return false; } catch (x) { return false; } }
        else if (e.tabIndex < 0 && !e.hasAttribute('tabindex')) return false;
        if (controlDisabled(e)) return false;
        return e.getClientRects().length > 0;
    }
    function fgItems(owner) {
        var out = [];
        (function walk(n) {
            for (var c = n.firstChild; c; c = c.nextSibling) {
                if (c.nodeType !== 1) continue;
                var t = fgTokens(c);
                if (fgScope(c) === owner && fgFocusable(c) && !(t && t[0] === 'none') && !fgInTopLayer(c, owner)) out.push(c);
                /* (a nested focusgroup's or an opted-out subtree's elements are not this one's) */
                if (t) continue;
                walk(c);
                if (c.shadowRoot) walk(c.shadowRoot);
            }
        })(owner);
        if (owner.shadowRoot) (function walkS(n) {
            for (var c = n.firstChild; c; c = c.nextSibling) {
                if (c.nodeType !== 1) continue;
                if (fgScope(c) === owner && fgFocusable(c) && out.indexOf(c) < 0) out.push(c);
                if (!fgTokens(c)) walkS(c);
            }
        })(owner.shadowRoot);
        return out;
    }
    /* (a grid focusgroup: a table's (or CSS table's) cells by row and column, spans taken in; past an edge, the wrap or
     * flow its tokens say, each axis apart) */
    function fgGridMap(owner) {
        var rows = [];
        var disp = function (e) { try { return global.getComputedStyle(e).display; } catch (x) { return ''; } };
        if (owner.localName === 'table' && owner.rows) {
            for (var i = 0; i < owner.rows.length; i++) rows.push(Array.prototype.filter.call(owner.rows[i].cells, function () { return true; }));
        } else if (disp(owner) === 'table' || disp(owner) === 'inline-table') {
            (function walk(n) {
                for (var c = n.firstElementChild; c; c = c.nextElementSibling) {
                    var d = disp(c);
                    if (d === 'table-row') rows.push(Array.prototype.filter.call(c.children, function (x) { return disp(x) === 'table-cell'; }));
                    else if (/^table-(row|header|footer)-group$/.test(d)) walk(c);
                }
            })(owner);
        } else return null;
        var grid = [], pos = new Map();
        for (var r = 0; r < rows.length; r++) {
            grid[r] = grid[r] || [];
            var col = 0;
            for (var k = 0; k < rows[r].length; k++) {
                var cell = rows[r][k];
                while (grid[r][col]) col++;
                var cs = Math.max(1, parseInt(cell.getAttribute && cell.getAttribute('colspan'), 10) || 1);
                var rs = Math.max(1, parseInt(cell.getAttribute && cell.getAttribute('rowspan'), 10) || 1);
                rs = Math.min(rs, rows.length - r);
                for (var a = 0; a < rs; a++) { grid[r + a] = grid[r + a] || []; for (var b = 0; b < cs; b++) grid[r + a][col + b] = cell; }
                pos.set(cell, { r: r, c: col, rs: rs, cs: cs });
                col += cs;
            }
        }
        var cols = 0;
        for (var g = 0; g < grid.length; g++) cols = Math.max(cols, grid[g].length);
        return { grid: grid, pos: pos, rows: grid.length, cols: cols };
    }
    function fgGridMove(key, el, owner, tk) {
        if (!/^Arrow/.test(key)) return false;
        var m = fgGridMap(owner);
        if (!m) return false;
        var cell = el;
        while (cell && !m.pos.has(cell) && cell !== owner) cell = flatParent(cell);
        if (!cell || !m.pos.has(cell)) return false;
        var p = m.pos.get(cell);
        var has = function (t) { return tk.indexOf(t) >= 0; };
        var rowWrap = has('wrap') || has('rowwrap'), colWrap = has('wrap') || has('colwrap');
        var rowFlow = has('flow') || has('rowflow'), colFlow = has('flow') || has('colflow');
        /* (conflicts: wrap and flow on one axis, or nowrap with either: a hard edge) */
        if (rowWrap && rowFlow) rowWrap = rowFlow = false;
        if (colWrap && colFlow) colWrap = colFlow = false;
        if (has('nowrap')) { rowWrap = colWrap = rowFlow = colFlow = false; }
        if (has('noflow')) { rowFlow = colFlow = false; }
        var horiz = key === 'ArrowRight' || key === 'ArrowLeft', dir = key === 'ArrowRight' || key === 'ArrowDown' ? 1 : -1;
        var rtl = false;
        try { rtl = global.getComputedStyle(owner).direction === 'rtl'; } catch (x) { }
        if (horiz && rtl) dir = -dir;
        var r = p.r, c = p.c;
        if (horiz) c = dir > 0 ? p.c + p.cs : p.c - 1; else r = dir > 0 ? p.r + p.rs : p.r - 1;
        for (var guard = 0; guard < (m.rows + 1) * (m.cols + 1) * 2; guard++) {
            if (horiz && (c < 0 || c >= m.cols)) {
                if (rowFlow) { r = (r + dir + m.rows) % m.rows; c = dir > 0 ? 0 : m.cols - 1; }
                else if (rowWrap) c = dir > 0 ? 0 : m.cols - 1;
                else return true;
            }
            if (!horiz && (r < 0 || r >= m.rows)) {
                if (colFlow) { c = (c + dir + m.cols) % m.cols; r = dir > 0 ? 0 : m.rows - 1; }
                else if (colWrap) r = dir > 0 ? 0 : m.rows - 1;
                else return true;
            }
            var x = m.grid[r] && m.grid[r][c];
            if (x === cell && guard > 0 && !rowFlow && !colFlow) return true;
            if (x && x !== cell && fgFocusable(x)) { global.__cyberLastInput = 'key'; x.focus(); return true; }
            if (horiz) c += dir; else r += dir;
        }
        return true;
    }
    function focusgroupMove(key, el) {
        if (!el || el.nodeType !== 1) return false;
        var own = fgTokens(el);
        if (own && own[0] === 'none') return false;
        /* (it takes the arrows itself) */
        if (fgDirectional(el) && /^(Arrow|Home|End)/.test(key)) return false;
        var owner = fgScope(el);
        if (!owner) return false;
        var tk = fgTokens(owner) || [];
        /* (a grid's own two-dimensional navigation) */
        if (tk[0] === 'grid') return fgGridMove(key, el, owner, tk);
        var inl = tk.indexOf('inline') >= 0, blk = tk.indexOf('block') >= 0;
        if (!inl && !blk) inl = blk = true;
        var wrap = tk.indexOf('wrap') >= 0 && tk.indexOf('nowrap') < 0;
        /* (a feed: the block axis, hard edges) */
        if (tk[0] === 'feed') { inl = false; blk = true; wrap = false; }
        /* (a behavior's defaults: its axis, and wrapping, unless the tokens say otherwise) */
        var DEF = { tablist: ['i', 1], menubar: ['i', 1], menu: ['b', 1], radiogroup: ['ib', 1], listbox: ['b', 0] }[tk[0]];
        if (DEF) {
            if (tk.indexOf('inline') < 0 && tk.indexOf('block') < 0) { inl = DEF[0].indexOf('i') >= 0; blk = DEF[0].indexOf('b') >= 0; }
            if (DEF[1] && tk.indexOf('nowrap') < 0) wrap = true;
        }
        /* (tabindex -1 items: not moved to, only from) */
        var items = fgItems(owner).filter(function (x) { return x === el || x.tabIndex >= 0; }), at = items.indexOf(el);
        if (at < 0) return false;
        var rtl = false;
        try { rtl = global.getComputedStyle(owner).direction === 'rtl'; } catch (x) { }
        var to = -2;
        if (key === 'Home') to = 0;
        else if (key === 'End') to = items.length - 1;
        else {
            var step = 0;
            if (key === 'ArrowRight' && inl) step = rtl ? -1 : 1;
            else if (key === 'ArrowLeft' && inl) step = rtl ? 1 : -1;
            else if (key === 'ArrowDown' && blk) step = 1;
            else if (key === 'ArrowUp' && blk) step = -1;
            if (!step) return /^Arrow/.test(key) && false;
            to = at + step;
            if (to < 0 || to >= items.length) to = wrap ? (to + items.length) % items.length : -2;
        }
        if (to >= 0 && to < items.length && items[to] !== el) { global.__cyberLastInput = 'key'; items[to].focus(); }
        return true;
    }
    function keyDefault(key, el) {
        var d = global.document, h = global.innerHeight, dx = 0, dy = 0;
        /* (in a focusgroup: its navigation, not scrolling) */
        if (/^(Arrow(Up|Down|Left|Right)|Home|End)$/.test(key) && focusgroupMove(key, el)) return;
        switch (key) {
        case 'Tab': global.__cyberLastInput = 'key'; focusNext(held.Shift); return;
        case 'ArrowDown': dy = 40; break;
        case 'ArrowUp': dy = -40; break;
        case 'ArrowRight': dx = 40; break;
        case 'ArrowLeft': dx = -40; break;
        case 'PageDown': case ' ': dy = h * 0.9; break;
        case 'PageUp': dy = -h * 0.9; break;
        case 'End': dy = 1e8; break;
        case 'Home': dy = -1e8; break;
        default:
            /* (typed into a text control) */
            if (key.length === 1 && el && /^(input|textarea)$/.test(el.localName) && !el.disabled && !el.readOnly) {
                /* (beforeinput first, which can stop it; then the edit and input; change when it loses focus) */
                if (!trusted(el, global.InputEvent || Event, 'beforeinput', { cancelable: true, data: key, inputType: 'insertText' })) return;
                el.value += key;
                trusted(el, global.InputEvent || Event, 'input', { cancelable: false, data: key, inputType: 'insertText' });
                if (!el.__cyberTypedChange) {
                    Object.defineProperty(el, '__cyberTypedChange', { value: true, configurable: true, writable: true });
                    var onBlur = function () {
                        el.removeEventListener('blur', onBlur, true);
                        el.__cyberTypedChange = false;
                        trusted(el, Event, 'change', { cancelable: false, composed: false });
                    };
                    el.addEventListener('blur', onBlur, true);
                }
            }
            return;
        }
        if (el && /^(input|textarea)$/.test(el.localName)) {
            /* (the caret moves, and the text scrolls to keep it in view; its
             * place is estimated: controls aren't laid out by character here) */
            if (!dx || typeof el.selectionEnd !== 'number') return;
            var caret = Math.max(0, Math.min(el.value.length, el.selectionEnd + (dx > 0 ? 1 : -1)));
            el.setSelectionRange(caret, caret);
            var cx = caret * (parseFloat(global.getComputedStyle(el).fontSize) || 13) * 0.5;
            if (cx > el.scrollLeft + el.clientWidth) el.scrollLeft = cx - el.clientWidth;
            else if (cx < el.scrollLeft) el.scrollLeft = cx;
            return;
        }
        /* (nothing focused: what was last clicked is where the keys go) */
        var from = el && el !== d.body ? el : pointer.lastDown && pointer.lastDown.isConnected ? pointer.lastDown : el;
        /* (Home and End go to their ends) */
        wheelScroll(from || d.documentElement, dx, dy, Math.abs(dy) < 1e8);
    }
    /* ---- drag and drop (HTML 6.11.5), from a press on something draggable and a move away ---- */
    var HTMLNS_D = 'http://www.w3.org/1999/xhtml';
    function dragSource(t) {
        for (var e = t; e && e.nodeType === 1; e = flatParent(e)) {
            /* (a press in text to edit or a list to pick from: that, not a drag) */
            if (e.isContentEditable) return null;
            if (e.namespaceURI === HTMLNS_D && (/^(textarea|select)$/.test(e.localName) ||
                (e.localName === 'input' && !/^(button|submit|reset|image|checkbox|radio|color|file|range|hidden)$/.test(String(e.type || ''))))) return null;
            var d = e.getAttribute('draggable');
            if (d !== null && /^true$/i.test(d)) return e;
            if (d !== null && /^false$/i.test(d)) return null;
            if (e.namespaceURI !== HTMLNS_D) continue;
            if (/^(input|button)$/.test(e.localName)) return null;
            if (e.localName === 'img' && e.hasAttribute('src')) return e;
            if ((e.localName === 'a' || e.localName === 'area') && e.hasAttribute('href')) return e;
        }
        return null;
    }
    function dropEffectFor(eff, src) {
        switch (eff) {
        case 'none': return 'none';
        case 'copy': case 'copyLink': case 'copyMove': case 'all': return 'copy';
        case 'link': case 'linkMove': return 'link';
        case 'move': return 'move';
        default: return src && src.localName === 'a' && src.hasAttribute('href') ? 'link' : 'copy';
        }
    }
    function dropAllowed(op, eff) {
        if (op === 'none') return false;
        if (eff === 'uninitialized' || eff === 'all') return true;
        return eff.toLowerCase().indexOf(op) >= 0;
    }
    function isTextTarget(el) {
        if (!el || el.nodeType !== 1) return false;
        if (el.isContentEditable) return true;
        if (el.localName === 'textarea') return !el.disabled && !el.readOnly;
        return el.localName === 'input' && /^(text|search|url|tel|email|password|)$/.test(String(el.type || '')) && !el.disabled && !el.readOnly;
    }
    function dragEv(D, el, type, x, y, cancelable, related) {
        var i = mouseInit(x, y, 0, related);
        i.cancelable = cancelable;
        i.dataTransfer = D.dt;
        return trusted(el, global.DragEvent || MouseEvent, type, i);
    }
    function dragStart(F, x, y) {
        if (typeof global.__cyberDragData !== 'function' || !F.el.isConnected) return false;
        var made = global.__cyberDragData(), D = { src: F.el, dt: made[0], st: made[1], target: null, immediate: null, op: 'none' };
        var s = D.src;
        try {
            if (s.localName === 'a' || s.localName === 'area') { D.st.items.push({ type: 'text/uri-list', data: s.href }); D.st.items.push({ type: 'text/plain', data: s.href }); }
            else if (s.localName === 'img') D.st.items.push({ type: 'text/uri-list', data: s.currentSrc || s.src });
        } catch (e) { }
        D.st.dropEffect = 'none';
        var ok = dragEv(D, s, 'dragstart', F.x, F.y, true);
        if (!ok) return false;
        pointer.drag = D;
        if (pointer.active) { chainState(pointer.active, 2, false); pointer.active = null; }
        return true;
    }
    function dragMove(x, y) {
        var D = pointer.drag, d = global.document;
        D.st.mode = 'protected';
        D.st.dropEffect = 'none';
        if (!dragEv(D, D.src, 'drag', x, y, true)) { D.cancelled = true; return; }
        var t = hitAt(x, y);
        if (t && t !== D.immediate) {
            D.immediate = t;
            D.st.dropEffect = dropEffectFor(D.st.effectAllowed, D.src);
            /* (the element under the pointer is the target, as engines have it, whether or not its dragenter was canceled) */
            dragEv(D, t, 'dragenter', x, y, true, D.target);
            var next = t;
            if (D.target && D.target !== next && D.target.isConnected) { D.st.dropEffect = 'none'; dragEv(D, D.target, 'dragleave', x, y, false, next); }
            D.target = next;
        }
        if (D.target) {
            D.st.dropEffect = dropEffectFor(D.st.effectAllowed, D.src);
            var over = dragEv(D, D.target, 'dragover', x, y, true);
            if (!over) D.op = dropAllowed(D.st.dropEffect, D.st.effectAllowed) ? D.st.dropEffect : 'none';
            else D.op = isTextTarget(D.target) && dropAllowed('copy', D.st.effectAllowed) ? 'copy' : 'none';
        }
    }
    function dragEnd(x, y) {
        var D = pointer.drag;
        pointer.drag = null;
        if (D.cancelled) D.op = 'none';
        if (!D.target || D.op === 'none') {
            if (D.target && D.target.isConnected) { D.st.mode = 'protected'; D.st.dropEffect = 'none'; dragEv(D, D.target, 'dragleave', x, y, false); }
            D.op = 'none';
        } else {
            D.st.mode = 'ro';
            D.st.dropEffect = D.op;
            var notCanceled = dragEv(D, D.target, 'drop', x, y, true);
            if (notCanceled) {
                /* (a text control or editing host: the text goes in) */
                if (isTextTarget(D.target) && (D.target.localName === 'input' || D.target.localName === 'textarea')) {
                    var txt = D.dt.getData('text/plain');
                    if (txt) { D.target.value += txt; trusted(D.target, global.InputEvent || Event, 'input', { cancelable: false, data: txt, inputType: 'insertFromDrop' }); }
                }   /* (else the operation stands, as engines report it at dragend) */
            } else D.op = D.st.dropEffect;
        }
        D.st.mode = 'protected';
        D.st.dropEffect = D.op;
        if (D.src.isConnected) dragEv(D, D.src, 'dragend', x, y, false);
        D.st.mode = 'gone';
    }
    Object.defineProperty(global, '__cyberInput', { configurable: true, value: {
        move: function (x, y) {
            /* (a drag going: drag events, no mouse ones) */
            if (pointer.drag) { dragMove(x, y); return; }
            var F = pointer.dragFrom;
            if (F && F.el && (pointer.buttons & 1) && Math.abs(x - F.x) + Math.abs(y - F.y) >= 4) {
                pointer.dragFrom = null;
                if (dragStart(F, x, y)) { dragMove(x, y); return; }
            }
            var t = hitAt(x, y);
            if (!t) return;
            var into = frameInput(t);
            if (into) { into.input.move(x - into.left, y - into.top); return; }
            if (pointer.bar) {
                var pos = pointer.bar.prop === 'scrollTop' ? y : x;
                if (global.__cyberSnapHold) global.__cyberSnapHold(1);
                pointer.bar.el[pointer.bar.prop] += (pos - pointer.bar.last) * pointer.bar.size / pointer.bar.track;
                if (global.__cyberSnapHold) global.__cyberSnapHold(-1);
                pointer.bar.last = pos;
            }
            var was = pointer.over;
            if (was !== t) {
                chainState(was, 1, false);
                chainState(t, 1, true);
                var common = was && was.isConnected ? commonAncestor(was, t) : null;
                if (was && was.isConnected) {
                    bothEvents(was, x, y, 0, 'pointerout', 'mouseout', t);
                    for (var l = was; l && l !== common; l = flatParent(l)) boundary(l, x, y, 'pointerleave', 'mouseleave', t);
                }
                pointer.over = t;
                bothEvents(t, x, y, 0, 'pointerover', 'mouseover', was);
                /* (outermost first) */
                var entered = [];
                for (var e = t; e && e !== common; e = flatParent(e)) entered.push(e);
                for (var i = entered.length - 1; i >= 0; i--) boundary(entered[i], x, y, 'pointerenter', 'mouseenter', was);
            }
            bothEvents(t, x, y, 0, 'pointermove', 'mousemove');
        },
        down: function (x, y, button) {
            var t = hitAt(x, y);
            if (!t) return;
            var into = frameInput(t);
            if (into) { into.input.down(x - into.left, y - into.top, button); return; }
            pointer.buttons |= 1 << button;
            pointer.pressed = t;
            /* (a press outside the viewport is on nothing of the page's) */
            if (x >= 0 && y >= 0 && x < global.innerWidth && y < global.innerHeight) pointer.lastDown = t;
            if (button === 0) { chainState(t, 2, true); pointer.active = t; }
            global.__cyberLastInput = 'mouse';
            var canceled = !bothEvents(t, x, y, button, 'pointerdown', 'mousedown');
            pointer.dragFrom = button === 0 && !canceled ? { el: dragSource(t), x: x, y: y } : null;
            global.__cyberFocusByMouse = true;
            try { focusTarget(t, canceled); } finally { global.__cyberFocusByMouse = false; }
            var bar = button === 0 ? scrollbarAt(t, x, y) : null;
            if (bar) barPress(bar);
        },
        up: function (x, y, button) {
            pointer.dragFrom = null;
            if (pointer.drag && button === 0) {
                dragEnd(x, y);
                pointer.buttons &= ~1;
                pointer.pressed = null;
                return;
            }
            var t = hitAt(x, y);
            if (!t) return;
            var into = frameInput(t);
            if (into) { into.input.up(x - into.left, y - into.top, button); return; }
            if (pointer.bar) { pointer.bar = null; global.__cyberScrollHold(false); }
            pointer.buttons &= ~(1 << button);
            if (button === 0 && pointer.active) { chainState(pointer.active, 2, false); pointer.active = null; }
            bothEvents(t, x, y, button, 'pointerup', 'mouseup');
            var on = pointer.pressed && commonAncestor(pointer.pressed, t);
            pointer.pressed = null;
            if (on && on.nodeType === 1 && !inDisabledControl(on)) {
                var i = mouseInit(x, y, button);
                i.detail = 1;
                var clicked = trusted(on, MouseEvent, button === 0 ? 'click' : 'auxclick', i);
                if (clicked && button === 0 && global.__cyberScrollPseudoClick) global.__cyberScrollPseudoClick(x, y);
            }
        },
        /* A finger: a press, then a drag scrolls (the browser takes the
         * pointer from the page: pointercancel), else it's a tap. */
        touchDown: function (x, y) {
            var t = hitAt(x, y);
            if (!t) return;
            var into = frameInput(t);
            pointer.touchFrame = into;
            if (into) { into.input.touchDown(x - into.left, y - into.top); return; }
            var c = pointer.touch = { target: t, x: x, y: y, scrolling: false, canceled: false };
            trusted(t, global.PointerEvent, 'pointerdown', touchInit(x, y, 0));
            c.canceled = !touchEvent(t, 'touchstart', x, y, false);
        },
        touchMove: function (x, y) {
            var into = pointer.touchFrame;
            if (into) { into.input.touchMove(x - into.left, y - into.top); return; }
            var c = pointer.touch;
            if (!c) return;
            var dx = c.x - x, dy = c.y - y;
            c.x = x; c.y = y;
            if (!dx && !dy) return;
            /* (a listener can keep it for the page) */
            if (!touchEvent(c.target, 'touchmove', x, y, false)) c.canceled = true;
            if (!c.scrolling) {
                if (c.canceled || global.getComputedStyle(c.target).touchAction === 'none') {
                    trusted(c.target, global.PointerEvent, 'pointermove', touchInit(x, y, -1));
                    return;
                }
                c.scrolling = true;
                global.__cyberScrollHold(true);
                trusted(c.target, global.PointerEvent, 'pointercancel', touchInit(x, y, 0));
            }
            wheelScroll(c.target, dx, dy);
        },
        touchUp: function (x, y) {
            var into = pointer.touchFrame;
            pointer.touchFrame = null;
            if (into) { into.input.touchUp(x - into.left, y - into.top); return; }
            var c = pointer.touch;
            pointer.touch = null;
            if (!c) return;
            var go = touchEvent(c.target, 'touchend', x, y, true);
            if (c.scrolling) { global.__cyberScrollHold(false); return; }
            trusted(c.target, global.PointerEvent, 'pointerup', touchInit(x, y, 0));
            if (!go || c.canceled) return;
            var i = mouseInit(x, y, 0);
            i.detail = 1;
            if (!controlDisabled(c.target)) trusted(c.target, MouseEvent, 'click', i);
        },
        wheelEnd: function () { pointer.wheelTarget = null; },
        /* (a frame with nothing left to scroll hands the rest to its container) */
        chain: function (from, dx, dy) { wheelScroll(from, dx, dy); },
        wheel: function (x, y, dx, dy) {
            var t = hitAt(x, y);
            if (!t) return;
            var into = frameInput(t);
            if (into) { into.input.wheel(x - into.left, y - into.top, dx, dy); return; }
            var now = global.performance.now();
            if (now < pointer.wheelUntil && pointer.wheelTarget && pointer.wheelTarget.isConnected &&
                docTop(pointer.wheelTarget) === pointer.wheelAt) t = pointer.wheelTarget;
            else { pointer.wheelTarget = t; pointer.wheelAt = docTop(t); }
            pointer.wheelUntil = now + WHEEL_TRANSACTION_MS;
            var i = mouseInit(x, y, 0);
            i.cancelable = wheelBlocked(t);
            i.deltaX = dx; i.deltaY = dy; i.deltaMode = 0;
            var go = trusted(t, WheelEvent, 'wheel', i);
            if (go) go = trusted(t, WheelEvent, 'mousewheel', i);
            if (go) wheelScroll(t, dx, dy);
        },
        keyDown: function (k, el) {
            global.__cyberLastInput = 'key';
            var key = keyOf(k), t = el || (global.document && (global.document.activeElement || global.document.body));
            if (key in held) held[key] = true;
            if (!t) return;
            var go = trusted(t, KeyboardEvent, 'keydown', { cancelable: true, key: key, view: global, shiftKey: held.Shift, ctrlKey: held.Control, altKey: held.Alt, metaKey: held.Meta });
            if (go && key.length === 1) go = trusted(t, KeyboardEvent, 'keypress', { cancelable: true, key: key, view: global });
            if (go) keyDefault(key, t);
        },
        keyUp: function (k, el) {
            var t = el || (global.document && (global.document.activeElement || global.document.body));
            var ku = keyOf(k);
            if (t) trusted(t, KeyboardEvent, 'keyup', { cancelable: true, key: ku, view: global, shiftKey: held.Shift, ctrlKey: held.Control, altKey: held.Alt, metaKey: held.Meta });
            if (ku in held) held[ku] = false;
        }
    } });

    /* (a <style> inserted: load, for whoever listens by now) */
    Object.defineProperty(global, '__cyberStyleInserted', { configurable: true, value: function (el) {
        var map = el[LISTENERS];
        if (!el.onload && !(map && map.load)) return;
        task(function () { global.__cyberDispatchTrusted(el, new Event('load')); });
    } });

    /* ---- MediaQueryList (CSSOM View 4.2): live, with change events; the query
     * itself is evaluated natively ---- */
    var evalMedia = global.matchMedia;
    if (typeof evalMedia === 'function') {
        var liveQueries = new Set();
        var MQL_PRIVATE = new WeakMap();
        var syncScheme = function () {
            var d = global.document, m = d && d.querySelector && d.querySelector('meta[name="color-scheme" i]');
            var words = m ? String(m.getAttribute('content') || '').trim().toLowerCase().split(/\s+/) : [];
            var one = words.length === 1 && (words[0] === 'dark' || words[0] === 'light') ? words[0] : null;
            if (global.__cyberColorScheme) global.__cyberColorScheme(one);
        };
        var mqlMatches = function (mql) {
            var p = MQL_PRIVATE.get(mql);
            if (/prefers-color-scheme/i.test(p.query)) syncScheme();
            return !!evalMedia.call(global, p.query).matches;
        };
        var MediaQueryList = function MediaQueryList() { throw new TypeError('Illegal constructor'); };
        MediaQueryList.prototype = Object.create(ETP);
        define(MediaQueryList.prototype, 'constructor', MediaQueryList);
        Object.defineProperty(MediaQueryList.prototype, Symbol.toStringTag, { value: 'MediaQueryList', configurable: true });
        Object.defineProperty(MediaQueryList.prototype, 'media', { configurable: true, enumerable: true, get: function () { return MQL_PRIVATE.get(this).media; } });
        Object.defineProperty(MediaQueryList.prototype, 'matches', { configurable: true, enumerable: true, get: function () { return mqlMatches(this); } });
        var watch = function (mql) {
            if (liveQueries.has(mql)) return;
            MQL_PRIVATE.get(mql).last = mqlMatches(mql);
            liveQueries.add(mql);
            if (global.__cyberRenderFeature) global.__cyberRenderFeature(8);
        };
        Object.defineProperty(MediaQueryList.prototype, 'onchange', { configurable: true, enumerable: true,
            get: function () { return MQL_PRIVATE.get(this).onchange || null; },
            set: function (f) {
                var p = MQL_PRIVATE.get(this);
                p.onchange = typeof f === 'function' ? f : null;
                if (p.onchange) watch(this);
            } });
        define(MediaQueryList.prototype, 'addEventListener', function (type, fn, opts) {
            addEventListener.call(this, type, fn, opts);
            if (String(type) === 'change') watch(this);
        });
        define(MediaQueryList.prototype, 'addListener', function (fn) {
            if (!MQL_PRIVATE.has(this)) throw new TypeError('Illegal invocation');
            if (arguments.length < 1) throw new TypeError("Failed to execute 'addListener' on 'MediaQueryList': 1 argument required");
            if (fn) { addEventListener.call(this, 'change', fn); watch(this); }
        });
        define(MediaQueryList.prototype, 'removeListener', function (fn) {
            if (!MQL_PRIVATE.has(this)) throw new TypeError('Illegal invocation');
            if (arguments.length < 1) throw new TypeError("Failed to execute 'removeListener' on 'MediaQueryList': 1 argument required");
            if (fn) removeEventListener.call(this, 'change', fn);
        });
        define(global, 'MediaQueryList', MediaQueryList);
        define(global, 'matchMedia', function matchMedia(query) {
            if (this !== undefined && this !== null && this !== global) throw new TypeError('Illegal invocation');
            if (arguments.length < 1) throw new TypeError("Failed to execute 'matchMedia' on 'Window': 1 argument required, but only 0 present.");
            var q = String(query), mql = Object.create(MediaQueryList.prototype);
            /* (the end of the input closes what's open) */
            var open = 0;
            for (var qi = 0; qi < q.length; qi++) { if (q[qi] === '(') open++; else if (q[qi] === ')' && open) open--; }
            while (open-- > 0) q += ')';
            /* (a resolution worked out in calc(): in dppx) */
            q = q.replace(/calc\(([\d.\s+\-*\/]*(?:x|dppx)[\d.\s+\-*\/xdp]*)\)/gi, function (all, e) {
                var t = e.replace(/(\d*\.?\d+)\s*(?:dppx|x)\b/gi, '$1');
                if (!/^[\d.\s+\-*\/()]+$/.test(t)) return all;
                var v;
                try { v = Function('return (' + t + ')')(); } catch (er) { return all; }
                return typeof v === 'number' && isFinite(v) ? 'calc(' + v + 'dppx)' : all;
            });
            MQL_PRIVATE.set(mql, { query: q, media: global.__cyberMediaText ? global.__cyberMediaText(q) : evalMedia.call(global, q).media, last: false, onchange: null });
            return mql;
        });
        Object.defineProperty(global, '__cyberMediaCheck', { configurable: true, value: function () {
            liveQueries.forEach(function (mql) {
                var p = MQL_PRIVATE.get(mql), now = mqlMatches(mql);
                if (now === p.last) return;
                p.last = now;
                global.__cyberDispatchTrusted(mql, new global.MediaQueryListEvent('change', { media: p.media, matches: now }));
            });
        } });
    }

    /* ---- ContainerQueryList (Element.matchContainer): like MediaQueryList, of the
     * element's containers ---- */
    if (global.Element) {
        var CQL_PRIVATE = new WeakMap(), liveContainers = new Set();
        var cqlMatches = function (l) {
            var p = CQL_PRIVATE.get(l);
            if (!p.text || !p.el.isConnected || typeof global.__cyberMatchContainer !== 'function') return false;
            return !!global.__cyberMatchContainer(p.el, p.raw);
        };
        var ContainerQueryList = function ContainerQueryList() { throw new TypeError('Illegal constructor'); };
        ContainerQueryList.prototype = Object.create(ETP);
        define(ContainerQueryList.prototype, 'constructor', ContainerQueryList);
        Object.defineProperty(ContainerQueryList.prototype, Symbol.toStringTag, { value: 'ContainerQueryList', configurable: true });
        Object.defineProperty(ContainerQueryList.prototype, 'query', { configurable: true, enumerable: true, get: function () { return CQL_PRIVATE.get(this).text; } });
        Object.defineProperty(ContainerQueryList.prototype, 'container', { configurable: true, enumerable: true, get: function () { return CQL_PRIVATE.get(this).text; } });
        Object.defineProperty(ContainerQueryList.prototype, 'matches', { configurable: true, enumerable: true, get: function () { return cqlMatches(this); } });
        var cqWatch = function (l) {
            if (liveContainers.has(l)) return;
            CQL_PRIVATE.get(l).last = cqlMatches(l);
            liveContainers.add(l);
            if (global.__cyberRenderFeature) global.__cyberRenderFeature(8);
        };
        Object.defineProperty(ContainerQueryList.prototype, 'onchange', { configurable: true, enumerable: true,
            get: function () { return CQL_PRIVATE.get(this).onchange || null; },
            set: function (f) {
                var p = CQL_PRIVATE.get(this);
                p.onchange = typeof f === 'function' ? f : null;
                if (p.onchange) cqWatch(this);
            } });
        define(ContainerQueryList.prototype, 'addEventListener', function (type, fn, opts) {
            addEventListener.call(this, type, fn, opts);
            if (String(type) === 'change') cqWatch(this);
        });
        define(global, 'ContainerQueryList', ContainerQueryList);
        define(global.Element.prototype, 'matchContainer', function matchContainer(query) {
            if (arguments.length < 1) throw new TypeError("Failed to execute 'matchContainer' on 'Element': 1 argument required, but only 0 present.");
            var raw = String(query), l = Object.create(ContainerQueryList.prototype);
            var text = global.__cyberContainerQueryText ? global.__cyberContainerQueryText(raw) : '';
            CQL_PRIVATE.set(l, { el: this, raw: raw, text: text, last: false, onchange: null });
            return l;
        });
        var mediaCheck = global.__cyberMediaCheck;
        Object.defineProperty(global, '__cyberMediaCheck', { configurable: true, value: function () {
            if (mediaCheck) mediaCheck();
            liveContainers.forEach(function (l) {
                var p = CQL_PRIVATE.get(l), now = cqlMatches(l);
                if (now === p.last) return;
                p.last = now;
                global.__cyberDispatchTrusted(l, new Event('change'));
            });
        } });
    }

    /* for the C entry points (event_api.c), which forward here */
    Object.defineProperty(global, '__cyberEventTarget', { value: Object.freeze({
        add: addEventListener, remove: removeEventListener, dispatch: dispatchEvent }) });
})
