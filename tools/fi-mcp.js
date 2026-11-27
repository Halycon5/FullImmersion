#!/usr/bin/env node
// FullImmersion as an MCP server over stdio, for the Switchboard's plugs (or any MCP client):
//   command: node   args: ["E:\\FullImmersion\\tools\\fi-mcp.js"]
// Each call goes through the room's agents' door (src/core/agents.h), found afresh in
// %APPDATA%\FullImmersion\agent_door.json, so the room can restart under it. No dependencies.
//
// The Switchboard shares one connection among every agent and doesn't say who's calling, so the tools that act
// take `as`: the agent's own name, which is how the room shows them.
'use strict';
const fs = require('fs');
const path = require('path');
const readline = require('readline');

const DOOR = path.join(process.env.APPDATA || path.join(require('os').homedir(), 'AppData', 'Roaming'),
                       'FullImmersion', 'agent_door.json');

const AS = { type: 'string', description: 'Your own name, as the room shows you (e.g. "Locus").' };
const PANEL = {
  description: 'A panel: its index from room_scene, or text from its title or program (e.g. "chrome").',
  anyOf: [{ type: 'integer' }, { type: 'string' }],
};
const PLACE = {
  yaw: { type: 'number', description: 'Degrees round Milk: 0 straight ahead, positive to her right.' },
  height: { type: 'number', description: 'Metres, the middle of the panel against her eyes (0 = eye level).' },
  distance: { type: 'number', description: 'Metres from her (0.4 to 8; 1.4 is usual).' },
  width: { type: 'number', description: 'Metres wide (0.15 to 8; about 1 is a normal window).' },
};

const TOOLS = [
  { name: 'room_scene', route: ['GET', '/scene'], reads: true,
    description: "What's in Milk's FullImmersion room: each panel (a real window) with its index, program, title and " +
      'where it is round her (degrees and metres), the agents in the room, the notes, and where she is looking.',
    inputSchema: { type: 'object', properties: {} } },
  { name: 'room_windows', route: ['GET', '/windows'], reads: true,
    description: "Milk's open windows that could be pinned into the room, with their ids and whether they're pinned.",
    inputSchema: { type: 'object', properties: {} } },
  { name: 'room_look', route: ['GET', '/look'], reads: true, image: true,
    description: 'A picture of the room from where Milk stands, looking any way (degrees; defaults: where she looks).',
    inputSchema: { type: 'object', properties: {
      yaw: { type: 'number', description: 'Degrees, 0 straight ahead, positive to the right.' },
      pitch: { type: 'number', description: 'Degrees, positive up.' },
      fov: { type: 'number', description: 'Degrees tall (default 70).' },
      w: { type: 'integer', description: 'Pixels wide (default 1280).' },
      h: { type: 'integer', description: 'Pixels tall (default 720).' } } } },
  { name: 'room_pin', route: ['POST', '/pin'],
    description: 'Pin one of Milk\'s open windows into the room as a panel. Give "window" (an id from room_windows) or ' +
      '"match" (text in its title or program), and where to put it if you like.',
    inputSchema: { type: 'object', properties: Object.assign({ as: AS, window: { type: 'integer' }, match: { type: 'string' } }, PLACE) } },
  { name: 'room_move', route: ['POST', '/move'],
    description: 'Move or resize a panel. Anything not given stays as it is.',
    inputSchema: { type: 'object', required: ['panel'], properties: Object.assign({ as: AS, panel: PANEL }, PLACE) } },
  { name: 'room_unpin', route: ['POST', '/unpin'],
    description: 'Take a panel out of the room (the window itself stays open).',
    inputSchema: { type: 'object', required: ['panel'], properties: { as: AS, panel: PANEL } } },
  { name: 'room_arrange', route: ['POST', '/arrange'],
    description: 'Set panels side by side in a row round Milk, left to right, centred on "around" (degrees), ' +
      'without overlapping. A quick way to lay out a workspace.',
    inputSchema: { type: 'object', required: ['panels'], properties: {
      as: AS, panels: { type: 'array', items: PANEL, description: 'Left to right.' },
      around: { type: 'number', description: 'Degrees for the middle of the row (default: where she looks).' },
      distance: PLACE.distance, height: PLACE.height,
      gap: { type: 'number', description: 'Metres between neighbours (default 0.06).' } } } },
  { name: 'room_presence', route: ['POST', '/presence'],
    description: "Show yourself in the room: what you're doing, something you say (shown for a few seconds), and " +
      "the panel you're working on (you float over it). You fade away after a few minutes without a word.",
    inputSchema: { type: 'object', properties: {
      as: AS, state: { type: 'string', description: 'A word or two: working, waiting, reading...' },
      says: { type: 'string' }, looking_at: PANEL } } },
  { name: 'room_note', route: ['POST', '/note'],
    description: 'Leave a note in the room, beside a panel ("near") or at a spot. Returns its id.',
    inputSchema: { type: 'object', required: ['text'], properties: Object.assign({ as: AS, text: { type: 'string' }, near: PANEL },
      { yaw: PLACE.yaw, height: PLACE.height, distance: PLACE.distance }) } },
  { name: 'room_unnote', route: ['POST', '/unnote'],
    description: 'Take a note away by its id.',
    inputSchema: { type: 'object', required: ['id'], properties: { as: AS, id: { type: 'integer' } } } },
  { name: 'room_leave', route: ['POST', '/leave'],
    description: 'Leave the room (your name card goes).',
    inputSchema: { type: 'object', properties: { as: AS } } },
];

