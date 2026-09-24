const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const {test} = require('node:test');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../interface_controle/index.html'), 'utf8');
function controls() {
    const events = {}, commands = [];
    const context = {
        window: {addEventListener: (name, fn) => events[name] = fn},
        document: {hidden: false, hasFocus: () => true, addEventListener: (name, fn) => events[name] = fn},
        gamepadIndex: null, navigator: {}, DEADZONE: .1,
        GAMEPAD_RIGHT_X_AXIS: 2, GAMEPAD_LEFT_Y_AXIS: 1,
        speedLimit: 1, currentSteer: 0, currentThrottle: 0,
        lastSentSteer: null, lastSentThrottle: null, lastSendTime: 0,
        SEND_THRESHOLD: .01, SEND_INTERVAL_MS: 0, performance: {now: () => 1000},
        hudCmd: {}, statCmd: {}, getDirLabel: () => '', updateDpadVisual() {},
        ws: {readyState: 1}, WebSocket: {OPEN: 1},
        sendCmd: (name, data) => { if (name === 'drive') commands.push(data); }, toggleFlashLed() {}
    };
    vm.createContext(context);
    vm.runInContext(html.slice(html.indexOf('function applySoftDeadzone('), html.indexOf('function getDirLabel(')), context);
    const start = html.indexOf('const activeKeys = new Set();');
    vm.runInContext(html.slice(start, html.indexOf('</script>', start)), context);
    return {context, commands, events,
        key(type, key, tagName='BODY', repeat=false) { events[type]({key, repeat, target: {tagName}, preventDefault() {}}); },
        axes(s,t) { context.setDriveAxes(s,t); },
        throttle() { return commands.at(-1).throttle; }};
}
test('Space stops keyboard and gamepad until controls return to neutral', () => {
    const c = controls();
    c.key('keydown', 'w'); assert.equal(c.throttle(), 1);
    c.key('keydown', ' '); assert.equal(c.throttle(), 0);
    c.axes(1,1); assert.equal(c.throttle(), 0);
    c.key('keyup', ' '); assert.equal(c.throttle(), 0);
    c.key('keyup', 'w');
    c.key('keydown', 'w'); assert.equal(c.throttle(), 1);
});
test('keyup inside an input releases acceleration', () => {
    const c = controls();
    c.key('keydown', 'w');
    c.key('keyup', 'w', 'INPUT'); assert.equal(c.throttle(), 0);
});
test('blur stops immediately and focus requires neutral gamepad before resuming', () => {
    const c = controls();
    c.key('keydown', 'w'); c.events.blur(); assert.equal(c.throttle(), 0);
    c.axes(0,1); assert.equal(c.throttle(), 0);
    c.events.focus(); c.axes(0,1); assert.equal(c.throttle(), 0);
    c.axes(0,0); c.axes(0,1); assert.equal(c.throttle(), 1);
});
test('hidden page stops and Space works even inside an input', () => {
    const c = controls();
    c.key('keydown', 'w'); c.key('keydown', ' ', 'INPUT'); assert.equal(c.throttle(), 0);
    c.context.document.hidden = true; c.events.visibilitychange();
    c.axes(0,1); assert.equal(c.throttle(), 0);
});
test('key repeat does not restart motion after returning to the window', () => {
    const c = controls();
    c.key('keydown', 'w'); c.events.blur(); c.events.focus(); c.axes(0,0);
    c.key('keydown', 'w', 'BODY', true); assert.equal(c.throttle(), 0);
    c.key('keyup', 'w'); c.key('keydown', 'w'); assert.equal(c.throttle(), 1);
});
