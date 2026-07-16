

(function(){
  function normalizeHeaders(init) {
    var h = {};
    if (init instanceof Headers) {
      init.forEach(function(v,k){ h[k] = v; });
    } else if (Array.isArray(init)) {
      for (var i=0;i<init.length;i++) h[init[i][0]] = init[i][1];
    } else if (init) {
      for (var k in init) h[k] = init[k];
    }
    return h;
  }

  function Headers(init) { this._h = normalizeHeaders(init); }
  Headers.prototype.append = function(name, value) {
    name = String(name); value = String(value);
    if (this.has(name)) this._h[name] += ', ' + value;
    else this._h[name] = value;
  };
  Headers.prototype.delete = function(name) { delete this._h[String(name)]; };
  Headers.prototype.get = function(name) { var v = this._h[String(name)]; return v === undefined ? null : v; };
  Headers.prototype.has = function(name) { return Object.prototype.hasOwnProperty.call(this._h, String(name)); };
  Headers.prototype.set = function(name, value) { this._h[String(name)] = String(value); };
  Headers.prototype.forEach = function(cb, thisArg) { for (var k in this._h) cb.call(thisArg, this._h[k], k, this); };
  Headers.prototype.keys = function() { return Object.keys(this._h)[Symbol.iterator](); };
  Headers.prototype.values = function() { var self=this; return Object.keys(this._h).map(function(k){return self._h[k];})[Symbol.iterator](); };
  Headers.prototype.entries = function() { var self=this; return Object.keys(this._h).map(function(k){return [k, self._h[k]];})[Symbol.iterator](); };
  Headers.prototype[Symbol.iterator] = Headers.prototype.entries;

  function Request(input, init) {
    init = init || {};
    if (input instanceof Request) {
      this.url = init.url !== undefined ? String(init.url) : input.url;
      this.method = init.method !== undefined ? String(init.method) : input.method;
      this.headers = new Headers(init.headers !== undefined ? init.headers : input.headers);
      this.body = init.body !== undefined ? init.body : input.body;
      this.mode = init.mode !== undefined ? init.mode : input.mode;
      this.credentials = init.credentials !== undefined ? init.credentials : input.credentials;
      this.cache = init.cache !== undefined ? init.cache : input.cache;
      this.redirect = init.redirect !== undefined ? init.redirect : input.redirect;
      this.referrer = init.referrer !== undefined ? init.referrer : input.referrer;
      this.referrerPolicy = init.referrerPolicy !== undefined ? init.referrerPolicy : input.referrerPolicy;
      this.integrity = init.integrity !== undefined ? init.integrity : input.integrity;
      this.keepalive = init.keepalive !== undefined ? init.keepalive : input.keepalive;
      this.signal = init.signal !== undefined ? init.signal : input.signal;
    } else {
      this.url = String(input);
      this.method = init.method ? String(init.method) : 'GET';
      this.headers = new Headers(init.headers);
      this.body = init.body !== undefined ? init.body : null;
      this.mode = init.mode || 'cors';
      this.credentials = init.credentials || 'same-origin';
      this.cache = init.cache || 'default';
      this.redirect = init.redirect || 'follow';
      this.referrer = init.referrer !== undefined ? init.referrer : 'about:client';
      this.referrerPolicy = init.referrerPolicy || '';
      this.integrity = init.integrity || '';
      this.keepalive = init.keepalive !== undefined ? init.keepalive : false;
      this.signal = init.signal || null;
    }
    if (this.body && (this.method === 'GET' || this.method === 'HEAD')) {
      throw new TypeError('Body not allowed for GET/HEAD request');
    }
  }
  Request.prototype.clone = function() { return new Request(this); };

  function Response(body, init) {
    init = init || {};
    this.type = 'default';
    this.url = '';
    this.redirected = false;
    this.status = init.status !== undefined ? Number(init.status) : 200;
    this.ok = this.status >= 200 && this.status < 300;
    this.statusText = init.statusText !== undefined ? String(init.statusText) : 'OK';
    this.headers = new Headers(init.headers);
    this.bodyUsed = false;
    this._body = body;
    this._json = undefined;
    this._text = undefined;
  }
  Response.prototype.clone = function() {
    if (this.bodyUsed) throw new TypeError('Already read');
    return new Response(this._body, {status:this.status, statusText:this.statusText, headers:this.headers});
  };
  Response.prototype._consume = function() { if (this.bodyUsed) throw new TypeError('Already read'); this.bodyUsed = true; };
  Response.prototype.text = function() {
    this._consume();
    var self = this;
    return Promise.resolve().then(function(){
      if (self._text !== undefined) return self._text;
      if (self._body === null || self._body === undefined) return '';
      if (typeof self._body === 'string') return self._body;
      return String(self._body);
    });
  };
  Response.prototype.json = function() { this._consume(); return this.text().then(function(t){ return JSON.parse(t); }); };
  Response.prototype.arrayBuffer = function() { this._consume(); return Promise.resolve(new ArrayBuffer(0)); };
  Response.prototype.blob = function() { this._consume(); return Promise.resolve({size:0, type:''}); };
  Response.error = function() { return new Response(null, {status:0}); };
  Response.redirect = function(url, status) { return new Response(null, {status: status||302, headers:{Location: url}}); };
  Response.json = function(data, init) { return new Response(JSON.stringify(data), init); };

  function sameOrigin(a, b) { try { return new URL(a).origin === new URL(b).origin; } catch(e){ return false; } }

  var nativeFetch = window.fetch;
  window.fetch = function(input, init) {
    var req = new Request(input, init);
    var headers = {};
    req.headers.forEach(function(v,k){ headers[k] = v; });

    var docUrl = (typeof document !== 'undefined' && document.URL) ||
                 (typeof location !== 'undefined' && location.href) || 'about:blank';
    var origin = '';
    try { origin = new URL(docUrl).origin; } catch(e){}

    if (req.mode !== 'no-cors' && !headers['Origin']) headers['Origin'] = origin;
    if (req.referrer && req.referrer !== 'no-referrer' && !headers['Referer']) {
      headers['Referer'] = (req.referrer === 'about:client') ? docUrl : req.referrer;
    }
    if (req.credentials === 'include' || (req.credentials === 'same-origin' && sameOrigin(req.url, origin))) {
      try { var cookies = document.cookie; if (cookies) headers['Cookie'] = cookies; } catch(e){}
    }

    return nativeFetch(req.url, {method:req.method, headers:headers, body:req.body, mode:req.mode, credentials:req.credentials}).then(function(nativeResp){
      var resp = new Response(null, {status:nativeResp.status, statusText:nativeResp.statusText, headers:nativeResp.headers});
      resp.url = nativeResp.url || req.url;
      resp.redirected = !!nativeResp.redirected;
      resp._text = nativeResp.__body_text;
      if (nativeResp.__body_json !== undefined) resp._json = nativeResp.__body_json;
      resp.text = function(){ var self=this; self._consume(); return nativeResp.text(); };
      resp.json = function(){ var self=this; self._consume(); return nativeResp.json(); };
      resp.arrayBuffer = function(){ var self=this; self._consume(); return nativeResp.arrayBuffer(); };
      resp.blob = function(){ var self=this; self._consume(); return nativeResp.blob(); };
      return resp;
    });
  };

  window.Headers = Headers;
  window.Request = Request;
  window.Response = Response;

  // Native scheduler implementation is installed by init_browser_api_impl.
})();
