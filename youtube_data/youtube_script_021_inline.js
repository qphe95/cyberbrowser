(function() {
  'use strict';
  var m = typeof Object.defineProperties == "function" ? Object.defineProperty : function(a, b, c) {
    if (a == Array.prototype || a == Object.prototype) return a;
    a[b] = c.value;
    return a
  };

  function n(a) {
    a = ["object" == typeof globalThis && globalThis, a, "object" == typeof window && window, "object" == typeof self && self, "object" == typeof global && global];
    for (var b = 0; b < a.length; ++b) {
      var c = a[b];
      if (c && c.Math == Math) return c
    }
    throw Error("Cannot find global object");
  }
  var q = n(this);

  function r(a, b) {
    if (b) a: {
      var c = q;a = a.split(".");
      for (var d = 0; d < a.length - 1; d++) {
        var f = a[d];
        if (!(f in c)) break a;
        c = c[f]
      }
      a = a[a.length - 1];d = c[a];b = b(d);b != d && b != null && m(c, a, {
        configurable: !0,
        writable: !0,
        value: b
      })
    }
  }

  function t(a) {
    var b = 0;
    return function() {
      return b < a.length ? {
        done: !1,
        value: a[b++]
      } : {
        done: !0
      }
    }
  }

  function u(a) {
    var b = typeof Symbol != "undefined" && Symbol.iterator && a[Symbol.iterator];
    if (b) return b.call(a);
    if (typeof a.length == "number") return {
      next: t(a)
    };
    throw Error(String(a) + " is not an iterable or ArrayLike");
  }

  function v(a) {
    for (var b, c = []; !(b = a.next()).done;) c.push(b.value);
    return c
  }

  function w(a) {
    return a instanceof Array ? a : v(u(a))
  }

  function x(a, b) {
    return Object.prototype.hasOwnProperty.call(a, b)
  }

  function y() {
    for (var a = Number(this), b = [], c = a; c < arguments.length; c++) b[c - a] = arguments[c];
    return b
  }
  r("Symbol", function(a) {
    function b(g) {
      if (this instanceof b) throw new TypeError("Symbol is not a constructor");
      return new c(d + (g || "") + "_" + f++, g)
    }

    function c(g, h) {
      this.g = g;
      m(this, "description", {
        configurable: !0,
        writable: !0,
        value: h
      })
    }
    if (a) return a;
    c.prototype.toString = function() {
      return this.g
    };
    var d = "jscomp_symbol_" + (Math.random() * 1E9 >>> 0) + "_",
      f = 0;
    return b
  });
  r("Symbol.iterator", function(a) {
    if (a) return a;
    a = Symbol("Symbol.iterator");
    m(Array.prototype, a, {
      configurable: !0,
      writable: !0,
      value: function() {
        return z(t(this))
      }
    });
    return a
  });

  function z(a) {
    a = {
      next: a
    };
    a[Symbol.iterator] = function() {
      return this
    };
    return a
  }
  r("WeakMap", function(a) {
    function b(e) {
      this.g = (p += Math.random() + 1).toString();
      if (e) {
        e = u(e);
        for (var k; !(k = e.next()).done;) k = k.value, this.set(k[0], k[1])
      }
    }

    function c() {}

    function d(e) {
      var k = typeof e;
      return k === "object" && e !== null || k === "function"
    }

    function f(e) {
      if (!x(e, h)) {
        var k = new c;
        m(e, h, {
          value: k
        })
      }
    }

    function g(e) {
      var k = Object[e];
      k && (Object[e] = function(l) {
        if (l instanceof c) return l;
        Object.isExtensible(l) && f(l);
        return k(l)
      })
    }
    if (function() {
        if (!a || !Object.seal) return !1;
        try {
          var e = Object.seal({}),
            k = Object.seal({}),
            l = new a([
              [e, 2],
              [k, 3]
            ]);
          if (l.get(e) != 2 || l.get(k) != 3) return !1;
          l.delete(e);
          l.set(k, 4);
          return !l.has(e) && l.get(k) == 4
        } catch (da) {
          return !1
        }
      }()) return a;
    var h = "$jscomp_hidden_" + Math.random();
    g("freeze");
    g("preventExtensions");
    g("seal");
    var p = 0;
    b.prototype.set = function(e, k) {
      if (!d(e)) throw Error("Invalid WeakMap key");
      f(e);
      if (!x(e, h)) throw Error("WeakMap key fail: " + e);
      e[h][this.g] = k;
      return this
    };
    b.prototype.get = function(e) {
      return d(e) && x(e, h) ? e[h][this.g] : void 0
    };
    b.prototype.has = function(e) {
      return d(e) && x(e, h) && x(e[h], this.g)
    };
    b.prototype.delete = function(e) {
      return d(e) && x(e, h) && x(e[h], this.g) ? delete e[h][this.g] : !1
    };
    return b
  });

  function A(a, b) {
    a instanceof String && (a += "");
    var c = 0,
      d = !1,
      f = {
        next: function() {
          if (!d && c < a.length) {
            var g = c++;
            return {
              value: b(g, a[g]),
              done: !1
            }
          }
          d = !0;
          return {
            done: !0,
            value: void 0
          }
        }
      };
    f[Symbol.iterator] = function() {
      return f
    };
    return f
  }
  r("Object.values", function(a) {
    return a ? a : function(b) {
      var c = [],
        d;
      for (d in b) x(b, d) && c.push(b[d]);
      return c
    }
  });
  r("Array.prototype.values", function(a) {
    return a ? a : function() {
      return A(this, function(b, c) {
        return c
      })
    }
  });
  /*

   Copyright The Closure Library Authors.
   SPDX-License-Identifier: Apache-2.0
  */
  var B = this || self;

  function C(a, b) {
    a = a.split(".");
    for (var c = B, d; a.length && (d = a.shift());) a.length || b === void 0 ? c[d] && c[d] !== Object.prototype[d] ? c = c[d] : c = c[d] = {} : c[d] = b
  }

  function D(a) {
    return Object.prototype.hasOwnProperty.call(a, E) && a[E] || (a[E] = ++F)
  }
  var E = "closure_uid_" + (Math.random() * 1E9 >>> 0),
    F = 0;
  Math.max.apply(Math, w(Object.values({
    v: 1,
    u: 2,
    o: 4,
    D: 8,
    G: 16,
    B: 32,
    h: 64,
    l: 128,
    i: 256,
    F: 512,
    j: 1024,
    m: 2048,
    C: 4096,
    A: 8192
  })));

  function G(a, b) {
    this.width = a;
    this.height = b
  }
  G.prototype.aspectRatio = function() {
    return this.width / this.height
  };
  G.prototype.ceil = function() {
    this.width = Math.ceil(this.width);
    this.height = Math.ceil(this.height);
    return this
  };
  G.prototype.floor = function() {
    this.width = Math.floor(this.width);
    this.height = Math.floor(this.height);
    return this
  };
  G.prototype.round = function() {
    this.width = Math.round(this.width);
    this.height = Math.round(this.height);
    return this
  };

  function H() {
    var a = document;
    var b = "DIV";
    a.contentType === "application/xhtml+xml" && (b = b.toLowerCase());
    return a.createElement(b)
  };
  var I = new WeakMap;

  function J(a, b) {
    a = [a];
    for (var c = b.length - 1; c >= 0; --c) a.push(typeof b[c], b[c]);
    return a.join("\v")
  };

  function K(a, b, c) {
    if (b instanceof G) c = b.height, b = b.width;
    else if (c == void 0) throw Error("missing height argument");
    a.style.width = L(b);
    a.style.height = L(c)
  }

  function L(a) {
    typeof a == "number" && (a = Math.round(a) + "px");
    return a
  };
  (function(a, b) {
    function c(f) {
      f = u(f);
      f.next();
      f = v(f);
      return b(d, f)
    }
    b = b === void 0 ? J : b;
    var d = D(a);
    return function() {
      var f = y.apply(0, arguments),
        g = this || B,
        h = I.get(g);
      h || (h = {}, I.set(g, h));
      g = h;
      h = [this].concat(w(f));
      f = c ? c(h) : h;
      if (Object.prototype.hasOwnProperty.call(g, f)) g = g[f];
      else {
        var p = u(h);
        h = p.next().value;
        p = v(p);
        h = a.apply(h, p);
        g = g[f] = h
      }
      return g
    }
  })(function(a) {
    var b = H();
    a && (b.className = a);
    b.style.cssText = "overflow:auto;position:absolute;top:0;width:100px;height:100px";
    a = H();
    K(a, "200px", "200px");
    b.appendChild(a);
    document.body.appendChild(b);
    a = b.offsetWidth - b.clientWidth;
    b && b.parentNode && b.parentNode.removeChild(b);
    return a
  });
  var M = B.window,
    N, O, P = (M == null ? void 0 : (N = M.yt) == null ? void 0 : N.config_) || (M == null ? void 0 : (O = M.ytcfg) == null ? void 0 : O.data_) || {};
  C("yt.config_", P);

  function Q() {
    var a = {};
    return "EXPERIMENT_FLAGS" in P ? P.EXPERIMENT_FLAGS : a
  };

  function R(a) {
    a = Q()[a];
    return typeof a === "string" && a === "false" ? !1 : !!a
  }

  function S(a, b) {
    a = Q()[a];
    return a === void 0 && b !== void 0 ? b : Number(a || 0)
  };
  var T = S("kevlar_watch_page_horizontal_margin", 24),
    U = S("kevlar_watch_page_columns_top_padding", 24),
    V = 426 + T * 2,
    W = S("kevlar_watch_two_column_width_threshold", 1E3),
    X = T * 2,
    aa = S("kevlar_watch_secondary_width", 402),
    ba = S("kevlar_watch_max_player_width", 1280),
    ca = S("kevlar_watch_flexy_metadata_height", 136);

  function Y(a, b, c) {
    c = c === void 0 ? !1 : c;
    var d = Math.max(a.width, V);
    d = a.width >= W ? d - (X + aa + T) : d - X;
    d = Math.min(d, ba);
    var f = 240;
    c && (f = 380);
    b < .5624 || (!R("kevlar_watch_flexy_disable_small_window_sizing") && (a.height < 630 && a.width >= 657 || a.height >= 630 && a.width >= W && a.width < 1327) ? f = 360 : !R("kevlar_watch_flexy_disable_large_window_sizing") && a.height >= 630 && a.width >= 1327 && (f = 480));
    var g = Math.min(d * b, a.height - (56 + U + ca));
    g = Math.max(g, f);
    b < 1 ? d = g / b : a.width >= W && (d = Y(a, .5625, c).width);
    return new G(Math.round(d), Math.round(g))
  }

  function Z(a) {
    if (a) {
      var b = .5625,
        c = a.querySelector(".html5-video-player");
      c && typeof c.getVideoAspectRatio === "function" && (b = 1 / c.getVideoAspectRatio());
      c = window.document;
      c = c.compatMode == "CSS1Compat" ? c.documentElement : c.body;
      K(a, Y(new G(c.clientWidth, c.clientHeight), b))
    }
  };
  C("yt.flexy.setPlayerlikeElementSize", Z);
  R("desktop_delay_player_resizing") || Z(document.querySelector("#player.skeleton #player-api"));
}).call(this);