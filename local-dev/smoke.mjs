// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';
import net from 'node:net';
import { setTimeout as delay } from 'node:timers/promises';
import { writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';

// The smoke config enables the engine's existing loopback-only API on port 5599.
const startupDeadline = Date.now() + 30_000;
async function connectStartup() {
  let lastError;
  while (Date.now() < startupDeadline) {
    try {
      return await new Promise((resolve, reject) => {
        const candidate = net.createConnection({ host: '127.0.0.1', port: 5599 });
        const timer = setTimeout(() => {
          const error = new Error('API startup connection attempt timed out');
          error.code = 'ETIMEDOUT';
          candidate.destroy(error);
        }, Math.min(2000, startupDeadline - Date.now()));
        candidate.once('connect', () => {
          clearTimeout(timer);
          candidate.removeListener('error', failed);
          resolve(candidate);
        });
        function failed(error) {
          clearTimeout(timer);
          candidate.destroy();
          reject(error);
        }
        candidate.once('error', failed);
      });
    } catch (error) {
      if (!['ECONNREFUSED', 'ETIMEDOUT'].includes(error.code))
        throw new Error(`API startup TCP attempt failed before connection (${error.code ?? 'unknown'}): ${error.message}`, {cause:error});
      lastError = error;
      const remaining = startupDeadline - Date.now();
      if (remaining > 0) await delay(Math.min(250, remaining));
    }
  }
  throw new Error(`API did not accept a connection within 30 seconds: ${lastError?.message ?? 'startup timeout'}`);
}
const socket = await connectStartup();
let sequence = 0;
let buffer = '';
let pending;
let connectionError;
let shuttingDown = false;
socket.on('error', error => {
  connectionError = new Error(`Established API TCP connection failed: ${error.message}`, {cause:error});
  pending?.reject(connectionError);
});
socket.on('close', () => {
  if (!shuttingDown) {
    connectionError ??= new Error('API connection closed unexpectedly');
    pending?.reject(connectionError);
  }
});
socket.setTimeout(Math.max(1, startupDeadline - Date.now()), () => socket.destroy(new Error('API connection timed out')));
socket.on('data', data => {
  buffer += data.toString('utf8').replaceAll('\0', '');
  let response;
  try { response = JSON.parse(buffer); } catch { return; }
  buffer = '';
  if (response.ack === pending?.ack) pending.resolve(response);
});
async function request(action, fields = {}, expectedSuccess = true, timeout = 10_000) {
  if (connectionError) throw connectionError;
  const ack = ++sequence;
  const response = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`Timed out: ${action}`)), timeout);
    pending = { ack, resolve: value => { clearTimeout(timer); resolve(value); }, reject: error => { clearTimeout(timer); reject(error); } };
    socket.write(JSON.stringify({ action, ack, ...fields }));
  });
  assert.equal(response.success, expectedSuccess, JSON.stringify(response));
  return response.data;
}
try {
  // The same socket waits through level loading until the game actually
  // accepts and services it. Startup includes the first API response.
  const version = await request('get_kfx_info', {}, true, Math.max(1, startupDeadline - Date.now()));
  socket.setTimeout(10_000);
  const level = await request('get_level_info');
  const first = await request('get_current_game_info');
  // Benchmark controls must remain unavailable in normal play/test launches.
  if (!process.argv.includes('--upstream')) await request('console_command', { command: 'renderbench 0' }, false);
  if (!process.argv.includes('--upstream')) await request('console_command', { command: 'statecheck' }, false);
  if (!process.argv.includes('--upstream')) await request('console_command', { command: 'statecheck replay-start' }, false);
  if (!process.argv.includes('--upstream')) await request('console_command', { command: 'scenecheck' }, false);
  if (!process.argv.includes('--upstream')) await request('console_command', { command: 'scenecheck capture' }, false);
  if (!process.argv.includes('--upstream')) await request('console_command', { command: 'scenecheck reference clear-ui' }, false);
  if (!process.argv.includes('--upstream')) await request('console_command', { command: 'scenecheck reference setup assert(true)' }, false);
  if (!process.argv.includes('--upstream')) await request('console_command', { command: 'scenecheck reference setting lighting 1' }, false);
  if (!process.argv.includes('--upstream')) await request('console_command', { command: 'scenecheck reference camera * * * 0 -266 5000 *' }, false);
  await delay(1500);
  const second = await request('get_current_game_info');
  assert.match(version.kfx_version, /1\.4\.0\.5491/);
  if (!process.argv.includes('--upstream')) assert.match(version.kfx_version, /Local/);
  assert.equal(level.level_number, 1);
  assert.equal(level.is_multiplayer, false);
  assert.ok(second.game_turn > first.game_turn, 'Simulation must advance');
  const result = { checkedAt: new Date().toISOString(), version, level, first, second, passed: true };
  const output = fileURLToPath(new URL('../../artifacts/' + (process.argv.includes('--upstream') ? 'smoke-upstream.json' : 'smoke-local.json'), import.meta.url));
  await writeFile(output, JSON.stringify(result, null, 2) + '\n');
  console.log(JSON.stringify(result, null, 2));
  if (process.argv.includes('--quit')) await request('console_command', { command: 'quit' });
} finally {
  shuttingDown = true;
  socket.destroy();
}
