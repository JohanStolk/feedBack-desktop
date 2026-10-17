// Virtual tuning (match each song + chart priming): the pure helpers in
// src/renderer/screen.js between VT-PURE-BEGIN and VT-PURE-END.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function loadHelpers() {
    const src = fs.readFileSync(path.join(__dirname, '..', 'src', 'renderer', 'screen.js'), 'utf8')
        .replace(/\r\n/g, '\n');
    const begin = src.indexOf('// VT-PURE-BEGIN');
    const end = src.indexOf('// VT-PURE-END');
    assert.ok(begin > 0 && end > begin, 'VT-PURE markers present');
    const ctx = {};
    vm.runInNewContext(src.slice(begin, end) + `
        this.magicTuningShifts = magicTuningShifts;
        this.magicTuningSchedule = magicTuningSchedule;
        this.magicTuningDescribe = magicTuningDescribe;
        this.virtualTuningOpenMidi = virtualTuningOpenMidi;
        this.virtualTuningHints = virtualTuningHints;
        this.virtualTuningChordProfile = virtualTuningChordProfile;
        this.virtualTuningWarning = virtualTuningWarning;`, ctx);
    return ctx;
}

const {
    magicTuningShifts: shifts,
    magicTuningSchedule: schedule,
    magicTuningDescribe: describe,
    virtualTuningOpenMidi: openMidi,
    virtualTuningHints: hints,
    virtualTuningChordProfile: profile,
    virtualTuningWarning: warning,
} = loadHelpers();

// vm objects come from another realm; compare as plain JSON values.
const plain = (v) => JSON.parse(JSON.stringify(v));

test('uniform tunings give one shift per string', () => {
    assert.deepEqual(plain(shifts({ tuning: [-1, -1, -1, -1, 0, 0], stringCount: 4 })), [-1, -1, -1, -1]);
    assert.deepEqual(plain(shifts({ tuning: [0, 0, 0, 0, 0, 0], stringCount: 6 })), [0, 0, 0, 0, 0, 0]);
});

test('Drop D shifts only the low string', () => {
    assert.deepEqual(plain(shifts({ tuning: [-2, 0, 0, 0, 0, 0], stringCount: 4 })), [-2, 0, 0, 0]);
});

test('centOffset is added: the −1200 octave-down bass convention', () => {
    // Future Breed Machine bass: stored as +6 with an octave down = −6.
    assert.deepEqual(plain(shifts({ tuning: [6, 6, 6, 6, 0, 0], stringCount: 4, centOffset: -1200 })), [-6, -6, -6, -6]);
    // A song a few cents off A440 keeps the fraction.
    assert.deepEqual(plain(shifts({ tuning: [0, 0, 0, 0], stringCount: 4, centOffset: -14 })), [-0.14, -0.14, -0.14, -0.14]);
});

test('shifts are clamped to ±12 and never −0', () => {
    const out = plain(shifts({ tuning: [-20, 30, 0, 0], stringCount: 4 }));
    assert.deepEqual(out, [-12, 12, 0, 0]);
    assert.ok(!Object.is(plain(shifts({ tuning: [0], stringCount: 1, centOffset: -0 }))[0], -0));
});

test('no usable tuning gives null; a missing stringCount falls back to the tuning length', () => {
    assert.equal(shifts(null), null);
    assert.equal(shifts({}), null);
    assert.equal(shifts({ tuning: [] }), null);
    assert.deepEqual(plain(shifts({ tuning: [-1, -1, -1] })), [-1, -1, -1]);
    assert.deepEqual(plain(shifts({ tuning: ['x', null, -1], stringCount: 3 })), [0, 0, -1]);
});

test('schedule: one entry when every note needs the same shift', () => {
    const notes = [{ t: 1, s: 1 }, { t: 2, s: 2 }];
    assert.deepEqual(plain(schedule(notes, [], [-2, 0, 0, 0], 0.06)), [{ t: null, semis: 0 }]);   // -Infinity → null in JSON
});

