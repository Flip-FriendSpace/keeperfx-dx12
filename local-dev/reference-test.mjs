// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';
import net from 'node:net';
import {setTimeout as delay} from 'node:timers/promises';
import {readFile, writeFile, readdir} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {join} from 'node:path';
import {sceneFingerprint, parseSceneCapture} from './reference-fingerprint.mjs';
import {inspectSelectionTrace} from './reference-selection.mjs';
import {inspectKeyTrace} from './reference-key-trace.mjs';
import {inspectHoverTrace} from './reference-hover.mjs';
import {inspectRoomPreviewTrace} from './reference-room-preview.mjs';

const [out, levelText, renderer, suite='baseline', caseText=''] = process.argv.slice(2);
const catalog = JSON.parse(await readFile(new URL('./reference-scenes.json', import.meta.url), 'utf8'));
const requested=caseText ? caseText.split(',') : [];
const scenes = catalog.scenes.filter(s => s.level === Number(levelText) && (s.suite??'baseline')===suite && (!requested.length||requested.includes(s.id)));
assert.ok(requested.every(id=>scenes.some(s=>s.id===id)),'Requested case is not in this level/suite');
for (let i=0;i<scenes.length;i++) if(scenes[i].continuationOf) {
  assert.equal(scenes[i].continuationOf,scenes[i-1]?.id,'Frozen variants require their preceding case');
  assert.ok(!scenes[i].commands.length && !scenes[i].setupCommands && !scenes[i].setupSettleMs && !scenes[i].setupStepTurns && !scenes[i].setupPhases,'Frozen variants cannot change the simulation');
  assert.ok(scenes[i].pausedCommands.every(c=>c.startsWith('scenecheck reference setting ')),'Frozen variants only change graphics settings');
}
assert.ok(out && scenes.length && ['OPENGL','SOFTWARE'].includes(renderer));
assert.ok(suite !== 'selection' || renderer === 'OPENGL', 'Selection references require owned GL composition');
assert.ok(!scenes.some(s=>s.expectHover) || renderer === 'OPENGL', 'Hover references require owned GL composition');
// Assertions use the bounded, validated owned trace rather than screenshot or
// ordinary scene metadata. Only the recorded phases inside composition count.
function inspectCompositionTrace(bytes, expected, failures) {
  const parsed=parseSceneCapture(bytes);
  if(parsed.version<11) {
    if(expected)failures.push('composition: actual owned V11 trace is required');
    return {version:parsed.version,available:false};
  }
  const actual=parsed.composition,phaseNames=['DepthWorld','FlatWorld','Swipe'],modeNames=['Sprite','Colored','Solid','Fade','Clut'];
  const operations=['None','Copy','Remap','Mist','Overlay','NotDrawn'],modeCounts=[0,0,0,0,0];
  for(let i=0;i<actual.overlays.draws;i++)modeCounts[bytes[parsed.offsets.overlayDraws+5+i*81]]++;
  const count=(value,requirement,label)=>{
    const limits=typeof requirement==='number'?{min:requirement,max:requirement}:requirement;
    assert.ok(limits&&Object.keys(limits).every(k=>['min','max'].includes(k)),'Invalid composition count declaration');
    const {min=0,max=Number.MAX_SAFE_INTEGER}=limits;
    assert.ok(Number.isSafeInteger(min)&&Number.isSafeInteger(max)&&min>=0&&max>=min,'Invalid composition count range');
    if(value<min||value>max)failures.push(`composition ${label}: expected ${min}..${max}, got ${value}`);
  };
  if(expected) {
    assert.ok(Object.keys(expected).every(k=>['phases','modes','lens'].includes(k)),'Unknown composition assertion');
    for(const [phase,range] of Object.entries(expected.phases??{})) {
      assert.ok(phaseNames.includes(phase),'Unknown composition phase');count(actual.phaseCounts[phaseNames.indexOf(phase)],range,phase);
    }
    for(const [mode,range] of Object.entries(expected.modes??{})) {
      assert.ok(modeNames.includes(mode),'Unknown composition mode');count(modeCounts[modeNames.indexOf(mode)],range,mode);
    }
    for(const [field,value] of Object.entries(expected.lens??{})) {
      if(field==='resources') {
        for(const [resource,range] of Object.entries(value)) {
          assert.ok(['remapPixels','overlayBytes','densityBytes','paletteBytes','fadeBytes','rgb6Bytes'].includes(resource),'Unknown lens resource assertion');
          count(actual.lens[resource],range,`lens ${resource}`);
        }
      } else {
        assert.ok(['operation','requestedType','requestedActive','captureBegan','fallbackCopy'].includes(field),'Unknown lens state assertion');
        const observed=field==='operation'?operations[actual.lens.operation]:actual.lens[field];
        if(observed!==value)failures.push(`composition lens ${field}: expected ${JSON.stringify(value)}, got ${JSON.stringify(observed)}`);
      }
    }
  }
  return {version:parsed.version,available:true,completedPhases:bytes[parsed.offsets.composition+2],
    ...actual,overlayModeCounts:Object.fromEntries(modeNames.map((mode,i)=>[mode,modeCounts[i]])),lensOperationName:operations[actual.lens.operation],
    assertionScope:'Executed world overlays and lens composition only; ordinary HUD, menus and complete UI coverage are outside this trace'};
}
const socket = net.createConnection({host:'127.0.0.1',port:5599});
await new Promise((resolve,reject) => { socket.once('connect',resolve); socket.once('error',reject); });
let sequence = 0, buffer = '', pending;
socket.on('error', error => pending?.reject(error));
socket.on('data', data => {
  buffer += data.toString('utf8').replaceAll('\0','');
  let response;
  try { response = JSON.parse(buffer); } catch { return; }
  buffer = '';
  if (response.ack === pending?.ack) pending.resolve(response);
});
async function request(action, fields={}, expectedSuccess=true) {
  const ack = ++sequence;
  const result = await new Promise((resolve,reject) => {
    const timer = setTimeout(() => reject(new Error(`Timed out: ${action} ${fields.command ?? ''}`)),30000);
    pending = {ack,resolve:value => {clearTimeout(timer);resolve(value);},reject:error => {clearTimeout(timer);reject(error);}};
    socket.write(JSON.stringify({action,ack,...fields}));
  });
  if (expectedSuccess === null) return result;
  assert.equal(result.success,expectedSuccess,`${fields.command ?? action}: ${JSON.stringify(result)}`);
  return result.data;
}
const command = command => request('console_command',{command});
async function advanceTurns(count) {
  assert.ok(Number.isInteger(count) && count > 0 && count <= 60);
  const steppedTurns={before:(await request('get_current_game_info')).game_turn,count};
  for (let step=1;step<=count;step++) {
    await command('step');
    let paused=false;
    for (let attempt=0;attempt<100;attempt++) {
      await delay(20);
      const turn=(await request('get_current_game_info')).game_turn;
      assert.ok(turn<=steppedTurns.before+step, 'Single-step advanced too many simulation turns');
      if (turn===steppedTurns.before+step) {
        // The pause packet can follow the tick. This guarded no-op proves
        // it has been applied before the next step or any frozen setup.
        const response=await request('console_command',{command:'scenecheck reference setup assert(true)'},null);
        if(response.success) { paused=true; break; }
      }
    }
    assert.ok(paused, 'Single-step did not advance exactly once and return to pause');
  }
  steppedTurns.after=(await request('get_current_game_info')).game_turn;
  return steppedTurns;
}
const digest = bytes => createHash('sha256').update(bytes).digest('hex');
const report = {version:1,renderer,suite,level:Number(levelText),passed:false,frames:[],scope:catalog.scope};
try {
  report.versionInfo = await request('get_kfx_info');
  report.levelInfo = await request('get_level_info');
  assert.equal(report.levelInfo.level_number,Number(levelText));
  assert.equal(report.levelInfo.is_multiplayer,false);
  await request('console_command',{command:'scenecheck reference camera * * * 0 -266 5000 *'},false);
  await request('console_command',{command:'scenecheck reference setup assert(true)'},false);
  if (suite==='selection') await request('console_command',{command:'scenecheck reference selector 63 63 1 1'},false);
  if (scenes.some(s=>s.expectRoomPreview)) await request('console_command',{command:'scenecheck reference room-preview 2 63 63 1 1 0'},false);
  if (scenes.some(s=>s.expectHover)) await request('console_command',{command:'scenecheck reference hover 0 6 199 193 1 1'},false);
  for (const scene of scenes) {
    for (const text of scene.commands) {
      await command(text);
      if (text.startsWith('zoomto.')) await delay(1500);
    }
    if (!scene.continuationOf) await delay(scene.settleMs);
    // Stop mutable legacy asset/animation updates while the paired frame is
    // queued and rendered. Snapshot capture itself never changes simulation.
    if (!scene.continuationOf) await command('pause');
    await request('console_command',{command:'scenecheck reference camera -1 * * 0 -266 5000 *'},false);
    await request('console_command',{command:'scenecheck reference camera * * * 0 -266'},false);
    await command('scenecheck reference clear-ui');
    if (scene.expectHover) {
      await command('scenecheck reference hover reset');
      for (const invalid of ['0 6 199 193 2 1','0 6 199 193 1 2','0 6 65535 193 1 1','0 6 199 193 1','0 6 199 193 1 1 extra'])
        await request('console_command',{command:`scenecheck reference hover ${invalid}`},false);
    }
    if (suite==='selection') {
      await command('scenecheck reference selector reset');
      for (const invalid of ['63 63 0 1','63 63 10 1','0 0 9 9','63 63 1','63 63 1 1 extra'])
        await request('console_command',{command:`scenecheck reference selector ${invalid}`},false);
    }
    if (scene.expectRoomPreview) {
      for (const invalid of ['0 63 63 1 1 0','512 63 63 1 1 0','2 63 63 0 1 0','2 63 63 1 1 2','2 63 63 1 1','2 63 63 1 1 0 extra'])
        await request('console_command',{command:`scenecheck reference room-preview ${invalid}`},false);
    }
    await command('scenecheck reference short-walls 0');
    await command('scenecheck reference setting reset');
    await request('console_command',{command:'scenecheck reference setting lighting 2'},false);
    await request('console_command',{command:'scenecheck reference setting shadow invalid'},false);
    for (const text of scene.setupCommands ?? []) await command(text);
    let steppedTurns;
    if (scene.setupStepTurns) {
      assert.ok(!scene.setupSettleMs && !scene.setupPhases, 'Use one setup stepping mode');
      steppedTurns=await advanceTurns(scene.setupStepTurns);
    }
    if (scene.setupSettleMs) {
      // Some engine presentation flags are derived during a simulation tick
      // (for example invisibility). Let the disposable setup settle before
      // pinning the final pose and capturing its immutable reference frame.
      await command('pause');
      await delay(scene.setupSettleMs);
      await command('pause');
    }
    const steppedPhases=[];
    for (const phase of scene.setupPhases ?? []) {
      assert.ok(!scene.setupSettleMs && !scene.setupStepTurns, 'Phased setup cannot mix settling modes');
      assert.ok(phase.commands.every(c=>c.startsWith('scenecheck reference setup ')), 'Phases use guarded disposable setup only');
      for (const text of phase.commands) await command(text);
      steppedPhases.push(await advanceTurns(phase.turns));
    }
    if (scene.expect.view === 0 && !scene.continuationOf) await command('scenecheck reference camera * * * 256 -266 5000 *');
    for (const text of scene.pausedCommands ?? []) await command(text);
    await delay(100);
    const previous = new Set(await readdir(out));
    await command('scenecheck capture');
    let file;
    for (let attempt=0;attempt<160;attempt++) {
      const files = await readdir(out);
      file = files.find(f => f.endsWith('.kfxscene') && !previous.has(f) && files.includes(f+'.png.ready.json') && files.includes(f+'.json'));
      if (file) break;
      await delay(250);
    }
    assert.ok(file,`Missing paired reference for ${scene.id}; inspect the engine log`);
    const bytes = await readFile(join(out,file));
    const png = await readFile(join(out,file+'.png'));
    const metadata = JSON.parse(await readFile(join(out,file+'.json'),'utf8'));
    const receipt = JSON.parse(await readFile(join(out,file+'.png.ready.json'),'utf8'));
    assert.equal(receipt.epoch,metadata.epoch);
    assert.equal(receipt.sequence,metadata.sequence);
    assert.equal(bytes.toString('ascii',0,4),'KFXC');
    assert.equal(png.subarray(0,8).toString('hex'),'89504e470d0a1a0a');
    assert.deepEqual([png.readUInt32BE(16),png.readUInt32BE(20)],(scene.resolution??'3840x2160').split('x').map(Number),'Reference resolution differs from its recipe');
    assert.equal(png.subarray(-8,-4).toString('ascii'),'IEND','Reference image must be complete');
    assert.equal(bytes.readBigUInt64LE(24),BigInt(metadata.epoch));
    assert.equal(bytes.readBigUInt64LE(32),BigInt(metadata.sequence));
    assert.equal(bytes.readInt32LE(40),metadata.turn);
    assert.equal(file,`scene-${metadata.epoch}-${metadata.sequence}.kfxscene`);
    const fingerprint=sceneFingerprint(bytes);
    const {frozenScheme,frozenSha256,effectiveWorldResourcesScheme,effectiveWorldResourcesSha256,
      appliedWorldStateScheme,appliedWorldStateSha256,compositionResourcesScheme,compositionResourcesSha256,
      compositionStateScheme,compositionStateSha256}=fingerprint;
    let worldVariantComparison;
    const expectationFailures=[];
    const keyTrace=scene.expectKeyTrace ? inspectKeyTrace(bytes,scene.expectKeyTrace,expectationFailures,renderer) : undefined;
    const compositionTrace=renderer==='OPENGL' ? inspectCompositionTrace(bytes,scene.expectComposition,expectationFailures) : undefined;
    const selectionTrace=scene.expectSelector ? inspectSelectionTrace(bytes,parseSceneCapture(bytes),metadata,scene.expectSelector,expectationFailures) : undefined;
    const hoverTrace=scene.expectHover ? inspectHoverTrace(bytes,parseSceneCapture(bytes),metadata,scene.expectHover,expectationFailures) : undefined;
    const roomPreviewTrace=scene.expectRoomPreview ? inspectRoomPreviewTrace(bytes,parseSceneCapture(bytes),metadata,scene.expectRoomPreview,expectationFailures) : undefined;
    if(scene.continuationOf) {
      const previous=report.frames.at(-1);
      if(frozenScheme!==previous.frozenScheme || frozenSha256!==previous.frozenSha256)
        expectationFailures.push(`Frozen scene/assets or fingerprint scheme changed after ${scene.continuationOf}`);
      if(fingerprint.version>=10) {
        worldVariantComparison={parent:scene.continuationOf,
          scope:'Scene identity equality does not imply equal effective world resources or applied shader state',
          effectiveResourcesEqual:effectiveWorldResourcesScheme===previous.effectiveWorldResourcesScheme
            && effectiveWorldResourcesSha256===previous.effectiveWorldResourcesSha256,
          appliedStateEqual:appliedWorldStateScheme===previous.appliedWorldStateScheme
            && appliedWorldStateSha256===previous.appliedWorldStateSha256};
        worldVariantComparison.reviewRequired=!worldVariantComparison.effectiveResourcesEqual || !worldVariantComparison.appliedStateEqual;
        if(fingerprint.version>=11) {
          worldVariantComparison.compositionResourcesEqual=compositionResourcesScheme===previous.compositionResourcesScheme
            && compositionResourcesSha256===previous.compositionResourcesSha256;
          worldVariantComparison.compositionStateEqual=compositionStateScheme===previous.compositionStateScheme
            && compositionStateSha256===previous.compositionStateSha256;
          worldVariantComparison.reviewRequired ||= !worldVariantComparison.compositionResourcesEqual || !worldVariantComparison.compositionStateEqual;
          worldVariantComparison.scope='Scene identity, world resources/state and post-world composition resources/state are compared separately';
        }
      }
      for(const key of ['turn','camera','angles','scaledZoom','fov','slabKinds','objectLocations','renderFlags']) {
        try { assert.deepEqual(metadata[key],previous.metadata[key]); }
        catch { expectationFailures.push(`Frozen variant changed ${key} after ${scene.continuationOf}`); }
      }
    }
    for (const [field,entries] of [['objectLocations',scene.expectObjects],['doorStates',scene.expectDoors]])
    for (const {where,min=0,max=Number.MAX_SAFE_INTEGER} of entries ?? []) {
      assert.ok(Array.isArray(metadata[field]),`Missing reference metadata ${field}`);
      const count=metadata[field].filter(object=>Object.entries(where).every(([key,value])=>JSON.stringify(object[key])===JSON.stringify(value))).length;
      if(count<min || count>max) expectationFailures.push(`${field} ${JSON.stringify(where)}: expected ${min}..${max}, got ${count}`);
    }
    for (const {field,keys,min} of scene.expectTotals ?? []) {
      const count=keys.reduce((sum,key)=>sum+(metadata[field]?.[key]??0),0);
      if(count<min) expectationFailures.push(`${field}[${keys.join(',')}]: expected total >= ${min}, got ${count}`);
    }
    for (const [key,expected] of Object.entries(scene.expectExact ?? {})) {
      try { assert.deepEqual(metadata[key],expected); }
      catch { expectationFailures.push(`${key}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(metadata[key])}`); }
    }
    for (const [key,expected] of Object.entries(scene.expectSettings ?? {})) {
      if(metadata.settings?.[key]!==expected) expectationFailures.push(`setting ${key}: expected ${expected}, got ${metadata.settings?.[key]}`);
    }
    for (const [key,expected] of Object.entries(scene.expect)) {
      if (typeof expected === 'object') {
        for (const [kind,min] of Object.entries(expected))
          if ((metadata[key]?.[kind]??0)<min) expectationFailures.push(`${key}.${kind}: expected >= ${min}, got ${metadata[key]?.[kind]??0}`);
      } else if (!Number.isFinite(metadata[key]) || (key === 'view' ? metadata[key]!==expected : metadata[key]<expected))
        expectationFailures.push(`${key}: expected ${expected}, got ${metadata[key]}`);
    }
    if(steppedPhases.length) assert.equal(metadata.turn,steppedPhases.at(-1).after,'Simulation moved after phased setup');
    if(steppedTurns) assert.equal(metadata.turn,steppedTurns.after,'Simulation moved after exact-step setup');
    const frame={id:scene.id,purpose:scene.purpose,recipeSha256:digest(JSON.stringify(scene)),steppedTurns,steppedPhases:steppedPhases.length?steppedPhases:undefined,file,sha256:digest(bytes),frozenScheme,frozenSha256,image:file+'.png',imageSha256:digest(png),
      effectiveWorldResourcesScheme,effectiveWorldResourcesSha256,appliedWorldStateScheme,appliedWorldStateSha256,worldVariantComparison,
      compositionResourcesScheme,compositionResourcesSha256,compositionStateScheme,compositionStateSha256,
      imageSize:[png.readUInt32BE(16),png.readUInt32BE(20)],metadata,keyTrace,compositionTrace,selectionTrace,hoverTrace,roomPreviewTrace,expectationFailures,visualReview:'pending'};
    report.frames.push(frame);
    await writeFile(join(out,'report.json'),JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify({id:scene.id,file,expectationFailures}));
    if (scenes[scenes.indexOf(scene)+1]?.continuationOf!==scene.id) await command('pause');
  }
  report.passed=report.frames.every(f=>f.expectationFailures.length===0);
  await writeFile(join(out,'report.json'),JSON.stringify(report,null,2)+'\n');
  await command('quit');
  assert.ok(report.passed,'Reference input assertions failed; retained all frames for investigation');
} finally {socket.destroy();}
