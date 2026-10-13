// Magical tuning (virtual drop tuning that follows the song): the pure helpers
// in src/renderer/screen.js between MAGIC-PURE-BEGIN and MAGIC-PURE-END.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function loadHelpers() {
    const src = fs.readFileSync(path.join(__dirname, '..', 'src', 'renderer', 'screen.js'), 'utf8')
        .replace(/\r\n/g, '\n');
    const begin = src.indexOf('// MAGIC-PURE-BEGIN');
    const end = src.indexOf('// MAGIC-PURE-END');
    assert.ok(begin > 0 && end > begin, 'MAGIC-PURE markers present');
    const ctx = {};
    vm.runInNewContext(src.slice(begin, end) + `
        this.magicTuningShifts = magicTuningShifts;
        this.magicTuningSchedule = magicTuningSchedule;
        this.magicTuningIndexAt = magicTuningIndexAt;
        this.magicTuningDescribe = magicTuningDescribe;`, ctx);
    return ctx;
}

const {
    magicTuningShifts: shifts,
    magicTuningSchedule: schedule,
    magicTuningIndexAt: indexAt,
    magicTuningDescribe: describe,
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

test('indexAt follows playback and handles seeks both ways', () => {
    const sched = [{ t: -Infinity, semis: -2 }, { t: 1, semis: 0 }, { t: 2, semis: -2 }, { t: 3, semis: 0 }];
    assert.equal(indexAt(sched, 0.5, 0), 0);
    assert.equal(indexAt(sched, 1.0, 0), 1);
    assert.equal(indexAt(sched, 1.5, 1), 1);
    assert.equal(indexAt(sched, 2.5, 1), 2);
    assert.equal(indexAt(sched, 99, 0), 3);     // seek forward
    assert.equal(indexAt(sched, 0.2, 3), 0);    // seek back / loop
    assert.equal(indexAt(sched, -5, 2), 0);     // count-in before the chart
    assert.equal(indexAt([{ t: -Infinity, semis: 0 }], 10, 0), 0);
});

test('describe says what the song will do', () => {
    assert.match(describe([0, 0, 0, 0]), /standard tuning: no shift/);
    assert.match(describe([-1, -1, -1, -1]), /−1 semitone on all strings/);
    assert.match(describe([-6, -6, -6, -6]), /−6 semitones on all strings/);
    assert.match(describe([-2, 0, 0, 0]), /per note, −2 \/ 0 \/ 0 \/ 0/);
});
