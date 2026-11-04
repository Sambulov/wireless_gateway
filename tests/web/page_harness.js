// Запуск скрипта страницы прибора в node без браузера: DOM — универсальная
// заглушка (любое свойство/вызов — снова заглушка), WebSocket и таймеры —
// управляемые подделки. Сценарий читает из stdin {page, steps} и печатает
// JSON-отчёт: что страница отправила в WebSocket.
const fs = require('fs');
const vm = require('vm');

function stub(name) {
  const f = function () { return stub(name + '()'); };
  return new Proxy(f, {
    get(t, k) {
      if (k === Symbol.toPrimitive) return () => (k === 'number' ? 0 : '');
      if (k === 'value') return t._value !== undefined ? t._value : '';
      if (k === 'length') return 0;
      if (k === 'then') return undefined;
      if (k in t) return t[k];
      return (t[k] = stub(name + '.' + String(k)));
    },
    set(t, k, v) { if (k === 'value') t._value = v; else t[k] = v; return true; },
    apply() { return stub(name + '()'); },
    construct() { return stub('new ' + name); },
  });
}

const input = JSON.parse(fs.readFileSync(0, 'utf8'));
const html = fs.readFileSync(input.page, 'utf8');
const code = [...html.matchAll(/<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi)].map(m => m[1]).join('\n');

const elements = {};
const document = stub('document');
document.getElementById = id => (elements[id] = elements[id] || stub('#' + id));
for (const [id, v] of Object.entries(input.values || {})) document.getElementById(id).value = v;

let now = 0;
const timers = [];
const sockets = [];
class FakeWS {
  constructor(url) { this.url = url; this.readyState = 0; this.sent = []; sockets.push(this); }
  send(d) { this.sent.push(d); }
  close() { this.readyState = 3; }
}
FakeWS.OPEN = 1;
const ctx = {
  document, window: stub('window'), console: { log() {}, error() {}, warn() {} },
  WebSocket: FakeWS, JSON, Math, Date: { now: () => now }, Array, Object, Map, Set, Number, String,
  parseInt, parseFloat, isNaN, Promise,
  setTimeout: (f, ms) => { timers.push({ f, at: now + (ms || 0), every: 0 }); return timers.length; },
  setInterval: (f, ms) => { timers.push({ f, at: now + ms, every: ms }); return timers.length; },
  clearInterval: id => { if (timers[id - 1]) timers[id - 1].dead = true; },
  clearTimeout: id => { if (timers[id - 1]) timers[id - 1].dead = true; },
  requestAnimationFrame: () => 0, performance: { now: () => now },
  location: { hostname: 'gw', port: '' }, alert() {}, confirm: () => true,
};
ctx.window.location = ctx.location;
vm.createContext(ctx);
vm.runInContext(code, ctx);

function advance(ms) {
  const end = now + ms;
  for (;;) {
    const due = timers.filter(t => !t.dead && t.at <= end).sort((a, b) => a.at - b.at)[0];
    if (!due) break;
    now = due.at;
    if (due.every) due.at += due.every; else due.dead = true;
    due.f();
  }
  now = end;
}
const ws = () => sockets[sockets.length - 1];
const report = [];
for (const st of input.steps) {
  if (st.open) { ws().readyState = 1; ws().onopen(); }
  if (st.advance) advance(st.advance);
  if (st.reply_all) {                       // ответить на все неотвеченные запросы
    const w = ws();
    for (const raw of w.sent.splice(0)) {
      const req = JSON.parse(raw);
      if (req.FID && req.FID.endsWith('.modbus'))
        w.onmessage({ data: JSON.stringify({ FID: req.FID, SID: req.SID, ARG: { RA: req.ARG.RA, RC: 0, RD: [] } }) });
    }
  }
  if (st.mark) report.push({ mark: st.mark, sent: ws().sent.map(s => JSON.parse(s)) });
  if (st.drain) ws().sent.length = 0;
  if (st.close) { const w = ws(); w.readyState = 3; if (w.onclose) w.onclose(); }
}
console.log(JSON.stringify(report));
