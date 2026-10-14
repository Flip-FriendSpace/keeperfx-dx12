// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';
import net from 'node:net';
import {setTimeout as delay} from 'node:timers/promises';
import {readFile, writeFile, rm, stat, rename, readdir} from 'node:fs/promises';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawn} from 'node:child_process';

const [out, levelText, renderer, resolution, ...options] = process.argv.slice(2);
const level = Number(levelText);
assert.ok(out && Number.isInteger(level) && level > 0 && ['OPENGL','SOFTWARE','DX12'].includes(renderer));
assert.match(resolution, /^\d+x\d+$/);
const expectedSize = resolution.split('x').map(Number);
assert.ok(options.every(option => ['--clear-ui','--palette-fade','--terrain-updates','--temporal-updates','--window-updates'].includes(option) || /^--reloads=[0-8]$/.test(option) || ['--runtime=','--game-pid=','--window-exe=','--window-host='].some(prefix=>option.startsWith(prefix))), 'Unknown live renderer option');
const reloads=Number(options.find(option=>option.startsWith('--reloads='))?.slice(10)??0);
const runtime=options.find(option=>option.startsWith('--runtime='))?.slice(10);
assert.ok(!reloads||(renderer==='DX12'&&runtime),'Reload qualification requires DX12 and its disposable runtime');
const terrainUpdates=options.includes('--terrain-updates');
assert.ok(!terrainUpdates||renderer==='DX12','Terrain updates require DX12 cache statistics');
const temporalUpdates=options.includes('--temporal-updates');
assert.ok(!temporalUpdates||renderer==='DX12','Temporal receipts require DX12');
assert.ok(!(terrainUpdates&&temporalUpdates),'Run frozen terrain reuse and live temporal surfaces separately');
const windowUpdates=options.includes('--window-updates');
const gamePid=Number(options.find(option=>option.startsWith('--game-pid='))?.slice(11));
const windowExe=options.find(option=>option.startsWith('--window-exe='))?.slice(13);
const windowHost=options.find(option=>option.startsWith('--window-host='))?.slice(14);
assert.ok(!windowUpdates||(renderer==='DX12'&&!terrainUpdates&&!temporalUpdates&&Number.isSafeInteger(gamePid)&&gamePid>0&&windowExe&&windowHost),'Window updates require DX12, the launched game process and a separate window sequence');
const report = {renderer, level, resolution, clearUI:options.includes('--clear-ui'), paletteFade:options.includes('--palette-fade'), reloads, terrainUpdates, temporalUpdates, windowUpdates, passed:false};
let socket, pending, failure, sequence = 0, buffer = '', shuttingDown = false, readerHoldArmed = false;
const startupDeadline = Date.now() + 30_000;
async function connect() {
  while (Date.now() < startupDeadline) {
    try {
      return await new Promise((resolve, reject) => {
        const candidate = net.createConnection({host:'127.0.0.1', port:5599});
        const timer = setTimeout(() => candidate.destroy(Object.assign(new Error('API connect timed out'), {code:'ETIMEDOUT'})), Math.min(2000, startupDeadline-Date.now()));
        candidate.once('error', failed);
        function failed(error) { clearTimeout(timer); candidate.destroy(); reject(error); }
        candidate.once('connect', () => { clearTimeout(timer); candidate.removeListener('error', failed); resolve(candidate); });
      });
    } catch (error) {
      if (!['ECONNREFUSED','ETIMEDOUT'].includes(error.code)) throw error;
      await delay(250);
    }
  }
  throw new Error('API connection did not become ready within 30 seconds');
}
async function request(action, fields={}, timeout=10_000) {
  if (failure) throw failure;
  const ack = ++sequence;
  const result = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`API request timed out: ${fields.command ?? action}`)), timeout);
    pending = {ack, resolve:value => {clearTimeout(timer); resolve(value);}, reject:error => {clearTimeout(timer); reject(error);}};
    socket.write(JSON.stringify({action, ack, ...fields}));
  });
  pending = undefined;
  assert.equal(result.success, true, `${fields.command ?? action}: ${JSON.stringify(result)}`);
  return result.data;
}
const command = text => request('console_command', {command:text});
async function waitPaused() {
  const deadline=Date.now()+10_000;
  while(Date.now()<deadline) {
    try {await command('scenecheck reference setup assert(true)');return;}
    catch(error) {if(failure||error.code!=='ERR_ASSERTION'||error.actual!==false)throw error;await delay(50);}
  }
  throw new Error('Campaign did not enter the paused reference state');
}
function checkStats(stats) {
  assert.equal(stats.schema,1);assert.equal(stats.backend,'DX12');
  for(const field of ['epoch','worldEpoch','frameSequence','submitted','rejected'])assert.ok(Number.isSafeInteger(stats[field])&&stats[field]>=0,`Invalid renderer statistic ${field}`);
  assert.equal(stats.rejected,0);assert.equal(stats.validation.errors,0);assert.equal(stats.validation.warnings,0);
  assert.ok(!stats.failed,'DX12 statistics report a failed renderer');
  for(const type of ['cpu','default','upload','readback','producer','geometryCpu']) {
    const current=stats.memory[`${type}Bytes`],peak=stats.memory[`${type}HighWater`],budget=stats.memory[`${type}BudgetBytes`];
    assert.ok([current,peak,budget].every(Number.isSafeInteger)&&current>=0&&peak>=current&&budget>0,`Invalid ${type} memory accounting`);
    assert.ok(peak<=budget,`${type} memory exceeded its configured budget`);
  }
  const {current,highWater,budget}=stats.descriptors;
  assert.ok([current,highWater,budget].every(Number.isSafeInteger)&&current>=0&&highWater>=current&&budget>0&&highWater<=budget,'Descriptor accounting exceeded its configured budget');
  for(const field of ['allocations','uploadBytes','internalWaits','globalIdleCalls'])assert.ok(Number.isSafeInteger(stats.counters[field])&&stats.counters[field]>=0,`Invalid renderer counter ${field}`);
}
async function rendererStats() {
  const path=join(out,'renderer-stats.json');
  await rm(path,{force:true});
  await command('scenecheck reference renderer-stats');
  const deadline=Date.now()+5000;
  while(Date.now()<deadline) {
    try {const stats=JSON.parse(await readFile(path,'utf8'));checkStats(stats);return stats;}
    catch(error) {if(!(error instanceof SyntaxError)&&!['ENOENT','EBUSY'].includes(error.code))throw error;await delay(50);}
  }
  throw new Error('Fresh renderer statistics were not written within 5 seconds');
}
async function waitWorldAfterReload(before) {
  const deadline=Date.now()+30_000;
  while(Date.now()<deadline) {
    const stats=await rendererStats();
    if(stats.epoch>before.epoch&&stats.worldEpoch===stats.epoch&&stats.frameSequence>before.frameSequence&&stats.submitted>before.submitted) {
      assert.equal(stats.counters.globalIdleCalls,before.counters.globalIdleCalls,'Normal reload introduced a global GPU idle wait');
      return stats;
    }
    await delay(100);
  }
  throw new Error('Load did not advance the renderer epoch and accept a fresh normal world within 30 seconds');
}
async function holdPendingReader() {
  const baseline=await rendererStats();
  assert.ok(baseline.readerHold&&baseline.readerHold.watchdogMs===5000,'Missing bounded GPU reader hold diagnostics');
  report.temporal.readerHold={commands:['scenecheck reference reader-hold','pause','game.load 9997 1','scenecheck reference reader-release'],baseline};
  const armDeadline=Date.now()+10_000;let armed=false;
  while(Date.now()<armDeadline) {
    try {await command('scenecheck reference reader-hold');armed=true;readerHoldArmed=true;break;}
    catch(error) {if(failure||error.code!=='ERR_ASSERTION'||error.actual!==false)throw error;await delay(25);}
  }
  assert.ok(armed,'Reader hold could not arm after normal frame-slot completion');
  const deadline=Date.now()+3000;
  while(Date.now()<deadline) {
    const stats=await rendererStats(),hold=stats.readerHold;
    assert.ok(hold.active,'Reader hold expired before a pending normal world was blocked');
    assert.equal(hold.automaticReleases,baseline.readerHold.automaticReleases,'Watchdog released the hold before load');
    if(hold.slots===3&&hold.pendingWorld&&hold.pendingEpoch===hold.blockedEpoch&&hold.pendingSequence===hold.blockedSequence&&hold.blockedPackets>baseline.readerHold.blockedPackets) {
      assert.equal(hold.arms,baseline.readerHold.arms+1);assert.ok(hold.heldSubmissions>=baseline.readerHold.heldSubmissions+3);
      report.temporal.readerHold.blocked=stats;
      await writeFile(join(out,'report.json'),JSON.stringify(report,null,2)+'\n');
      return stats;
    }
    await delay(25);
  }
  throw new Error('Held GPU readers did not block an exact pending normal world within 3 seconds');
}
async function verifyHeldResetAndRelease(blocked) {
  const deadline=Date.now()+3000;let reset;
  while(Date.now()<deadline) {
    const stats=await rendererStats(),hold=stats.readerHold,old=blocked.readerHold;
    assert.ok(hold.active,'GPU reader hold expired before reset qualification');
    assert.equal(hold.automaticReleases,old.automaticReleases,'Watchdog released readers before reset qualification');
    if(stats.epoch>blocked.epoch&&stats.sceneEpoch>blocked.sceneEpoch&&hold.resetsWhileHeld===old.resetsWhileHeld+1&&hold.droppedPendingOnReset===old.droppedPendingOnReset+1) {
      assert.equal(hold.droppedEpoch,old.blockedEpoch);assert.equal(hold.droppedSequence,old.blockedSequence);
      reset=stats;break;
    }
    await delay(25);
  }
  assert.ok(reset,'Reset did not discard the exact blocked old world while its GPU readers remained held');
  report.temporal.readerHold.reset=reset;
  await writeFile(join(out,'report.json'),JSON.stringify(report,null,2)+'\n');
  await command('scenecheck reference reader-release');readerHoldArmed=false;
  const released=await rendererStats();assert.equal(released.readerHold.active,false);
  report.temporal.readerHold.released=released;
}
async function terrainPNG(file,turn) {
  await command('scenecheck reference screenshot');
  const path=join(out,'presentation.png'),deadline=Date.now()+30_000;let png;
  while(Date.now()<deadline) {
    try {
      const bytes=await readFile(path);
      if(bytes.length>=33&&bytes.subarray(0,8).toString('hex')==='89504e470d0a1a0a'&&bytes.subarray(-12).toString('hex')==='0000000049454e44ae426082'){png=bytes;break;}
    } catch(error) {if(!['ENOENT','EBUSY'].includes(error.code))throw error;}
    await delay(100);
  }
  assert.ok(png,'Complete terrain presentation PNG was not written');
  const dimensions=[png.readUInt32BE(16),png.readUInt32BE(20)];assert.deepEqual(dimensions,expectedSize);
  assert.equal((await request('get_current_game_info')).game_turn,turn,'Paused turn changed during terrain screenshot');
  const renameDeadline=Date.now()+5000;
  while(true) {
    try {await rename(path,join(out,file));break;}
    catch(error) {if(!['EBUSY','EPERM','EACCES'].includes(error.code)||Date.now()>=renameDeadline)throw error;await delay(100);}
  }
  return {file,dimensions,turn};
}
function checkTerrainCache(stats) {
  assert.ok(stats.terrainCache,'Renderer did not expose terrain cache statistics');
  for(const key of ['requests','hits','builds','evictions','resets','bytes','entries'])assert.ok(Number.isSafeInteger(stats.terrainCache[key])&&stats.terrainCache[key]>=0,`Invalid terrain cache ${key}`);
  assert.ok(stats.terrainCache.bytes>0&&stats.terrainCache.entries>0,'Terrain cache has no bounded storage');
  for(const key of ['modeRequests','modeHits','modeBuilds'])assert.ok(Array.isArray(stats.terrainCache[key])&&stats.terrainCache[key].length===3&&stats.terrainCache[key].every(value=>Number.isSafeInteger(value)&&value>=0),`Invalid terrain ${key} mode counters`);
  const front=stats.terrainCache.front;
  assert.ok(front,'Renderer did not expose its separate front terrain cache');
  for(const key of ['requests','hits','builds','evictions','epochResets','capacityBytes','capacityEntries'])assert.ok(Number.isSafeInteger(front[key])&&front[key]>=0,`Invalid front terrain cache ${key}`);
  assert.ok(front.capacityBytes>0&&front.capacityEntries>0,'Front terrain cache has no bounded storage');
}
async function terrainSequence() {
  let turn=(await request('get_current_game_info')).game_turn;
  const modes=['isometric','cluedo','perspective'];
  report.terrain={turn,subtileBoundary:[192,192],patchSlabs:[62,66],changedSlabs:[63,64],modeCommands:['scenecheck overhead','scenecheck reference short-walls 0'],transitions:[],stages:[],scope:'Disposable normal-ISO, short-wall room/door, real possession-perspective and separate front-view terrain cache updates/reuse/reprojection. Turn is constant within each paused sequence; legitimate mode transitions may advance simulation. No full temporal/session/picking, shader or pixel-equality qualification'};
  async function stage(id,commands,{mode=0,builds=false,hits=false,uploads=false,unchanged=false,screenshot}={}) {
    const before=await rendererStats();checkTerrainCache(before);
    const front=mode==='front';
    const activity=(stats,key)=>front?stats.terrainCache.front[key]:stats.terrainCache[`mode${key[0].toUpperCase()+key.slice(1)}`][mode];
    const evidence={id,mode:front?'front':modes[mode],modeIndex:front?null:mode,commands,before};report.terrain.stages.push(evidence);
    for(const text of commands)await command(text);
    // Take the acceptance barrier after the last mutation: an earlier normal
    // frame between commands cannot satisfy the required post-mutation frame.
    const issued=await rendererStats();checkTerrainCache(issued);
    const deadline=Date.now()+30_000;let after;
    while(Date.now()<deadline) {
      const stats=await rendererStats();checkTerrainCache(stats);
      if(stats.epoch===issued.epoch&&stats.worldEpoch===stats.epoch&&stats.frameSequence>=issued.frameSequence+3&&stats.submitted>=issued.submitted+3
          &&activity(stats,'requests')>activity(before,'requests')
          &&(!builds||activity(stats,'builds')>activity(before,'builds'))&&(!hits||activity(stats,'hits')>activity(before,'hits'))
          &&(!uploads||stats.counters.terrainUploadBytes>before.counters.terrainUploadBytes)){after=stats;break;}
      await delay(100);
    }
    assert.ok(after,`${id}: no completed normal world with the required cache activity within 30 seconds`);
    assert.equal((await request('get_current_game_info')).game_turn,turn,`${id}: paused simulation turn changed`);
    assert.equal(after.counters.globalIdleCalls,before.counters.globalIdleCalls,`${id}: global GPU idle wait introduced`);
    assert.equal(after.counters.internalWaits,before.counters.internalWaits,`${id}: internal fence wait introduced`);
    evidence.after=after;evidence.turn=turn;
    evidence.cacheDelta=Object.fromEntries(['requests','hits','builds','evictions','resets'].map(key=>[key,after.terrainCache[key]-before.terrainCache[key]]));
    evidence.modeDelta=Object.fromEntries(['requests','hits','builds'].map(key=>[`mode${key[0].toUpperCase()+key.slice(1)}`,activity(after,key)-activity(before,key)]));
    if(front)evidence.frontDelta=Object.fromEntries(['requests','hits','builds','evictions','epochResets'].map(key=>[key,after.terrainCache.front[key]-before.terrainCache.front[key]]));
    const terrainCounters=['geometryUploadBytes','terrainComparedBytes','terrainUploadBytes','terrainReusedBytes','terrainDirtyRanges'];
    for(const key of terrainCounters)assert.ok(Number.isSafeInteger(before.counters[key])&&before.counters[key]>=0&&Number.isSafeInteger(after.counters[key])&&after.counters[key]>=before.counters[key],`Invalid terrain counter ${key}`);
    evidence.terrainUploadDelta=Object.fromEntries(terrainCounters.map(key=>[key,after.counters[key]-before.counters[key]]));
    if(unchanged) {
      assert.ok(front,'Strict unchanged qualification belongs to the warmed front sequence');
      assert.equal(evidence.frontDelta.builds,0,`${id}: unchanged front topology rebuilt after slot warm-up`);
      assert.equal(evidence.terrainUploadDelta.terrainUploadBytes,0,`${id}: unchanged front terrain uploaded after slot warm-up`);
      evidence.warmFrameSlots=3;
    }
    for(const key of ['gpuAllocationBytes','gpuCapacityBytes'])assert.ok(Number.isSafeInteger(after.terrainCache[key])&&after.terrainCache[key]>=0,`Invalid terrain storage ${key}`);
    evidence.terrainGpuStorage={allocationBytes:after.terrainCache.gpuAllocationBytes,capacityBytes:after.terrainCache.gpuCapacityBytes};
    assert.ok(evidence.modeDelta.modeRequests>0,`${id}: requested cache mode was not exercised`);
    if(screenshot)evidence.screenshot=await terrainPNG(screenshot,turn);
    await writeFile(join(out,'report.json'),JSON.stringify(report,null,2)+'\n');
  }
  async function transition(id,modeCommand) {
    const before=(await request('get_current_game_info')).game_turn;
    const commands=['pause',modeCommand,'pause'];
    await command(commands[0]);await command(commands[1]);
    // Reuse the tested possession-entry settling bound before pausing its
    // real controlled creature/camera. No paused equivalent is invented.
    await delay(600);await command(commands[2]);await waitPaused();
    turn=(await request('get_current_game_info')).game_turn;
    assert.ok(turn>=before,'Mode transition moved simulation backwards');
    report.terrain.transitions.push({id,commands,settleMs:600,turnBefore:before,turnAfter:turn});
  }
  await stage('visible-wall-patch',[
    'scenecheck reference short-walls 0',
    'scenecheck reference setup for x=62,66 do for y=62,66 do ChangeSlabType(x,y,11); ChangeSlabOwner(x,y,PLAYER0) end end; for x=63,64 do for y=63,64 do ChangeSlabType(x,y,2); ChangeSlabOwner(x,y,PLAYER0) end end; RevealMapRect(PLAYER0,192,192,30,30)',
    'scenecheck reference camera 49280 49280 0 256 -266 6500 94'
  ],{builds:true,screenshot:'terrain-before.png'});
  await stage('unchanged-wall',[],{hits:true});
  await stage('dig-to-floor',['scenecheck reference setup for x=63,64 do for y=63,64 do ChangeSlabType(x,y,11); ChangeSlabOwner(x,y,PLAYER0) end end'],{builds:true,screenshot:'terrain-floor.png'});
  await stage('rebuild-wall',['scenecheck reference setup for x=63,64 do for y=63,64 do ChangeSlabType(x,y,2); ChangeSlabOwner(x,y,PLAYER0) end end'],{builds:true,screenshot:'terrain-rebuilt.png'});
  await stage('conceal-patch',['scenecheck reference setup ConcealMapRect(PLAYER0,192,192,9,9,true)'],{builds:true,screenshot:'terrain-concealed.png'});
  await stage('reveal-patch',['scenecheck reference setup RevealMapRect(PLAYER0,192,192,9,9)'],{builds:true,screenshot:'terrain-revealed.png'});
  await stage('camera-reprojection',['scenecheck reference camera 50048 49792 0 512 -266 6500 94'],{hits:true});
  await stage('unchanged-camera',[],{hits:true});
  await stage('short-wall-room-door',[
    'scenecheck reference short-walls 1',
    'scenecheck reference setup ChangeSlabType(62,63,"TREASURY_AREA"); ChangeSlabOwner(62,63,PLAYER0); local roomSlab=GetSlab(62,63); assert(roomSlab.kind=="TREASURY_AREA" and roomSlab.room and roomSlab.room.type=="TREASURE","Expected an actual treasury room slab"); ChangeSlabType(64,65,2); ChangeSlabType(66,65,2); ChangeSlabType(65,65,11); ChangeSlabOwner(65,65,PLAYER0); PlaceDoor(PLAYER0,"WOOD",65,65,true,true); local found=false; for _,t in ipairs(GetThingsOfClass("DOOR")) do local p=t.pos; if t.model=="WOOD" and t.owner==PLAYER0 and p.val_x>=49920 and p.val_x<50688 and p.val_y>=49920 and p.val_y<50688 then found=true end end; assert(found,"Expected a real door on the disposable short-wall patch"); RevealMapRect(PLAYER0,192,192,30,30)',
    'scenecheck reference camera 49280 49280 0 256 -266 6500 94'
  ],{mode:1,builds:true,screenshot:'terrain-short-wall-room-door.png'});
  await stage('short-wall-unchanged',[],{mode:1,hits:true});
  await stage('short-wall-dig',['scenecheck reference setup for x=63,64 do for y=63,64 do ChangeSlabType(x,y,11); ChangeSlabOwner(x,y,PLAYER0) end end'],{mode:1,builds:true,screenshot:'terrain-short-wall-floor.png'});
  await stage('short-wall-rebuild',['scenecheck reference setup for x=63,64 do for y=63,64 do ChangeSlabType(x,y,2); ChangeSlabOwner(x,y,PLAYER0) end end'],{mode:1,builds:true});
  await stage('short-wall-camera',['scenecheck reference camera 50048 49792 0 512 -266 6500 94'],{mode:1,hits:true});
  await stage('short-wall-camera-unchanged',[],{mode:1,hits:true});
  await transition('enter-real-imp-possession','scenecheck possession IMP');
  await stage('perspective-wall',[
    'scenecheck reference camera 48000 49280 769 512 0 * 94',
    'scenecheck reference setup RevealMapRect(PLAYER0,192,192,30,30)'
  ],{mode:2,builds:true,screenshot:'terrain-perspective-wall.png'});
  await stage('perspective-unchanged',[],{mode:2,hits:true});
  await stage('perspective-dig',['scenecheck reference setup for x=63,64 do for y=63,64 do ChangeSlabType(x,y,11); ChangeSlabOwner(x,y,PLAYER0) end end'],{mode:2,builds:true,screenshot:'terrain-perspective-floor.png'});
  await stage('perspective-rebuild',['scenecheck reference setup for x=63,64 do for y=63,64 do ChangeSlabType(x,y,2); ChangeSlabOwner(x,y,PLAYER0) end end'],{mode:2,builds:true});
  await stage('perspective-camera',['scenecheck reference camera 48000 49792 769 640 0 * 94'],{mode:2,hits:true});
  await stage('perspective-camera-unchanged',[],{mode:2,hits:true});
  await transition('return-overhead','scenecheck overhead');
  await stage('returned-overhead',[
    'scenecheck reference short-walls 0',
    'scenecheck reference camera 49280 49280 0 256 -266 6500 94'
  ],{mode:0,hits:true,screenshot:'terrain-returned-overhead.png'});
  await transition('enter-front','scenecheck front');
  await stage('front-wall',[
    'scenecheck reference camera 49280 49280 0 0 -266 6500 94'
  ],{mode:'front',hits:true,screenshot:'terrain-front-before.png'});
  // Every stage waits for three accepted post-command worlds, warming all
  // frame slots before an unchanged interval measures cache/upload reuse.
  await stage('front-unchanged-wall',[],{mode:'front',hits:true,unchanged:true});
  // Raw front recipes are shared by exact inputs: a changed column can reuse
  // an existing floor/wall recipe while its changed geometry must be uploaded.
  await stage('front-dig',['scenecheck reference setup for x=63,64 do for y=63,64 do ChangeSlabType(x,y,11); ChangeSlabOwner(x,y,PLAYER0) end end'],{mode:'front',hits:true,uploads:true,screenshot:'terrain-front-floor.png'});
  await stage('front-unchanged-floor',[],{mode:'front',hits:true,unchanged:true});
  await stage('front-rebuild',['scenecheck reference setup for x=63,64 do for y=63,64 do ChangeSlabType(x,y,2); ChangeSlabOwner(x,y,PLAYER0) end end'],{mode:'front',hits:true,uploads:true,screenshot:'terrain-front-rebuilt.png'});
  await stage('front-camera',['scenecheck reference camera 50048 49792 0 512 -266 6500 94'],{mode:'front',hits:true,screenshot:'terrain-front-reprojection.png'});
  await stage('front-unchanged-camera',[],{mode:'front',hits:true,unchanged:true});
  await transition('return-overhead-from-front','scenecheck overhead');
  await stage('front-returned-overhead',[
    'scenecheck reference short-walls 0',
    'scenecheck reference camera 49280 49280 0 256 -266 6500 94'
  ],{mode:0,hits:true,screenshot:'terrain-front-returned-overhead.png'});
  report.terrain.finalTurn=turn;
  report.turn=turn;
}
let lastTemporalReceipt,lastTemporalStats;
const sameIdentity=(a,b)=>a.slot===b.slot&&a.incarnation===b.incarnation;
const inTemporalPatch=o=>o.simulationPosition[0]>=46080&&o.simulationPosition[0]<52992&&o.simulationPosition[1]>=46080&&o.simulationPosition[1]<52992;
async function temporalReceipt(id,turn,commands=[],live=false) {
  const evidence={id,commands,live,expectedTurn:turn};report.temporal.stages.push(evidence);
  const existing=new Set(await readdir(out));
  await command('scenecheck receipt');
  const deadline=Date.now()+30_000;let receipt,file;
  while(Date.now()<deadline) {
    const candidates=(await readdir(out)).filter(name=>!existing.has(name)&&/^temporal-\d+-\d+\.png\.ready\.json$/.test(name));
    assert.ok(candidates.length<=1,'Serial receipt request produced multiple completion files');
    if(candidates.length) {
      file=candidates[0];
      try {receipt=JSON.parse(await readFile(join(out,file),'utf8'));break;}
      catch(error) {if(!(error instanceof SyntaxError)&&!['ENOENT','EBUSY'].includes(error.code))throw error;}
    }
    await delay(50);
  }
  assert.ok(receipt,`${id}: no accepted presentation receipt within 30 seconds`);
  evidence.file=file;evidence.image=file.slice(0,-11);
  evidence.epoch=receipt.epoch;evidence.sequence=receipt.sequence;evidence.turn=receipt.turn;
  assert.equal(receipt.schema,1);assert.equal(receipt.backend,'DX12');
  for(const key of ['epoch','sequence','turn'])assert.ok(Number.isSafeInteger(receipt[key])&&receipt[key]>=0,`Invalid receipt ${key}`);
  for(const key of ['epoch','sequence'])assert.ok(Number.isSafeInteger(receipt.presentation[key])&&receipt.presentation[key]>0,`Invalid presentation ${key}`);
  evidence.presentation=receipt.presentation;
  assert.equal(file,`temporal-${receipt.epoch}-${receipt.sequence}.png.ready.json`);
  if(lastTemporalReceipt) {
    assert.ok(receipt.epoch>lastTemporalReceipt.epoch||(receipt.epoch===lastTemporalReceipt.epoch&&receipt.sequence>lastTemporalReceipt.sequence),'Receipt did not advance the owned scene identity');
    const previous=lastTemporalReceipt.presentation,current=receipt.presentation;
    assert.ok(current.epoch>previous.epoch||(current.epoch===previous.epoch&&current.sequence>previous.sequence),'Receipt did not advance the accepted presentation identity');
  }
  if(!live)assert.equal(receipt.turn,turn,`${id}: accepted snapshot has a different paused turn`);
  const png=await readFile(join(out,evidence.image));
  assert.equal(png.subarray(0,8).toString('hex'),'89504e470d0a1a0a');
  assert.equal(png.subarray(-12).toString('hex'),'0000000049454e44ae426082');
  assert.deepEqual([png.readUInt32BE(16),png.readUInt32BE(20)],expectedSize);
  assert.deepEqual([receipt.imageWidth,receipt.imageHeight],expectedSize);
  evidence.camera=receipt.camera;
  evidence.surface={lavaScroll:receipt.surface.lavaScroll,waterScroll:receipt.surface.waterScroll,textureGeneration:receipt.surface.textureGeneration};
  if(!live) {
    const stats=await rendererStats();
    assert.equal(stats.epoch,receipt.presentation.epoch,'Renderer epoch differs from accepted presentation');
    assert.equal(stats.sceneEpoch,receipt.epoch,'Renderer scene epoch differs from captured scene');
    assert.ok(stats.frameSequence>=receipt.presentation.sequence,'Statistics precede the completed presentation');
    if(lastTemporalStats)for(const key of ['globalIdleCalls','internalWaits'])assert.equal(stats.counters[key],lastTemporalStats.counters[key],`${id}: temporal presentation introduced ${key}`);
    lastTemporalStats=stats;evidence.stats=stats;
    assert.equal((await request('get_current_game_info')).game_turn,turn,`${id}: paused turn changed`);
  }
  lastTemporalReceipt=receipt;
  await writeFile(join(out,'report.json'),JSON.stringify(report,null,2)+'\n');
  return receipt;
}
async function temporalSequence() {
  report.temporal={stages:[],transitions:[],liveSurfaces:true,scope:'One controlled creature, local particles/light lifetime, moving TORCH/light, boundary water/lava/bridge replacement, a real door cycle driven by scripted creature approach/retreat, exact paused ticks, real speed/camera windows and optional save/load reset. Receipts pair immutable scene metadata with accepted DX12 PNGs. Object.frame is the captured animation frame; no autonomous door navigation, resolved-emission, shader, pixel-equality or full gameplay qualification'};
  const pose='scenecheck reference camera 49536 49536 0 256 -266 6500 94';
  const cameraCommands=['pause','scenecheck overhead','zoomto.subtilecoord 193 193','pause'];
  const cameraBefore=(await request('get_current_game_info')).game_turn;
  await command(cameraCommands[0]);await command(cameraCommands[1]);await command(cameraCommands[2]);await delay(1500);await command(cameraCommands[3]);await waitPaused();
  let turn=(await request('get_current_game_info')).game_turn;
  report.temporal.transitions.push({id:'real-camera-to-patch',commands:cameraCommands,settleMs:1500,turnBefore:cameraBefore,turnAfter:turn});
  const setup=[
    'scenecheck reference setup for _,kind in ipairs({"CREATURE","TRAP","OBJECT","EFFECTELEM","EFFECT","EFFECTGEN","DEADCREATURE","SHOT","DOOR"}) do for _,t in ipairs(GetThingsOfClass(kind)) do local p=t.pos; if p.val_x>=46080 and p.val_x<52992 and p.val_y>=46080 and p.val_y<52992 then t:delete() end end end; for x=60,68 do for y=60,68 do ChangeSlabType(x,y,11); ChangeSlabOwner(x,y,PLAYER0) end end; ChangeSlabType(62,62,13); ChangeSlabType(66,62,12); ChangeSlabType(62,66,13); ChangeSlabType(62,66,51); RevealMapRect(PLAYER0,193,193,36,36)',
    'scenecheck reference setup temporalTroll=AddCreatureToLevel(PLAYER0,"TROLL",{stl_x=193,stl_y=193},3,0); assert(temporalTroll); temporalTroll.pos={val_x=49536,val_y=49536,val_z=256}; temporalTroll.state="ManualControl"; temporalTroll.anim_speed=256; temporalTorch=AddObjectToLevelAtPos("TORCH",197,193,0,PLAYER0,0); assert(temporalTorch)',
    'scenecheck reference setup assert(GetSlab(62,62).kind=="WATER" and GetSlab(66,62).kind=="LAVA" and GetSlab(62,66).kind=="BRIDGE_FRAME","Expected actual water, lava and bridge slabs")',
    'scenecheck reference short-walls 0',pose
  ];
  for(const text of setup)await command(text);
  lastTemporalStats=await rendererStats();
  const initial=await temporalReceipt('baseline',turn,setup);
  const trolls=initial.objects.filter(o=>o.class===5&&o.model===16&&o.owner===0&&inTemporalPatch(o));
  assert.equal(trolls.length,1,'Expected one actual controlled TROLL on the disposable patch');
  const troll=trolls[0],torch=initial.objects.find(o=>o.class===1&&o.model===2&&o.simulationPosition[0]===50560&&o.simulationPosition[1]===49536);
  assert.ok(torch,'Actual TORCH missing from accepted baseline');
  const near=(light,x,y)=>Math.abs(light.position[0]-x)<256&&Math.abs(light.position[1]-y)<256;
  async function exactTicks(count,commands,perTick) {
    assert.ok(Number.isInteger(count)&&count>0&&count<=60);
    for(let tick=0;tick<count;tick++) {
      if(perTick){await command(perTick);commands.push(perTick);}
      const before=(await request('get_current_game_info')).game_turn;
      await command('step');commands.push('step');
      const deadline=Date.now()+10_000;let completed=false;
      while(Date.now()<deadline) {
        const observed=(await request('get_current_game_info')).game_turn;
        assert.ok(observed<=before+1,'Single step advanced more than one turn');
        if(observed===before+1){await waitPaused();completed=true;break;}
        await delay(20);
      }
      assert.ok(completed,'Single step did not complete within 10 seconds');
      turn=before+1;
    }
    await command(pose);commands.push(pose);
  }
  const torchLight=initial.lighting.lights.find(l=>near(l,50560,49536));
  assert.ok(torchLight&&torchLight.intensity>0,'TORCH did not create a retained positive light');
  report.temporal.controlled={troll,torch,torchLight};
  await delay(350);
  const paused=await temporalReceipt('paused-live-surfaces',turn);
  assert.deepEqual(paused.objects.find(o=>sameIdentity(o,troll))?.simulationPosition,troll.simulationPosition,'Controlled creature moved while paused');
  assert.ok(JSON.stringify(paused.surface.wibble)!==JSON.stringify(initial.surface.wibble)||paused.surface.waterScroll!==initial.surface.waterScroll||paused.surface.lavaScroll!==initial.surface.lavaScroll,'Opt-in live surfaces did not change during the paused presentation window');
  const explosion='scenecheck reference setup temporalEffect=CreateEffectAtCoords("EFFECT_EXPLOSION_2",49536,50560,768); assert(temporalEffect)';
  await command(explosion);
  const spawned=await temporalReceipt('explosion-spawn',turn,[explosion]);
  const effectLight=spawned.lighting.lights.find(l=>near(l,49536,50560)&&!initial.lighting.lights.some(old=>sameIdentity(old,l)));
  assert.ok(effectLight&&effectLight.intensity>0,'Explosion did not create an actual local light');
  report.temporal.controlled.effectLight=effectLight;
  const samples=[];let elapsed=0;
  for(const target of [1,2,4,8]) {
    const commands=[];
    await exactTicks(target-elapsed,commands,'scenecheck reference setup assert(temporalTroll); temporalTroll:set_velocity(96,1024,0)');
    elapsed=target;
    const receipt=await temporalReceipt(`step-${target}`,turn,commands);
    const actor=receipt.objects.find(o=>sameIdentity(o,troll));assert.ok(actor,`Controlled TROLL missing after ${target} exact ticks`);
    const particles=receipt.objects.filter(o=>o.class===3&&Math.abs(o.simulationPosition[0]-49536)<2048&&Math.abs(o.simulationPosition[1]-50560)<2048);
    const light=receipt.lighting.lights.find(l=>sameIdentity(l,effectLight));
    const sample={target,turn,actor,particles:particles.map(o=>({slot:o.slot,incarnation:o.incarnation,position:o.simulationPosition,animation:o.animation,frame:o.frame})),effectLight:light??null};
    samples.push(sample);report.temporal.timeline=samples;
  }
  assert.ok(samples.some(s=>JSON.stringify(s.actor.simulationPosition)!==JSON.stringify(troll.simulationPosition)),'Controlled TROLL did not move across the accepted exact-tick timeline');
  assert.ok(samples.some(s=>s.actor.frame!==troll.frame),'Controlled TROLL animation frame did not advance');
  assert.ok(samples[0].particles.length>0,'Explosion did not produce local particles on the first exact tick');
  assert.ok(samples.some(s=>s.effectLight&&s.effectLight.intensity<effectLight.intensity),'Explosion light intensity did not decay across exact ticks');
  assert.equal(samples.at(-1).effectLight,null,'Expired explosion light remained after eight exact ticks');
  const remove='scenecheck reference setup temporalTroll:delete(); temporalTorch:delete(); temporalTroll=nil; temporalTorch=nil';
  await command(remove);
  const deleted=await temporalReceipt('delete-controlled-objects',turn,[remove]);
  for(const old of [troll,torch])assert.ok(!deleted.objects.some(o=>sameIdentity(o,old)),'Deleted controlled incarnation remained in the accepted scene');
  assert.ok(!deleted.lighting.lights.some(l=>sameIdentity(l,torchLight)),'Deleted TORCH light remained in the accepted scene');
  const recreate='scenecheck reference setup temporalTroll=AddCreatureToLevel(PLAYER0,"TROLL",{stl_x=193,stl_y=193},3,0); assert(temporalTroll); temporalTroll.pos={val_x=49536,val_y=49536,val_z=256}; temporalTroll.state="ManualControl"; temporalTroll.anim_speed=256; temporalTorch=AddObjectToLevelAtPos("TORCH",197,193,0,PLAYER0,0); assert(temporalTorch)';
  await command(recreate);
  const recreated=await temporalReceipt('recreate-controlled-objects',turn,[recreate]);
  const replacement=recreated.objects.find(o=>o.class===5&&o.model===16&&o.owner===0&&inTemporalPatch(o));assert.ok(replacement);
  if(replacement.slot===troll.slot)assert.notEqual(replacement.incarnation,troll.incarnation,'Recycled creature slot kept its old incarnation');
  const replacementTorch=recreated.objects.find(o=>o.class===1&&o.model===2&&o.simulationPosition[0]===50560&&o.simulationPosition[1]===49536);
  assert.ok(replacementTorch,'Recreated TORCH missing from accepted scene');
  const replacementLight=recreated.lighting.lights.find(l=>near(l,50560,49536));assert.ok(replacementLight&&replacementLight.intensity>0,'Recreated TORCH light missing');
  for(const [old,current] of [[torch,replacementTorch],[torchLight,replacementLight]])if(current.slot===old.slot)assert.notEqual(current.incarnation,old.incarnation,'Recycled controlled slot kept its old incarnation');
  report.temporal.recreated={troll:replacement,torch:replacementTorch,torchLight:replacementLight,reusedSlot:replacement.slot===troll.slot};
  report.temporal.movingLight=[];
  let previousTorchLight=replacementLight;
  for(const x of [49024,49536]) {
    const move=`scenecheck reference setup temporalTorch.pos={val_x=${x},val_y=49536,val_z=512}`;
    await command(move);const commands=[move];await exactTicks(1,commands);
    const receipt=await temporalReceipt(`moving-torch-${x}`,turn,commands);
    const object=receipt.objects.find(o=>sameIdentity(o,replacementTorch)),light=receipt.lighting.lights.find(l=>sameIdentity(l,replacementLight));
    assert.ok(object&&light,'Moved TORCH or its original light incarnation disappeared');
    assert.deepEqual(object.simulationPosition.slice(0,2),[x,49536]);assert.deepEqual(light.position.slice(0,2),[x,49536]);
    assert.notDeepEqual(light.position.slice(0,2),previousTorchLight.position.slice(0,2),'TORCH light did not follow the controlled move');
    assert.deepEqual(light.previousPosition.slice(0,2),previousTorchLight.position.slice(0,2),'Moving light lost its previous position');
    // update_things records get_gameturn before game_update increments the
    // completed-turn counter captured by the following presentation receipt.
    const updateTurn=turn-1;
    assert.equal(light.lastTurnMoved,updateTurn,'Moving light did not record the exact update tick');
    report.temporal.movingLight.push({turn,updateTurn,object,light});previousTorchLight=light;
  }
  const safeObjects='scenecheck reference setup temporalTroll.pos={val_x=51584,val_y=51584,val_z=256}; temporalTorch.pos={val_x=50560,val_y=49536,val_z=512}';
  await command(safeObjects);const boundaryCommands=[safeObjects];await exactTicks(1,boundaryCommands);
  const boundaryBefore=await temporalReceipt('boundary-terrain-before',turn,boundaryCommands);
  const boundaryCells=[[191,191],[192,191],[191,192],[192,192]];
  function boundaryGeometry(receipt,cells=boundaryCells) {
    assert.ok(Array.isArray(receipt.columns),'Temporal receipt lacks immutable terrain columns');
    return cells.map(([x,y])=>{
      const column=receipt.columns.find(c=>c.x===x&&c.y===y);
      assert.ok(column&&column.revealed&&column.inView,`Boundary column ${x},${y} was not actually captured in view`);
      assert.ok(Array.isArray(column.cubes)&&column.cubes.length===8,'Invalid immutable column cube stack');
      return {x,y,cubes:column.cubes,solidMask:column.solidMask,floorTexture:column.floorTexture,mapFlags:column.mapFlags};
    });
  }
  let previousGeometry=boundaryGeometry(boundaryBefore),previousUpload=lastTemporalStats.counters.terrainUploadBytes;
  report.temporal.terrainReplacement={subtileBoundary:[192,192],slabs:[63,64],cells:boundaryCells,baseline:previousGeometry,phases:[]};
  for(const [id,kind,name] of [['water',13,'WATER'],['lava',12,'LAVA'],['bridge',51,'BRIDGE_FRAME'],['bridge-to-water',13,'WATER'],['restored-floor',11,'PRETTY_PATH']]) {
    const replace=`scenecheck reference setup for x=63,64 do for y=63,64 do ChangeSlabType(x,y,${kind}); assert(GetSlab(x,y).kind=="${name}","Unexpected boundary slab replacement") end end; RevealMapRect(PLAYER0,193,193,36,36)`;
    await command(replace);const commands=[replace];await exactTicks(1,commands);
    const receipt=await temporalReceipt(`boundary-terrain-${id}`,turn,commands),geometry=boundaryGeometry(receipt);
    for(let i=0;i<geometry.length;i++)assert.notDeepEqual(geometry[i],previousGeometry[i],`Boundary column ${geometry[i].x},${geometry[i].y} retained its previous terrain recipe`);
    const terrainUploadBytes=lastTemporalStats.counters.terrainUploadBytes-previousUpload;
    assert.ok(terrainUploadBytes>0,'Boundary replacement did not upload actual changed terrain');
    report.temporal.terrainReplacement.phases.push({id,kind,name,turn,geometry,terrainUploadBytes});
    previousGeometry=geometry;previousUpload=lastTemporalStats.counters.terrainUploadBytes;
  }
  const doorSetup='scenecheck reference setup ChangeSlabType(62,65,2); ChangeSlabType(64,65,2); ChangeSlabType(63,65,11); ChangeSlabOwner(63,65,PLAYER0); PlaceDoor(PLAYER0,"WOOD",63,65,false,true); local d=GetThingsOnSlab(63,65,"DOOR"); assert(#d==1 and d[1].model=="WOOD","Expected one actual unlocked WOOD door")';
  await command(doorSetup);const doorBefore=await temporalReceipt('door-closed-before',turn,[doorSetup]);
  function retainedDoor(receipt) {
    assert.ok(Array.isArray(receipt.doors),'Temporal receipt lacks immutable door metadata');
    const door=receipt.doors.find(d=>d.slab[0]===63&&d.slab[1]===65);
    assert.ok(door&&door.model===1&&door.owner===0&&door.locked===0,'Actual owned unlocked WOOD door missing from retained scene');
    return door;
  }
  const originalDoor=retainedDoor(doorBefore);assert.equal(originalDoor.state,2);assert.equal(originalDoor.openAmount,0);
  const doorCells=[[189,195],[190,195],[191,195],[189,196],[190,196],[191,196],[189,197],[190,197],[191,197]];
  let previousDoorGeometry=boundaryGeometry(doorBefore,doorCells);
  report.temporal.door={slab:[63,65],trigger:'Scripted owned TROLL approach/retreat invokes normal proximity opening and timeout closing; no direct door-state assignment or autonomous navigation claim',phases:[{id:'closed-before',turn,door:originalDoor,geometry:previousDoorGeometry}]};
  for(const [id,move,count,state,amount] of [
    ['opening','scenecheck reference setup temporalTroll.pos={val_x=48768,val_y=50816,val_z=256}',2,3,256],
    ['open',null,2,1,768],
    ['closing','scenecheck reference setup temporalTroll.pos={val_x=51584,val_y=51584,val_z=256}',12,4,512],
    ['closed',null,3,2,0]
  ]) {
    const commands=[];if(move){await command(move);commands.push(move);}await exactTicks(count,commands);
    const receipt=await temporalReceipt(`door-${id}`,turn,commands),door=retainedDoor(receipt);
    assert.ok(sameIdentity(door,originalDoor),'Door incarnation changed during its real open/close cycle');
    assert.equal(door.state,state,`Door did not reach its actual ${id} simulation phase`);assert.equal(door.openAmount,amount);
    const geometry=boundaryGeometry(receipt,doorCells);
    assert.notDeepEqual(geometry,previousDoorGeometry,`Door ${id} phase did not change its retained terrain geometry`);
    report.temporal.door.phases.push({id,turn,ticks:count,door,geometry});previousDoorGeometry=geometry;
  }
  for(const fps of [10,20]) {
    const before=turn,commands=['pause',`fps ${fps}`,'pause'];
    await command(commands[0]);await command(commands[1]);await delay(600);
    const live=await temporalReceipt(`live-fps-${fps}`,null,commands.slice(0,2),true);
    assert.ok(live.turn>before,`Real fps ${fps} window did not advance the accepted snapshot turn`);
    if(fps===20){await command('fps');commands.splice(2,0,'fps');}
    await command('pause');await waitPaused();turn=(await request('get_current_game_info')).game_turn;
    report.temporal.transitions.push({id:`fps-${fps}`,commands,settleMs:600,turnBefore:before,receiptTurn:live.turn,turnAfter:turn});
    await command(pose);await temporalReceipt(`paused-after-fps-${fps}`,turn,[pose]);
  }
  const camera='scenecheck reference camera 50048 49792 0 512 -266 6500 94';
  await command(camera);const reprojected=await temporalReceipt('paused-camera-reprojection',turn,[camera]);
  assert.deepEqual(reprojected.camera.position,[50048,49792,0]);
  await command(pose);await temporalReceipt('paused-camera-restored',turn,[pose]);
  report.temporal.finalTurn=turn;report.turn=turn;
}
let windowRequestSequence=0;
async function windowOperation(operation,fields={}) {
  const id=++windowRequestSequence,requestPath=join(out,`window-${id}-request.json`),resultPath=join(out,`window-${id}-result.json`);
  await writeFile(requestPath,JSON.stringify({operation,...fields},null,2)+'\n');
  const helper=fileURLToPath(new URL('./live-window.ps1',import.meta.url));
  const exitCode=await new Promise((resolve,reject)=>{
    const child=spawn(windowHost,['-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',helper,'-GameProcessId',String(gamePid),'-GameExecutable',windowExe,'-RequestPath',requestPath,'-ResultPath',resultPath],{windowsHide:true,stdio:['ignore','ignore','pipe']});
    let errors='';child.stderr.on('data',data=>{if(errors.length<8192)errors+=data.toString();});
    const timer=setTimeout(()=>{child.kill();reject(new Error(`Owned window ${operation} exceeded 15 seconds`));},15_000);
    child.once('error',error=>{clearTimeout(timer);reject(error);});
    child.once('exit',code=>{clearTimeout(timer);if(code===null)reject(new Error(`Window helper terminated: ${errors}`));else resolve(code);});
  });
  const result=JSON.parse(await readFile(resultPath,'utf8'));
  assert.equal(result.processId,gamePid,'Window helper acted on a different game process');
  if(!result.passed||exitCode!==0)throw Object.assign(new Error(result.error??`Window helper exited ${exitCode}`),{windowResult:result});
  assert.ok(result.windowHandle,'Owned window helper did not return its validated HWND');
  return result;
}
async function windowSequence() {
  const turn=(await request('get_current_game_info')).game_turn;
  const original=await windowOperation('inspect');let restored=false;
  report.window={turn,original,stages:[],scalingQualified:false,scope:'Owned game HWND client resize/minimize/restore, submitted-frame plateau/resumption, configured resource budgets and actual client-area capture. Native source resolution stays fixed; pending-readback lifecycle, picking, DPI transitions, fullscreen, device loss and full menu lifecycle remain open'};
  function checkWindow(stats) {
    const window=stats.window;assert.ok(window,'Missing SDL/DX12 window diagnostics');
    for(const key of ['size','drawable','availableDrawable','native'])assert.ok(Array.isArray(window[key])&&window[key].length===2&&window[key].every(value=>Number.isSafeInteger(value)&&value>=0),`Invalid window ${key}`);
    assert.ok(Array.isArray(window.presentRect)&&window.presentRect.length===4&&window.presentRect.every(Number.isSafeInteger),'Invalid game present rectangle');
    assert.deepEqual(window.native,expectedSize,'Client window changes resized the native game surface');
    assert.ok(Number.isSafeInteger(window.resizes)&&window.resizes>=0,'Invalid backbuffer resize count');
    if(stats.readerHold.pendingEpoch)assert.equal(stats.readerHold.pendingEpoch,stats.epoch,'Pending window frame belongs to a stale renderer epoch');
  }
  async function applied(id,operation,before,predicate) {
    const deadline=Date.now()+10_000;let after;
    while(Date.now()<deadline) {
      const stats=await rendererStats();checkWindow(stats);
      if(predicate(stats)){after=stats;break;}await delay(50);
    }
    assert.ok(after,`${id}: SDL/DX12 window state did not apply within 10 seconds`);
    assert.equal((await request('get_current_game_info')).game_turn,turn,`${id}: paused turn changed`);
    report.window.stages.push({id,operation,before,after,counterDelta:Object.fromEntries(Object.keys(after.counters).map(key=>[key,after.counters[key]-before.counters[key]]))});
    await writeFile(join(out,'report.json'),JSON.stringify(report,null,2)+'\n');return after;
  }
  const baseline=await rendererStats();checkWindow(baseline);
  assert.ok(baseline.window.resizable&&!baseline.window.suspended&&!original.minimized,'Game window is not ready for scoped resizing');
  assert.deepEqual(baseline.window.drawable,original.clientSize,'SDL raw drawable differs from owned client dimensions');
  report.window.baseline=baseline;
  try {
    const resized=await windowOperation('resize',{width:1280,height:900});
    const resizeStats=await applied('unequal-aspect-resize',resized,baseline,stats=>!stats.window.suspended&&stats.window.availableDrawable[0]===1280&&stats.window.availableDrawable[1]===900&&stats.window.resizes>baseline.window.resizes&&stats.worldEpoch===stats.epoch&&stats.submitted>=baseline.submitted+3);
    const [x,y,w,h]=resizeStats.window.presentRect;
    assert.ok(x>=0&&y>=0&&w>0&&h>0&&x+w<=1280&&y+h<=900,'Present rectangle lies outside the resized client');
    const points=[];
    if(y>0)points.push({x:640,y:Math.floor(y/2)});
    if(y+h<900)points.push({x:640,y:Math.floor((y+h+900)/2)});
    if(x>0)points.push({x:Math.floor(x/2),y:450});
    if(x+w<1280)points.push({x:Math.floor((x+w+1280)/2),y:450});
    assert.ok(points.length>0,'Unequal-aspect resize produced no letterbox margins');
    try {
      const capture=await windowOperation('capture',{image:'window-resized-client.png',points});
      assert.deepEqual(capture.clientSize,[1280,900]);
      for(const pixel of capture.pixels)assert.deepEqual(pixel.rgba.slice(0,3),[0,0,0],'Actual client letterbox margin is not black');
      report.window.clientCapture=capture;report.window.scalingQualified=true;
    }catch(error) {
      if(!error.windowResult?.windowHandle||!/blank or lacks visible world detail/.test(error.message))throw error;
      report.window.clientCapture=error.windowResult;
      report.window.captureLimitation='Owned HWND capture returned no reliable world detail; window lifecycle statistics only, final scaling image remains unqualified';
    }
    report.window.nativeScreenshot=await terrainPNG('window-resized-native.png',turn);
    const minimized=await windowOperation('minimize');
    const suspended=await applied('minimize',minimized,resizeStats,stats=>stats.window.minimized&&stats.window.suspended&&stats.window.availableDrawable.every(value=>value===0));
    await delay(350);const settled=await rendererStats();checkWindow(settled);
    assert.ok(settled.window.minimized&&settled.window.suspended);assert.deepEqual(settled.window.availableDrawable,[0,0]);
    assert.equal(settled.submitted,suspended.submitted,'DX12 submitted a new frame while minimized after suspension applied');
    assert.equal(settled.frameSequence,suspended.frameSequence,'Accepted window frame advanced while minimized');
    report.window.minimizedInterval={settleMs:350,before:suspended,after:settled};
    const shown=await windowOperation('restore',{rect:resized.rect,maximized:false,minimized:false});
    const resumed=await applied('restore-resized-client',shown,settled,stats=>!stats.window.minimized&&!stats.window.suspended&&stats.window.availableDrawable[0]===1280&&stats.window.availableDrawable[1]===900&&stats.worldEpoch===stats.epoch&&stats.submitted>settled.submitted&&stats.frameSequence>settled.frameSequence);
    const restoredWindow=await windowOperation('restore',{rect:original.rect,maximized:original.maximized,minimized:original.minimized});
    assert.deepEqual(restoredWindow.rect.slice(0,2),original.rect.slice(0,2),'Restored window position differs from its original position');
    assert.equal(restoredWindow.minimized,original.minimized);assert.equal(restoredWindow.maximized,original.maximized);
    if(!restoredWindow.rectMatches)assert.ok(restoredWindow.allowedClamp&&restoredWindow.clamp,'Window size changed without an exact reported maximum-tracking clamp');
    const final=await applied('restore-original-client',restoredWindow,resumed,stats=>!stats.window.suspended&&stats.window.availableDrawable[0]===restoredWindow.clientSize[0]&&stats.window.availableDrawable[1]===restoredWindow.clientSize[1]&&stats.worldEpoch===stats.epoch&&stats.submitted>=resumed.submitted+3);
    assert.deepEqual(final.window.drawable,restoredWindow.clientSize);restored=true;
    report.window.restored=true;report.window.restoreGeometry={requestedRect:original.rect,appliedRect:restoredWindow.rect,requestedClient:original.clientSize,appliedClient:restoredWindow.clientSize,exact:restoredWindow.rectMatches,clamp:restoredWindow.clamp??null};
  }finally {
    if(!restored) {
      try {report.window.failureRestore=await windowOperation('restore',{rect:original.rect,maximized:original.maximized,minimized:original.minimized});report.window.restored=true;}
      catch(error){report.window.restoreError=error.message;}
    }
    await writeFile(join(out,'report.json'),JSON.stringify(report,null,2)+'\n');
  }
}
try {
  socket = await connect();
  socket.on('error', error => { failure=error; pending?.reject(error); });
  socket.on('close', () => { if (!shuttingDown) { failure ??= new Error('API connection closed unexpectedly'); pending?.reject(failure); } });
  socket.on('data', data => {
    buffer += data.toString('utf8').replaceAll('\0','');
    let response;
    try { response=JSON.parse(buffer); } catch { return; }
    buffer='';
    if (response.ack === pending?.ack) pending.resolve(response);
  });
  report.version = await request('get_kfx_info', {}, Math.max(1, startupDeadline-Date.now()));
  const levelDeadline = Date.now()+30_000;
  while (Date.now() < levelDeadline) {
    report.levelInfo = await request('get_level_info');
    report.turn = (await request('get_current_game_info')).game_turn;
    if (report.levelInfo.level_number === level && report.turn > 0) break;
    await delay(250);
  }
  assert.equal(report.levelInfo.level_number, level, 'Requested campaign level did not become ready');
  assert.equal(report.levelInfo.is_multiplayer, false);
  assert.ok(report.turn > 0, 'Campaign simulation did not become ready');
  if(terrainUpdates||temporalUpdates)await command('scenecheck overhead');
  await command('pause');
  // This guarded no-op confirms the pause packet has taken effect before the
  // screenshot request; pausing does not stop the real renderer presenting.
  await waitPaused();
  if (report.clearUI) await command('scenecheck reference clear-ui');
  report.turn = (await request('get_current_game_info')).game_turn;
  if (report.paletteFade) {
    await request('console_command', {command:'scenecheck reference palette-fade'}, 30_000);
    const restoredTurn = (await request('get_current_game_info')).game_turn;
    assert.equal(restoredTurn, report.turn, 'Simulation advanced during the blocking palette fade');
    report.fade = {steps:4, reachedBlack:true, startingPaletteRestored:true, turnBefore:report.turn, turnAfter:restoredTurn};
  }
  if(terrainUpdates)await terrainSequence();
  if(temporalUpdates)await temporalSequence();
  if(reloads) {
    const savePath=join(runtime,'save','fx1g9997.sav');
    try {await stat(savePath);throw new Error('Save slot 9997 is occupied; refusing to overwrite it');}
    catch(error) {if(error.code!=='ENOENT')throw error;}
    await waitPaused();
    const beforeSaveTurn=(await request('get_current_game_info')).game_turn;
    let before=await rendererStats();
    assert.ok(before.worldEpoch===before.epoch&&before.frameSequence>0,'Initial renderer did not accept its current normal world');
    report.reload={slot:9997,saveTurnBounds:{before:beforeSaveTurn},baseline:before,iterations:[],scope:'Real save/load through normal unpaused API commands, then paused world acceptance and configured resource bounds; first loaded turn is bounded by pre/post-save observations and later loads restore it exactly; first counter delta includes save plus load; no complete-state, session or performance qualification'};
    await writeFile(join(out,'reload-save-requested.json'),JSON.stringify({slot:9997,savePath})+'\n');
    // The normal API rejects generic save/load commands while paused. Use its
    // ordinary pause toggle, then let game.load(...,1) restore a paused state.
    await command('pause');
    await request('console_command',{command:'game.save 9997 Live-renderer-reload'},30_000);
    const afterSaveTurn=(await request('get_current_game_info')).game_turn;
    assert.ok(afterSaveTurn>=beforeSaveTurn,'Simulation turn decreased during save');
    report.reload.saveTurnBounds.after=afterSaveTurn;
    assert.ok((await stat(savePath)).size>0,'Disposable save was not written');
    let held;
    if(temporalUpdates) {
      // Save/load use the normal unpaused API. Pause after save only to arm
      // the diagnostic hold, then load while the old pending packet is blocked.
      await command('pause');await waitPaused();
      held=await holdPendingReader();before=held;
      report.reload.scope=report.reload.scope.replace('first counter delta includes save plus load','temporal first-load counters begin at the held pending world after save');
    }
    let savedTurn;
    for(let iteration=1;iteration<=reloads;iteration++) {
      if(iteration>1)before=await rendererStats();
      await writeFile(join(out,`reload-${iteration}-before.json`),JSON.stringify(before,null,2)+'\n');
      if(iteration>1||held)await command('pause');
      await request('console_command',{command:'game.load 9997 1'},30_000);
      await waitPaused();
      if(held){await verifyHeldResetAndRelease(held);held=undefined;}
      const after=await waitWorldAfterReload(before);
      for(const field of ['allocations','uploadBytes','internalWaits','globalIdleCalls'])assert.ok(after.counters[field]>=before.counters[field],`Renderer counter ${field} reset during reload`);
      const loadedLevel=await request('get_level_info'),turn=(await request('get_current_game_info')).game_turn;
      assert.equal(loadedLevel.level_number,level);assert.equal(loadedLevel.is_multiplayer,false);
      if(iteration===1) {
        assert.ok(turn>=beforeSaveTurn&&turn<=afterSaveTurn,'First loaded turn falls outside the observed save bounds');
        savedTurn=turn;report.reload.savedTurn=turn;
      } else assert.equal(turn,savedTurn,'Later load did not restore the exact first loaded paused turn');
      await writeFile(join(out,`reload-${iteration}-after.json`),JSON.stringify(after,null,2)+'\n');
      report.reload.iterations.push({iteration,turn,before,after,counterDelta:Object.fromEntries(Object.keys(after.counters).map(key=>[key,after.counters[key]-before.counters[key]]))});
      await writeFile(join(out,'report.json'),JSON.stringify(report,null,2)+'\n');
    }
    report.turn=savedTurn;
  }
  if(temporalUpdates&&reloads) {
    const before=lastTemporalReceipt;
    lastTemporalStats=undefined;
    const loaded=await temporalReceipt('after-real-reload',report.turn,[`game.load 9997 1 (${reloads} reloads)`]);
    assert.ok(loaded.epoch>before.epoch,'Real reload did not advance the accepted temporal scene epoch');
    report.temporal.reload={epochBefore:before.epoch,epochAfter:loaded.epoch,turn:loaded.turn};
  }
  if(windowUpdates)await windowSequence();
  await command('scenecheck reference screenshot');
  const imagePath = join(out, 'presentation.png');
  const imageDeadline = Date.now()+30_000;
  let png;
  while (Date.now() < imageDeadline) {
    try {
      const bytes = await readFile(imagePath);
      if (bytes.length >= 33 && bytes.subarray(0,8).toString('hex') === '89504e470d0a1a0a' && bytes.subarray(-12).toString('hex') === '0000000049454e44ae426082') { png=bytes; break; }
    } catch (error) { if (!['ENOENT','EBUSY'].includes(error.code)) throw error; }
    await delay(100);
  }
  assert.ok(png, 'A complete presentation.png was not written within 30 seconds');
  report.dimensions = [png.readUInt32BE(16), png.readUInt32BE(20)];
  assert.deepEqual(report.dimensions, expectedSize, 'Actual presentation dimensions differ from requested resolution');
  assert.equal((await request('get_current_game_info')).game_turn, report.turn, 'Simulation advanced while capturing the paused frame');
  report.screenshot = 'presentation.png';
  await command('quit');
  report.passed = true;
} catch (error) {
  report.error = error.message;
  if(readerHoldArmed) {
    try {await command('scenecheck reference reader-release');readerHoldArmed=false;report.temporal.readerHold.failureCleanupReleased=true;}
    catch(cleanupError) {report.temporal.readerHold.failureCleanupError=cleanupError.message;}
  }
  throw error;
} finally {
  shuttingDown = true;
  socket?.destroy();
  await writeFile(join(out, 'report.json'), JSON.stringify(report, null, 2)+'\n');
  console.log(JSON.stringify(report, null, 2));
}