test('schedule: changes land `lead` before the note, but not before halfway', () => {
    const drop = [-2, 0, 0, 0];
    const notes = [
        { t: 1.0, s: 0 },   // low D
        { t: 2.0, s: 1 },   // A string: change at 2.0 - 0.06
        { t: 2.1, s: 0 },   // back to low: halfway (2.05) beats 2.1 - 0.06 = 2.04
    ];
    const out = schedule(notes, [], drop, 0.06);
    assert.equal(out.length, 3);
    assert.equal(out[0].t, -Infinity);
    assert.equal(out[0].semis, -2);
    assert.ok(Math.abs(out[1].t - 1.94) < 1e-9);
    assert.equal(out[1].semis, 0);
    assert.ok(Math.abs(out[2].t - 2.05) < 1e-9);
    assert.equal(out[2].semis, -2);
});

test('schedule: chords use their lowest string; notes and chords are merged in time order', () => {
    const drop = [-2, 0, 0, 0];
    const notes = [{ t: 3, s: 2 }];
    const chords = [{ t: 1, notes: [{ s: 2 }, { s: 0 }, { s: 1 }] }, { t: 5, notes: [{ s: 1 }, { s: 2 }] }];
    const out = plain(schedule(notes, chords, drop, 0.06)).map((e) => e.semis);
    assert.deepEqual(out, [-2, 0]);   // chord@1 → −2, note@3 → 0, chord@5 → 0 (no change)
});

test('schedule: ignores notes on strings outside the tuning and malformed entries', () => {
    const out = schedule([{ t: 1, s: 7 }, null, { t: NaN, s: 0 }, { t: 2, s: 0 }], [{ t: 1 }, null], [-2, 0], 0.06);
    assert.equal(out.length, 1);
    assert.equal(out[0].semis, -2);
});

test('describe says what the song will do', () => {
    assert.match(describe([0, 0, 0, 0]), /standard tuning: no shift/);
    assert.match(describe([-1, -1, -1, -1]), /−1 semitone on all strings/);
    assert.match(describe([-6, -6, -6, -6]), /−6 semitones on all strings/);
    assert.match(describe([-2, 0, 0, 0]), /per note, −2 \/ 0 \/ 0 \/ 0/);
});

const hz = (midi) => Math.round(440 * Math.pow(2, (midi - 69) / 12) * 1000) / 1000;

test('open strings of the instrument the player holds (standard tuning)', () => {
    assert.deepEqual(plain(openMidi(4, true)), [28, 33, 38, 43]);
    assert.deepEqual(plain(openMidi(5, true)), [23, 28, 33, 38, 43]);
    assert.deepEqual(plain(openMidi(6, true)), [23, 28, 33, 38, 43, 48]);
    assert.deepEqual(plain(openMidi(6, false)), [40, 45, 50, 55, 59, 64]);
    assert.deepEqual(plain(openMidi(7, false)), [35, 40, 45, 50, 55, 59, 64]);
    assert.deepEqual(plain(openMidi(8, false)), [30, 35, 40, 45, 50, 55, 59, 64]);
});

test('hints: match each song expects the fingered pitch on a standard bass', () => {
    // Drop D bass, low string -2 and the shift undoes it: open low string = E1.
    const song = { tuning: [-2, 0, 0, 0, 0, 0], stringCount: 4, isBass: true };
    const per = [-2, 0, 0, 0];
    const out = plain(hints([{ t: 1, s: 0, f: 0 }, { t: 2, s: 0, f: 2 }, { t: 3, s: 1, f: 3 }], [], song, per));
    assert.deepEqual(out.t, [1, 2, 3]);
    assert.deepEqual(out.hz, [hz(28), hz(30), hz(36)]);   // E1, F#1, C2
});

