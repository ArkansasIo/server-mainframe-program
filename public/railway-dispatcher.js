'use strict';

(function dispatcher(global) {
  const esc = (v) => String(v ?? '').replace(/[&<>\"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  let root = null;
  let timer = null;
  let state = null;

  async function api(path, options) {
    if (global.MFRailwayApi) return path === 'status' ? global.MFRailwayApi.status() : global.MFRailwayApi[path]();
    const r = await fetch(`/api/railway/${path}`, { cache: 'no-store', ...(options || {}) });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return r.json();
  }

  function render() {
    if (!root || !state) return;
    const s = state;
    root.innerHTML = `
      <div class="rc-dispatcher">
        <header class="rcd-head"><div><strong>MAINFRAME RAILWAY DISPATCHER</strong><span>SIMULATION / DESIGN CONSOLE</span></div><button data-close>×</button></header>
        <nav class="rcd-tabs"><button class="active" data-tab="overview">Overview</button><button data-tab="trains">Trains</button><button data-tab="track">Track</button><button data-tab="interlocking">Interlocking</button><button data-tab="events">Events</button></nav>
        <section class="rcd-body" data-view="overview">
          <div class="rcd-grid">
            <article class="rc-card"><h3>System</h3><div class="rc-metrics"><b>${esc(s.system.mode)}</b><span>Interlocking ${esc(s.system.interlocking)}</span><span>ATC ${esc(s.system.atc)}</span><span>Track circuits ${esc(s.system.trackCircuits)}</span></div></article>
            <article class="rc-card"><h3>Traffic</h3><div class="rc-metrics"><b>${s.trains.length}</b><span>trains</span><span>${s.sections.filter(x => x.occupied).length} occupied sections</span></div></article>
          </div>
          <div class="rc-card"><h3>Track occupancy</h3><div class="rc-trackline">${s.sections.map(x => `<div class="rc-section ${x.occupied ? 'occupied' : ''}"><b>${esc(x.id)}</b><small>${esc(x.name)}</small>${x.trainId ? `<em>${esc(x.trainId)}</em>` : ''}</div>`).join('')}</div></div>
          <div class="rc-card"><h3>Route plan</h3>${s.routes.map(r => `<div class="rc-route"><b>${esc(r.id)}</b><span>${esc(r.from)} → ${esc(r.to)}</span><i>${esc(r.state)}</i></div>`).join('')}</div>
        </section>
        <section class="rcd-body hidden" data-view="trains"><div class="rc-card"><h3>Train control</h3><div class="rc-table">${s.trains.map(t => `<div class="rc-row"><b>${esc(t.id)}</b><span>${esc(t.type)}</span><span>${esc(t.state)}</span><span>${esc(t.section)}</span><span>${t.speed} km/h</span><span>${esc(t.authority)}</span><span>${esc(t.destination)}</span><button data-run="${esc(t.id)}">RUN</button><button data-hold="${esc(t.id)}">HOLD</button></div>`).join('')}</div></div></section>
        <section class="rcd-body hidden" data-view="track"><div class="rc-card"><h3>Signals</h3>${s.signals.map(x => `<div class="rc-row"><b>${esc(x.id)}</b><span>${esc(x.aspect)}</span><span>${esc(x.section)}</span></div>`).join('')}</div><div class="rc-card"><h3>Points</h3>${s.points.map(x => `<div class="rc-row"><b>${esc(x.id)}</b><span>position ${esc(x.position)}</span><span>${x.locked ? 'LOCKED' : 'FREE'}</span></div>`).join('')}</div></section>
        <section class="rcd-body hidden" data-view="interlocking"><div class="rc-card"><h3>Interlocking routes</h3>${s.routes.map(r => `<div class="rc-route"><b>${esc(r.id)}</b><span>${esc(r.from)} → ${esc(r.to)}</span><i>${esc(r.state)}</i></div>`).join('')}</div></section>
        <section class="rcd-body hidden" data-view="events"><div class="rc-card"><h3>Recent events</h3>${s.events.slice(0, 40).map(e => `<div class="rc-event"><time>${esc(e.time)}</time><b>${esc(e.type)}</b><span>${esc(e.message)}</span></div>`).join('')}</div></section>
        <footer class="rcd-actions"><button data-dispatch>DISPATCH R2</button><button class="danger" data-estop>EMERGENCY STOP</button><button data-reset>RESET E-STOP</button><button data-clear>CLEAR ALARMS</button></footer>
        <p class="rc-warning">Simulation and design environment only. Not certified or intended for real-world railway control.</p>
      </div>`;
    bind();
  }

  async function command(command, args = {}) {
    try { await global.MFRailwayApi.command(command, args); await refresh(); } catch (e) { alert(e.message); }
  }
  async function refresh() {
    try { state = await api('status'); render(); } catch (e) { if (root) root.innerHTML = `<div class="rc-error">Railway API unavailable: ${esc(e.message)}</div>`; }
  }
  function bind() {
    root.querySelector('[data-close]')?.addEventListener('click', close);
    root.querySelectorAll('[data-tab]').forEach((b) => b.addEventListener('click', () => { root.querySelectorAll('[data-tab]').forEach(x => x.classList.toggle('active', x === b)); root.querySelectorAll('[data-view]').forEach(x => x.classList.toggle('hidden', x.dataset.view !== b.dataset.tab)); }));
    root.querySelectorAll('[data-run]').forEach(b => b.addEventListener('click', () => command('RUN', { trainId: b.dataset.run, speed: 20 })));
    root.querySelectorAll('[data-hold]').forEach(b => b.addEventListener('click', () => command('HOLD', { trainId: b.dataset.hold })));
    root.querySelector('[data-dispatch]')?.addEventListener('click', () => command('DISPATCH', { routeId: 'R2' }));
    root.querySelector('[data-estop]')?.addEventListener('click', () => command('ESTOP'));
    root.querySelector('[data-reset]')?.addEventListener('click', () => command('RESET_ESTOP'));
    root.querySelector('[data-clear]')?.addEventListener('click', () => command('CLEAR_ALARMS'));
  }
  function open() {
    if (!root) { root = document.createElement('div'); root.id = 'railway-dispatcher'; root.className = 'rc-overlay'; document.body.appendChild(root); }
    root.classList.add('open'); refresh(); clearInterval(timer); timer = setInterval(refresh, 3000);
  }
  function close() { clearInterval(timer); root?.classList.remove('open'); }
  global.MFRailwayDispatcher = { open, close, refresh };
})(window);
