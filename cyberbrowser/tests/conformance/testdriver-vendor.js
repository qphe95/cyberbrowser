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

    /* An origin (viewport, pointer or an element, whose center it is) as
     * coordinates in the window. */
    function at(w, action, last) {
        const o = action.origin;
        if (o === 'pointer') return [last[0] + action.x, last[1] + action.y];
        if (o && typeof o === 'object') {
            const r = o.getBoundingClientRect();
            return [r.left + r.width / 2 + action.x, r.top + r.height / 2 + action.y];
        }
        return [action.x, action.y];
    }

    internal.action_sequence = async function (actions, context) {
        /* (no touch or pen input here: say so, rather than a test waiting for it) */
        if (actions.some(s => s.type === 'pointer' && s.parameters && s.parameters.pointerType && s.parameters.pointerType !== 'mouse'))
            throw new Error('action_sequence: only mouse pointers are implemented');
        const w = context || window;
        const input = w.__cyberInput;
        const ticks = actions.reduce((n, s) => Math.max(n, s.actions.length), 0);
        const last = new Map();
        for (let t = 0; t < ticks; t++) {
            let wait = 0;
            for (const source of actions) {
                const a = source.actions[t];
                if (!a) continue;
                if (a.duration > wait) wait = a.duration;
                const pos = last.get(source.id) || [0, 0];
                switch (a.type) {
                case 'pointerMove': {
                    const p = at(w, a, pos);
                    last.set(source.id, p);
                    input.move(p[0], p[1]);
                    break;
                }
                case 'pointerDown': input.down(pos[0], pos[1], a.button || 0); break;
                case 'pointerUp': input.up(pos[0], pos[1], a.button || 0); break;
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
    };
})();
