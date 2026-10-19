'use strict';

/**
 * Menu/toolbar feature bridge.
 *
 * The dashboard keeps one authoritative action table inside dashboard.js.
 * This file adds the missing operator-facing shortcut buttons without
 * duplicating that action logic: each button navigates the real menu and
 * activates the existing menu command. That means mouse, menu and keyboard
 * continue to execute the same action path.
 *
 * Loaded after dashboard.js so the normal console has already built its menu.
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

  /**
   * Activate an existing menu command by following its visible submenu path.
   * Example: ['View', 'Go to Panel', 'Users'].
   */
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

  function install() {
    const toolbar = doc.querySelector('.mf-toolbar');
    if (!toolbar || toolbar.dataset.quickActionsInstalled === 'true') return;
    toolbar.dataset.quickActionsInstalled = 'true';

    // Panel shortcuts that were previously available only through nested menus.
    addButton(toolbar, 'Status', 'Open the Status panel', ['View', 'Go to Panel', 'Status']);
    addButton(toolbar, 'Users', 'Open the Users panel', ['View', 'Go to Panel', 'Users']);
    addButton(toolbar, 'Datasets', 'Open the Datasets panel', ['View', 'Go to Panel', 'Datasets']);
    addButton(toolbar, 'Jobs', 'Open the Jobs panel', ['View', 'Go to Panel', 'Jobs']);
    addButton(toolbar, 'Transactions', 'Open the Transactions panel', ['View', 'Go to Panel', 'Transactions']);
    addButton(toolbar, 'Audit', 'Open the Audit Log panel', ['View', 'Go to Panel', 'Audit Log']);

    // Window management shortcuts expose features that were previously buried
    // in the Windows submenu.
    addButton(toolbar, 'Tile', 'Tile open windows', ['Windows', 'Tile Windows']);
    addButton(toolbar, 'Cascade', 'Cascade open windows', ['Windows', 'Cascade Windows']);
    addButton(toolbar, 'Close All', 'Close all open windows', ['Windows', 'Close All Windows']);
  }

  // dashboard.js builds the toolbar synchronously during its boot path.
  if (doc.readyState === 'loading') {
    doc.addEventListener('DOMContentLoaded', install, { once: true });
  } else {
    install();
  }
})();
