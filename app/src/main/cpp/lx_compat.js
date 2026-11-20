/* 最小 LX 适配；只执行可信音源。没有浏览器、Node、文件或模块接口。 */
(() => {
  'use strict';
  let handler, sources, initialized = false;
  const events = Object.freeze({request: 'request', inited: 'inited', updateAlert: 'updateAlert'});
  const fail = message => { throw new Error(message); };
  const pairs = data => Object.keys(data || {}).map(k =>
    encodeURIComponent(k) + '=' + encodeURIComponent(String(data[k]))).join('&');
  const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  function from(value, encoding = 'utf8') {
    if (typeof value !== 'string') return Uint8Array.from(value);
    encoding = encoding.toLowerCase();
    if (encoding === 'hex') {
      if (value.length % 2 || /[^0-9a-f]/i.test(value)) fail('无效 hex');
      return Uint8Array.from(value.match(/../g) || [], n => parseInt(n, 16));
    }
    if (encoding === 'base64') {
      let s = value.replace(/\s/g, '').replace(/-/g, '+').replace(/_/g, '/');
      if (s.length % 4 === 1 || !/^[A-Za-z0-9+/]*={0,2}$/.test(s)) fail('无效 base64');
      s = s.padEnd(Math.ceil(s.length / 4) * 4, '=');
      const out = []; let bits = 0, n = 0;
      for (const c of s.replace(/=+$/, '')) {
        bits = bits * 64 + alphabet.indexOf(c); n += 6;
        if (n >= 8) { n -= 8; out.push((bits >>> n) & 255); bits &= (1 << n) - 1; }
      }
      return Uint8Array.from(out);
    }
    if (encoding !== 'utf8' && encoding !== 'utf-8') fail('不支持的 buffer 编码');
    const bytes = unescape(encodeURIComponent(value));
    return Uint8Array.from(bytes, c => c.charCodeAt(0));
  }
  function toString(value, encoding = 'utf8') {
    const bytes = Uint8Array.from(value); encoding = encoding.toLowerCase();
    if (encoding === 'hex') return Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
    if (encoding === 'base64') {
      let s = '';
      for (let i = 0; i < bytes.length; i += 3) {
        const n = (bytes[i] << 16) | ((bytes[i + 1] || 0) << 8) | (bytes[i + 2] || 0);
        s += alphabet[n >>> 18] + alphabet[(n >>> 12) & 63] +
          (i + 1 < bytes.length ? alphabet[(n >>> 6) & 63] : '=') +
          (i + 2 < bytes.length ? alphabet[n & 63] : '=');
      }
      return s;
    }
    if (encoding !== 'utf8' && encoding !== 'utf-8') fail('不支持的 buffer 编码');
    return decodeURIComponent(Array.from(bytes, b => '%' + b.toString(16).padStart(2, '0')).join(''));
  }
  globalThis.setTimeout = (fn, delay = 0, ...args) => {
    if (typeof fn !== 'function') fail('timer 必须是函数');
    return __lxQueue('', () => fn(...args), Math.min(30000, Math.max(0, Number(delay) || 0)));
  };
  globalThis.clearTimeout = id => __lxCancel(id);
  globalThis.console = Object.freeze({log() {}, info() {}, warn() {}, error() {}});
  globalThis.lx = Object.freeze({
    version: '2.0.0', env: 'mobile', currentScriptInfo: Object.freeze(__lxScriptInfo),
    EVENT_NAMES: events,
    on(event, fn) {
      if (event !== events.request || typeof fn !== 'function') fail('不支持的 LX 事件');
      handler = fn;
    },
    send(event, data) {
      if (event === events.inited) {
        if (!data || typeof data.sources !== 'object' || !data.sources) fail('inited 缺少 sources');
        sources = data.sources; initialized = true;
      } else if (event !== events.updateAlert) fail('不支持的 LX 消息');
      return Promise.resolve();
    },
    request(url, options, callback) {
      if (typeof options === 'function') { callback = options; options = {}; }
      options = options || {};
      if (typeof callback !== 'function') fail('lx.request 缺少 callback');
      if (options.formData || options.binary || options.encoding === null) fail('暂不支持 multipart/二进制 HTTP');
      if (options.qs) url += (url.includes('?') ? '&' : '?') + pairs(options.qs);
      const headers = Object.assign({}, options.headers);
      let body = options.body;
      if (options.form) { body = pairs(options.form); headers['Content-Type'] = 'application/x-www-form-urlencoded'; }
      if (body != null && typeof body !== 'string') {
        body = JSON.stringify(body); headers['Content-Type'] = 'application/json';
      }
      const request = JSON.stringify({url, method: options.method || 'GET', headers,
        body: body == null ? null : body, timeout: options.timeout || 15000});
      const id = __lxQueue(request, raw => {
        const response = JSON.parse(raw);
        if (response.error) { callback(new Error(response.error), null, null); return; }
        let data = response.body;
        if (options.json !== false) {
          try { data = JSON.parse(data); }
          catch (e) {
            if (options.json === true) { callback(new Error('HTTP JSON 解析失败: ' + e.message), response, null); return; }
          }
        }
        response.body = data;
        callback(null, response, data);
      }, -1);
      return () => __lxCancel(id);
    },
    utils: Object.freeze({buffer: Object.freeze({from, bufToString: toString})})
  });
  globalThis.__lxReady = () => initialized;
  globalThis.__lxInvoke = (provider, action, info) => {
    if (!initialized || !handler) fail('音源未完成初始化或没有 request handler');
    if (!provider) provider = Object.keys(sources).find(key =>
      Array.isArray(sources[key].actions) && sources[key].actions.includes(action));
    if (!provider || !sources[provider] || !Array.isArray(sources[provider].actions) ||
        !sources[provider].actions.includes(action)) fail('音源不支持 ' + action + '；LX 标准源通常不提供 search');
    return Promise.resolve().then(() => handler({source: provider, action, info}));
  };
})();
