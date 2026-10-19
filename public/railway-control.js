'use strict';

/*
 * MAINFRAME RailwayControl / Train Operations front-end.
 *
 * The actual simulator remains RailwayControl/ and is built as rcp.exe or
 * rcp-gui.exe. This browser panel makes that program visible from the MAINFRAME
 * operator console and documents the live train-control command surface. It is
 * deliberately a simulator dashboard; it does not claim to control real rail
 * equipment.
 */
(function railwayControlModule(global) {
  const doc = global.document;
  let overlay = null;

  const trains = [
    { id: '1A34', className: 'PASSENGER', state: 'READY', section: 'T1', speed: '0 km/h', authority: 'R2' },
    { id: '2B17', className: 'FREIGHT', state: 'HELD', section: 'T3', speed: '0 km/h', authority: 'NONE' },
    { id: '3C05', className: 'MAINTENANCE', state: 'READY', section: 'T5', speed: '0 km/h', authority: 'NONE' },
  ];

  function el(tag, attrs = {}, children = []) {
    const node = doc.createElement(tag);
    Object.entries(attrs).forEach(([key, value]) => {
      if (key === 'text') node.textContent = value;
      else if (key === 'className') node.className = value;
      else node.setAttribute(key, value);
    });
    children.forEach((child) => node.appendChild(child));
    return node;
  }

  function close() {
    if (overlay && overlay.parentNode) overlay.parentNode.removeChild(overlay);
    overlay = null;
  }

  function commandLine(text) {
    const line = el('div', { className: 'mf-rail-command', text: `> ${text}` });
    return line;
  }

  function open() {
    if (overlay) return;

    const panel = el('div', { className: 'mf-dialog mf-railway-panel', role: 'dialog', 'aria-modal': 'true' });
    const title = el('div', { className: 'mf-game-title', text: 'RAILWAY CONTROL / TRAIN OPERATIONS' });
    const subtitle = el('div', {
      className: 'mf-game-subtitle',
      text: 'MAINFRAME subsystem · Railway Traffic Management & Interlocking Simulator',
    });
    panel.appendChild(title);
    panel.appendChild(subtitle);

    const banner = el('div', { className: 'mf-rail-banner' });
    banner.appendChild(el('strong', { text: 'SIMULATOR ONLINE' }));
    banner.appendChild(el('span', { text: '  |  Interlocking: ACTIVE  |  ATC: ACTIVE  |  Track circuits: ACTIVE' }));
    panel.appendChild(banner);

    const table = el('table', { className: 'mf-table' });
    table.appendChild(el('thead', {}, [el('tr', {}, [
      el('th', { text: 'Train' }), el('th', { text: 'Class' }), el('th', { text: 'State' }),
      el('th', { text: 'Section' }), el('th', { text: 'Speed' }), el('th', { text: 'Authority' }),
    ])]));
    const body = el('tbody');
    trains.forEach((train) => {
      body.appendChild(el('tr', {}, [
        el('td', { text: train.id }), el('td', { text: train.className }),
        el('td', { text: train.state }), el('td', { text: train.section }),
        el('td', { text: train.speed }), el('td', { text: train.authority }),
      ]));
    });
    table.appendChild(body);
    panel.appendChild(table);

    const controls = el('div', { className: 'mf-dialog-actions' });
    [
      ['LIST TRAINS', 'List all simulator trains'],
      ['DISPATCH R2', 'Set route R2 and give authority to the first train'],
      ['TRAIN 1A34 RUN', 'Request train 1A34 to run'],
      ['TRAIN 2B17 HOLD', 'Hold train 2B17'],
      ['EMERGENCY STOP', 'Stop the simulator movement clock'],
    ].forEach(([label, hint]) => {
      const button = el('button', { className: 'mf-btn', type: 'button', text: label, title: hint });
      button.addEventListener('click', () => {
        output.appendChild(commandLine(label));
        output.scrollTop = output.scrollHeight;
      });
      controls.appendChild(button);
    });
    panel.appendChild(controls);

    const output = el('div', { className: 'mf-rail-output' });
    output.appendChild(commandLine('RailwayControl simulator interface ready'));
    output.appendChild(commandLine('Use RailwayControl/build/rcp-gui.exe for the native live dispatch diagram'));
    panel.appendChild(output);

    const footer = el('div', { className: 'mf-dialog-actions' });
    const closeButton = el('button', { className: 'mf-btn is-primary', type: 'button', text: 'Close' });
    closeButton.addEventListener('click', close);
    footer.appendChild(closeButton);
    panel.appendChild(footer);

    overlay = el('div', { className: 'mf-overlay' }, [panel]);
    overlay.addEventListener('click', (event) => {
      if (event.target === overlay) close();
    });
    doc.body.appendChild(overlay);
  }

  global.MFRailwayControl = { open, close };
})(window);
