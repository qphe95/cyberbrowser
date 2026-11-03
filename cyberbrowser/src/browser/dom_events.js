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
    eventClass('PromiseRejectionEvent', Event, { promise: any(undefined), reason: any(undefined) });
    eventClass('HashChangeEvent', Event, { oldURL: str(''), newURL: str('') });
    eventClass('PopStateEvent', Event, { state: nul(), hasUAVisualTransition: bool(false) });
    eventClass('PageTransitionEvent', Event, { persisted: bool(false) });
    eventClass('ProgressEvent', Event, { lengthComputable: bool(false), loaded: num(0), total: num(0) });
    eventClass('AnimationEvent', Event, { animationName: str(''), elapsedTime: num(0), pseudoElement: str('') });
    eventClass('TransitionEvent', Event, { propertyName: str(''), elapsedTime: num(0), pseudoElement: str('') });
    eventClass('SubmitEvent', Event, { submitter: nul() });
    eventClass('FormDataEvent', Event, { formData: nul() });
    eventClass('ToggleEvent', Event, { oldState: str(''), newState: str('') });
    eventClass('StorageEvent', Event, { key: nul(), oldValue: nul(), newValue: nul(), url: str(''),
                                         storageArea: nul() });
    eventClass('MessageEvent', Event, { data: nul(), origin: str(''), lastEventId: str(''),
                                             source: nul(), ports: function (v) { return arguments.length ? v : []; } });
    eventClass('DeviceMotionEvent', Event, { acceleration: nul(), accelerationIncludingGravity: nul(), rotationRate: nul(),
                                             interval: num(0) });
    eventClass('DeviceOrientationEvent', Event, { alpha: nul(), beta: nul(), gamma: nul(), absolute: bool(false) });
    /* (made by createEvent only, in browsers; a constructor here) */
    eventClass('BeforeUnloadEvent', Event, { returnValue: str('') });
    eventClass('GamepadEvent', Event, { gamepad: nul() });
    var TextEvent = eventClass('TextEvent', UIEvent, { data: str('') });
    define(TextEvent.prototype, 'initTextEvent', function (type, bubbles, cancelable, view, data) {
        this.initEvent(type, bubbles, cancelable);
        st(this).view = view === undefined ? null : view;
        st(this).data = data === undefined ? '' : String(data);
    });
    expose('Event', Event);

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
        var l = { callback: callback, capture: o.capture, once: !!o.once,
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
            if (g && g !== global && typeof g.reportError === 'function') g.reportError(err);
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
    function reportError(err) {
        if (reporting) return;
        reporting = true;
        try {
            var msg;
            try { msg = 'Uncaught ' + (err && typeof err === 'object' && 'message' in err ? (err.name ? err.name + ': ' : '') + err.message : String(err)); }
            catch (e) { msg = 'Uncaught exception'; }
            var line = 0, file = '';
            try { if (err && typeof err === 'object') { line = +err.lineNumber || 0; file = String(err.fileName || ''); } } catch (e) { /* (getters) */ }
            var ev = new global.ErrorEvent('error', { message: msg, filename: file, lineno: line, colno: 0,
                                                       error: err, cancelable: true });
            if (global.dispatchEvent(ev) && global.console)
                global.console.error(msg + (err && err.stack ? '\n' + err.stack : ''));
        } catch (e) { /* the report itself failed */ }
        reporting = false;
    }
    Object.defineProperty(global, 'reportError', { value: reportError, writable: true, configurable: true, enumerable: true });

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
    function innerInvoke(ev, s, entry, item, capturePass) {
        /* (where the object was when the path was made) */
        var shadow = entry.flags === undefined ? entry.shadow : (entry.flags & EP_SHADOW) !== 0;
        var map = item[LISTENERS];
        var list = map && map[s.type];
        var found = false;
        if (list) for (var k = 0; k < list.length && !found; k++) found = !list[k].removed;
        if (list) {
            /* (the listeners there now: DOM 2.10 clones the list) */
            for (var i = 0, end = list.length; i < end; i++) {
                var l = list[i];
                if (l.removed) continue;
                if (l.capture !== capturePass) continue;
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
        /* the on<type> handler, in the non-capture pass: read through its
         * property unless the native path says there is no content attribute
         * and none was set (nor forwarded from <body> to the window) */
        /* (a native path's flags say whether a handler may be set) */
        var h = (entry.flags === undefined || (entry.flags & (EP_ON_ATTR | EP_HANDLER | EP_ALWAYS | EP_ON_PROP))) &&
                !inactive(item, null) ? handlerProp(item, s.type) : null;
        if (h && inactive(item, listenerGlobal(h))) h = null;
        if (h) found = true;
        if (!capturePass) {
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
                else if (s.type === 'beforeunload' && r != null) cancel(s);
            }
        }
        return found;
    }

    /* DOM 2.9 "dispatch" with activation: a click MouseEvent's activation
     * target (the target, else, as it bubbles, the nearest ancestor with
     * activation behavior) gets its legacy-pre-activation behavior first,
     * then its activation behavior, or (canceled) its legacy-canceled one. */
    function dispatch(target, ev) {
        var s = st(ev), act = null, saved;
        if (s.type === 'click' && !s.dispatching && s.initialized && global.MouseEvent && ev instanceof global.MouseEvent) {
            act = activationTargetOf(target, s.bubbles);
            if (act) saved = preActivate(act);
        }
        var r = dispatchEvent0(target, ev);
        if (act) {
            if (s.canceled) canceledActivate(act, saved);
            else runActivation(act, saved);
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
        if (id !== null && id !== undefined) {
            var f = el.isConnected && el.ownerDocument ? el.ownerDocument.getElementById(id) : null;
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
    function buttonType(el) {
        var t = String(el.getAttribute('type') || 'submit').toLowerCase();
        return t === 'reset' || t === 'button' ? t : 'submit';
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
        var type = inputType(el), saved = { type: type, checked: getChecked(el), radio: null };
        if (type === 'checkbox') setChecked(el, !saved.checked);
        else if (type === 'radio') {
            radioGroup(el).forEach(function (r) { if (getChecked(r)) saved.radio = r; });
            setChecked(el, true);
        }
        return saved;
    }
    function canceledActivate(el, saved) {
        if (!saved) return;
        if (saved.type === 'checkbox') setChecked(el, saved.checked);
        else if (saved.type === 'radio') { setChecked(el, saved.checked); if (saved.radio) setChecked(saved.radio, true); }
    }
    function runActivation(el, saved) {
        var view = el.ownerDocument && el.ownerDocument.defaultView || global;
        var isInput = el.localName === 'input', type = isInput ? inputType(el) : '';
        if (type === 'checkbox' || type === 'radio') {
            /* (a disconnected one changes, with no events) */
            if (!el.isConnected) return;
            if (type === 'checkbox' || (saved && !saved.checked)) {
                fire(el, 'input', Event, { bubbles: true, composed: true });
                fire(el, 'change', Event, { bubbles: true });
            }
            return;
        }
        if ((isInput && (type === 'submit' || type === 'image')) || (el.localName === 'button' && buttonType(el) === 'submit')) {
            var form = disabledControl(el) ? null : formOf(el);
            if (form && form.isConnected) requestSubmit(form, el);
            return;
        }
        if ((isInput && type === 'reset') || (el.localName === 'button' && buttonType(el) === 'reset')) {
            var f2 = disabledControl(el) ? null : formOf(el);
            if (f2 && f2.isConnected && typeof f2.reset === 'function') f2.reset();
            return;
        }
        if (isInput || el.localName === 'button') return;
        if (el.localName === 'label') {
            var control = el.control !== undefined ? el.control
                : (el.htmlFor ? (el.ownerDocument.getElementById(el.htmlFor)) : el.querySelector('input,button,select,textarea'));
            if (control && control !== el && typeof control.click === 'function') control.click();
            return;
        }
        if (el.localName === 'summary') {
            var d = summaryOf(el);
            if (d) {
                var was = d.hasAttribute('open');
                if (was) d.removeAttribute('open'); else d.setAttribute('open', '');
                /* (details' toggle event, in a task) */
                global.setTimeout(function () {
                    fire(d, 'toggle', global.ToggleEvent || Event, { oldState: was ? 'open' : 'closed',
                                                                     newState: was ? 'closed' : 'open' });
                }, 0);
            }
            return;
        }
        followHyperlink(el, view);
    }
    function requestSubmit(form, submitter) {
        var ok = form.dispatchEvent(new global.SubmitEvent('submit', { bubbles: true, cancelable: true,
                                                                       submitter: submitter || null }));
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
        var cur;
        try { cur = new global.URL(loc.href); } catch (e) { return; }
        /* same-document navigation: a fragment change */
        if (url.origin === cur.origin && url.pathname === cur.pathname && url.search === cur.search) {
            if (url.hash !== cur.hash) loc.hash = url.hash;
            else scrollToFragment(view, url.hash);
            return;
        }
        if (typeof view.__cyberNavigate === 'function') view.__cyberNavigate(url.href, a);
    }
    var HEP = global.HTMLElement && global.HTMLElement.prototype;
    if (HEP) define(HEP, 'click', click);
    var ELP = global.Element && global.Element.prototype;
    if (ELP && Object.prototype.hasOwnProperty.call(ELP, 'click')) define(ELP, 'click', click);
    var FP = global.HTMLFormElement && global.HTMLFormElement.prototype;
    if (FP) {
        define(FP, 'requestSubmit', function (submitter) { requestSubmit(this, submitter); });
    }

    /* ---- event handler attributes (HTML 8.1.8.1) ---- */
    var HANDLERS = Symbol.for('cyber.eventHandlers');
    var GLOBAL_HANDLERS = ['abort', 'auxclick', 'beforeinput', 'beforematch', 'beforetoggle', 'blur',
        'cancel', 'canplay', 'canplaythrough', 'change', 'click', 'close', 'contextlost', 'contextmenu',
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
                /* <body>'s window handlers are the window's (HTML 8.1.8.2) */
                var self = bodyForwards(t, type) ? global : t;
                var h = self[HANDLERS];
                if (h && Object.prototype.hasOwnProperty.call(h, type)) return h[type];
                /* the content attribute, compiled on first use (the window's
                 * are <body>'s) */
                var holder = self === global && BODY_FORWARDED.indexOf(type) >= 0 && global.document ? global.document.body : self;
                if (holder && typeof holder.getAttribute === 'function') {
                    var code = holder.getAttribute(name);
                    if (code !== null) {
                        var fn = null;
                        try {
                            fn = type === 'error' && t === global
                                ? new Function('event', 'source', 'lineno', 'colno', 'error', code)
                                : new Function('event', code);
                        } catch (e) { report(e); }
                        setHandler(self, type, fn);
                        return fn;
                    }
                }
                return null;
            },
            set: function (v) {
                var t = handlerThis(this, P);
                var self = bodyForwards(t, type) ? global : t;
                setHandler(self, type, typeof v === 'function' || (v !== null && typeof v === 'object') ? v : null);
            } });
    }
    /* The handlers <body> (and <frameset>) forward to the window. */
    var BODY_FORWARDED = ['blur', 'error', 'focus', 'load', 'resize', 'scroll'].concat(WINDOW_HANDLERS);
    function bodyForwards(el, type) {
        return el !== global && el && (el.localName === 'body' || el.localName === 'frameset') &&
               el.namespaceURI === 'http://www.w3.org/1999/xhtml' && BODY_FORWARDED.indexOf(type) >= 0;
    }
    function setHandler(target, type, v) {
        listenerChanges++;
        var h = target[HANDLERS];
        if (!h) {
            h = Object.create(null);
            Object.defineProperty(target, HANDLERS, { value: h, configurable: true });
        }
        h[type] = v;
    }
    var ELEMENT_P = global.HTMLElement && global.HTMLElement.prototype;
    var DOC_P = global.Document && global.Document.prototype;
    [ELEMENT_P, global.SVGElement && global.SVGElement.prototype, DOC_P].forEach(function (P) {
        if (P) GLOBAL_HANDLERS.forEach(function (t) { handlerAccessor(P, t); });
    });
    if (DOC_P) DOCUMENT_HANDLERS.forEach(function (t) { handlerAccessor(DOC_P, t); });
    /* (the legacy prefixed handlers: GlobalEventHandlers' too) */
    [ELEMENT_P, global.SVGElement && global.SVGElement.prototype, DOC_P, Object.getPrototypeOf(global) !== Object.prototype ? Object.getPrototypeOf(global) : null].forEach(function (P) {
        if (P) Object.keys(HANDLER_NAME).forEach(function (k) { handlerAccessor(P, HANDLER_NAME[k]); });
    });
    /* (the Fullscreen API's on Element and Document; slotchange on ShadowRoot) */
    [global.Element && global.Element.prototype, DOC_P].forEach(function (P) {
        if (P) ['fullscreenchange', 'fullscreenerror'].forEach(function (t) { handlerAccessor(P, t); });
    });
    if (global.ShadowRoot) handlerAccessor(global.ShadowRoot.prototype, 'slotchange');
    var WINP = Object.getPrototypeOf(global);
    if (WINP && WINP !== Object.prototype)
        GLOBAL_HANDLERS.concat(WINDOW_HANDLERS).forEach(function (t) { handlerAccessor(WINP, t); });
    if (ELEMENT_P) ['afterprint', 'beforeprint', 'beforeunload', 'hashchange', 'message', 'offline', 'online',
                    'pagehide', 'pageshow', 'popstate', 'storage', 'unload'].forEach(function (t) {
        /* (<body> and <frameset> forward these to the window; kept on the element here) */
        handlerAccessor(ELEMENT_P, t);
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
    function clone(v) {
        if (typeof global.structuredClone === 'function') {
            try { return global.structuredClone(v); } catch (e) { if (e && e.name === 'DataCloneError') throw e; }
        }
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
            var data = s.queue.shift();
            target.dispatchEvent(new global.MessageEvent('message', { data: data }));
        }
    }
    define(MessagePort.prototype, 'postMessage', function postMessage(message) {
        var s = port(this), other = s.other;
        if (s.closed || !other) return;
        var data = clone(message);
        task(function () {
            var o = other[PORT];
            if (o.closed) return;
            o.queue.push(data);
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
        Object.defineProperty(this, 'port1', { value: a, enumerable: true });
        Object.defineProperty(this, 'port2', { value: b, enumerable: true });
    }
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
        var data = clone(message);
        task(function () {
            if (targetOrigin !== '*' && targetOrigin !== '/' && targetOrigin !== originOf(target)) return;
            var ME = target.MessageEvent || global.MessageEvent;
            target.dispatchEvent(new ME('message', { data: data, origin: originOf(source), source: source }));
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
        var fireHistoryEvents = function (oldURL, newURL, state, hashChanged) {
            task(function () {
                global.dispatchEvent(new global.PopStateEvent('popstate', { state: state }));
                if (hashChanged)
                    global.dispatchEvent(new global.HashChangeEvent('hashchange', { oldURL: oldURL, newURL: newURL }));
            });
        };
        var cloneState = function (s) { return s === undefined ? null : clone(s); };
        var updateState = function (method, state, url, push) {
            var serialized = cloneState(state), cur = getHref(), target = cur;
            if (url !== undefined && url !== null) {
                target = resolve(url);
                if (!sameOrigin(target, cur))
                    throw new (global.DOMException || Error)("Failed to execute '" + method + "' on 'History': a history state object with URL '" + target + "' cannot be created in a document with origin '" + new global.URL(cur).origin + "'.", 'SecurityError');
            }
            if (push) { entries.length = index + 1; entries.push({ url: target, state: serialized }); index++; }
            else entries[index] = { url: target, state: serialized };
            if (target !== cur) setHref(target);
        };
        var traverse = function (delta) {
            var to = index + delta;
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
            fireHistoryEvents(oldURL, newURL, null, true);
        };
        /* (the URL's fragment as the document finishes loading) */
        global.addEventListener('load', function () {
            var h = new global.URL(getHref()).hash;
            if (h) scrollToFragment(global, h);
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
        define(HP, 'pushState', function pushState(state, unused, url) { histOf(this).update('pushState', state, url, true); });
        define(HP, 'replaceState', function replaceState(state, unused, url) { histOf(this).update('replaceState', state, url, false); });
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
                u.hash = v;
                if (u.href !== getHref()) navigateToFragment(u.href);
            } });
        var fragmentOnly = function (u) {
            var target;
            try { target = new global.URL(String(u), getHref()).href; } catch (e) { return null; }
            return stripHash(target) === stripHash(getHref()) && target.indexOf('#') >= 0 ? target : null;
        };
        Object.defineProperty(loc, 'href', { configurable: true, enumerable: true,
            get: hrefDesc.get,
            set: function (v) {
                var t = fragmentOnly(v);
                if (t) navigateToFragment(t);
                else { setHref(String(v)); entries[index] = { url: getHref(), state: null }; }
            } });
        ['assign', 'replace'].forEach(function (k) {
            var orig = loc[k];
            if (typeof orig !== 'function') return;
            define(loc, k, function (v) {
                var t = fragmentOnly(v);
                if (t) {
                    if (k === 'replace') { var old = getHref(); setHref(t); entries[index] = { url: t, state: null }; fireHistoryEvents(old, t, null, true); }
                    else navigateToFragment(t);
                    return;
                }
                return orig.call(loc, v);
            });
        });
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
        init.composed = true;
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
    function bothEvents(t, x, y, button, ptype, mtype, related) {
        trusted(t, global.PointerEvent, ptype, pointerInit(x, y, button, related));
        if (controlDisabled(t)) return true;
        return trusted(t, MouseEvent, mtype, mouseInit(x, y, button, related));
    }
    /* pointerenter/leave, mouseenter/leave: at each element, not bubbling */
    function boundary(el, x, y, ptype, mtype, related) {
        if (el.nodeType !== 1) return;
        var pi = pointerInit(x, y, 0, related), mi = mouseInit(x, y, 0, related);
        pi.bubbles = mi.bubbles = false;
        pi.cancelable = mi.cancelable = false;
        trusted(el, global.PointerEvent, ptype, pi);
        if (!controlDisabled(el)) trusted(el, MouseEvent, mtype, mi);
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
    function wheelScroll(t, dx, dy) {
        for (var a = t; a && (dx || dy); a = flatParent(a)) {
            if (a.nodeType !== 1 || a === a.ownerDocument.documentElement) continue;
            /* (the viewport has a body's overflow when the root has none) */
            if (a === a.ownerDocument.body && global.getComputedStyle(a.parentNode).overflowY === 'visible') continue;
            var cs = global.getComputedStyle(a), byX = /auto|scroll/.test(cs.overflowX), byY = /auto|scroll/.test(cs.overflowY);
            if (dy && byY) {
                if (dy > 0 ? a.scrollTop < a.scrollHeight - a.clientHeight : a.scrollTop > 0) { a.scrollTop += dy; dy = 0; }
                else if (/contain|none/.test(cs.overscrollBehaviorY)) dy = 0;
            }
            if (dx && byX) {
                if (dx > 0 ? a.scrollLeft < a.scrollWidth - a.clientWidth : a.scrollLeft > 0) { a.scrollLeft += dx; dx = 0; }
                else if (/contain|none/.test(cs.overscrollBehaviorX)) dx = 0;
            }
        }
        if (!dx && !dy) return;
        var was = [global.scrollX, global.scrollY];
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
        if (el) el.scrollIntoView({ block: 'start', inline: 'nearest' });
        else if (!name || name.toLowerCase() === 'top') view.scrollTo(0, 0);
    }
    var KEYS = { '\uE003': 'Backspace', '\uE004': 'Tab', '\uE006': 'Enter', '\uE007': 'Enter', '\uE008': 'Shift', '\uE009': 'Control',
                 '\uE00A': 'Alt', '\uE00C': 'Escape', '\uE00D': ' ', '\uE00E': 'PageUp', '\uE00F': 'PageDown', '\uE010': 'End',
                 '\uE011': 'Home', '\uE012': 'ArrowLeft', '\uE013': 'ArrowUp', '\uE014': 'ArrowRight', '\uE015': 'ArrowDown',
                 '\uE017': 'Delete', '\uE03D': 'Meta' };
    function keyOf(k) { return KEYS[k] || k; }
    /* Tab: the next element in the sequential focus order (positive tabindex
     * first, then tree order), round to the first. */
    function focusNext() {
        var d = global.document, all = d.querySelectorAll('*'), list = [];
        for (var i = 0; i < all.length; i++) {
            var e = all[i];
            if (typeof e.tabIndex === 'number' && e.tabIndex >= 0 && !controlDisabled(e) && e.getClientRects().length) list.push(e);
        }
        list = list.filter(function (e) { return e.tabIndex > 0; }).sort(function (a, b) { return a.tabIndex - b.tabIndex; })
            .concat(list.filter(function (e) { return e.tabIndex === 0; }));
        if (!list.length) return;
        var at = list.indexOf(d.activeElement);
        list[(at + 1) % list.length].focus();
    }
    function keyDefault(key, el) {
        var d = global.document, h = global.innerHeight, dx = 0, dy = 0;
        switch (key) {
        case 'Tab': global.__cyberLastInput = 'key'; focusNext(); return;
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
                el.value += key;
                trusted(el, global.InputEvent || Event, 'input', { cancelable: false, data: key, inputType: 'insertText' });
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
        wheelScroll(from || d.documentElement, dx, dy);
    }
    Object.defineProperty(global, '__cyberInput', { configurable: true, value: {
        move: function (x, y) {
            var t = hitAt(x, y);
            if (!t) return;
            var into = frameInput(t);
            if (into) { into.input.move(x - into.left, y - into.top); return; }
            if (pointer.bar) {
                var pos = pointer.bar.prop === 'scrollTop' ? y : x;
                pointer.bar.el[pointer.bar.prop] += (pos - pointer.bar.last) * pointer.bar.size / pointer.bar.track;
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
            pointer.lastDown = t;
            if (button === 0) { chainState(t, 2, true); pointer.active = t; }
            global.__cyberLastInput = 'mouse';
            var canceled = !bothEvents(t, x, y, button, 'pointerdown', 'mousedown');
            global.__cyberFocusByMouse = true;
            try { focusTarget(t, canceled); } finally { global.__cyberFocusByMouse = false; }
            var bar = button === 0 ? scrollbarAt(t, x, y) : null;
            if (bar) barPress(bar);
        },
        up: function (x, y, button) {
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
            if (on && on.nodeType === 1 && !controlDisabled(on)) {
                var i = mouseInit(x, y, button);
                i.detail = 1;
                trusted(on, MouseEvent, button === 0 ? 'click' : 'auxclick', i);
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
            if (!t) return;
            var go = trusted(t, KeyboardEvent, 'keydown', { cancelable: true, key: key, view: global });
            if (go && key.length === 1) go = trusted(t, KeyboardEvent, 'keypress', { cancelable: true, key: key, view: global });
            if (go) keyDefault(key, t);
        },
        keyUp: function (k, el) {
            var t = el || (global.document && (global.document.activeElement || global.document.body));
            if (t) trusted(t, KeyboardEvent, 'keyup', { cancelable: true, key: keyOf(k), view: global });
        }
    } });

    /* (a <style> inserted: load, for whoever listens by now) */
    Object.defineProperty(global, '__cyberStyleInserted', { configurable: true, value: function (el) {
        var map = el[LISTENERS];
        if (!el.onload && !(map && map.load)) return;
        task(function () { global.__cyberDispatchTrusted(el, new Event('load')); });
    } });

    /* for the C entry points (event_api.c), which forward here */
    Object.defineProperty(global, '__cyberEventTarget', { value: Object.freeze({
        add: addEventListener, remove: removeEventListener, dispatch: dispatchEvent }) });
})
