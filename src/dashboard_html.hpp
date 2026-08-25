#pragma once

namespace swiftkv {

/// The dashboard, embedded in the binary as a raw string literal.
///
/// Embedded rather than read from disk so the server has no runtime file
/// dependency: it cannot be started from a directory where the asset is missing
/// and then serve a broken page. It is a single file with no external requests,
/// so it also works on a host with no internet access.
///
/// Every figure it shows comes from /stats.json. There are no placeholder or
/// sample numbers anywhere in it -- before the first poll returns, fields read
/// as "--" rather than as a plausible-looking zero.
///
/// The one number the server does not provide is the live request rate. A rate
/// needs a window, and any window chosen server-side would be arbitrary, so the
/// page derives it from the change in the command counter between two polls and
/// labels it with the interval it actually measured.
inline constexpr const char* kDashboardHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>SwiftKV</title>
<style>
  :root {
    --bg: #f6f7f9; --panel: #ffffff; --ink: #16191d; --muted: #6b7280;
    --line: #e4e7eb; --accent: #2563eb; --ok: #15803d; --warn: #b45309;
    --bad: #b91c1c;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg: #0f1216; --panel: #171b21; --ink: #e6e9ee; --muted: #9aa4b2;
      --line: #262c35; --accent: #60a5fa; --ok: #4ade80; --warn: #fbbf24;
      --bad: #f87171;
    }
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 24px; background: var(--bg); color: var(--ink);
    font: 15px/1.5 ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif;
  }
  header { display: flex; align-items: baseline; gap: 12px; margin-bottom: 4px; }
  h1 { font-size: 20px; margin: 0; letter-spacing: -0.01em; }
  .sub { color: var(--muted); font-size: 13px; margin-bottom: 20px; }
  .dot { width: 9px; height: 9px; border-radius: 50%; display: inline-block;
         background: var(--muted); margin-right: 6px; vertical-align: middle; }
  .dot.up { background: var(--ok); }
  .dot.down { background: var(--bad); }
  .grid { display: grid; gap: 14px; grid-template-columns: repeat(auto-fit, minmax(210px, 1fr)); }
  .card { background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 14px 16px; }
  .card h2 { font-size: 11px; text-transform: uppercase; letter-spacing: 0.06em;
             color: var(--muted); margin: 0 0 8px; font-weight: 600; }
  .value { font-size: 26px; font-weight: 600; font-variant-numeric: tabular-nums; letter-spacing: -0.02em; }
  .unit { font-size: 13px; color: var(--muted); font-weight: 400; margin-left: 4px; }
  .note { font-size: 12px; color: var(--muted); margin-top: 6px; }
  section { margin-top: 26px; }
  section > h2 { font-size: 13px; text-transform: uppercase; letter-spacing: 0.06em;
                 color: var(--muted); margin: 0 0 10px; font-weight: 600; }
  table { width: 100%; border-collapse: collapse; background: var(--panel);
          border: 1px solid var(--line); border-radius: 10px; overflow: hidden; }
  th, td { text-align: left; padding: 9px 14px; border-bottom: 1px solid var(--line);
           font-variant-numeric: tabular-nums; }
  th { font-size: 12px; color: var(--muted); font-weight: 600; }
  tr:last-child td { border-bottom: none; }
  td.num { text-align: right; }
  .bar { height: 6px; background: var(--line); border-radius: 3px; overflow: hidden; margin-top: 8px; }
  .bar > i { display: block; height: 100%; background: var(--accent); width: 0; transition: width .3s; }
  footer { margin-top: 26px; color: var(--muted); font-size: 12px; }
  code { background: var(--bg); padding: 1px 5px; border-radius: 4px; font-size: 12px; }
  .err { color: var(--bad); }
</style>
</head>
<body>
<header>
  <h1>SwiftKV</h1>
  <span id="status"><span class="dot"></span><span id="statusText">connecting…</span></span>
</header>
<div class="sub" id="uptime">&mdash;</div>

<div class="grid">
  <div class="card">
    <h2>Requests / sec</h2>
    <div class="value" id="rps">&mdash;</div>
    <div class="note" id="rpsNote">measured between polls</div>
  </div>
  <div class="card">
    <h2>Active connections</h2>
    <div class="value" id="conns">&mdash;</div>
    <div class="note" id="connsNote">&mdash;</div>
  </div>
  <div class="card">
    <h2>Keys stored</h2>
    <div class="value" id="keys">&mdash;</div>
    <div class="note" id="keysNote">&mdash;</div>
  </div>
  <div class="card">
    <h2>Cache hit rate</h2>
    <div class="value" id="hit">&mdash;</div>
    <div class="bar"><i id="hitBar"></i></div>
    <div class="note" id="hitNote">&mdash;</div>
  </div>
</div>

<section>
  <h2>Server-side latency</h2>
  <table>
    <thead><tr><th>Percentile</th><th class="num">Microseconds</th></tr></thead>
    <tbody>
      <tr><td>p50</td><td class="num" id="p50">&mdash;</td></tr>
      <tr><td>p90</td><td class="num" id="p90">&mdash;</td></tr>
      <tr><td>p95</td><td class="num" id="p95">&mdash;</td></tr>
      <tr><td>p99</td><td class="num" id="p99">&mdash;</td></tr>
      <tr><td>p99.9</td><td class="num" id="p999">&mdash;</td></tr>
      <tr><td>max</td><td class="num" id="pmax">&mdash;</td></tr>
    </tbody>
  </table>
  <div class="note">
    Service time inside the server: parse, execute, encode. It excludes network
    transit, so it is smaller than what a client measures. Bucketed with at most
    6.25% overstatement &mdash; never understated.
  </div>
