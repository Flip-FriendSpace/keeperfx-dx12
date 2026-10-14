// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';
import net from 'node:net';
import { setTimeout as delay } from 'node:timers/promises';
import { readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';

const target = Number(process.argv[2]);
const reportPath = process.argv[3];
const levelIndex = process.argv.indexOf('--level');
const expectedLevel = levelIndex < 0 ? 1 : Number(process.argv[levelIndex + 1]);
assert.ok(Number.isInteger(expectedLevel) && expectedLevel >= 1 && expectedLevel <= 65535, 'Invalid expected level');
const campaignIndex = process.argv.indexOf('--campaign');
const expectedCampaign = campaignIndex < 0 ? null : process.argv[campaignIndex + 1];
const setupIndex = process.argv.indexOf('--setup-script');
const setupScript = setupIndex < 0 ? null : process.argv[setupIndex + 1];
assert.ok(setupIndex < 0 || (setupScript && process.argv.includes('--midgame-record')), 'Setup script requires mid-game recording');
const setup = setupScript ? {path:setupScript, sha256:null, completedSteps:0, pausedTurn:null} : null;
const verifyIndex = process.argv.indexOf('--verify-script');
const verifyScript = verifyIndex < 0 ? null : process.argv[verifyIndex + 1];
const verifyHashIndex = process.argv.indexOf('--verify-hash');
const verifyHash = verifyHashIndex < 0 ? null : process.argv[verifyHashIndex + 1];
const verifyModeIndex = process.argv.indexOf('--verify-mode');
const verifyMode = verifyModeIndex < 0 ? null : process.argv[verifyModeIndex + 1];
const verify = verifyScript ? {path:verifyScript, sha256:verifyHash, mode:verifyMode, completedSteps:0, markerLine:null, turn:null} : null;
assert.ok(!verify || (/^[0-9a-f]{64}$/.test(verifyHash) && ['record','playback'].includes(verifyMode)), 'Verification requires expected hash and mode');
assert.ok(verify || (verifyHashIndex < 0 && verifyModeIndex < 0), 'Verification arguments require a script');
const finishIndex = process.argv.indexOf('--finish-game-turn');
const finishGameTurn = finishIndex < 0 ? null : Number(process.argv[finishIndex + 1]);
const frontendIndex = process.argv.indexOf('--expected-frontend');
const expectedFrontend = frontendIndex < 0 ? null : process.argv[frontendIndex + 1];
const logIndex = process.argv.indexOf('--log-file');
const logFile = logIndex < 0 ? null : process.argv[logIndex + 1];
const baseIndex = process.argv.indexOf('--base-turn');
const baseTurn = baseIndex < 0 ? null : Number(process.argv[baseIndex + 1]);
const integrityIndex = process.argv.indexOf('--integrity');
const integrity = integrityIndex < 0 ? null : process.argv[integrityIndex + 1];
if (finishGameTurn !== null) {
  assert.ok(Number.isInteger(finishGameTurn) && finishGameTurn > target && Number.isInteger(baseTurn) && baseTurn > 0 && target > baseTurn, 'Invalid terminal replay turns');
  assert.ok(['LEVEL_STATS','LAND_VIEW'].includes(expectedFrontend) && logFile && /^\d+$/.test(integrity), 'Terminal replay requires frontend, log and integrity');
  assert.ok(!setup && !verify && !process.argv.includes('--midgame-record'), 'Terminal replay is playback only without a verification script');
} else assert.ok(frontendIndex < 0 && (logIndex < 0 || verify) && baseIndex < 0 && integrityIndex < 0, 'Terminal replay arguments require finish-game-turn');
assert.ok(!verify || logFile, 'Verification requires the runtime log');
let result = {passed:false, target, setup, verify, expectedLevel, expectedCampaign, finishGameTurn, expectedFrontend};
const timeoutIndex = process.argv.indexOf('--timeout-seconds');
const timeoutSeconds = timeoutIndex < 0 ? 60 : Number(process.argv[timeoutIndex + 1]);
assert.ok(Number.isInteger(timeoutSeconds) && timeoutSeconds >= 30 && timeoutSeconds <= 1800, 'Timeout must be 30..1800 seconds');
const playbackDeadline = timeoutIndex < 0 ? null : Date.now() + timeoutSeconds * 1000;
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
async function request(action, fields = {}) {
  const ack = ++sequence;
  const response = await new Promise((resolve, reject) => {
    const remaining = playbackDeadline === null ? 30000 : Math.max(1, Math.min(30000, playbackDeadline - Date.now()));
    const timer = setTimeout(() => reject(new Error(`Timed out: ${action}`)), remaining);
    pending = { ack, resolve: value => { clearTimeout(timer); resolve(value); }, reject: error => { clearTimeout(timer); reject(error); } };
    socket.write(JSON.stringify({ action, ack, ...fields }));
  });
  assert.equal(response.success, true, JSON.stringify(response));
  return response.data;
}
try {
  const first = await request(finishGameTurn === null ? 'get_current_game_info' : 'get_kfx_info');
  if (finishGameTurn !== null) {
    const outcome = expectedFrontend === 'LEVEL_STATS' ? 'Victory achieved' : 'Lost level';
    const boundaryPattern = new RegExp(`^.*\\[${finishGameTurn}\\] get_startup_menu_state: ${outcome} state selected\\r?$`, 'm');
    const transitionPattern = new RegExp(`^.*Frontend state change from 0 \\(FeSt_INITIAL\\) into [0-9]+ \\(FeSt_${expectedFrontend}\\)\\r?$`, 'm');
    let terminal = null;
    const terminalDeadline = playbackDeadline ?? Date.now() + timeoutSeconds * 1000;
    while (Date.now() < terminalDeadline) {
      const log = await readFile(logFile, 'utf8');
      assert.ok(!/Out of sync|Cannot replay|Recorded resync could not|Lua error|Failed to execute Lua code|Replay reached|Packet File unreadable|replay aborted|level on disk differs/.test(log), 'Replay log reports a failure');
      assert.ok(!/Replay terminal outcome mismatch:|Replay terminal cleanup failed for player|Replay script player identity is invalid|process_dungeon_control_packet_clicks: Unrecognized[^\r\n]*work state|light_turn_light_on: Attempt to turn on unallocated light|move_object: Attempt to move non-existing object/.test(log), 'Terminal replay reports an outcome, script identity or cleanup failure');
      const startup = /Replay startup: base_turn=(\d+) snapshot=1 checksum=1 integrity=(\d+)/.exec(log);
      if (startup) {
        assert.equal(Number(startup[1]), baseTurn, 'Terminal replay restored a different base');
        assert.equal(startup[2], integrity, 'Terminal replay startup integrity differs');
      }
      const boundary = boundaryPattern.exec(log);
      const outcomes = [...log.matchAll(/Replay terminal outcome: turn=(\d+) player=(\d+) simulated=(-?\d+) recorded=(-?\d+) verified=(\d+)\b/g)];
      assert.ok(outcomes.length <= 1, 'Multiple terminal outcome receipts');
      const expectedState = expectedFrontend === 'LEVEL_STATS' ? 1 : 2;
      const terminalOutcome = outcomes[0];
      if (terminalOutcome) {
        assert.equal(Number(terminalOutcome[1]), finishGameTurn, 'Terminal outcome turn differs from recorded FinishGame');
        assert.equal(Number(terminalOutcome[3]), expectedState, 'Simulated terminal outcome differs');
        assert.equal(Number(terminalOutcome[4]), expectedState, 'Recorded terminal outcome differs');
        assert.equal(Number(terminalOutcome[5]), 1, 'Terminal outcome was not verified');
      }
      const summaries = [...log.matchAll(/Replay verification: base_turn=(\d+) verified=(\d+) mismatches=(\d+)/g)];
      for (const summary of summaries) assert.equal(Number(summary[3]), 0, 'Replay checksum mismatches');
      if (boundary && startup && terminalOutcome && terminalOutcome.index > startup.index && terminalOutcome.index < boundary.index) {
        const transition = transitionPattern.exec(log.slice(boundary.index + boundary[0].length));
        const summary = summaries.find(value => Number(value[1]) === baseTurn && value.index > terminalOutcome.index && value.index < boundary.index);
        if (transition && summary) {
          assert.ok(Number(summary[2]) >= target - baseTurn, 'Terminal replay verified too few pre-terminal turns');
          terminal = {finishGameTurn, expectedFrontend, terminalOutcome:{line:terminalOutcome[0], turn:Number(terminalOutcome[1]), player:Number(terminalOutcome[2]), simulated:Number(terminalOutcome[3]), recorded:Number(terminalOutcome[4]), verified:true}, outcomeLine:boundary[0].trim(), frontendLine:transition[0].trim(), verificationLine:summary[0], verifiedTurns:Number(summary[2]), mismatches:0, startupLine:startup[0], observedUtc:new Date().toISOString(), normalUiCloseRequired:true};
          break;
        }
      }
      await delay(100);
    }
    assert.ok(terminal, 'Recorded outcome boundary and frontend were not confirmed before the deadline');
    result = {passed:true, target, first, timeoutSeconds, expectedLevel, expectedCampaign, terminal};
    console.log(JSON.stringify(result));
  } else {
  let recordingRequestedAt = null;
  if (process.argv.includes('--midgame-record')) {
    const startDeadline = Date.now() + 30_000;
    let info = first;
    while (info.game_turn < 120 && Date.now() < startDeadline) {
      await delay(100);
      info = await request('get_current_game_info');
    }
    assert.ok(info.game_turn >= 120, 'Mid-game setup did not advance');
    if (setup) {
      const bytes = await readFile(setupScript);
      setup.sha256 = createHash('sha256').update(bytes).digest('hex');
      const lines = bytes.toString('utf8').replace(/^\uFEFF/, '').split(/\r?\n/).map(line=>line.trim()).filter(line=>line && !line.startsWith('--'));
      assert.ok(lines.length > 0, 'Setup script contains no Lua lines');
      await request('console_command', {command:'pause'});
      await request('console_command', {command:'scenecheck reference setup assert(true)'});
      setup.pausedTurn = (await request('get_current_game_info')).game_turn;
      for (const line of lines) {
        await request('console_command', {command:`scenecheck reference setup ${line}`});
        setup.completedSteps++;
      }
      assert.equal((await request('get_current_game_info')).game_turn, setup.pausedTurn, 'Setup must remain paused');
      await request('console_command', {command:'pause'});
      info = await request('get_current_game_info');
    }
    recordingRequestedAt = info.game_turn;
    await request('console_command', { command: 'statecheck replay-start' });
  }
  let turn = 0;
  const deadline = playbackDeadline ?? Date.now() + timeoutSeconds * 1000;
  while (turn < target && Date.now() < deadline) {
    turn = (await request('get_current_game_info')).game_turn;
    await delay(100);
  }
  assert.ok(turn >= target, `Replay stopped advancing at ${turn}`);
  const level = await request('get_level_info');
  assert.equal(level.level_number, expectedLevel);
  if (expectedCampaign) assert.equal(level.campaign.campaign_fname.toLowerCase(), `${expectedCampaign.toLowerCase()}.cfg`);
  if (verify) {
    const bytes = await readFile(verifyScript);
    assert.equal(createHash('sha256').update(bytes).digest('hex'), verifyHash, 'Verification file hash changed');
    const lines = bytes.toString('utf8').replace(/^\uFEFF/, '').split(/\r?\n/).map(line=>line.trim()).filter(line=>line && !line.startsWith('--'));
    assert.ok(lines.length > 0 && lines.every(line=>/^(?:assert|print)\s*\(/.test(line)), 'Verification file must contain reviewed read-only assert/print lines');
    for (const line of lines) {
      await request('console_command', {command:`lua ${line}`});
      verify.completedSteps++;
    }
    await request('console_command', {command:`lua print("KFX_REPLAY_VERIFY_SCRIPT_OK", "${verifyHash}", ${verify.completedSteps}, PLAYER0.GAME_TURN, "${verifyMode}")`});
    const markerPattern = /^\[(\d+)\] lua_print: KFX_REPLAY_VERIFY_SCRIPT_OK\s+([0-9a-f]{64})\s+(\d+)\s+(\d+)\s+(record|playback)\r?$/gm;
    while (Date.now() < deadline) {
      const log = await readFile(logFile, 'utf8');
      assert.ok(!/Lua error|Failed to execute Lua code/.test(log), 'Read-only Lua verification failed');
      const markers = [...log.matchAll(markerPattern)];
      assert.ok(markers.length <= 1, 'Multiple verification success markers');
      if (markers.length === 1) {
        const marker = markers[0];
        assert.equal(marker[2], verifyHash);
        assert.equal(Number(marker[3]), verify.completedSteps);
        assert.equal(marker[5], verifyMode);
        assert.ok(Number(marker[4]) >= target, 'Verification ran before target turn');
        assert.equal(Number(marker[1]), Number(marker[4]), 'Verification log/player turn differs');
        verify.markerLine=marker[0].trim(); verify.turn=Number(marker[4]);
        break;
      }
      await delay(50);
    }
    assert.ok(verify.markerLine, 'Printed Lua verification marker absent before deadline');
  }
  result = { passed: true, target, first, recordingRequestedAt, turn, timeoutSeconds, setup, verify, expectedLevel, expectedCampaign };
  console.log(JSON.stringify(result));
  await request('console_command', { command: 'quit' });
  }
} catch (error) {
  result.passed = false;
  result.failure = error.message;
  throw error;
} finally {
  socket.destroy();
  if (reportPath) await writeFile(reportPath, JSON.stringify(result, null, 2) + '\n');
}