test('hints: the octave-down bass convention and a fixed shift', () => {
    // Future Breed Machine bass: +6 with -1200 cents, matched by -6: E1 again.
    const fbm = { tuning: [6, 6, 6, 6], stringCount: 4, centOffset: -1200, isBass: true };
    assert.deepEqual(plain(hints([{ t: 0, s: 0, f: 0 }], [], fbm, [-6, -6, -6, -6])).hz, [hz(28)]);
    // Fixed -1 on an Eb song: the instrument sounds E standard pitches.
    const eb = { tuning: [-1, -1, -1, -1], stringCount: 4, isBass: true };
    assert.deepEqual(plain(hints([{ t: 0, s: 1, f: 5 }], [], eb, [-1, -1, -1, -1])).hz, [hz(38)]);
    // A fixed shift that doesn't match the song is still honest: what the bass sounds.
    assert.deepEqual(plain(hints([{ t: 0, s: 0, f: 0 }], [], eb, [-2, -2, -2, -2])).hz, [hz(29)]);
});

test('hints: capo raises open strings only; chords use their lowest pitched note', () => {
    const song = { tuning: [0, 0, 0, 0], stringCount: 4, capo: 2, isBass: true };
    const per = [0, 0, 0, 0];
    assert.deepEqual(plain(hints([{ t: 0, s: 1, f: 0 }, { t: 1, s: 1, f: 5 }], [], song, per)).hz, [hz(35), hz(38)]);
    const chords = [{ t: 2, notes: [{ s: 2, f: 2 }, { s: 1, f: 2, mt: true }, { s: 0, f: 3 }] }];
    assert.deepEqual(plain(hints([], chords, song, per)), { t: [2], hz: [hz(31)] });   // low string fret 3 = G1
});

test('hints: harmonics, mutes, bad entries and out-of-range pitches are skipped; sorted by time', () => {
    const guitar = { tuning: [0, 0, 0, 0, 0, 0], stringCount: 6, isBass: false };
    const per = [0, 0, 0, 0, 0, 0];
    const notes = [
        { t: 5, s: 0, f: 3 },                 // G2 98 Hz: kept
        { t: 1, s: 0, f: 0, hm: true },       // harmonic
        { t: 2, s: 1, f: 0, mt: true },       // muted
        { t: 3, s: 2, f: 0, fhm: true },      // fret-hand mute
        { t: 4, s: 5, f: 12 },                // E5 659 Hz: above 400 Hz
        { t: 0.5, s: 9, f: 0 },               // no such string
        { t: NaN, s: 0, f: 0 }, null,
        { t: 0.2, s: 0, f: 0 },               // E2 82 Hz: kept, sorts first
    ];
    assert.deepEqual(plain(hints(notes, [{ t: 6 }, null], guitar, per)), { t: [0.2, 5], hz: [hz(40), hz(43)] });
});

// Native bindings: garbage in must never throw or crash (fail-soft no-ops).
const ADDON = path.join(__dirname, '..', 'build', 'Release', 'slopsmith_audio.node');
test('setDropTuneChart / setDropTuneClock tolerate garbage', { skip: !fs.existsSync(ADDON) && 'addon not built' }, () => {
    const audio = require(ADDON);
    audio.init();
    try {
        const GARBAGE = [NaN, Infinity, -Infinity, -1, 1.5, 2 ** 31, 'x', null, undefined, {}, [], true];
        for (const g of GARBAGE) {
            audio.setDropTuneChart(g);
            audio.setDropTuneChart({ shiftTimes: g, shiftSemis: g, noteTimes: g, noteHz: g });
            audio.setDropTuneChart({ shiftTimes: [g, 1], shiftSemis: [g], noteTimes: [1, g, 2], noteHz: [g, 50] });
            audio.setDropTuneClock(g, g);
        }
        audio.setDropTuneChart();
        audio.setDropTuneClock();
        audio.setDropTuneChart({ shiftTimes: [-1e9, 1], shiftSemis: [-2, 0], noteTimes: [0.5], noteHz: [41.2] });
        audio.setDropTuneClock(0.25, true);
        audio.setDropTuneChart(null);
        assert.equal(typeof audio.getDropTuneSemitones(), 'number');
    } finally {
        audio.shutdown?.();
    }
});

