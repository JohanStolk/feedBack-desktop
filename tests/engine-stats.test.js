// Engine load + virtual-tuning delay readout: the pure helper in
// src/renderer/screen.js between ES-PURE-BEGIN and ES-PURE-END.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function loadHelper() {
    const src = fs.readFileSync(path.join(__dirname, '..', 'src', 'renderer', 'screen.js'), 'utf8')
        .replace(/\r\n/g, '\n');
    const begin = src.indexOf('// ES-PURE-BEGIN');
    const end = src.indexOf('// ES-PURE-END');
    assert.ok(begin > 0 && end > begin, 'ES-PURE markers present');
    const ctx = {};
    vm.runInNewContext(src.slice(begin, end) + '\nthis.engineStatsView = engineStatsView; this.engineHudText = engineHudText; this.vtInputLeakWarning = vtInputLeakWarning;', ctx);
    return ctx;
}

const { engineStatsView: view, engineHudText: hudText, vtInputLeakWarning: leak } = loadHelper();
const base = {
    running: true, duplex: true, inputLoad: 0.23, inputPeak: 0.31, outputLoad: 0, outputPeak: 0,
    dropTune: { shifting: false, nowMs: 0, averageMs: 0, attackMs: 0, plucks: 0 },
};

test('nothing to show without stats or while stopped', () => {
    for (const st of [null, undefined, {}, { ...base, running: false }]) {
        const v = view(st, 0.9);
        assert.equal(v.cpu, '');
        assert.equal(v.delay, '');
        assert.equal(v.level, 'ok');
    }
});

test('load as a percentage with the recent peak', () => {
    const v = view(base, 0.4);
    assert.equal(v.cpu, 'CPU 23% · peak 40%');
    assert.equal(v.level, 'ok');
    assert.equal(view(base, 0).cpu, 'CPU 23% · peak 31%', 'the poll\'s own peak counts too');
    assert.equal(view({ ...base, inputLoad: 0.0028, inputPeak: 0.0046 }, 0).cpu, 'CPU 0.3% · peak 0.5%',
        'one decimal below 10 %');
});

test('split mode: the busier device callback counts', () => {
    const v = view({ ...base, duplex: false, outputLoad: 0.55, outputPeak: 0.6 }, 0);
    assert.equal(v.cpu, 'CPU 55% · peak 60%');
    assert.equal(v.level, 'warn');
});

test('levels: amber from 50 %, red from 80 % peak', () => {
    assert.equal(view(base, 0.49).level, 'ok');
    assert.equal(view(base, 0.5).level, 'warn');
    assert.equal(view(base, 0.8).level, 'high');
    assert.equal(view({ ...base, inputLoad: 1.3, inputPeak: 1.6 }, 0).cpu, 'CPU 130% · peak 160%');
});

test('virtual tuning delay only while shifting, attack once measured', () => {
    const shifting = { ...base, dropTune: { shifting: true, nowMs: 9.8, averageMs: 11.04, attackMs: 0, plucks: 0 } };
    assert.equal(view(shifting, 0).delay, 'Virtual tuning +11.0 ms');
    const primed = { ...shifting, dropTune: { ...shifting.dropTune, attackMs: 6.26, plucks: 12 } };
    assert.equal(view(primed, 0).delay, 'Virtual tuning +11.0 ms (attack 6.3 ms)');
    const off = { ...base, dropTune: { shifting: false, nowMs: 0, averageMs: 11, attackMs: 6, plucks: 12 } };
    assert.equal(view(off, 0).delay, '');
});

test('garbage values read as zero', () => {
    const v = view({ running: true, inputLoad: 'x', inputPeak: NaN, dropTune: { shifting: true, averageMs: null } }, undefined);
    assert.equal(v.cpu, 'CPU 0.0% · peak 0.0%');
    assert.equal(v.delay, 'Virtual tuning +0.0 ms');
});

test('HUD line: virtual tuning with its delay, then the load', () => {
    const shifting = view({ ...base, dropTune: { shifting: true, averageMs: 7.06, attackMs: 0, plucks: 0 } }, 0);
    assert.equal(hudText(shifting, '−2 · re-fret'), 'Virtual tuning −2 · re-fret · +7.1 ms · CPU 23% · peak 31%');
    assert.equal(hudText(view(base, 0), 'standard'), 'Virtual tuning standard · CPU 23% · peak 31%');
    assert.equal(hudText(view(base, 0), ''), 'CPU 23% · peak 31%', 'off: only the load');
    assert.equal(hudText(view(null, 0), '−2 fixed'), 'Virtual tuning −2 fixed', 'engine stopped: setting only');
    assert.equal(hudText(view(null, 0), ''), '');
});

test('unshifted-input warning: NAM browser amp is certain, other streams are a hint', () => {
    assert.deepEqual({ ...leak(false, true, 3) }, { level: '', text: '' }, 'virtual tuning off: nothing to warn');
    assert.deepEqual({ ...leak(true, false, 0) }, { level: '', text: '' });
    const nam = leak(true, true, 1);
    assert.equal(nam.level, 'high');
    assert.match(nam.text, /NAM Tone plays your input unshifted/);
    const other = leak(true, false, 2);
    assert.equal(other.level, 'warn');
    assert.match(other.text, /^⚠ 2 raw input streams open/);
    assert.match(leak(true, false, 1).text, /^⚠ 1 raw input stream open/);
    assert.equal(hudText(view(base, 0), '−1', nam), `Virtual tuning −1 · CPU 23% · peak 31% · ${nam.text}`);
});
