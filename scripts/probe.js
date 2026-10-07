'use strict';

/**
 * Ask the browser what is actually on top of the console at boot, and why.
 */

const path = require('node:path');
const EDGE = 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe';

async function main() {
  // The bundled browsers cannot be downloaded on this machine (the downloads
  // are blocked), but Edge is installed. Driving it directly by path is the
  // only route to a real rendering, so resolve playwright from the npx cache
  // and point it at Edge.
  const path0 = require('node:path');
  const os = require('node:os');
  const playwright = require(path0.join(
    os.homedir(), 'AppData', 'Local', 'npm-cache', '_npx',
    'e41f203b7505f1fb', 'node_modules', 'playwright',
  ));

  const browser = await playwright.chromium.launch({
    executablePath: EDGE,
    headless: true,
  });
  const page = await browser.newPage();

  await page.goto(process.argv[2] || 'http://localhost:8080/', {
    waitUntil: 'networkidle', timeout: 30000,
  });
  await page.waitForTimeout(3000);

  const report = await page.evaluate(() => {
    const describe = (sel) => {
      const el = document.querySelector(sel);
      if (!el) return { found: false };
      const cs = getComputedStyle(el);
      return {
        found: true,
        display: cs.display,
        visibility: cs.visibility,
        opacity: cs.opacity,
        zIndex: cs.zIndex,
        position: cs.position,
        hiddenAttr: el.hasAttribute('hidden'),
        hiddenProp: el.hidden === true,
      };
    };

    return {
      game: describe('.mf-game'),
      boot: describe('.mf-boot'),
      // What is at the centre of the screen?
      atCentre: (() => {
        const el = document.elementFromPoint(
          Math.floor(window.innerWidth / 2), Math.floor(window.innerHeight / 2),
        );
        return el ? `${el.tagName}.${el.className}` : null;
      })(),
      gameMenuOpen: window.MFApp && window.MFApp.getGameMenu
        ? window.MFApp.getGameMenu().isOpen() : 'n/a',
    };
  });

  console.log(JSON.stringify(report, null, 2));
  await browser.close();
}

main().catch((e) => { console.error('FAILED:', e.message); process.exitCode = 1; });
