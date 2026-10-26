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
    }
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
    getter(EP, 'isTrusted', function () { return st(this).isTrusted; });
    getter(EP, 'timeStamp', function () { return st(this).timeStamp; });
    Object.defineProperty(EP, 'returnValue', { configurable: true, enumerable: true,
        get: function () { return !st(this).canceled; },
        set: function (v) { if (!v) cancel(st(this)); } });
    Object.defineProperty(EP, 'cancelBubble', { configurable: true, enumerable: true,
        get: function () { return st(this).stop; },
        set: function (v) { if (v) st(this).stop = true; } });
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
        return s.path.map(function (p) { return p.item; });
    });
    define(EP, 'initEvent', function initEvent(type, bubbles, cancelable) {
        var s = st(this);
        if (s.dispatching) return;
        s.type = String(type); s.bubbles = !!bubbles; s.cancelable = !!cancelable;
        s.stop = s.stopImmediate = s.canceled = false; s.target = null;
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
        this.initEvent(type, bubbles, cancelable);
        st(this).detail = detail === undefined ? null : detail;
    });
    var UIEvent = eventClass('UIEvent', Event, { view: nul(), detail: int(0), which: int(0) });
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
    expose('Event', Event);

    /* ---- EventTarget ---- */
    function flatten(options) {
        if (typeof options === 'boolean') return { capture: options };
        if (!options || typeof options !== 'object') return { capture: false };
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
    function addEventListener(type, callback, options) {
        var target = this == null ? global : this;
        var o = flatten(options);
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
        var target = this == null ? global : this;
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

    function report(err) {
        try {
            if (typeof global.reportError === 'function' && global.reportError !== report) global.reportError(err);
            else if (global.console) global.console.error('Uncaught ' + (err && err.stack ? err + '\n' + err.stack : err));
        } catch (e) { /* the report itself failed */ }
    }

    function handlerProp(item, type) {
        /* an on<type> event handler: an own or inherited data/accessor value */
        var h;
        try { h = item['on' + type]; } catch (e) { return null; }
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
        var map = item[LISTENERS];
        var list = map && map[s.type];
        if (list) {
            for (var i = 0; i < list.length; i++) {
                var l = list[i];
                if (l.removed) continue;
                if (l.capture !== capturePass) continue;
                if (l.once) removeListener(item, s.type, l);
                var fn = l.callback, thisArg = item;
                if (typeof fn !== 'function') {
                    thisArg = fn;
                    try { fn = fn.handleEvent; } catch (e) { report(e); continue; }
                    if (typeof fn !== 'function') { report(new TypeError("'handleEvent' is not a function")); continue; }
                }
                if (l.passive) s.inPassive = true;
                try { fn.call(thisArg, ev); } catch (e) { report(e); }
                s.inPassive = false;
                if (s.stopImmediate) return;
            }
        }
        /* the on<type> handler, in the non-capture pass */
        if (!capturePass) {
            var h = handlerProp(item, s.type);
            if (h) {
                var r;
                try {
                    r = s.type === 'error' && ev instanceof global.ErrorEvent && item === global
                        ? h.call(item, ev.message, ev.filename, ev.lineno, ev.colno, ev.error)
                        : h.call(item, ev);
                } catch (e) { report(e); r = undefined; }
                if (s.type === 'error' && item === global ? r === true : r === false) cancel(s);
                else if (s.type === 'beforeunload' && r != null) cancel(s);
            }
        }
    }

    function dispatch(target, ev) {
        var s = st(ev);
        if (s.dispatching || !s.initialized)
            throw new DOMExceptionOr("Failed to execute 'dispatchEvent': The event is already being dispatched.", 'InvalidStateError');
        s.dispatching = true;
        s.stop = s.stopImmediate = false;
        var origRelated = s.relatedTarget;
        var path = [];
        var t = target, related = retarget(origRelated, target);
        var guard = 0;
        s.path = path;
        while (t && guard++ < 4096) {
            path.push({ item: t, target: retarget(target, t),
                        relatedTarget: origRelated ? retarget(origRelated, t) : null });
            t = parentOf(t, ev, s);
        }
        (void related);
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
        s.target = path.length ? path[0].target : target;
        s.relatedTarget = origRelated ? retarget(origRelated, target) : null;
        s.dispatching = false;
        s.stop = s.stopImmediate = false;
        return !s.canceled;
    }
    function DOMExceptionOr(msg, name) {
        if (typeof global.DOMException === 'function') return new global.DOMException(msg, name);
        var e = new Error(msg); e.name = name; return e;
    }
    function dispatchEvent(event) {
        var target = this == null ? global : this;
        if (!event || !event[STATE])
            throw new TypeError("Failed to execute 'dispatchEvent': parameter 1 is not of type 'Event'.");
        st(event).isTrusted = false;
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
        if (Object.getPrototypeOf(WP) === Object.prototype) Object.setPrototypeOf(WP, ETP);
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
        mouseevents: MouseEvent, keyboardevent: KeyboardEvent, wheelevent: WheelEvent,
        compositionevent: global.CompositionEvent, touchevent: global.TouchEvent,
        hashchangeevent: global.HashChangeEvent, messageevent: global.MessageEvent,
        storageevent: global.StorageEvent, dragevent: global.DragEvent, errorevent: global.ErrorEvent
    };
    function createEvent(iface) {
        var C = createEventMap[String(iface).toLowerCase()];
        if (!C) throw DOMExceptionOr("Failed to execute 'createEvent': The provided event type ('" + iface + "') is invalid.", 'NotSupportedError');
        var e = new C('');
        st(e).initialized = false;
        var s = st(e);
        var init = e.initEvent;
        define(e, 'initEvent', function (type, bubbles, cancelable) { init.call(this, type, bubbles, cancelable); s.initialized = true; });
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
    function closestAnchor(n) {
        for (; n && n.nodeType === 1; n = n.parentNode)
            if ((n.localName === 'a' || n.localName === 'area') && n.hasAttribute('href')) return n;
        return null;
    }
    function disabledControl(el) {
        var n = el.localName;
        return (n === 'button' || n === 'input' || n === 'select' || n === 'textarea' ||
                n === 'optgroup' || n === 'option' || n === 'fieldset') && el.hasAttribute('disabled');
    }
    function formOf(el) {
        if (el.form !== undefined) return el.form;
        for (var n = el.parentNode; n; n = n.parentNode) if (n.localName === 'form') return n;
        return null;
    }
    function inputType(el) { return String(el.getAttribute('type') || 'text').toLowerCase(); }
    function radioGroup(el) {
        var name = el.getAttribute('name'), root = formOf(el) || rootOf(el);
        if (!name || !root || !root.querySelectorAll) return [];
        return Array.prototype.filter.call(root.querySelectorAll('input[type=radio]'), function (r) {
            return r !== el && r.getAttribute('name') === name && formOf(r) === formOf(el);
        });
    }
    var clicking = false;
    function click() {
        if (disabledControl(this)) return;
        if (clicking && this.__cyberClickInProgress) return;
        this.__cyberClickInProgress = true;
        clicking = true;
        try { activate(this); } finally { this.__cyberClickInProgress = false; clicking = false; }
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
    function activate(el) {
        var view = el.ownerDocument && el.ownerDocument.defaultView || global;
        var isInput = el.localName === 'input', type = isInput ? inputType(el) : '';
        /* legacy-pre-activation behavior */
        var oldChecked, oldRadio = null;
        if (type === 'checkbox') {
            oldChecked = getChecked(el); setChecked(el, !oldChecked);
        } else if (type === 'radio') {
            oldChecked = getChecked(el);
            radioGroup(el).forEach(function (r) { if (getChecked(r)) oldRadio = r; });
            setChecked(el, true);
        }
        var ok = el.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, composed: true,
                                                            view: view, detail: 1 }));
        if (!ok) {
            /* canceled: the legacy-canceled-activation behavior */
            if (type === 'checkbox') setChecked(el, oldChecked);
            else if (type === 'radio') { setChecked(el, oldChecked); if (oldRadio) setChecked(oldRadio, true); }
            return;
        }
        if (type === 'checkbox' || (type === 'radio' && !oldChecked)) {
            fire(el, 'input', Event, { bubbles: true, composed: true });
            fire(el, 'change', Event, { bubbles: true });
            return;
        }
        if ((isInput && (type === 'submit' || type === 'image')) ||
            (el.localName === 'button' && String(el.getAttribute('type') || 'submit').toLowerCase() === 'submit')) {
            var form = formOf(el);
            if (form) requestSubmit(form, el);
            return;
        }
        if (isInput && type === 'reset' || (el.localName === 'button' && String(el.getAttribute('type')).toLowerCase() === 'reset')) {
            var f2 = formOf(el);
            if (f2 && typeof f2.reset === 'function') f2.reset();
            return;
        }
        if (el.localName === 'label') {
            var control = el.control !== undefined ? el.control
                : (el.htmlFor ? (el.ownerDocument.getElementById(el.htmlFor)) : el.querySelector('input,button,select,textarea'));
            if (control && control !== el && typeof control.click === 'function') control.click();
            return;
        }
        var a = closestAnchor(el);
        if (a) followHyperlink(a, view);
    }
    function requestSubmit(form, submitter) {
        var ok = form.dispatchEvent(new global.SubmitEvent('submit', { bubbles: true, cancelable: true,
                                                                       submitter: submitter || null }));
        if (ok && typeof form.__cyberSubmit === 'function') form.__cyberSubmit(submitter);
    }
    function followHyperlink(a, view) {
        var href = a.getAttribute('href');
        if (href === null) return;
        var loc = view.location;
        if (!loc) return;
        var url;
        try { url = new global.URL(href, a.baseURI || loc.href); } catch (e) { return; }
        var cur;
        try { cur = new global.URL(loc.href); } catch (e) { return; }
        /* same-document navigation: a fragment change */
        if (url.origin === cur.origin && url.pathname === cur.pathname && url.search === cur.search) {
            if (url.hash !== cur.hash) loc.hash = url.hash;
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
        'wheel', 'animationstart', 'animationiteration', 'animationend', 'animationcancel',
        'transitionrun', 'transitionstart', 'transitionend', 'transitioncancel', 'pointerover',
        'pointerenter', 'pointerdown', 'pointermove', 'pointerup', 'pointercancel', 'pointerout',
        'pointerleave', 'gotpointercapture', 'lostpointercapture', 'touchstart', 'touchend',
        'touchmove', 'touchcancel', 'selectstart', 'selectionchange'];
    var WINDOW_HANDLERS = ['afterprint', 'beforeprint', 'beforeunload', 'hashchange', 'languagechange',
        'message', 'messageerror', 'offline', 'online', 'pagehide', 'pageshow', 'popstate',
        'rejectionhandled', 'storage', 'unhandledrejection', 'unload', 'pagereveal', 'pageswap'];
    var DOCUMENT_HANDLERS = ['readystatechange', 'visibilitychange', 'fullscreenchange', 'fullscreenerror'];
    function handlerAccessor(P, type) {
        var name = 'on' + type;
        var own = Object.getOwnPropertyDescriptor(P, name);
        if (own && (own.get || own.set)) return;    /* (a native one already) */
        Object.defineProperty(P, name, { configurable: true, enumerable: true,
            get: function () {
                var h = this[HANDLERS];
                if (h && Object.prototype.hasOwnProperty.call(h, type)) return h[type];
                /* the content attribute, compiled on first use */
                if (typeof this.getAttribute === 'function') {
                    var code = this.getAttribute(name);
                    if (code !== null) {
                        var fn = null;
                        try {
                            fn = type === 'error' && this === global
                                ? new Function('event', 'source', 'lineno', 'colno', 'error', code)
                                : new Function('event', code);
                        } catch (e) { report(e); }
                        setHandler(this, type, fn);
                        return fn;
                    }
                }
                return null;
            },
            set: function (v) {
                setHandler(this, type, typeof v === 'function' || (v !== null && typeof v === 'object') ? v : null);
            } });
    }
    function setHandler(target, type, v) {
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
        var target = this == null ? global : this, source = global;
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
            catch (e) { throw new (global.DOMException || Error)("Failed to execute 'pushState': the URL '" + u + "' is invalid.", 'SecurityError'); }
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
            fireHistoryEvents(oldURL, newURL, null, true);
        };
        var HP = Object.getPrototypeOf(hist) && Object.getPrototypeOf(hist) !== Object.prototype ? Object.getPrototypeOf(hist) : hist;
        ['pushState', 'replaceState', 'back', 'forward', 'go'].forEach(function (k) {
            if (Object.prototype.hasOwnProperty.call(hist, k)) delete hist[k];
        });
        ['state', 'length'].forEach(function (k) {
            if (Object.prototype.hasOwnProperty.call(hist, k)) delete hist[k];
        });
        define(HP, 'pushState', function pushState(state, unused, url) { updateState('pushState', state, url, true); });
        define(HP, 'replaceState', function replaceState(state, unused, url) { updateState('replaceState', state, url, false); });
        define(HP, 'back', function back() { traverse(-1); });
        define(HP, 'forward', function forward() { traverse(1); });
        define(HP, 'go', function go(delta) { traverse(delta | 0); });
        Object.defineProperty(HP, 'state', { configurable: true, enumerable: true, get: function () { return entries[index].state; } });
        Object.defineProperty(HP, 'length', { configurable: true, enumerable: true, get: function () { return entries.length; } });
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

    /* for the C entry points (event_api.c), which forward here */
    Object.defineProperty(global, '__cyberEventTarget', { value: Object.freeze({
        add: addEventListener, remove: removeEventListener, dispatch: dispatchEvent }) });
})