test('the Audio screen offers every fixed shift the engine and settings accept (±12)', () => {
    const html = fs.readFileSync(path.join(__dirname, '..', 'src', 'renderer', 'screen.html'), 'utf8');
    const select = html.slice(html.indexOf('id="ae-drop-tune"'), html.indexOf('</select>', html.indexOf('id="ae-drop-tune"')));
    const values = [...select.matchAll(/<option value="([^"]+)"/g)].map((m) => m[1]);
    const fixed = values.filter((v) => /^-?\d+$/.test(v) && v !== '0').map(Number).sort((a, b) => a - b);
    const expected = [];
    for (let v = -12; v <= 12; v++) if (v !== 0) expected.push(v);
    assert.deepEqual(fixed, expected);
    assert.ok(values.includes('0') && values.includes('auto'));
    // Option values must round-trip through the saved-settings normaliser
    // (Math.round + clamp to ±12) and String(), or a restore falls back to Off.
    for (const v of fixed) assert.equal(String(Math.max(-12, Math.min(12, Math.round(v)))), String(v));
});

// Guitar chord shapes as chart chords (string 0 = low E).
const chordOf = (frets) => ({ t: 0, notes: frets.map((f, s) => (f === null ? null : { s, f })).filter(Boolean) });
const E5 = chordOf([0, 2, 2, null, null, null]);          // E B E
const A5 = chordOf([null, 0, 2, 2, null, null]);          // A E A
const C = chordOf([null, 3, 2, 0, 1, 0]);                 // C E G C E
const Am = chordOf([null, 0, 2, 2, 1, 0]);                // A E A C E
const D5drop = chordOf([0, 0, 0, null, null, null]);      // Drop D: D A D
const guitarStd = { tuning: [0, 0, 0, 0, 0, 0], stringCount: 6, isBass: false };
const dropD = { tuning: [-2, 0, 0, 0, 0, 0], stringCount: 6, isBass: false };

test('chord profile: power chords are clean, chords with a 3rd are full', () => {
    const per = [-6, -6, -6, -6, -6, -6];
    assert.deepEqual(plain(profile([E5, A5, E5], guitarStd, per)), { total: 3, full: 0, split: 0 });
    assert.deepEqual(plain(profile([C, Am, E5], guitarStd, per)), { total: 3, full: 2, split: 0 });
    // Muted notes don't count; a "chord" with fewer than two pitched notes is skipped.
    const mutedC = { t: 0, notes: [{ s: 1, f: 3 }, { s: 2, f: 2, mt: true }, { s: 3, f: 0, fhm: true }] };
    assert.deepEqual(plain(profile([mutedC], guitarStd, per)), { total: 0, full: 0, split: 0 });
});

test('chord profile: a Drop D power chord across the low strings is split', () => {
    const per = [-2, 0, 0, 0, 0, 0];
    const p = plain(profile([D5drop, A5], dropD, per));
    assert.equal(p.total, 2);
    assert.equal(p.split, 1);       // D5 spans the -2 low string and 0-shift strings
    assert.equal(p.full, 0);        // D A D in Drop D is still a power chord
});

test('warnings: only for big shifts with many full chords, or many split chords', () => {
    const many = (c, n) => Array.from({ length: n }, () => c);
    const big = [-6, -6, -6, -6, -6, -6], small = [-2, -2, -2, -2, -2, -2];
    const fullHeavy = profile([...many(C, 10), ...many(E5, 20)], guitarStd, big);   // 33 % full
    assert.match(warning(fullHeavy, big), /33% of the chords are full chords; at 6 semitones/);
    assert.equal(warning(profile([...many(C, 10), ...many(E5, 20)], guitarStd, small), small), '');
    assert.equal(warning(profile([...many(C, 2), ...many(E5, 28)], guitarStd, big), big), '');   // 7 %: fine
    assert.equal(warning(profile(many(C, 5), guitarStd, big), big), '');            // too few chords to judge
    const dd = [-2, 0, 0, 0, 0, 0];
    assert.match(warning(profile([...many(D5drop, 10), ...many(A5, 20)], dropD, dd), dd),
        /33% of the chords span strings with different shifts.*Tune those strings down/);
});
