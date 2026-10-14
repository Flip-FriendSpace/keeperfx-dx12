// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';
import net from 'node:net';
import { setTimeout as delay } from 'node:timers/promises';
import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';

const runtime = process.argv[2];
const output = process.argv[3];
assert.ok(runtime && output, 'Pass the isolated runtime and output directories');

const socket = net.createConnection({ host: '127.0.0.1', port: 5599 });
await new Promise((resolve, reject) => { socket.once('connect', resolve); socket.once('error', reject); });
let sequence = 0, buffer = '', pending;
socket.on('error', error => pending?.reject(error));
socket.on('data', data => {
  buffer += data.toString('utf8').replaceAll('\0', '');
  let response;
  try { response = JSON.parse(buffer); } catch { return; }
  buffer = '';
  if (response.ack === pending?.ack) pending.resolve(response);
});
async function request(action, fields = {}, expectedSuccess = true) {
  const ack = ++sequence;
  const response = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`Timed out: ${action} ${fields.command ?? ''}`)), 30_000);
    pending = { ack, resolve: value => { clearTimeout(timer); resolve(value); }, reject: error => { clearTimeout(timer); reject(error); } };
    socket.write(JSON.stringify({ action, ack, ...fields }));
  });
  assert.equal(response.success, expectedSuccess, JSON.stringify(response));
  return response.data;
}
try {
  // The listener can accept before the first simulation tick; state diagnostics
  // deliberately reject turn zero. Wait on actual readiness using this socket.
  const startupDeadline = Date.now() + 30_000;
  let startup = await request('get_current_game_info');
  while (startup.game_turn === 0 && Date.now() < startupDeadline) {
    await delay(100);
    startup = await request('get_current_game_info');
  }
  assert.ok(startup.game_turn > 0, 'State-check session did not start simulating');
  await request('console_command', { command: 'statecheck' });
  const beforeSave = await request('get_current_game_info');
  await request('console_command', { command: 'game.save 9998 State-format-test' });
  const afterSave = await request('get_current_game_info');
  const file = await readFile(join(runtime, 'save/fx1g9998.sav'));
  const chunks = [];
  for (let offset = 0; offset < file.length;) {
    assert.ok(offset + 12 <= file.length);
    const length = file.readUInt32LE(offset), id = file.toString('ascii', offset + 4, offset + 8), version = file.readUInt32LE(offset + 8);
    offset += 12;
    assert.ok(offset + length <= file.length);
    if (id !== 'LUA ') {
      assert.equal(file.toString('ascii', offset, offset + 4), 'KFXS');
      assert.equal(file.readUInt32LE(offset + 4), 1);
      assert.equal(file.readUInt32LE(offset + 12) + 32, length);
    } else {
      assert.equal(version, 1);
      const { crc32 } = await import('node:zlib');
      assert.equal(file.readUInt32LE(offset), crc32(file.subarray(offset + 4, offset + length)));
    }
    chunks.push({ id, version, length });
    offset += length;
  }
  assert.deepEqual(chunks.map(c => c.id), ['INFO', 'GSTA', 'ILVL', 'LUA ']);
  await delay(2000);
  const advanced = await request('get_current_game_info');
  assert.ok(advanced.game_turn > afterSave.game_turn);
  await request('console_command', { command: 'game.load 9998' });
  const loaded = await request('get_current_game_info');
  assert.equal((await request('get_level_info')).level_number, 1);
  assert.ok(loaded.game_turn >= beforeSave.game_turn && loaded.game_turn <= afterSave.game_turn + 2,
    `Save turn was not restored: ${JSON.stringify({ beforeSave, afterSave, advanced, loaded })}`);
  await request('console_command', { command: 'statecheck' });
  await delay(1500);
  const resumed = await request('get_current_game_info');
  assert.ok(resumed.game_turn > loaded.game_turn);
  // Malformed metadata must be rejected before applying a saved Game.
  const savePath = join(runtime, 'save/fx1g9998.sav');
  try {
    const badLength = Buffer.from(file);
    badLength.writeUInt32LE(0xffffffff, 0);
    await writeFile(savePath, badLength);
    await request('console_command', { command: 'game.load 9998' }, false);
    const badChecksum = Buffer.from(file);
    badChecksum[12 + 32] ^= 1;
    await writeFile(savePath, badChecksum);
    await request('console_command', { command: 'game.load 9998' }, false);
  } finally { await writeFile(savePath, file); }
  const result = { passed: true, chunks, beforeSave, afterSave, advanced, loaded, resumed };
  await writeFile(join(output, 'live-state-test.json'), JSON.stringify(result, null, 2) + '\n');
  console.log(JSON.stringify(result, null, 2));
  await request('console_command', { command: 'quit' });
} finally { socket.destroy(); }
