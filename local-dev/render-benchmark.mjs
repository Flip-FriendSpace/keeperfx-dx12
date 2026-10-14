// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';
import net from 'node:net';
import { setTimeout as delay } from 'node:timers/promises';
import { readFile, writeFile, stat, readdir, rm, rename } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import path from 'node:path';

const out = process.argv[2];
assert.ok(out, 'Output directory is required');
const live = process.argv.includes('--live');
const option = (name, fallback) => { const index=process.argv.indexOf(name); return index < 0 ? fallback : process.argv[index+1]; };
const expectedLevel = Number(option('--level',1));
const campaign = option('--campaign','keeporig');
const warmupSeconds = Number(option('--warmup-seconds',30));
const sampleSeconds = Number(option('--sample-seconds',120));
const expectedVsync = option('--vsync','OFF') === 'ON';
const observeScript = option('--observe-script',null);
const observeHash = option('--observe-hash',null);
assert.ok(!observeScript || (live && option('--setup-script',null) && /^[0-9a-f]{64}$/.test(observeHash)), 'Observer requires live setup and expected hash');
assert.ok(observeScript || !observeHash, 'Observer hash requires a script');
const runtimeLogPath=fileURLToPath(new URL('../../runtime/keeperfx-x64/keeperfx.log',import.meta.url));
const cycles=Number(option('--cycles',1)), reloads=Number(option('--reloads',0));
const runtime=option('--runtime',null), processId=Number(option('--process-id',0));
const wallSeconds=Number(option('--wall-seconds',850));
assert.ok(Number.isInteger(cycles)&&cycles>=1&&cycles<=40&&Number.isInteger(reloads)&&reloads>=0&&reloads<=2);
assert.ok(reloads===0||cycles>=3,'Reloads require at least three cycles');
assert.ok(cycles===1||(live&&runtime&&Number.isInteger(processId)&&processId>0&&Number.isFinite(wallSeconds)&&Number.isInteger(wallSeconds)&&wallSeconds>0&&wallSeconds<=7150),'Invalid soak ownership/bound');
const sessionDeadline=cycles>1?Date.now()+wallSeconds*1000:null;
const boundedDeadline=milliseconds=>Math.min(Date.now()+milliseconds,sessionDeadline??Infinity);
assert.ok(Number.isInteger(expectedLevel) && expectedLevel >= 1 && expectedLevel <= 65535);
assert.ok(Number.isInteger(warmupSeconds) && warmupSeconds >= 1 && warmupSeconds <= 120);
assert.ok(Number.isInteger(sampleSeconds) && sampleSeconds >= 1 && sampleSeconds <= 300);
const socket = net.createConnection({ host: '127.0.0.1', port: 5599 });
let pending, buffer = '', sequence = 0;
socket.on('error', error => pending?.reject(error));
socket.on('close', () => pending?.reject(new Error('API connection closed')));
socket.on('data', data => {
  buffer += data.toString('utf8').replaceAll('\0', '');
  let response;
  try { response = JSON.parse(buffer); } catch { return; }
  buffer = '';
  if (response.ack === pending?.ack) pending.resolve(response);
});
await new Promise((resolve, reject) => { socket.once('connect', resolve); socket.once('error', reject); });
async function request(action, fields = {}, expectedSuccess = true, timeout = 10_000) {
  const ack = ++sequence;
  const response = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`API timed out: ${action}`)), Math.max(1,Math.min(timeout,(sessionDeadline??Infinity)-Date.now())));
    pending = { ack, resolve: value => { clearTimeout(timer); resolve(value); }, reject: error => { clearTimeout(timer); reject(error); } };
    socket.write(JSON.stringify({ action, ack, ...fields }));
  });
  pending = undefined;
  assert.equal(response.success, expectedSuccess, JSON.stringify(response));
  return response.data;
}
function summarize(values) {
  assert.ok(values.length > 0, 'Timing sample set is empty');
  const sorted = [...values].sort((a, b) => a - b);
  const percentile = p => sorted[Math.ceil(p * sorted.length) - 1];
  return { median: percentile(0.5), p95: percentile(0.95), p99: percentile(0.99), min: sorted[0], max: sorted.at(-1) };
}
async function combatObservation(label) {
  const marker=`BENCH_COMBAT_${label.toUpperCase()}`;
  const lua=`local n,alive,melee,ranged,hp=0,0,0,0,0; for _,t in ipairs(GetThingsOfClass("CREATURE")) do local p=t.pos; if p.val_x>=46080 and p.val_x<54528 and p.val_y>=46080 and p.val_y<54528 then n=n+1; for _,r in ipairs(Game.BenchmarkCombat) do if t.ThingIndex==r.id and t.creation_turn==r.born then alive=alive+1; hp=hp+t.health end end; if t.combat_type=="MELEE" then melee=melee+1 elseif t.combat_type=="RANGED" then ranged=ranged+1 end end end; local counts={}; for _,kind in ipairs({"SHOT","EFFECT","EFFECTELEM"}) do local c=0; for _,t in ipairs(GetThingsOfClass(kind)) do local p=t.pos; if p.val_x>=46080 and p.val_x<54528 and p.val_y>=46080 and p.val_y<54528 then c=c+1 end end; table.insert(counts,c) end; print("${marker}",PLAYER0.GAME_TURN,n,alive,melee,ranged,hp,counts[1],counts[2],counts[3])`;
  await request('console_command',{command:`lua ${lua}`});
  let values;
  for(let attempt=0;attempt<40;attempt++) {
    const text=await readFile(runtimeLogPath,'utf8');
    const line=text.split(/\r?\n/).find(line=>line.includes(`lua_print: ${marker}`));
    if(line){values=line.split(marker)[1].trim().split(/\s+/).map(Number);assert.equal(values.length,9);assert.ok(values.every(Number.isFinite));break;}
    await delay(50);
  }
  assert.ok(values,`Missing ${label} workload observation`);
  const fields=['turn','arenaCreatures','fixtureSurvivors','melee','ranged','fixtureHealthSum','shots','effects','effectElements'];
  const observation=Object.fromEntries(fields.map((field,i)=>[field,values[i]]));
  return captureObservationImage(label,observation);
}
async function captureObservationImage(label,observation) {
  const existing=new Set(await readdir(out));
  await request('console_command',{command:'scenecheck receipt'});
  const deadline=Date.now()+30_000;
  while(Date.now()<deadline) {
    const files=(await readdir(out)).filter(name=>!existing.has(name)&&/^temporal-\d+-\d+\.png\.ready\.json$/.test(name));
    assert.ok(files.length<=1,'Unexpected concurrent screenshot receipt');
    if(files.length) {
      try {
        const receipt=JSON.parse(await readFile(path.join(out,files[0]),'utf8'));
        assert.equal(receipt.backend,'DX12');
        assert.equal(receipt.imageWidth,3840);assert.equal(receipt.imageHeight,2160);
        observation.receipt=files[0];observation.image=files[0].slice(0,-11);observation.imageTurn=receipt.turn;
        const png=await readFile(path.join(out,observation.image));
        assert.ok(png.length>24&&png.subarray(-8,-4).toString()==='IEND','Observation PNG is incomplete');
        assert.equal(png.readUInt32BE(16),3840);assert.equal(png.readUInt32BE(20),2160);
        return observation;
      } catch(error){if(!(error instanceof SyntaxError)&&!['ENOENT','EBUSY'].includes(error.code))throw error;}
    }
    await delay(50);
  }
  throw new Error(`Missing ${label} workload image`);
}
async function scriptObservation(label) {
  const bytes=await readFile(observeScript);
  assert.equal(createHash('sha256').update(bytes).digest('hex'),observeHash,'Observer file hash changed');
  const lines=bytes.toString('utf8').replace(/^\uFEFF/,'').split(/\r?\n/).map(line=>line.trim()).filter(line=>line&&!line.startsWith('--'));
  assert.ok(lines.length>0,'Observer contains no read-only Lua lines');
  const logStart=(await readFile(runtimeLogPath,'utf8')).length;
  const observation={path:observeScript,sha256:observeHash,phase:label,completedSteps:0,scope:'Reviewed read-only Lua and native receipt outside benchmark timing; endpoint activity is not continuous visibility proof.'};
  for(const line of lines){await request('console_command',{command:`lua ${line}`});observation.completedSteps++;}
  await request('console_command',{command:`lua print("KFX_BENCHMARK_OBSERVE_OK", "${observeHash}", "${label}", ${observation.completedSteps}, PLAYER0.GAME_TURN)`});
  const deadline=Date.now()+2000;
  const markerPattern=/^\[(\d+)\] lua_print: KFX_BENCHMARK_OBSERVE_OK\s+([0-9a-f]{64})\s+(before|after)\s+(\d+)\s+(\d+)\r?$/gm;
  while(Date.now()<deadline){
    const log=await readFile(runtimeLogPath,'utf8');
    assert.ok(!/Failed to execute Lua code|Lua error/.test(log),'Setup/observer Lua failed; ACK is not qualification');
    const appended=log.slice(logStart);
    const markers=[...appended.matchAll(markerPattern)];
    assert.ok(markers.length<=1,'Multiple observer phase markers');
    if(markers.length){
      const marker=markers[0];assert.equal(marker[2],observeHash);assert.equal(marker[3],label);assert.equal(Number(marker[4]),observation.completedSteps);assert.equal(Number(marker[1]),Number(marker[5]));
      observation.turn=Number(marker[5]);observation.markerLine=marker[0].trim();
      observation.outputLines=appended.slice(0,marker.index).split(/\r?\n/).filter(line=>/^\[\d+\] lua_print: /.test(line));
      return captureObservationImage(label,observation);
    }
    await delay(50);
  }
  throw new Error(`Missing printed ${label} observer success marker`);
}
let memoryRequestSequence=0;
async function processMemory(label) {
  const sequence=++memoryRequestSequence;
  await rm(path.join(out,'process-memory-response.json'),{force:true});
  await writeFile(path.join(out,'process-memory-request.json'),JSON.stringify({sequence,processId,label})+'\n');
  const deadline=boundedDeadline(5000);
  while(Date.now()<deadline){
    try{
      const sample=JSON.parse(await readFile(path.join(out,'process-memory-response.json'),'utf8'));
      if(sample.sequence===sequence){assert.equal(sample.processId,processId);assert.equal(sample.label,label);for(const key of ['privateBytes','workingSetBytes','peakWorkingSetBytes'])assert.ok(Number.isSafeInteger(sample[key])&&sample[key]>=0,key);return sample;}
    }catch(error){if(!(error instanceof SyntaxError)&&!['ENOENT','EBUSY'].includes(error.code))throw error;}
    await delay(50);
  }
  throw new Error('Owned process memory sample timed out');
}
async function rendererStats() {
  const file=path.join(out,'renderer-stats.json');
  await rm(file,{force:true});
  await request('console_command',{command:'scenecheck reference renderer-stats'});
  const deadline=boundedDeadline(5000);
  while(Date.now()<deadline){
    try{
      const stats=JSON.parse(await readFile(file,'utf8'));
      assert.equal(stats.schema,1);assert.equal(stats.backend,'DX12');assert.ok(!stats.failed);assert.equal(stats.rejected,0);
      assert.equal(stats.validation.errors,0);assert.equal(stats.validation.warnings,0);
      for(const type of ['cpu','default','upload','readback','producer','geometryCpu']){const current=stats.memory[`${type}Bytes`],peak=stats.memory[`${type}HighWater`],budget=stats.memory[`${type}BudgetBytes`];assert.ok([current,peak,budget].every(Number.isSafeInteger)&&current>=0&&peak>=current&&budget>0&&peak<=budget,`${type} memory budget`);}
      assert.ok(stats.descriptors.current>=0&&stats.descriptors.highWater>=stats.descriptors.current&&stats.descriptors.highWater<=stats.descriptors.budget,'Descriptor budget');
      assert.deepEqual(stats.window.native,[3840,2160]);return stats;
    }catch(error){if(!(error instanceof SyntaxError)&&!['ENOENT','EBUSY'].includes(error.code))throw error;}
    await delay(50);
  }
  throw new Error('Renderer statistics timed out');
}
async function pausedSnapshot(label, alreadyPaused=false) {
  if(!alreadyPaused)await request('console_command',{command:'pause'});
  const deadline=boundedDeadline(10000);let stats;
  while(Date.now()<deadline){
    try{stats=await rendererStats();break;}
    catch(error){if(error.code!=='ERR_ASSERTION'||error.actual!==false)throw error;await delay(50);}
  }
  assert.ok(stats,'Game did not reach paused diagnostics');
  const turn=(await request('get_current_game_info')).game_turn;
  const memory=await processMemory(label);
  assert.equal((await request('get_current_game_info')).game_turn,turn,'Diagnostics must stay paused');
  const snapshot={label,turn,stats,processMemory:memory};
  await writeFile(path.join(out,`${label}-stats.json`),JSON.stringify(snapshot,null,2)+'\n');return snapshot;
}
async function liveBenchmark() {
  const report = {passed:false, gate8Accepted:false, renderer:'DX12', campaign, expectedLevel, warmupSeconds, sampleSeconds,
    scope:'Normal unpaused campaign or prepared workload candidate at native 4K; workload demand is not inferred from its name.',
    timingScope:'Application admission and successful Present-return intervals are not scanout FPS. Producer wall includes draw gaps; preparation CPU includes retries and excludes Present; GPU upload/render excludes queue latency and Present.',
    memoryScope:'Tracked allocation high-water values and real DXGI process segment endpoints; no total-process peak claim.'};
  const sessionStarted=Date.now();
  let saveTurnBounds, savedTurn;
  if(cycles>1){report.cyclesRequested=cycles;report.cycles=[];report.reloads={requested:reloads,slot:9997,iterations:[]};report.soakScope='Same process/socket; paused diagnostics and ordinary reloads outside capture. Normal campaign soak does not qualify continuously demanding visibility. Process private bytes include caches/heaps; no automatic leak or whole-process peak-budget claim.';report.latestCycleFields='Top-level engine/samples/timings/applicationCadence/before describe the latest completed cycle; cycles retains every independent summary.';}
  try {
    const startup = Date.now()+30_000;
    let state;
    do { state=await request('get_current_game_info'); if(state.game_turn>0)break; await delay(100); } while(Date.now()<startup);
    assert.ok(state.game_turn>0,'Live campaign did not become ready');
    const setupScript=option('--setup-script',null);
    let combatWorkload=false;
    if(setupScript) {
      const bytes=await readFile(setupScript);
      combatWorkload=bytes.toString('utf8').includes('-- KFX_BENCHMARK_COMBAT_V1');
      if(combatWorkload){assert.equal(campaign,'keeporig');assert.equal(expectedLevel,1);}
      const lines=bytes.toString('utf8').replace(/^\uFEFF/,'').split(/\r?\n/).map(line=>line.trim()).filter(line=>line&&!line.startsWith('--'));
      assert.ok(lines.length>0,'Setup contains no Lua commands');
      report.setup={path:setupScript,sha256:createHash('sha256').update(bytes).digest('hex'),completedSteps:0};
      await request('console_command',{command:'pause'});
      await request('console_command',{command:'scenecheck reference setup assert(true)'});
      const pausedTurn=(await request('get_current_game_info')).game_turn;
      for(const line of lines) {await request('console_command',{command:`scenecheck reference setup ${line}`});report.setup.completedSteps++;}
      assert.equal((await request('get_current_game_info')).game_turn,pausedTurn,'Preparation must stay paused');
      await request('console_command',{command:'pause'});
    }
    if(combatWorkload) {
      await request('console_command',{command:'scenecheck overhead'});
      await request('console_command',{command:'zoomto.subtilecoord 195 195'});
      await delay(1000);
      report.workload={kind:'prepared-combat-48',scope:'Ordinary AI after paused setup. Counts are arena-local endpoint observations, not a visibility or sustained-combat guarantee. Images and Lua observations occur at separate turns outside timing.',before:await combatObservation('before')};
    }
    report.version=await request('get_kfx_info');
    report.level=await request('get_level_info');
    assert.equal(report.level.level_number,expectedLevel);
    assert.equal(report.level.is_multiplayer,false);
    assert.equal(report.level.campaign.campaign_fname.toLowerCase(),`${campaign.toLowerCase()}.cfg`);
    if(observeScript){
      report.observer={path:observeScript,sha256:observeHash,untimedWarmupSeconds:warmupSeconds,scope:'Extra observer warm-up precedes before Lua/image; collector retains its own requested warm-up. Both observations are outside captured timing.'};
      await delay(warmupSeconds*1000);
      report.observer.before=await scriptObservation('before');
    }
    if(cycles>1){
      report.initialDiagnostics=await pausedSnapshot('initial');
      if(reloads){
        const savePath=path.join(runtime,'save','fx1g9997.sav');
        try{await stat(savePath);throw new Error('Reserved save9997 is occupied');}catch(error){if(error.code!=='ENOENT')throw error;}
        saveTurnBounds={before:report.initialDiagnostics.turn};
        await writeFile(path.join(out,'soak-save-requested.json'),JSON.stringify({slot:9997,savePath})+'\n');
        await request('console_command',{command:'pause'});
        await request('console_command',{command:'game.save 9997 Benchmark-soak'},true,30000);
        saveTurnBounds.after=(await request('get_current_game_info')).game_turn;
        assert.ok((await stat(savePath)).size>0,'Disposable save was not written');
        report.reloads.saveTurnBounds=saveTurnBounds;
      }else await request('console_command',{command:'pause'});
    }
    for(let cycle=1;cycle<=cycles;cycle++){
    assert.ok(Date.now()<(sessionDeadline??Infinity),'Soak exceeded its wall bound');
    report.before=await request('get_current_game_info');
    await request('console_command',{command:'renderbench live'});
    const deadline=boundedDeadline((warmupSeconds+sampleSeconds+15)*1000);
    let completed;
    while(Date.now()<deadline) {
      try {completed=JSON.parse(await readFile(path.join(out,'dx12-live.json'),'utf8'));break;}
      catch(error){if(!(error instanceof SyntaxError)&&!['ENOENT','EBUSY','EACCES'].includes(error.code))throw error;}
      await delay(250);
    }
    assert.ok(completed,'Live benchmark completion timed out');
    report.engine=completed;
    assert.equal(completed.passed,true,JSON.stringify(completed));
    assert.equal(completed.debug,false,'Performance run must disable the debug layer');
    assert.equal(completed.gpuValidation,false,'Performance run must disable GPU validation');
    assert.equal(completed.vsync,expectedVsync);
    for(const field of ['heldRows','missingGpu','overflow','completionOverflows'])assert.equal(completed[field],0,field);
    assert.equal(completed.warmupSeconds,warmupSeconds);assert.equal(completed.sampleSeconds,sampleSeconds);
    assert.ok(Number.isSafeInteger(completed.timestampFrequency)&&completed.timestampFrequency>0);
    const csv=(await readFile(path.join(out,'dx12-live.csv'),'utf8')).trim().split(/\r?\n/);
    const headers=csv.shift().split(',');
    assert.equal(new Set(headers).size,headers.length);
    const fields=['submission_id','epoch','sequence','producer_wall_ms','preparation_submit_cpu_ms','present_cpu_ms','admission_interval_ms','admission_to_next_ms','present_completion_interval_ms','gpu_upload_render_ms','native_width','native_height','palette_replay','diagnostic'];
    assert.deepEqual(headers,fields,'Live CSV contract changed');
    const rows=csv.map(line=>{const values=line.split(',').map(Number);assert.equal(values.length,fields.length);assert.ok(values.every(Number.isFinite));return Object.fromEntries(fields.map((field,index)=>[field,values[index]]));});
    assert.equal(rows.length,completed.rows);assert.ok(rows.length>1&&rows.length<=65536);
    for(let index=0;index<rows.length;index++) {
      const row=rows[index];
      for(const field of ['submission_id','epoch','sequence'])assert.ok(Number.isSafeInteger(row[field])&&row[field]>0,field);
      if(index)assert.ok(row.submission_id>rows[index-1].submission_id,'Submission IDs must strictly increase');
      assert.equal(row.native_width,3840);assert.equal(row.native_height,2160);
      assert.equal(row.palette_replay,0,'Separate palette replay from normal world performance');assert.equal(row.diagnostic,0,'Diagnostic readback must not contaminate capture');
      for(const field of ['producer_wall_ms','preparation_submit_cpu_ms','present_cpu_ms','gpu_upload_render_ms'])assert.ok(row[field]>=0,`Missing ${field}`);
    }
    report.samples=rows.length;report.timings={};
    for(const field of fields.filter(field=>field.endsWith('_ms'))) {
      const valid=rows.map(row=>row[field]).filter(value=>value>=0);
      assert.ok(valid.length>=rows.length-1,`Incomplete ${field} observations`);
      report.timings[field]={...summarize(valid),samples:valid.length,missing:rows.length-valid.length};
    }
    const intervals=rows.map(row=>row.present_completion_interval_ms).filter(value=>value>0);
    assert.ok(intervals.length>=rows.length-1,'Present cadence samples are incomplete');
    report.applicationCadence={samples:intervals.length,meanFps:1000/(intervals.reduce((sum,value)=>sum+value,0)/intervals.length),fractionOver60Budget:intervals.filter(value=>value>1000/60).length/intervals.length,fractionOver120Budget:intervals.filter(value=>value>1000/120).length/intervals.length};
    for(const memory of [completed.memoryBefore,completed.memoryAfter]) {
      for(const field of ['assetCpuBytes','terrainCpuBytes','defaultBytes','uploadBytes','readbackBytes','defaultHighWaterBytes','uploadHighWaterBytes','globalIdleCalls','fenceWaits'])assert.ok(Number.isSafeInteger(memory[field])&&memory[field]>=0,field);
      assert.ok(Array.isArray(memory.segments)&&memory.segments.length===2);
      assert.ok(memory.segments[0].applicable,'Local process memory segment must be applicable');
      for(const segment of memory.segments)if(segment.applicable){assert.ok(segment.available,'DXGI process budget telemetry unavailable');assert.ok(!segment.injected,'Injected budget data cannot qualify performance');for(const field of ['ageMs','budgetBytes','usageBytes','allocatorBlockBytes','allocatorAllocationBytes'])assert.ok(Number.isFinite(segment[field])&&segment[field]>=0,field);}
    }
    report.adapter={name:completed.adapter??null,vendorId:completed.vendorId??null,scope:completed.adapter?'Device-reported adapter; match the requested hardware separately.':'Device identity not exported in this manifest; hardware attestation remains separate.'};
    if(cycles>1){
      const after=await request('get_current_game_info');assert.ok(after.game_turn>report.before.game_turn,'Cycle simulation did not advance');
      assert.equal((await request('get_level_info')).level_number,expectedLevel);
      const name=`cycle-${String(cycle).padStart(2,'0')}`;
      const result={cycle,before:report.before,after,engine:completed,samples:report.samples,timings:report.timings,applicationCadence:report.applicationCadence,csv:`${name}.csv`,manifest:`${name}.json`,passed:true};
      await rename(path.join(out,'dx12-live.csv'),path.join(out,result.csv));await rename(path.join(out,'dx12-live.json'),path.join(out,result.manifest));
      result.diagnostics=await pausedSnapshot(name);
      report.cycles.push(result);
      const reloadIndex=report.reloads.iterations.length+1;
      if(reloadIndex<=reloads&&cycle===Math.floor(reloadIndex*cycles/(reloads+1))){
        const before=result.diagnostics.stats;
        await request('console_command',{command:'pause'});
        await request('console_command',{command:'game.load 9997 1'},true,30000);
        const loadDeadline=boundedDeadline(30000);let accepted;
        while(Date.now()<loadDeadline){
          try{const stats=await rendererStats();if(stats.epoch>before.epoch&&stats.worldEpoch===stats.epoch&&stats.frameSequence>before.frameSequence&&stats.submitted>before.submitted){accepted=stats;break;}}
          catch(error){if(error.code!=='ERR_ASSERTION'||error.actual!==false)throw error;}
          await delay(100);
        }
        assert.ok(accepted,'Reload did not accept a fresh normal world');
        const snapshot=await pausedSnapshot(`reload-${reloadIndex}`,true);
        const turn=snapshot.turn;
        if(savedTurn===undefined){assert.ok(turn>=saveTurnBounds.before&&turn<=saveTurnBounds.after,'Loaded turn outside save bounds');savedTurn=turn;}else assert.equal(turn,savedTurn,'Reloads must restore the same saved turn');
        assert.equal((await request('get_level_info')).level_number,expectedLevel);
        report.reloads.iterations.push({index:reloadIndex,afterCycle:cycle,before,after:snapshot,savedTurn});
      }
      await request('console_command',{command:'pause'});
      await writeFile(path.join(out,'benchmark-report.json'),JSON.stringify(report,null,2)+'\n');
    }
    }
    if(combatWorkload)report.workload.after=await combatObservation('after');
    if(observeScript)report.observer.after=await scriptObservation('after');
    report.after=await request('get_current_game_info');assert.ok(report.after.game_turn>report.before.game_turn,'Live simulation must advance');
    assert.equal((await request('get_level_info')).level_number,expectedLevel);
    report.passed=true;
    if(cycles>1){assert.equal(report.cycles.length,cycles);assert.equal(report.reloads.iterations.length,reloads);report.aggregate={cyclesCompleted:cycles,measuredSeconds:cycles*sampleSeconds,totalRows:report.cycles.reduce((n,c)=>n+c.samples,0),largestCycleP99:Object.fromEntries(Object.keys(report.timings).map(field=>[field,Math.max(...report.cycles.map(c=>c.timings[field].p99))])),scope:'Worst per-cycle p99, not a pooled percentile. Pauses, reloads, process/stat samples and endpoint images are untimed.'};report.sessionElapsedSeconds=(Date.now()-sessionStarted)/1000;}
    console.log(JSON.stringify(report.timings));
    await request('console_command',{command:'quit'});
  } catch(error) {report.passed=false;report.failure=error.message;throw error;}
  finally {await writeFile(path.join(out,'benchmark-report.json'),JSON.stringify(report,null,2)+'\n');}
}
try {
  if(live) {
    await liveBenchmark();
  } else {
  let state;
  for (let i = 0; i < 100; i++) {
    state = await request('get_current_game_info');
    if (state.game_turn >= 120) break;
    await delay(500);
  }
  assert.equal(state.game_turn, 120, 'Must stop at the exact benchmark turn');
  const level = await request('get_level_info');
  assert.equal(level.level_number, 1);
  assert.equal(level.is_multiplayer, false);
  await request('console_command', { command: 'turn' }, false);
  await request('console_command', { command: 'renderbench 3' }, false);
  const report = {
    checkedAt: new Date().toISOString(), version: await request('get_kfx_info'), level,
    scope: 'Paused Eversmile renderer baseline; not a combat/simulation or temporal-upscaling benchmark.',
    warmupFrames: 120, sampleFrames: 360,
    timing: 'CPU is render-thread submission only. GPU timestamps cover GL work through presentation blit. Swap and frame cadence are separate. Screenshot encoding is outside all sampled frames.',
    scenes: [],
  };
  for (const [scene, name] of ['heart', 'rotated', 'wide'].entries()) {
    await request('console_command', { command: `renderbench ${scene}` });
    const pngPath = path.join(out, `scene-${scene}.png`);
    let ready = false;
    for (let i = 0; i < 240; i++) {
      await delay(500);
      assert.equal((await request('get_current_game_info')).game_turn, 120);
      try {
        const png = await readFile(pngPath);
        ready = png.length > 24 && png.subarray(-8, -4).toString() === 'IEND';
      } catch (error) { if (!['ENOENT', 'EBUSY', 'EACCES'].includes(error.code)) throw error; }
      if (ready) break;
    }
    assert.ok(ready, `Timed out waiting for scene ${scene}`);
    const csv = (await readFile(path.join(out, `scene-${scene}.csv`), 'utf8')).trim().split(/\r?\n/);
    const headers = csv.shift().split(',');
    const rows = csv.map(line => Object.fromEntries(line.split(',').map((v, i) => [headers[i], Number(v)])));
    assert.equal(rows.length, 360);
    assert.ok(rows.every(r => r.width === 3840 && r.height === 2160 && r.terrain_vertices > 0));
    assert.equal(new Set(rows.map(r => r.terrain_vertices)).size, 1, 'Fixed scene geometry must not drift during capture');
    assert.ok(rows.every(r => Object.values(r).every(Number.isFinite)));
    const validGpu = rows.filter(r => r.gpu_ms >= 0);
    assert.equal(validGpu.length, rows.length, 'GPU query results must be complete');
    const png = await readFile(pngPath);
    assert.equal(png.readUInt32BE(16), 3840); assert.equal(png.readUInt32BE(20), 2160);
    const summary = { scene, name, camera: { anchor: 'dungeon heart', zoom: scene === 2 ? 1600 : 6000, yaw: scene === 1 ? 256 : 0, pitch: -266 }, width: 3840, height: 2160, terrainVertices: summarize(rows.map(r => r.terrain_vertices)), pngBytes: (await stat(pngPath)).size };
    for (const field of ['render_cpu_ms', 'gpu_ms', 'frame_interval_ms', 'swap_cpu_ms']) summary[field] = summarize(rows.map(r => r[field]));
    report.scenes.push(summary);
    console.log(JSON.stringify(summary));
  }
  report.passed = true;
  await writeFile(path.join(out, 'benchmark-report.json'), JSON.stringify(report, null, 2) + '\n');
  await request('console_command', { command: 'quit' });
  }
} finally { socket.destroy(); }
