'use strict';

/**
 * Look at the dashboard in a real browser, using the Edge that is already on
 * this machine instead of the Chromium Playwright wanted to download.
 *
 * Reports exactly what a person would see: console errors, failed requests,
 * the page title, and whether #app actually mounted.
 */

const path = require('node:path');

const EDGE = 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe';
const URL = process.argv[2] || 'http://localhost:8080/';

async function main() {
  let patchright;
  try {
    patchright = require('patchright');
  } catch {
    // Fall back to a npx-installed copy if one is around.
    patchright = require(path.join(
      process.env.LOCALAPPDATA || '', 'npm-cache', '_npx',
      'e41f203b7505f1fb', 'node_modules', 'playwright-core',
    ));
  }

  const browser = await patchright.chromium.launch({
    executablePath: EDGE,
    headless: true,
  });

  const page = await browser.newPage();
  const consoleErrors = [];
  const failed = [];

  page.on('console', (msg) => {
    if (msg.type() === 'error') consoleErrors.push(msg.text());
  });
  page.on('pageerror', (err) => consoleErrors.push(`UNCAUGHT: ${err.message}`));
  page.on('requestfailed', (req) => failed.push(`${req.url()} ${req.failure()?.errorText}`));
  page.on('response', (res) => {
    if (res.status() >= 400) failed.push(`${res.url()} HTTP ${res.status()}`);
  });

  await page.goto(URL, { waitUntil: 'networkidle', timeout: 30000 });
  // Give the boot sequence a moment to finish and hand over to the console.
  await page.waitForTimeout(2500);

  const report = await page.evaluate(() => {
    const app = document.getElementById('app');
    const boot = document.querySelector('.mf-boot');
    return {
      title: document.title,
      bodyChars: document.body.innerText.length,
      appExists: Boolean(app),
      appChildren: app ? app.children.length : -1,
      bootVisible: Boolean(boot),
      bootLog: boot ? [...boot.querySelectorAll('.mf-boot-line, .mf-boot-unit')]
        .map((n) => n.textContent).slice(-8) : [],
      menuTitles: [...document.querySelectorAll('.mf-menu-title')].map((n) => n.textContent),
      panelTitles: [...document.querySelectorAll('.mf-panel-title')].map((n) => n.textContent),
      toolbar: [...document.querySelectorAll('.mf-toolbar-btn')].map((n) => n.textContent),
      tableRows: document.querySelectorAll('.mf-table tbody tr').length,
      globals: {
        MFUI: typeof window.MFUI,
        MFKeybinds: typeof window.MFKeybinds,
        MFViews: typeof window.MFViews,
        MFApp: typeof window.MFApp,
      },
      bodyText: document.body.innerText.slice(0, 800),
    };
  });

  console.log('===== DASHBOARD IN A REAL BROWSER =====');
  console.log(JSON.stringify(report, null, 2));
  console.log('');
  console.log('console errors:', consoleErrors.length ? consoleErrors : '(none)');
  console.log('failed requests:', failed.length ? failed : '(none)');

  await page.screenshot({ path: path.join(__dirname, '..', 'data', 'dashboard.png'), fullPage: false });
  console.log('');
  console.log('screenshot -> data/dashboard.png');

  await browser.close();
}

main().catch((err) => {
  console.error('LOOK FAILED:', err.message);
  process.exitCode = 1;
});
