// Checks tools/fi-mcp.js against the running room (tools\e2e_m3.bat runs it after e2e_m3.ps1).
'use strict';
const { spawn } = require('child_process');
const path = require('path');
const child = spawn(process.execPath, [path.join(__dirname, 'fi-mcp.js')], { stdio: ['pipe', 'pipe', 'inherit'] });
let buf = '', next = 1, failed = 0;
const waiting = new Map();
child.stdout.on('data', (d) => {
  buf += d;
  let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    const m = JSON.parse(buf.slice(0, i));
    buf = buf.slice(i + 1);
    if (waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
  }
});
const rpc = (method, params) => new Promise((ok) => { const id = next++; waiting.set(id, ok); child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n'); });
const check = (c, what) => { console.log((c ? 'PASS  ' : 'FAIL  ') + what); if (!c) failed++; };
(async () => {
  const init = await rpc('initialize', { protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 'test', version: '1' } });
  check(init.result && init.result.serverInfo.name === 'fullimmersion', 'mcp: initialize');
  child.stdin.write(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n');
  const list = await rpc('tools/list', {});
  const names = list.result.tools.map((t) => t.name);
  check(names.includes('room_scene') && names.includes('room_arrange') && !names.some((n) => /click|type/.test(n)), 'mcp: tools listed, none that click or type (' + names.length + ')');
  check(list.result.tools.find((t) => t.name === 'room_look').annotations.readOnlyHint === true, 'mcp: looking only reads');
  const scene = await rpc('tools/call', { name: 'room_scene', arguments: {} });
  check(!scene.result.isError && JSON.parse(scene.result.content[0].text).panels.length >= 1, 'mcp: room_scene');
  const pres = await rpc('tools/call', { name: 'room_presence', arguments: { as: 'Locus', state: 'arranging', looking_at: 0 } });
  check(!pres.result.isError, 'mcp: room_presence as Locus');
  const look = await rpc('tools/call', { name: 'room_look', arguments: { w: 640, h: 360 } });
  check(look.result.content[0].type === 'image' && look.result.content[0].data.length > 1000, 'mcp: room_look gives an image');
  const bad = await rpc('tools/call', { name: 'room_move', arguments: { panel: 99 } });
  check(bad.result.isError, 'mcp: a panel that isn\'t there is an error');
  child.kill();
  console.log(failed ? 'FAILED: ' + failed : 'ALL PASSED');
  process.exit(failed ? 1 : 0);
})();
