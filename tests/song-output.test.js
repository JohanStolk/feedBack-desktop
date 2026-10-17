// "Play the song on this output too": match the engine's output device name to
// the page's audio-output deviceId (src/renderer/screen.js, SO-PURE block).
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const src = fs.readFileSync(path.join(__dirname, '..', 'src', 'renderer', 'screen.js'), 'utf8').replace(/\r\n/g, '\n');
const begin = src.indexOf('// SO-PURE-BEGIN'), end = src.indexOf('// SO-PURE-END');
assert.ok(begin > 0 && end > begin, 'SO-PURE markers present');
const ctx = {};
vm.runInNewContext(src.slice(begin, end) + '\nthis.songOutputDeviceId = songOutputDeviceId;', ctx);
const { songOutputDeviceId: match } = ctx;

const devices = [
    { kind: 'audioinput', deviceId: 'mic1', label: 'Scarlett Solo USB' },
    { kind: 'audiooutput', deviceId: 'default', label: 'Default - MacBook Pro Speakers' },
    { kind: 'audiooutput', deviceId: 'spk', label: 'MacBook Pro Speakers (Built-in)' },
    { kind: 'audiooutput', deviceId: 'scar', label: 'Scarlett Solo USB' },
    { kind: 'audiooutput', deviceId: 'bt', label: 'AirPods Pro (Bluetooth)' },
];

test('exact label match, case-insensitive; inputs and the default alias are ignored', () => {
    assert.equal(match(devices, 'Scarlett Solo USB'), 'scar');
    assert.equal(match(devices, 'scarlett solo usb'), 'scar');
    assert.equal(match(devices, 'Default - MacBook Pro Speakers'), '');
});

test('a label with a suffix in brackets still matches the bare engine name', () => {
    assert.equal(match(devices, 'MacBook Pro Speakers'), 'spk');
    assert.equal(match(devices, 'AirPods Pro'), 'bt');
});

test('no name, no devices or no match means the system default', () => {
    assert.equal(match(devices, ''), '');
    assert.equal(match(devices, null), '');
    assert.equal(match([], 'Scarlett Solo USB'), '');
    assert.equal(match(null, 'Scarlett Solo USB'), '');
    assert.equal(match(devices, 'Focusrite Clarett'), '');
});
