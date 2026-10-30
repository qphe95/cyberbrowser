/*
 * form_controls.js - the IDL attributes of the form controls (HTML section
 * 4.10): input, textarea, select, option, button, form, label, fieldset.
 * Installed in each realm by platform_api_install (browser/platform_api.c).
 *
 * A control's value and checkedness are state of their own, initialized
 * from (and, until script or the user changes them, following) the content
 * attributes: the "dirty value flag" and "dirty checkedness flag".  That
 * state is kept under a symbol shared by all realms.
 * Called as fn(global, window).
 */
(function (global, win) {
    'use strict';
    var STATE = Symbol.for('cyber.formState');
    function proto(name) { var C = global[name]; return C && C.prototype; }
    function accessor(P, name, get, set) {
        if (!P) return;
        Object.defineProperty(P, name, { get: get, set: set || function () {}, configurable: true, enumerable: true });
    }
    function method(P, name, fn) {
        if (P) Object.defineProperty(P, name, { value: fn, writable: true, configurable: true });
    }
    function state(el) {
        var s = el[STATE];
        if (!s) {
            s = { dirtyValue: false, value: '', dirtyChecked: false, checked: false,
                  dirtySelected: false, selected: false, selStart: 0, selEnd: 0, selDir: 'none' };
            Object.defineProperty(el, STATE, { value: s, configurable: true });
        }
        return s;
    }
    function reflectString(P, prop, attr) {
        attr = attr || prop.toLowerCase();
        accessor(P, prop, function () { var v = this.getAttribute(attr); return v === null ? '' : v; },
                 function (v) { this.setAttribute(attr, String(v)); });
    }
    function reflectBool(P, prop, attr) {
        attr = attr || prop.toLowerCase();
        accessor(P, prop, function () { return this.hasAttribute(attr); },
                 function (v) { if (v) this.setAttribute(attr, ''); else this.removeAttribute(attr); });
    }
    function reflectInt(P, prop, attr, dflt) {
        attr = attr || prop.toLowerCase();
        accessor(P, prop, function () {
            var v = parseInt(this.getAttribute(attr), 10);
            return isNaN(v) ? dflt : v;
        }, function (v) { this.setAttribute(attr, String(v | 0)); });
    }
    function enumerated(P, prop, attr, values, missing, invalid) {
        accessor(P, prop, function () {
            var v = this.getAttribute(attr);
            if (v === null) return missing;
            v = v.toLowerCase();
            return values.indexOf(v) >= 0 ? v : invalid;
        }, function (v) { this.setAttribute(attr, String(v)); });
    }
    function textOf(el) {
        var t = el.textContent;
        return t == null ? '' : String(t);
    }
    function stripNewlines(v) { return v.replace(/[\r\n]/g, ''); }
    function formOwner(el) {
        var id = el.getAttribute && el.getAttribute('form');
        if (id !== null && id !== undefined) {
            var d = el.ownerDocument, f = d && d.getElementById(id);
            return f && f.localName === 'form' ? f : null;
        }
        for (var n = el.parentNode; n; n = n.parentNode) if (n.localName === 'form') return n;
        return null;
    }
    function rootOf(n) { while (n.parentNode) n = n.parentNode; return n; }

    /* ---- input ---- */
    var IP = proto('HTMLInputElement');
    var TYPES = ['hidden', 'text', 'search', 'tel', 'url', 'email', 'password', 'date', 'month', 'week',
                 'time', 'datetime-local', 'number', 'range', 'color', 'checkbox', 'radio', 'file',
                 'submit', 'image', 'reset', 'button'];
    function inputType(el) {
        var v = el.getAttribute('type');
        if (v === null) return 'text';
        v = v.toLowerCase();
        return TYPES.indexOf(v) >= 0 ? v : 'text';
    }
    /* the value mode of a type (HTML 4.10.5.1) */
    function valueMode(type) {
        if (type === 'hidden' || type === 'submit' || type === 'image' || type === 'reset' || type === 'button') return 'default';
        if (type === 'checkbox' || type === 'radio') return 'default/on';
        if (type === 'file') return 'filename';
        return 'value';
    }
    function sanitize(type, v) {
        switch (type) {
        case 'text': case 'search': case 'tel': case 'password': return stripNewlines(v);
        case 'url': case 'email': return stripNewlines(v).trim();
        case 'number': return v !== '' && isFinite(Number(v)) && /^-?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$/.test(v) ? v : '';
        case 'range': {
            var n = Number(v);
            if (v === '' || !isFinite(n)) n = 50;
            return String(n);
        }
        case 'color': return /^#[0-9a-fA-F]{6}$/.test(v) ? v.toLowerCase() : '#000000';
        default: return v;
        }
    }
    if (IP) {
        accessor(IP, 'type', function () { return inputType(this); },
                 function (v) { this.setAttribute('type', String(v)); });
        accessor(IP, 'value', function () {
            var t = inputType(this), mode = valueMode(t), s = state(this);
            if (mode === 'value') return s.dirtyValue ? s.value : sanitize(t, this.getAttribute('value') || '');
            if (mode === 'default/on') { var a = this.getAttribute('value'); return a === null ? 'on' : a; }
            if (mode === 'filename') return '';
            var d = this.getAttribute('value'); return d === null ? '' : d;
        }, function (v) {
            v = String(v);
            var t = inputType(this), mode = valueMode(t), s = state(this);
            if (mode === 'value') {
                s.value = sanitize(t, v); s.dirtyValue = true;
                s.selStart = s.selEnd = s.value.length;
            } else if (mode !== 'filename') {
                this.setAttribute('value', v);
            }
        });
        reflectString(IP, 'defaultValue', 'value');
        accessor(IP, 'checked', function () {
            var s = state(this);
            return s.dirtyChecked ? s.checked : this.hasAttribute('checked');
        }, function (v) {
            var s = state(this);
            s.checked = !!v; s.dirtyChecked = true;
            if (s.checked && inputType(this) === 'radio') uncheckGroup(this);
        });
        reflectBool(IP, 'defaultChecked', 'checked');
        accessor(IP, 'indeterminate', function () { return !!state(this).indeterminate; },
                 function (v) { state(this).indeterminate = !!v; });
        ['name', 'placeholder', 'autocomplete', 'min', 'max', 'step', 'pattern', 'accept', 'alt', 'src',
         'inputMode', 'enterKeyHint', 'dirName', 'formAction', 'formMethod', 'formTarget'].forEach(function (p) {
            reflectString(IP, p);
        });
        ['disabled', 'required', 'multiple', 'autofocus', 'formNoValidate'].forEach(function (p) { reflectBool(IP, p); });
        reflectBool(IP, 'readOnly', 'readonly');
        reflectInt(IP, 'maxLength', 'maxlength', -1);
        reflectInt(IP, 'minLength', 'minlength', -1);
        reflectInt(IP, 'size', 'size', 20);
        accessor(IP, 'form', function () { return formOwner(this); });
        accessor(IP, 'valueAsNumber', function () {
            var t = inputType(this);
            if (t !== 'number' && t !== 'range') return NaN;
            var v = this.value; return v === '' ? NaN : Number(v);
        }, function (n) { this.value = isNaN(n) ? '' : String(n); });
        accessor(IP, 'files', function () { return inputType(this) === 'file' ? [] : null; });
        accessor(IP, 'list', function () {
            var id = this.getAttribute('list'), d = this.ownerDocument;
            var l = id && d ? d.getElementById(id) : null;
            return l && l.localName === 'datalist' ? l : null;
        });
        textSelection(IP, function (el) { var m = valueMode(inputType(el)); return m === 'value' ? el.value : null; });
        method(IP, 'stepUp', function (n) { stepBy(this, n === undefined ? 1 : n); });
        method(IP, 'stepDown', function (n) { stepBy(this, -(n === undefined ? 1 : n)); });
    }
    function stepBy(el, n) {
        var step = Number(el.getAttribute('step')) || 1, v = Number(el.value) || 0;
        el.value = String(v + step * n);
    }
    function uncheckGroup(el) {
        var name = el.getAttribute('name');
        if (!name) return;
        var f = formOwner(el), root = f || rootOf(el);
        if (!root.querySelectorAll) return;
        var all = root.querySelectorAll('input');
        for (var i = 0; i < all.length; i++) {
            var r = all[i];
            if (r !== el && inputType(r) === 'radio' && r.getAttribute('name') === name && formOwner(r) === f) {
                var s = state(r); s.checked = false; s.dirtyChecked = true;
            }
        }
    }
    /* select(), selectionStart/End/Direction, setSelectionRange, setRangeText */
    function textSelection(P, textFn) {
        function clamp(el, s) {
            var t = textFn(el);
            if (t === null) return null;
            var n = t.length;
            if (s.selStart > n) s.selStart = n;
            if (s.selEnd > n) s.selEnd = n;
            return t;
        }
        accessor(P, 'selectionStart', function () {
            var s = state(this); return clamp(this, s) === null ? null : s.selStart;
        }, function (v) { var s = state(this); this.setSelectionRange(v, Math.max(v, s.selEnd)); });
        accessor(P, 'selectionEnd', function () {
            var s = state(this); return clamp(this, s) === null ? null : s.selEnd;
        }, function (v) { this.setSelectionRange(state(this).selStart, v); });
        accessor(P, 'selectionDirection', function () { return state(this).selDir; },
                 function (v) { state(this).selDir = String(v); });
        method(P, 'setSelectionRange', function (start, end, dir) {
            var s = state(this), t = textFn(this);
            if (t === null) throw new (global.DOMException || Error)('not a text control', 'InvalidStateError');
            var n = t.length;
            start = Math.min(start >>> 0, n); end = Math.min(end >>> 0, n);
            if (start > end) start = end;
            s.selStart = start; s.selEnd = end; s.selDir = dir === undefined ? 'none' : String(dir);
        });
        method(P, 'select', function () {
            var t = textFn(this);
            if (t !== null) this.setSelectionRange(0, t.length);
        });
        method(P, 'setRangeText', function (text, start, end, mode) {
            var s = state(this), v = textFn(this);
            if (v === null) throw new (global.DOMException || Error)('not a text control', 'InvalidStateError');
            if (start === undefined) { start = s.selStart; end = s.selEnd; }
            start = Math.min(start >>> 0, v.length); end = Math.min(end >>> 0, v.length);
            this.value = v.slice(0, start) + String(text) + v.slice(end);
            var after = start + String(text).length;
            if (mode === 'select') { s.selStart = start; s.selEnd = after; }
            else if (mode === 'start') { s.selStart = s.selEnd = start; }
            else if (mode === 'end') { s.selStart = s.selEnd = after; }
        });
    }

    /* ---- textarea ---- */
    var TP = proto('HTMLTextAreaElement');
    if (TP) {
        accessor(TP, 'value', function () {
            var s = state(this);
            return s.dirtyValue ? s.value : textOf(this).replace(/\r\n?/g, '\n');
        }, function (v) {
            var s = state(this);
            s.value = String(v).replace(/\r\n?/g, '\n'); s.dirtyValue = true;
            s.selStart = s.selEnd = s.value.length;
        });
        accessor(TP, 'defaultValue', function () { return textOf(this); },
                 function (v) { this.textContent = String(v); });
        accessor(TP, 'textLength', function () { return this.value.length; });
        accessor(TP, 'type', function () { return 'textarea'; });
        accessor(TP, 'form', function () { return formOwner(this); });
        ['name', 'placeholder', 'autocomplete', 'wrap', 'dirName'].forEach(function (p) { reflectString(TP, p); });
        ['disabled', 'required', 'autofocus'].forEach(function (p) { reflectBool(TP, p); });
        reflectBool(TP, 'readOnly', 'readonly');
        reflectInt(TP, 'maxLength', 'maxlength', -1);
        reflectInt(TP, 'minLength', 'minlength', -1);
        reflectInt(TP, 'rows', 'rows', 2);
        reflectInt(TP, 'cols', 'cols', 20);
        textSelection(TP, function (el) { return el.value; });
    }

    /* ---- select / option ---- */
    var SP = proto('HTMLSelectElement'), OP = proto('HTMLOptionElement');
    /* the list of options: the select's option children and those of its
     * optgroup children, in tree order (one native walk of the store) */
    var nativeOptions = global.__cyberOptions;
    function optionsOf(sel) {
        if (nativeOptions) return nativeOptions(sel);
        var out = [];
        (function walk(n) {
            for (var c = n.firstChild; c; c = c.nextSibling) {
                if (c.localName === 'option') out.push(c);
                else if (c.localName === 'optgroup' && n === sel) walk(c);
            }
        })(sel);
        return out;
    }
    function selectOf(opt) {
        var p = opt.parentNode;
        if (p && p.localName === 'optgroup') p = p.parentNode;
        return p && p.localName === 'select' ? p : null;
    }
    function optionSelected(o) {
        var s = state(o);
        return s.dirtySelected ? s.selected : o.hasAttribute('selected');
    }
    function setSelected(o, v) { var s = state(o); s.selected = !!v; s.dirtySelected = true; }
    /* the selectedness setting algorithm, for a single select: the last
     * selected option, else the first non-disabled one (display size 1) */
    function selectedIndexOf(sel, opts) {
        opts = opts || optionsOf(sel);
        var idx = -1;
        for (var i = 0; i < opts.length; i++) if (optionSelected(opts[i])) idx = sel.multiple ? (idx < 0 ? i : idx) : i;
        if (idx < 0 && !sel.multiple && !(Number(sel.getAttribute('size')) > 1)) {
            for (var j = 0; j < opts.length; j++) if (!opts[j].hasAttribute('disabled')) return j;
        }
        return idx;
    }
    if (OP) {
        accessor(OP, 'value', function () { var v = this.getAttribute('value'); return v === null ? this.text : v; },
                 function (v) { this.setAttribute('value', String(v)); });
        accessor(OP, 'text', function () { return textOf(this).replace(/\s+/g, ' ').trim(); },
                 function (v) { this.textContent = String(v); });
        reflectString(OP, 'label');
        reflectBool(OP, 'disabled');
        reflectBool(OP, 'defaultSelected', 'selected');
        accessor(OP, 'selected', function () {
            var sel = selectOf(this);
            if (sel && !sel.multiple) { var opts = optionsOf(sel); return opts[selectedIndexOf(sel, opts)] === this; }
            return optionSelected(this);
        }, function (v) {
            var sel = selectOf(this);
            if (sel && !sel.multiple && v) optionsOf(sel).forEach(function (o) { setSelected(o, false); });
            setSelected(this, v);
        });
        accessor(OP, 'index', function () { var sel = selectOf(this); return sel ? optionsOf(sel).indexOf(this) : 0; });
        accessor(OP, 'form', function () { var sel = selectOf(this); return sel ? formOwner(sel) : null; });
    }
    if (SP) {
        accessor(SP, 'options', function () {
            var l = optionsOf(this);
            var HC = global.HTMLCollection;
            if (HC) Object.setPrototypeOf(l, HC.prototype);
            return l;
        });
        accessor(SP, 'length', function () { return optionsOf(this).length; });
        accessor(SP, 'selectedIndex', function () { return selectedIndexOf(this); }, function (i) {
            var opts = optionsOf(this);
            opts.forEach(function (o, k) { setSelected(o, k === (i | 0)); });
        });
        accessor(SP, 'value', function () {
            var opts = optionsOf(this), o = opts[selectedIndexOf(this, opts)];
            return o ? o.value : '';
        }, function (v) {
            v = String(v);
            var found = false;
            optionsOf(this).forEach(function (o) {
                var hit = !found && o.value === v;
                if (hit) found = true;
                setSelected(o, hit);
            });
        });
        accessor(SP, 'selectedOptions', function () {
            var self = this;
            return optionsOf(this).filter(function (o, i) { return self.multiple ? optionSelected(o) : i === selectedIndexOf(self); });
        });
        accessor(SP, 'type', function () { return this.multiple ? 'select-multiple' : 'select-one'; });
        accessor(SP, 'form', function () { return formOwner(this); });
        reflectString(SP, 'name');
        reflectString(SP, 'autocomplete');
        ['disabled', 'required', 'multiple', 'autofocus'].forEach(function (p) { reflectBool(SP, p); });
        reflectInt(SP, 'size', 'size', 0);
        method(SP, 'item', function (i) { return optionsOf(this)[i >>> 0] || null; });
        method(SP, 'add', function (el, before) {
            var ref = typeof before === 'number' ? optionsOf(this)[before] : before;
            this.insertBefore(el, ref || null);
        });
        method(SP, 'remove', function (i) {
            if (arguments.length === 0) { if (this.parentNode) this.parentNode.removeChild(this); return; }
            var o = optionsOf(this)[i | 0];
            if (o && o.parentNode) o.parentNode.removeChild(o);
        });
    }

    /* ---- button ---- */
    var BP = proto('HTMLButtonElement');
    if (BP) {
        enumerated(BP, 'type', 'type', ['submit', 'reset', 'button'], 'submit', 'submit');
        reflectString(BP, 'value');
        reflectString(BP, 'name');
        ['disabled', 'autofocus', 'formNoValidate'].forEach(function (p) { reflectBool(BP, p); });
        accessor(BP, 'form', function () { return formOwner(this); });
    }

    /* ---- form ---- */
    var FP = proto('HTMLFormElement');
    var LISTED = ['button', 'fieldset', 'input', 'object', 'output', 'select', 'textarea'];
    function elementsOf(form) {
        var out = [];
        var doc = form.ownerDocument, root = doc && rootOf(form) === doc ? doc : rootOf(form);
        var all = root.querySelectorAll ? root.querySelectorAll(LISTED.join(',')) : [];
        for (var i = 0; i < all.length; i++) if (formOwner(all[i]) === form) out.push(all[i]);
        return out;
    }
    if (FP) {
        accessor(FP, 'elements', function () {
            var l = elementsOf(this), HC = global.HTMLCollection;
            l.forEach(function (e) { var n = e.getAttribute('name') || e.id; if (n && !(n in l)) l[n] = e; });
            if (HC) Object.setPrototypeOf(l, HC.prototype);
            return l;
        });
        accessor(FP, 'length', function () { return elementsOf(this).length; });
        ['name', 'action', 'target', 'acceptCharset', 'autocomplete'].forEach(function (p) {
            reflectString(FP, p, p === 'acceptCharset' ? 'accept-charset' : undefined);
        });
        enumerated(FP, 'method', 'method', ['get', 'post', 'dialog'], 'get', 'get');
        enumerated(FP, 'enctype', 'enctype', ['application/x-www-form-urlencoded', 'multipart/form-data', 'text/plain'],
                   'application/x-www-form-urlencoded', 'application/x-www-form-urlencoded');
        reflectBool(FP, 'noValidate', 'novalidate');
        method(FP, 'submit', function () {
            if (typeof this.__cyberSubmit === 'function') this.__cyberSubmit(null);
        });
        method(FP, 'reset', function () {
            var E = global.Event;
            if (!this.dispatchEvent(new E('reset', { bubbles: true, cancelable: true }))) return;
            elementsOf(this).forEach(function (el) {
                var s = el[STATE];
                if (s) { s.dirtyValue = false; s.dirtyChecked = false; }
                if (el.localName === 'select') optionsOf(el).forEach(function (o) { if (o[STATE]) o[STATE].dirtySelected = false; });
            });
        });
        method(FP, 'checkValidity', function () { return true; });
        method(FP, 'reportValidity', function () { return true; });
    }

    /* ---- label / fieldset / output ---- */
    var LABELABLE = ['button', 'input', 'meter', 'output', 'progress', 'select', 'textarea'];
    var LP = proto('HTMLLabelElement');
    if (LP) {
        reflectString(LP, 'htmlFor', 'for');
        accessor(LP, 'control', function () {
            var f = this.getAttribute('for');
            if (f !== null) {
                var d = this.ownerDocument, el = d && d.getElementById(f);
                return el && LABELABLE.indexOf(el.localName) >= 0 ? el : null;
            }
            var all = this.querySelectorAll(LABELABLE.join(','));
            return all.length ? all[0] : null;
        });
        accessor(LP, 'form', function () { var c = this.control; return c ? formOwner(c) : null; });
    }
    var FSP = proto('HTMLFieldSetElement');
    if (FSP) {
        reflectBool(FSP, 'disabled');
        reflectString(FSP, 'name');
        accessor(FSP, 'type', function () { return 'fieldset'; });
        accessor(FSP, 'form', function () { return formOwner(this); });
    }

    /* Checkedness as the user agent changes it (activation behavior,
     * browser/dom_events.js): the state itself, not through the `checked`
     * setter, which pages may wrap (React tracks values that way). */
    if (!global[Symbol.for('cyber.formInternals')])
        Object.defineProperty(global, Symbol.for('cyber.formInternals'), { value: Object.freeze({
            getChecked: function (el) {
                var s = el[STATE];
                return s && s.dirtyChecked ? s.checked : el.hasAttribute('checked');
            },
            setChecked: function (el, v) {
                var s = state(el);
                s.checked = !!v; s.dirtyChecked = true;
                if (s.checked && el.localName === 'input' && inputType(el) === 'radio') uncheckGroup(el);
            }
        }) });

    /* validity: everything is valid (no constraint validation yet) */
    var validity = Object.freeze({ valueMissing: false, typeMismatch: false, patternMismatch: false,
        tooLong: false, tooShort: false, rangeUnderflow: false, rangeOverflow: false, stepMismatch: false,
        badInput: false, customError: false, valid: true });
    [IP, TP, SP, BP, FSP].forEach(function (P) {
        if (!P) return;
        accessor(P, 'validity', function () { return validity; });
        accessor(P, 'validationMessage', function () { return ''; });
        accessor(P, 'willValidate', function () { return true; });
        method(P, 'checkValidity', function () { return true; });
        method(P, 'reportValidity', function () { return true; });
        method(P, 'setCustomValidity', function () {});
        if (P !== FSP) accessor(P, 'labels', function () {
            var d = this.ownerDocument, out = [];
            if (d && d.querySelectorAll) {
                var ls = d.querySelectorAll('label');
                for (var i = 0; i < ls.length; i++) if (ls[i].control === this) out.push(ls[i]);
            }
            return out;
        });
    });
})
