// Checks a mod's main.js without opening the game.
//
//   node tools/modcheck.js mods/spawn-car/main.js
//
// It does two things, in this order:
//
//   1. reads the names: does every function the file CALLS exist in it?
//   2. runs it: loads the mod with a fake `speed` and fires every registered
//      event, looking for reference errors.
//
// The first is the one that matters, and it exists for a concrete reason:
// `node --check` validates syntax and finds everything in order in a file a
// function was deleted from by mistake; the game only complains the moment
// someone clicks the button that goes through it. The second covers less than
// it seems — a stub only reaches the paths it can, and whatever comes after a
// successful native call does not run here.

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const target = process.argv[2];
if (!target) {
  console.error('usage: node tools/modcheck.js <main.js>');
  process.exit(2);
}

const source = fs.readFileSync(target, 'utf8');
let failures = 0;

// ---------------------------------------------------------------- the names

// Comments and literals become blanks: "function" inside a string does not
// declare anything, and a parenthesis inside a comment does not call anything.
const code = source
  .replace(/\/\/[^\n]*/g, ' ')
  .replace(/\/\*[\s\S]*?\*\//g, ' ')
  .replace(/'(?:[^'\\]|\\.)*'/g, "''")
  .replace(/"(?:[^"\\]|\\.)*"/g, '""')
  .replace(/`(?:[^`\\]|\\.)*`/g, '``');

const declared = new Set();
for (const m of code.matchAll(/function\s+([A-Za-z_$][\w$]*)/g)) declared.add(m[1]);
for (const m of code.matchAll(/(?:const|let|var)\s+([A-Za-z_$][\w$]*)/g)) declared.add(m[1]);
for (const m of code.matchAll(/([A-Za-z_$][\w$]*)\s*=>/g)) declared.add(m[1]);

// What comes from another file of the mod (`import { refuel } from "./fuel.js"`)
// exists just as much as what was declared here.
for (const m of code.matchAll(/import\s*{([^}]*)}/g)) {
  for (const name of m[1].split(',')) {
    const clean = name.trim().split(/\s+as\s+/).pop().trim();
    if (clean) declared.add(clean);
  }
}
for (const m of code.matchAll(/import\s+([A-Za-z_$][\w$]*)\s+from/g)) declared.add(m[1]);

const allowed = new Set([
  'if', 'for', 'while', 'switch', 'catch', 'return', 'typeof', 'function',
  'do', 'else', 'new', 'delete', 'void', 'in', 'of', 'instanceof', 'await',
  'yield', 'throw', 'case',
  'speed', 'console', 'Math', 'Number', 'String', 'Boolean', 'Array', 'Object',
  'JSON', 'Set', 'Map', 'Date', 'RegExp', 'Error', 'Promise', 'parseInt',
  'parseFloat', 'isNaN', 'isFinite', 'setTimeout', 'setInterval',
  'clearTimeout', 'clearInterval', 'Symbol', 'BigInt', 'structuredClone',
  'encodeURIComponent', 'decodeURIComponent',
  'Uint8Array', 'Int8Array', 'Uint16Array', 'Int16Array', 'Uint32Array',
  'Int32Array', 'Float32Array', 'Float64Array', 'ArrayBuffer', 'DataView'
]);

for (const m of code.matchAll(/(^|[^.\w$])([A-Za-z_$][\w$]*)\s*\(/gm)) {
  const name = m[2];
  if (allowed.has(name) || declared.has(name)) continue;
  console.error('MISSING: ' + name + '() is called and does not exist in this file');
  failures++;
}

if (failures) process.exit(1);

// ------------------------------------------------------------- the run

const events = {};
const store = {};
const calls = [];

// Addresses become plausible numbers and reads return small integers:
// the idea is to get past the bounds checks and walk through the code, not
// to simulate the game.
const mem = new Proxy({}, {
  get: (_, name) => {
    const n = String(name);
    if (n === 'readString') return () => 'FAKE';
    if (n === 'readBytes') return () => new ArrayBuffer(0x940);
    if (n === 'valid') return () => true;
    if (n === 'alloc') return () => 0x10000000;
    if (n.startsWith('write') || n === 'patch' || n === 'nop') return () => true;
    if (/^read[IU](8|16|32)$/.test(n)) return () => 1;
    if (/^readF/.test(n)) return () => 1.5;
    return () => 0x1000;
  }
});

const speed = {
  version: '0.0.0-fake',
  mod: { id: 'check', name: 'check', dir: '.' },
  mods: () => [],
  now: () => Date.now(),
  on: (ev, cb) => { (events[ev] = events[ev] || []).push(cb); },
  off: () => {},
  call: (addr) => { calls.push(addr); return 0x2000; },
  print: () => {},
  command: () => {},
  emit: () => {},
  mem,
  menu: { add: () => 1, hash: () => 0x1234 },
  game: {
    state: () => 6, stateName: () => 'gameplay', isRacing: () => true,
    hasFocus: () => true, player: () => 0x1000, car: () => 0x1000,
    gearbox: () => 0x1000, physics: () => 0x1000, engine: () => 0x1000,
    window: () => 1, carId: () => 1, carModel: () => 'FAKE',
    telemetry: () => null, hash: () => 1, sendFrontendMessage: () => {},
    offset: {}, addr: {}
  },
  store: {
    get: (k, d) => (k in store ? store[k] : d),
    set: (k, v) => { store[k] = v; },
    all: () => store, clear: () => {},
    car: { get: (k, d) => d, set: () => true, id: () => 1 }
  },
  audio: {
    load: () => 1, play: () => true, stop: () => {}, stopAll: () => {},
    unload: () => true, volume: () => 1, ready: () => true
  },
  ui: {
    send: () => {}, show: () => {}, hide: () => {}, toggle: () => true,
    visible: () => true, capture: () => {}, capturing: () => false,
    reload: () => {}, devtools: () => {}, size: () => ({ width: 0, height: 0 }),
    sound: () => true
  },
  settings: {
    get: (k, d) => d, all: () => ({}), set: () => {}, define: () => {}
  }
};

const ctx = vm.createContext({
  speed, console,
  setTimeout: () => 0, setInterval: () => 0,
  clearTimeout: () => {}, clearInterval: () => {}
});

try {
  vm.runInContext(source, ctx, { filename: path.resolve(target) });
} catch (e) {
  // A mod that uses `import` is an ES module: out of this harness's reach, and
  // not its fault.
  if (/import statement/.test(e.message)) {
    console.log('names ok; run skipped (ES module)');
    process.exit(0);
  }
  console.error('FAILED to load: ' + e.message);
  process.exit(1);
}

function fire(label, cb, arg) {
  try {
    cb(arg);
  } catch (e) {
    // A ReferenceError is missing code. The rest is the stub being a stub.
    if (e instanceof ReferenceError) {
      console.error('FAILED in ' + label + ': ' + e.message);
      failures++;
    }
  }
}

for (const [ev, cbs] of Object.entries(events)) {
  for (const cb of cbs) {
    fire('"' + ev + '"', cb, { channel: ev, data: {}, indice: 0, valor: 0, key: 0x75 });
  }
}

// The keys one by one: that is where a mod usually hangs its functions.
for (const cb of (events['keydown'] || [])) {
  for (let vk = 0x70; vk <= 0x7B; vk++) {
    fire('key 0x' + vk.toString(16), cb, { key: vk });
  }
}

console.log(failures
  ? failures + ' failure(s)'
  : 'ok: ' + Object.keys(events).length + ' events, ' +
    calls.length + ' simulated native calls');
process.exit(failures ? 1 : 0);