function door() {
  try { return JSON.parse(fs.readFileSync(DOOR, 'utf8')); } catch { return null; }
}

async function call(tool, args) {
  const d = door();
  if (!d) return { isError: true, content: [{ type: 'text', text: "Milk's FullImmersion room isn't running." }] };
  args = Object.assign({}, args || {});
  const as = String(args.as || 'agent').slice(0, 40);
  delete args.as;
  const [method, route] = tool.route;
  let url = 'http://127.0.0.1:' + d.port + route;
  if (method === 'GET' && Object.keys(args).length) url += '?' + new URLSearchParams(args).toString();
  let res;
  try {
    res = await fetch(url, {
      method,
      headers: Object.assign({ Authorization: 'Bearer ' + d.token, 'X-Agent': as },
                             method === 'POST' ? { 'Content-Type': 'application/json' } : {}),
      body: method === 'POST' ? JSON.stringify(args) : undefined,
      signal: AbortSignal.timeout(15000),
    });
  } catch (e) {
    return { isError: true, content: [{ type: 'text', text: "Milk's FullImmersion room isn't answering (" + e.message + ').' }] };
  }
  if (tool.image && res.ok) {
    const b = Buffer.from(await res.arrayBuffer());
    return { content: [{ type: 'image', data: b.toString('base64'), mimeType: 'image/png' }] };
  }
  const text = await res.text();
  return { isError: !res.ok, content: [{ type: 'text', text }] };
}

function send(msg) { process.stdout.write(JSON.stringify(msg) + '\n'); }

async function handle(m) {
  if (m.id === undefined) return;  // a notification
  const ok = (result) => send({ jsonrpc: '2.0', id: m.id, result });
  const err = (code, message) => send({ jsonrpc: '2.0', id: m.id, error: { code, message } });
  switch (m.method) {
    case 'initialize':
      return ok({ protocolVersion: (m.params && m.params.protocolVersion) || '2025-06-18',
                  capabilities: { tools: {} }, serverInfo: { name: 'fullimmersion', version: '0.3.0' },
                  instructions: "Milk's 3D desktop. You can see and arrange it, never click or type into her windows." });
    case 'ping': return ok({});
    case 'tools/list':
      return ok({ tools: TOOLS.map((t) => ({ name: t.name, description: t.description, inputSchema: t.inputSchema,
                                             annotations: { readOnlyHint: !!t.reads } })) });
    case 'tools/call': {
      const t = TOOLS.find((x) => x.name === (m.params && m.params.name));
      if (!t) return err(-32602, 'No such tool: ' + (m.params && m.params.name));
      return ok(await call(t, m.params.arguments));
    }
    default: return err(-32601, 'Not here: ' + m.method);
  }
}

if (require.main === module) {
  const rl = readline.createInterface({ input: process.stdin });
  rl.on('line', (line) => {
    if (!line.trim()) return;
    let m;
    try { m = JSON.parse(line); } catch { return send({ jsonrpc: '2.0', id: null, error: { code: -32700, message: 'not JSON' } }); }
    handle(m).catch((e) => send({ jsonrpc: '2.0', id: m.id, error: { code: -32603, message: e.message } }));
  });
}

module.exports = { TOOLS, handle };
