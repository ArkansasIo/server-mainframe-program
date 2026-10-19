'use strict';

(function designer(global) {
  const KEY = 'mf-railway-layout-v1';
  const base = { name: 'MAINFRAME DEMO LAYOUT', sections: [], points: [], signals: [], routes: [] };
  let layout = load();
  let host = null;
  const esc = (v) => String(v ?? '').replace(/[&<>\"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  function load() { try { return JSON.parse(localStorage.getItem(KEY)) || structuredClone(base); } catch { return structuredClone(base); } }
  function save() { localStorage.setItem(KEY, JSON.stringify(layout)); render(); }
  function render() {
    if (!host) return;
    host.innerHTML = `<div class="rc-designer"><header><b>RAILWAY LAYOUT DESIGNER</b><button data-close>×</button></header><div class="rcd-form"><label>Layout name<input data-name value="${esc(layout.name)}"></label><button data-section>+ Section</button><button data-point>+ Point</button><button data-signal>+ Signal</button><button data-route>+ Route</button><button data-export>Export JSON</button><label class="filebtn">Import JSON<input type="file" accept="application/json" data-import></label></div><div class="rc-design-grid"><article><h3>Sections</h3>${layout.sections.map((x,i)=>`<div class="rc-design-row"><input data-field="sections" data-i="${i}" data-k="id" value="${esc(x.id)}"><input data-field="sections" data-i="${i}" data-k="name" value="${esc(x.name)}"><button data-del="sections" data-i="${i}">×</button></div>`).join('')}</article><article><h3>Points</h3>${layout.points.map((x,i)=>`<div class="rc-design-row"><input data-field="points" data-i="${i}" data-k="id" value="${esc(x.id)}"><input data-field="points" data-i="${i}" data-k="position" value="${esc(x.position)}"><button data-del="points" data-i="${i}">×</button></div>`).join('')}</article><article><h3>Signals</h3>${layout.signals.map((x,i)=>`<div class="rc-design-row"><input data-field="signals" data-i="${i}" data-k="id" value="${esc(x.id)}"><input data-field="signals" data-i="${i}" data-k="aspect" value="${esc(x.aspect)}"><button data-del="signals" data-i="${i}">×</button></div>`).join('')}</article><article><h3>Routes</h3>${layout.routes.map((x,i)=>`<div class="rc-design-row"><input data-field="routes" data-i="${i}" data-k="id" value="${esc(x.id)}"><input data-field="routes" data-i="${i}" data-k="from" value="${esc(x.from)}"><input data-field="routes" data-i="${i}" data-k="to" value="${esc(x.to)}"><button data-del="routes" data-i="${i}">×</button></div>`).join('')}</article></div></div>`;
    host.querySelector('[data-close]').onclick = close;
    host.querySelector('[data-name]').onchange = e => { layout.name=e.target.value; save(); };
    host.querySelector('[data-section]').onclick=()=>{layout.sections.push({id:`T${layout.sections.length+1}`,name:'New Section'});save();};
    host.querySelector('[data-point]').onclick=()=>{layout.points.push({id:`P${layout.points.length+1}`,position:'NORMAL'});save();};
    host.querySelector('[data-signal]').onclick=()=>{layout.signals.push({id:`S${layout.signals.length+1}`,aspect:'RED'});save();};
    host.querySelector('[data-route]').onclick=()=>{layout.routes.push({id:`R${layout.routes.length+1}`,from:'T1',to:'T2'});save();};
    host.querySelectorAll('[data-field]').forEach(e=>e.onchange=()=>{layout[e.dataset.field][Number(e.dataset.i)][e.dataset.k]=e.value;save();});
    host.querySelectorAll('[data-del]').forEach(e=>e.onclick=()=>{layout[e.dataset.del].splice(Number(e.dataset.i),1);save();});
    host.querySelector('[data-export]').onclick=()=>{const blob=new Blob([JSON.stringify(layout,null,2)],{type:'application/json'});const a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download='railway-layout.json';a.click();URL.revokeObjectURL(a.href);};
    host.querySelector('[data-import]').onchange=async e=>{const f=e.target.files[0];if(!f)return;try{layout=JSON.parse(await f.text());save();}catch{alert('Invalid railway layout JSON');}};
  }
  function open(){if(!host){host=document.createElement('div');host.className='rc-overlay';document.body.appendChild(host);}host.classList.add('open');render();}
  function close(){host?.classList.remove('open');}
  global.MFRailwayDesigner={open,close,getLayout:()=>structuredClone(layout)};
})(window);
