'use strict';

(function consolePanel(global) {
  let host;
  const esc = v => String(v ?? '').replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  async function run(line) {
    const parts=line.trim().split(/\s+/); const command=parts.shift()?.toUpperCase(); if(!command)return;
    const args={}; if(parts[0])args.trainId=parts[0];
    try { const r=await global.MFRailwayApi.command(command,args); append(JSON.stringify(r)); } catch(e){append(`ERROR: ${e.message}`);}
  }
  function append(text){const out=host.querySelector('[data-out]');out.textContent+=`${out.textContent?'\n':''}${text}`;out.scrollTop=out.scrollHeight;}
  function open(){if(!host){host=document.createElement('div');host.className='rc-overlay';host.innerHTML='<div class="rc-console"><header><b>RAILWAY COMMAND CONSOLE</b><button data-close>×</button></header><pre data-out>MAINFRAME RAILWAY CONTROL SIMULATOR\nType STATUS, LIST_TRAINS, RUN 1A34, HOLD 1A34, ESTOP, RESET_ESTOP, CLEAR_ALARMS</pre><form><span>RCP&gt;</span><input autocomplete="off" autofocus></form></div>';document.body.appendChild(host);host.querySelector('[data-close]').onclick=close;host.querySelector('form').onsubmit=e=>{e.preventDefault();const i=e.target.querySelector('input');append(`RCP&gt; ${i.value}`);run(i.value);i.value='';};}host.classList.add('open');}
  function close(){host?.classList.remove('open');}
  global.MFRailwayConsole={open,close};
})(window);
