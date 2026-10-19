'use strict';

/**
 * Menu/toolbar feature bridge.
 * Adds operator-facing shortcuts for features that are otherwise nested in
 * the main menu, including the RailwayControl / train simulator program.
 */
(function menuFeatures() {
  const doc = window.document;

  function textOf(node) {
    const label = node && node.querySelector('.mf-menu-label');
    return String(label ? label.textContent : node ? node.textContent : '').trim();
  }

  function findMenuTitle(label) {
    return Array.from(doc.querySelectorAll('.mf-menu-title'))
      .find((node) => String(node.textContent).trim() === label);
  }

  function findMenuItem(label) {
    return Array.from(doc.querySelectorAll('.mf-menu-item'))
      .find((node) => textOf(node) === label && !node.classList.contains('is-disabled'));
  }

  function waitFrame() {
    return new Promise((resolve) => window.requestAnimationFrame(resolve));
  }

  async function invoke(path) {
    if (!Array.isArray(path) || !path.length) return false;
    const title = findMenuTitle(path[0]);
    if (!title) return false;
    title.click();
    await waitFrame();
    for (let i = 1; i < path.length; i += 1) {
      const item = findMenuItem(path[i]);
      if (!item) return false;
      item.click();
      await waitFrame();
    }
    return true;
  }

  function addButton(toolbar, label, title, path) {
    const button = doc.createElement('button');
    button.type = 'button';
    button.className = 'mf-btn mf-toolbar-btn mf-quick-btn';
    button.textContent = label;
    button.title = title;
    button.addEventListener('click', async () => {
      button.disabled = true;
      try {
        if (!(await invoke(path))) {
          console.warn(`[menu-features] command path not found: ${path.join(' > ')}`);
        }
      } finally {
        button.disabled = false;
      }
    });
    toolbar.appendChild(button);
  }

  function addRailwayButton(toolbar) {
    const button = doc.createElement('button');
    button.type = 'button';
    button.className = 'mf-btn mf-toolbar-btn mf-quick-btn mf-railway-btn';
    button.textContent = 'Railway / Trains';
    button.title = 'Open the MAINFRAME RailwayControl train program';
    button.addEventListener('click', () => {
      if (window.MFRailwayControl && typeof window.MFRailwayControl.open === 'function') {
        window.MFRailwayControl.open();
      } else {
        console.warn('[menu-features] RailwayControl module is not loaded');
      }
    });
    toolbar.appendChild(button);
  }

  function install() {
    const toolbar = doc.querySelector('.mf-toolbar');
    if (!toolbar || toolbar.dataset.quickActionsInstalled === 'true') return;
    toolbar.dataset.quickActionsInstalled = 'true';

    addButton(toolbar, 'Status', 'Open the Status panel', ['View', 'Go to Panel', 'Status']);
    addButton(toolbar, 'Users', 'Open the Users panel', ['View', 'Go to Panel', 'Users']);
    addButton(toolbar, 'Datasets', 'Open the Datasets panel', ['View', 'Go to Panel', 'Datasets']);
    addButton(toolbar, 'Jobs', 'Open the Jobs panel', ['View', 'Go to Panel', 'Jobs']);
    addButton(toolbar, 'Transactions', 'Open the Transactions panel', ['View', 'Go to Panel', 'Transactions']);
    addButton(toolbar, 'Audit', 'Open the Audit Log panel', ['View', 'Go to Panel', 'Audit Log']);
    addButton(toolbar, 'Tile', 'Tile open windows', ['Windows', 'Tile Windows']);
    addButton(toolbar, 'Cascade', 'Cascade open windows', ['Windows', 'Cascade Windows']);
    addButton(toolbar, 'Close All', 'Close all open windows', ['Windows', 'Close All Windows']);
    addRailwayButton(toolbar);
  }

  if (doc.readyState === 'loading') {
    doc.addEventListener('DOMContentLoaded', install, { once: true });
  } else {
    install();
  }
})();
