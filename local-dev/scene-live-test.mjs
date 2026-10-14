// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';
import net from 'node:net';
import { setTimeout as delay } from 'node:timers/promises';
import { readFile, writeFile, readdir } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { join } from 'node:path';

const out = process.argv[2];
assert.ok(out);
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
async function request(action, fields = {}, success = true) {
  const ack = ++sequence;
  const response = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`Timed out: ${action} ${fields.command ?? ''}`)), 30_000);
    pending = { ack, resolve: value => { clearTimeout(timer); resolve(value); }, reject: error => { clearTimeout(timer); reject(error); } };
    socket.write(JSON.stringify({ action, ack, ...fields }));
  });
  assert.equal(response.success, success, JSON.stringify(response));
  return response.data;
}
const command = text => request('console_command', { command: text });
const samples = [];
const assetFiles = [];
async function captureAssets(view) {
  if (!process.argv.includes('--assets')) return;
  const previous = new Set(await readdir(out));
  await command('scenecheck capture');
  let file;
  for (let attempt = 0; attempt < 120; ++attempt) {
    file = (await readdir(out)).find(name => name.endsWith('.kfxscene') && !previous.has(name));
    if (file) break;
    await delay(250);
  }
  assert.ok(file, 'Asset capture failed or timed out; see keeperfx.log');
  const bytes = await readFile(join(out, file));
  assert.equal(bytes.toString('ascii', 0, 4), 'KFXC');
  assetFiles.push({ file, view, bytes: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex') });
}
async function capture(view) {
  await command('scenecheck');
  const scene = JSON.parse(await readFile(join(out, 'scene.json'), 'utf8'));
  assert.equal(scene.view, view, 'Expected camera path');
  assert.ok(scene.columns > 0 && scene.bytes > 0);
  assert.equal(scene.consumedErrors, 0, 'Consumer rejected snapshot');
  assert.ok(scene.consumedSequence > 0 && scene.consumedSequence <= scene.sequence);
  if (samples.length) {
    assert.ok(scene.sequence > samples.at(-1).sequence, 'Must capture a fresh rendered frame');
    assert.ok(scene.consumedSequence > samples.at(-1).consumedSequence, 'Consumer must advance too');
  }
  samples.push(scene);
  return scene;
}
try {
  const before = await request('get_current_game_info');
  const first = await capture(0);
  await captureAssets(0);
  assert.ok(first.objects > 0 && first.hidden > 0);
  for (let i = 0; i < 8; ++i) { await delay(150); await capture(0); }
  const firstIds = new Map(first.ids);
  const last = samples.at(-1);
  const common = last.ids.filter(([slot, id]) => firstIds.get(slot) === id);
  assert.ok(common.length > 0, 'Living objects must retain identity between frames');
  await command('game.save 9997 Scene-test');
  await command('scenecheck front');
  await delay(1500);
  await capture(1);
  await captureAssets(1);
  await command('scenecheck possession');
  await delay(2500);
  await capture(2);
  await captureAssets(2);
  await command('scenecheck overhead');
  await delay(1500);
  const returned = await capture(0);
  assert.equal(returned.epoch, first.epoch, 'Camera changes must not reset scene identity');
  await command('game.load 9997');
  await delay(1000);
  const loaded = await capture(0);
  await captureAssets(0);
  assert.ok(loaded.epoch > first.epoch, 'Load must invalidate old identities');
  await command('statecheck');
  await delay(1000);
  const resynced = await capture(0);
  await captureAssets(0);
  assert.ok(resynced.epoch > loaded.epoch, 'Local resync must invalidate old identities');
  const after = await request('get_current_game_info');
  assert.ok(after.game_turn > before.game_turn);
  for (const saved of assetFiles) {
    const bytes = await readFile(join(out, saved.file));
    assert.equal(createHash('sha256').update(bytes).digest('hex'), saved.sha256, 'Retained capture changed across reload');
  }
  const report = { passed: true, before, after, samples, assetFiles,
    maxCaptureMs: Math.max(...samples.map(s => s.captureMs)),
    maxPayloadBytes: Math.max(...samples.map(s => s.bytes)),
    scope: 'Live overhead/front/possession capture, consumer validation, save/load and local resync; not a network session.' };
  await writeFile(join(out, 'report.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify({ ...report, samples: samples.length }, null, 2));
  await command('quit');
} finally { socket.destroy(); }
