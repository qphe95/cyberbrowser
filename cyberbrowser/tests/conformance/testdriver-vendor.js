// testdriver.js's automation for wpt_server.js: its clicks, keys and action
// sequences go in through the engine's input entry (__cyberInput, dom_events.js),
// the way the browser's own input does.
(function () {
    'use strict';
    const internal = window.test_driver_internal;
    internal.in_automation = true;

    const frame = w => new Promise(r => w.requestAnimationFrame(() => r()));
    const pause = ms => new Promise(r => setTimeout(r, ms));

    internal.click = async function (element, coords) {
        const w = element.ownerDocument.defaultView || window;
        const input = w.__cyberInput;
        input.move(coords.x, coords.y);
        input.down(coords.x, coords.y, 0);
        input.up(coords.x, coords.y, 0);
        await frame(w);
    };

    internal.send_keys = async function (element, keys) {
        const w = element.ownerDocument.defaultView || window;
        if (element.ownerDocument.activeElement !== element) element.focus();
        for (const ch of keys) {
            w.__cyberInput.keyDown(ch, element);
            w.__cyberInput.keyUp(ch, element);
        }
        await frame(w);
    };

    /* An origin (viewport, pointer or an element, the center of what's of it
     * in view: WebDriver "get in view center point") as coordinates in the
     * window. */
    function at(w, action, last) {
        const o = action.origin;
        if (o === 'pointer') return [last[0] + action.x, last[1] + action.y];
        if (o && typeof o === 'object') {
            const r = o.getBoundingClientRect();
            const left = Math.max(0, r.left), right = Math.min(w.innerWidth, r.right);
            const top = Math.max(0, r.top), bottom = Math.min(w.innerHeight, r.bottom);
            return [Math.floor((left + right) / 2) + action.x, Math.floor((top + bottom) / 2) + action.y];
        }
        return [action.x, action.y];
    }

    internal.action_sequence = async function (actions, context) {
        const w = context || window;
        /* (a touch or pen pointer: a finger) */
        const finger = s => s.type === 'pointer' && s.parameters && s.parameters.pointerType && s.parameters.pointerType !== 'mouse';
        const input = w.__cyberInput;
        const ticks = actions.reduce((n, s) => Math.max(n, s.actions.length), 0);
        /* (WebDriver's input state: a pointer stays where an earlier chain left it) */
        const last = w.__cyberPointers || (w.__cyberPointers = new Map());
        /* (each chain numbers its sources anew: the nth pointer of a kind is the same one) */
        const place = new Map(), kinds = new Map();
        for (const s of actions) {
            const kind = s.type + ':' + (s.parameters && s.parameters.pointerType || 'mouse');
            const n = kinds.get(kind) || 0;
            kinds.set(kind, n + 1);
            place.set(s, kind + ':' + n);
        }
        for (let t = 0; t < ticks; t++) {
            let wait = 0;
            for (const source of actions) {
                const a = source.actions[t];
                if (!a) continue;
                if (a.duration > wait) wait = a.duration;
                const pos = last.get(place.get(source)) || [0, 0];
                switch (a.type) {
                case 'pointerMove': {
                    const p = at(w, a, pos);
                    last.set(place.get(source), p);
                    if (!finger(source)) input.move(p[0], p[1]);
                    else if (source.down) input.touchMove(p[0], p[1]);
                    break;
                }
                case 'pointerDown':
                    if (finger(source)) { source.down = true; input.touchDown(pos[0], pos[1]); }
                    else input.down(pos[0], pos[1], a.button || 0);
                    break;
                case 'pointerUp':
                    if (finger(source)) { source.down = false; input.touchUp(pos[0], pos[1]); }
                    else input.up(pos[0], pos[1], a.button || 0);
                    break;
                case 'scroll': {
                    const p = at(w, a, pos);
                    input.wheel(p[0], p[1], a.deltaX || 0, a.deltaY || 0);
                    break;
                }
                case 'keyDown': input.keyDown(a.value, w.document.activeElement); break;
                case 'keyUp': input.keyUp(a.value, w.document.activeElement); break;
                }
            }
            if (wait) await pause(wait);
            await frame(w);
        }
        /* (an action chain's wheel events are a transaction of their own) */
        input.wheelEnd();
    };
})();