</section>

<section>
  <h2>Commands</h2>
  <table>
    <thead><tr><th>Type</th><th class="num">Count</th></tr></thead>
    <tbody>
      <tr><td>Total</td><td class="num" id="cTotal">&mdash;</td></tr>
      <tr><td>GET</td><td class="num" id="cGets">&mdash;</td></tr>
      <tr><td>SET</td><td class="num" id="cSets">&mdash;</td></tr>
      <tr><td>DEL</td><td class="num" id="cDels">&mdash;</td></tr>
      <tr><td>Errors</td><td class="num err" id="cErrs">&mdash;</td></tr>
    </tbody>
  </table>
</section>

<section>
  <h2>Connections &amp; traffic</h2>
  <table>
    <tbody>
      <tr><td>Accepted (total)</td><td class="num" id="accepted">&mdash;</td></tr>
      <tr><td>Rejected (limit reached)</td><td class="num" id="rejected">&mdash;</td></tr>
      <tr><td>Bytes read</td><td class="num" id="bread">&mdash;</td></tr>
      <tr><td>Bytes written</td><td class="num" id="bwritten">&mdash;</td></tr>
      <tr><td>Evictions</td><td class="num" id="evict">&mdash;</td></tr>
      <tr><td>Persistence</td><td class="num" id="persist">&mdash;</td></tr>
      <tr><td>Store shards</td><td class="num" id="shards">&mdash;</td></tr>
    </tbody>
  </table>
</section>

<footer>
  Live data from <code>/stats.json</code>, polled every second. Prometheus
  metrics at <code>/metrics</code>; health at <code>/health</code>.
  Nothing on this page is hardcoded &mdash; before the first successful poll
  every field reads &ldquo;&mdash;&rdquo;.
</footer>

<script>
(function () {
  var prev = null;
  var fmt = new Intl.NumberFormat();

  function set(id, text) { document.getElementById(id).textContent = text; }
  function num(v, digits) {
    if (v === null || v === undefined) return '—';
    return digits ? v.toFixed(digits) : fmt.format(v);
  }
  function bytes(n) {
    if (n === null || n === undefined) return '—';
    var units = ['B', 'KB', 'MB', 'GB', 'TB'], i = 0, v = n;
    while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
    return (i === 0 ? v : v.toFixed(1)) + ' ' + units[i];
  }
  function duration(s) {
    if (s === null || s === undefined) return '—';
    var d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600);
    var m = Math.floor(s % 3600 / 60), sec = Math.floor(s % 60);
    if (d) return d + 'd ' + h + 'h ' + m + 'm';
    if (h) return h + 'h ' + m + 'm ' + sec + 's';
    if (m) return m + 'm ' + sec + 's';
    return sec + 's';
  }
  function status(up, text) {
    var dot = document.querySelector('#status .dot');
    dot.className = 'dot ' + (up ? 'up' : 'down');
    set('statusText', text);
  }

  function render(d) {
    status(true, 'running');
    set('uptime', 'up ' + duration(d.uptime_seconds) +
        '  ·  version ' + d.version + '  ·  ' +
        num(d.latency_us.count) + ' commands timed');

    // Rate is derived from the change between two polls, and labelled with the
    // interval actually observed rather than the interval we asked for.
    if (prev) {
      var dt = d.uptime_seconds - prev.uptime_seconds;
      var dc = d.commands.total - prev.commands.total;
      if (dt > 0) {
        set('rps', num(Math.round(dc / dt)));
        set('rpsNote', 'over the last ' + dt.toFixed(1) + 's');
      }
    } else {
      set('rpsNote', 'waiting for a second sample…');
    }

    set('conns', num(d.connections.current));
    set('connsNote', num(d.connections.accepted) + ' accepted since start');

    set('keys', num(d.keyspace.keys));
    set('keysNote', bytes(d.keyspace.bytes) + ' stored');

    var lookups = d.keyspace.hits + d.keyspace.misses;
    if (lookups > 0) {
      set('hit', (d.keyspace.hit_rate * 100).toFixed(1) + '%');
      document.getElementById('hitBar').style.width = (d.keyspace.hit_rate * 100) + '%';
      set('hitNote', num(d.keyspace.hits) + ' hits / ' + num(lookups) + ' lookups');
    } else {
      set('hit', '—');
      set('hitNote', 'no lookups yet');
    }

    ['p50', 'p90', 'p95', 'p99', 'p999'].forEach(function (k) {
      set(k, d.latency_us.count ? num(d.latency_us[k], 2) : '—');
    });
    set('pmax', d.latency_us.count ? num(d.latency_us.max, 2) : '—');

    set('cTotal', num(d.commands.total));
    set('cGets', num(d.commands.gets));
    set('cSets', num(d.commands.sets));
    set('cDels', num(d.commands.deletes));
    set('cErrs', num(d.commands.errors));

    set('accepted', num(d.connections.accepted));
    set('rejected', num(d.connections.rejected));
    set('bread', bytes(d.traffic.bytes_read));
    set('bwritten', bytes(d.traffic.bytes_written));
    set('evict', num(d.keyspace.evictions));
    set('persist', d.server.persistence ? 'append-only log' : 'disabled');
    set('shards', num(d.server.shards));

    prev = d;
  }

  function poll() {
    fetch('stats.json', { cache: 'no-store' })
      .then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(render)
      .catch(function (e) { status(false, 'unreachable (' + e.message + ')'); });
  }

  poll();
  setInterval(poll, 1000);
})();
</script>
</body>
</html>
)HTML";

}  // namespace swiftkv
