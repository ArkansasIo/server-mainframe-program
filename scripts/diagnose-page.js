'use strict';

/**
 * Reproduce the black screen: load the real dashboard page the way a browser
 * does - fetch the HTML, then execute each <script src> in order in one shared
 * scope - and report what actually failed.
 */

const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const ROOT = path.resolve(__dirname, '..');
const { fakeDocument } = require(path.join(ROOT, 'tests', 'helpers', 'fake-dom'));

const { loadConfig } = require(path.join(ROOT, 'src', 'config'));
const { dashboard } = require(path.join(ROOT, 'src', 'index'));

const config = loadConfig({ argv: [] });
const page = dashboard(config, null);

const scripts = [...page.matchAll(/<script src="([^"]+)"/g)].map((m) => m[1]);
console.log('scripts in page order:', scripts.join(' -> '));

const FILE_FOR = {
  '/components.js': 'src/ui/components.js',
  '/keybinds.js': 'src/ui/keybinds.js',
  '/views.js': 'src/ui/views.js',
  '/dashboard.js': 'public/dashboard.js',
};

/** A document with the methods the real page actually uses. */
function pageDocument() {
  const doc = fakeDocument();

  // The dashboard looks the mount point up by id; the fake DOM has no such
  // index, so give it one backed by a real #app node.
  const app = doc.createElement('div');
  app.id = 'app';
  doc.body.appendChild(app);

  doc.getElementById = (id) => {
    const found = doc.body.findAll((n) => n.id === id);
    return found.length ? found[0] : null;
  };
  doc.querySelectorAll = () => [];
  return doc;
}

const doc = pageDocument();

const sandbox = {
  console,
  setTimeout,
  clearTimeout,
  Promise,
  Date,
  Math,
  JSON,
  Object,
  Array,
  Set,
  Map,
  Number,
  String,
  Boolean,
  Error,
  isNaN,
  parseInt,
  parseFloat,
  RegExp,
  URL,
  Blob,
  fetch: () => Promise.reject(new Error('offline in harness')),
  performance: { now: () => 0 },
};
sandbox.window = {};
sandbox.document = doc;
sandbox.window.document = doc;
sandbox.window.innerWidth = 1440;
sandbox.window.innerHeight = 900;
sandbox.window.setTimeout = setTimeout;
sandbox.document.defaultView = sandbox.window;
doc.readyState = 'complete';

vm.createContext(sandbox);

let broke = null;

for (const src of scripts) {
  const file = FILE_FOR[src];
  if (!file) {
    console.log(`  ?? unknown script ${src}`);
    continue;
  }
  const source = fs.readFileSync(path.join(ROOT, file), 'utf8');
  try {
    vm.runInContext(source, sandbox, { filename: file });
    console.log(`  ok      ${file}`);
  } catch (error) {
    console.log(`  THREW   ${file}: ${error.name}: ${error.message}`);
    if (!broke) broke = { file, error };
  }
}

console.log('');
console.log('globals:',
  'MFUI=' + typeof sandbox.window.MFUI,
  'MFKeybinds=' + typeof sandbox.window.MFKeybinds,
  'MFViews=' + typeof sandbox.window.MFViews,
  'MFApp=' + typeof sandbox.window.MFApp);

const app = doc.getElementById('app');
console.log('#app children:', app ? app.children.length : '(no #app)');

if (broke) {
  console.log('');
  console.log('FIRST FAILURE in', broke.file);
  console.log(String(broke.error.stack).split('\n').slice(0, 6).join('\n'));
  process.exitCode = 1;
}
