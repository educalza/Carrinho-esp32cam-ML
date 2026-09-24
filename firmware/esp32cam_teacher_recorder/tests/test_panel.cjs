const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../esp32cam_teacher_recorder.ino'), 'utf8');
const script = source.match(/<script>([\s\S]*?)<\/script>/)[1].replace(/poll\(\);$/, '');
const elements = {};
let nextReply = 'Piloto habilitado';
let status = 200;
let timeout = false;
const context = vm.createContext({
    document: {getElementById(id) {return elements[id] ||= {textContent: ''};}},
    AbortController, TextDecoder, Uint8Array, Date, JSON, Error,
    setTimeout(callback, delay) {if(timeout && delay === 5000) callback();return 1;},
    clearTimeout() {},
    async fetch(url, options) {
        assert.equal(options.cache, 'no-store');
        if(options.signal.aborted) throw Error('timeout');
        return {ok: status === 200, status, async arrayBuffer() {return new TextEncoder().encode(nextReply).buffer;}};
    }
});
vm.runInContext(script, context);
(async () => {
    await context.send('start');
    assert.equal(elements.message.textContent, 'Piloto habilitado');
    status = 409; nextReply = 'Piloto não iniciado: linha ausente. 0/5.';
    await context.send('start');
    assert.match(elements.message.textContent, /Comando recusado.*0\/5/);
    timeout = true;
    await context.send('stop');
    assert.match(elements.message.textContent, /Sem confirmação em 5 segundos/);
    timeout = false; status = 200;
    nextReply = JSON.stringify({quadros_processados: 45, visao: 'linha acompanhada', ultima_falha: 'nenhuma'});
    await context.poll();
    assert.match(elements.status.textContent, /45/);
    assert.match(elements.vision.textContent, /linha acompanhada/);
    console.log('panel: start, refusal, timeout and polling tests passed');
})().catch(error => {console.error(error);process.exitCode = 1;});
