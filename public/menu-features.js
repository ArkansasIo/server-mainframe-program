'use strict';

(function menuFeatures() {
  const doc = window.document;
  function textOf(node){const label=node&&node.querySelector('.mf-menu-label');return String(label?label.textContent:node?node.textContent:'').trim();}
  function findMenuTitle(label){return Array.from(doc.querySelectorAll('.mf-menu-title')).find(n=>String(n.textContent).trim()===label);}
  function findMenuItem(label){return Array.from(doc.querySelectorAll('.mf-menu-item')).find(n=>textOf(n)===label&&!n.classList.contains('is-disabled'));}
  function waitFrame(){return new Promise(r=>window.requestAnimationFrame(r));}
  async function invoke(path){if(!Array.isArray(path)||!path.length)return false;const title=findMenuTitle(path[0]);if(!title)return false;title.click();await waitFrame();for(let i=1;i<path.length;i++){const item=findMenuItem(path[i]);if(!item)return false;item.click();await waitFrame();}return true;}
  function addButton(toolbar,label,title,path){const b=doc.createElement('button');b.type='button';b.className='mf-btn mf-toolbar-btn mf-quick-btn';b.textContent=label;b.title=title;b.addEventListener('click',async()=>{b.disabled=true;try{if(!(await invoke(path)))console.warn(`[menu-features] command path not found: ${path.join(' > ')}`);}finally{b.disabled=false;}});toolbar.appendChild(b);}
  function directButton(toolbar,label,title,handler){const b=doc.createElement('button');b.type='button';b.className='mf-btn mf-toolbar-btn mf-quick-btn';b.textContent=label;b.title=title;b.addEventListener('click',handler);toolbar.appendChild(b);}
  function install(){const toolbar=doc.querySelector('.mf-toolbar');if(!toolbar||toolbar.dataset.quickActionsInstalled==='true')return;toolbar.dataset.quickActionsInstalled='true';[['Status','Open Status',['View','Go to Panel','Status']],['Users','Open Users',['View','Go to Panel','Users']],['Datasets','Open Datasets',['View','Go to Panel','Datasets']],['Jobs','Open Jobs',['View','Go to Panel','Jobs']]].forEach(x=>addButton(toolbar,x[0],x[1],x[2]));directButton(toolbar,'Dispatcher','Open Railway dispatcher',()=>window.MFRailwayDispatcher?.open());directButton(toolbar,'Layout','Open railway layout designer',()=>window.MFRailwayDesigner?.open());directButton(toolbar,'Console','Open railway command console',()=>window.MFRailwayConsole?.open());directButton(toolbar,'Railway','Open RailwayControl overview',()=>window.MFRailwayControl?.open());}
  if(doc.readyState==='loading')doc.addEventListener('DOMContentLoaded',install,{once:true});else install();
})();
