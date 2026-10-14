// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Explicit selected RGBA operator/replay runs. A completed run and exact
// comparison are distinct; no byte tolerance or visual/Gate 2 acceptance.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFile,stat,realpath,mkdir,writeFile} from 'node:fs/promises';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {parseSceneCapture,sceneFingerprint} from './reference-fingerprint.mjs';
const hash=b=>createHash('sha256').update(b).digest('hex');
const validCount=n=>Number.isSafeInteger(n)&&n>=0;
const shaPattern=/^[a-f0-9]{64}$/i;
async function load(file) {
  assert.ok((await stat(file)).size<=128*1024*1024,`Oversized evidence: ${file}`);
  const bytes=await readFile(file);return {bytes,binding:{path:file,bytes:bytes.length,sha256:hash(bytes)}};
}
async function json(file) {
  const loaded=await load(file);return {...loaded,value:JSON.parse(loaded.bytes.toString('utf8').replace(/^\uFEFF/,''))};
}
async function optional(file) {try{return await load(file);}catch(error){if(error.code==='ENOENT')return null;throw error;}}
function compare(a,b) {
  assert.equal(a.length,b.length,'RGBA lengths differ');assert.equal(a.length%4,0);
  let rgb=0,alpha=0,pixels=0,max=0,oneByte=0,larger=0;const signed=[0,0,0,0];
  for(let p=0;p<a.length;p+=4) {
    let changed=false;
    for(let c=0;c<4;c++) {const delta=a[p+c]-b[p+c],absolute=Math.abs(delta);signed[c]+=delta;max=Math.max(max,absolute);oneByte+=absolute===1;larger+=absolute>1;changed||=delta!==0;if(c===3)alpha+=delta!==0;else rgb+=delta!==0;}
    pixels+=changed;
  }
  return {comparedBytes:a.length,differingBytes:rgb+alpha,differingRgbBytes:rgb,differingAlphaBytes:alpha,differingPixels:pixels,
    oneByteDifferences:oneByte,largerByteDifferences:larger,maximumByteDelta:max,signedChannelSum:signed};
}
async function pair(directory,prefix,size,required) {
  const gpu=await optional(path.join(directory,prefix+'-gpu.rgba')),gl=await optional(path.join(directory,prefix+'-gl.rgba'));
  if(!gpu&&!gl&&!required)return null;
  assert.ok(gpu&&gl,`Missing retained RGBA pair: ${prefix}`);assert.equal(gpu.bytes.length,size);assert.equal(gl.bytes.length,size);
  return {metrics:compare(gpu.bytes,gl.bytes),bindings:{gpu:gpu.binding,gl:gl.binding}};
}
const perPath=['opaque','effective-remap','clipped-phase-content-flip','zero-initialized-clut','opacity-0.000000',
  'opacity-0.750000','opacity-0.333333','opacity-0.666667','lookup-alpha','all-glow-codes','glow-asymmetric-matrix',
  'normal-glow-order','glow-normal-order','saturate-then-subtract','subtract-then-saturate','glow-does-not-write-depth','outline-does-not-write-depth'];
const operatorNames=new Set([...Array.from({length:3},(_,p)=>perPath.map(name=>`${name}-path${p}`)).flat(),
  'equal-depth-outline-rejected-normal-visible','normal-behind-depth','normal-does-not-write-depth','two-outline-normal-glow-groups']);
export async function inspectEnhancedSpriteRun(directory) {
  const run={directory:path.resolve(directory),executionPassed:false,exactComparison:false,errors:[],warnings:[],bindings:{},
    operator:{cases:[]},replay:{recorded:false,cases:[]},qualifiedOperatorRawExactBytes:0,qualifiedReplayExactBytes:0,
    visualAccepted:false,gate2Accepted:false};
  const check=(ok,message)=>{if(!ok)run.errors.push(message);};
  const docs={};
  for(const [key,name] of [['manifest','manifest.json'],['main','report.json'],['operator','enhanced-sprite-operator.json']]) {
    try{const loaded=await json(path.join(run.directory,name));docs[key]=loaded.value;run.bindings[key]=loaded.binding;}
    catch(error){run.errors.push(`${name}: ${error.message}`);}
  }
  const {manifest:m,main:f,operator:o}=docs;
  let captured=false,capture,parsed,replay,operatorExactBytes=0,replayExactBytes=0;
  if(m) {
    run.source={revision:m.sourceRevision,status:m.sourceStatus,timestamp:m.timestamp};
    try {
      assert.ok(Array.isArray(m.arguments)&&m.arguments.includes('--gpu-validation'),'GPU validation was not requested');
      captured=m.arguments.includes('--enhanced-sprite-replay');
      assert.ok(captured||m.arguments.includes('--enhanced-sprite-test'),'Not an enhanced-sprite run');
      run.kind=captured?'captured-replay':'operator-only';
      if(captured) {
        const loaded=await json(path.join(run.directory,'enhanced-sprite-replay.json'));replay=loaded.value;run.bindings.replay=loaded.binding;
        Object.assign(run.replay,{recorded:true,executionPassed:replay.executionPassed,passed:replay.passed,scope:replay.scope,
          sourceLimitations:replay.sourceLimitations,error:replay.error,reviewRequired:replay.reviewRequired});
        const at=m.arguments.indexOf('--scene-capture');assert.ok(at>=0&&typeof m.arguments[at+1]==='string','Missing capture argument');
        const file=path.resolve(m.arguments[at+1].replace(/^"|"$/g,''));
        assert.equal(await realpath(file),await realpath(replay.capture),'Replay and manifest capture differ');
        capture=await load(file);parsed=parseSceneCapture(capture.bytes);assert.equal(parsed.version,9,'Enhanced replay requires v9 effective colour bindings');
        assert.match(m.sceneCaptureSha256??'',shaPattern,'Missing capture hash');
        run.bindings.capture={...capture.binding,recordedSha256:m.sceneCaptureSha256.toLowerCase()};
        assert.equal(capture.binding.sha256,run.bindings.capture.recordedSha256,'Capture changed after GPU run');run.frozenFingerprint=sceneFingerprint(capture.bytes);
      }
    } catch(error){run.errors.push(`Input binding: ${error.message}`);}
    try {
      assert.match(m.executableSha256??'',shaPattern,'Missing recorded executable hash');
      const file=path.resolve(m.executablePath??path.join(run.directory,'../bin/keeperfx-dx12.exe'));
      const binding=run.bindings.executable={path:file,recordedSha256:m.executableSha256.toLowerCase(),verification:'historical_hash_only'};
      const current=await optional(file);
      if(current){binding.currentSha256=current.binding.sha256;if(binding.currentSha256===binding.recordedSha256)binding.verification='matching_current_binary';else run.warnings.push('Current executable changed; recorded run hash retained');}
      else run.warnings.push('Historical executable unavailable; recorded hash retained');
    } catch(error){run.errors.push(`Executable binding: ${error.message}`);}
  }
  if(f) {
    run.device={adapter:f.adapter,vendorId:f.vendorId,debugLayer:f.debugLayer,gpuValidation:f.gpuValidation,validationErrors:f.validationErrors,validationWarnings:f.validationWarnings};
    check(f.status==='passed','Overall execution failed');
    check(f.debugLayer===true&&f.gpuValidation===true&&f.validationErrors===0&&f.validationWarnings===0,'Zero debug/GPU validation issues missing');
    if(captured)check(f.comparisonReviewRequired===true,'World/visual comparison review must remain pending');
  }
  if(o) {
    Object.assign(run.operator,{strictPassed:o.strictPassed,passed:o.passed,scope:o.scope,error:o.error,invalidCases:o.invalidCases,
      reportedComparedBytes:o.comparedBytes,reportedGlDifferingBytes:o.glDifferingBytes,reviewRequired:o.reviewRequired});
    check(o.schema===1&&o.strictPassed===true&&!o.error,'Strict operator execution failed');
    check(o.gate2Accepted===false,'Operator Gate 2 acceptance must remain false');
    let reportedChecks=0,glDifferences=0,rawBytes=0;const names=new Set();
    for(const c of o.cases??[])try {
      assert.ok(operatorNames.has(c.name)&&!names.has(c.name),'Unknown/duplicate operator case');names.add(c.name);
      assert.equal(c.bytes,8192,'Operator native RGBA size changed');assert.equal(typeof c.strictGeometry,'boolean');
      assert.ok(validCount(c.glDifferingBytes)&&validCount(c.analyticDifferingBytes),'Invalid operator difference count');
      check(c.analyticDifferingBytes===0,`Analytic operator differences: ${c.name}`);
      if(c.strictGeometry)check(c.glDifferingBytes===0,`Strict geometry differs: ${c.name}`);
      // Current fixtures supply two analytic comparisons for every strict
      // geometry case; these report-only counts are not raw GL qualification.
      reportedChecks+=c.bytes*(c.strictGeometry?3:1);glDifferences+=c.glDifferingBytes;
      const retained=await pair(run.directory,'enhanced-'+c.name,c.bytes,!c.strictGeometry||c.glDifferingBytes!==0||c.analyticDifferingBytes!==0);
      const record={...c,recomputed:!!retained};
      if(retained){record.raw=retained;rawBytes+=c.bytes;check(retained.metrics.differingBytes===c.glDifferingBytes,`Raw operator count disagrees: ${c.name}`);if(!retained.metrics.differingBytes)operatorExactBytes+=c.bytes;}
      run.operator.cases.push(record);
    } catch(error){run.errors.push(`Operator ${c.name}: ${error.message}`);run.operator.cases.push({...c,error:error.message});}
    run.operator.recomputedRetainedBytes=rawBytes;run.operator.reportedAnalyticBytes=reportedChecks-(o.cases??[]).reduce((sum,c)=>sum+(validCount(c.bytes)?c.bytes:0),0);
    check(reportedChecks===o.comparedBytes&&glDifferences===o.glDifferingBytes,'Operator aggregate counts disagree');
    check(o.passed===(o.strictPassed===true&&!o.error&&glDifferences===0)&&o.reviewRequired===(glDifferences!==0),'Operator exact/review status contradicts differences');
    if(o.strictPassed)check(names.size===operatorNames.size&&o.invalidCases===13,'Operator fixture/rejection coverage incomplete');
  }
  if(replay&&capture&&parsed)try {
    const b=capture.bytes,s=parsed.offsets,camera=s.camera,outline=s.outline;
    const width=b.readInt32LE(camera+45),height=b.readInt32LE(camera+49),pixels=width*height;
    assert.ok(width>0&&height>0&&width<=4096&&height<=4096,'Invalid captured native viewport');
    const sprites=b.readUInt32LE(s.outlineSprites),commands=b.readUInt32LE(s.outlineCommands),paths=[0,0,0],resolutions=[0,0,0,0,0,0];
    let border=0,normal=0,add=0,edge=0,darken=0;const groups=new Set();
    for(let i=0;i<sprites;i++){const p=s.outlineSprites+4+i*82;paths[b[p+65]]++;border+=b[p+64];resolutions[b[s.colourBindings+4+i*5+4]]++;}
    for(let i=0;i<commands;i++){const p=s.outlineCommands+4+i*9,kind=b[p],sprite=b.readUInt32LE(p+1);groups.add(b.readUInt32LE(p+5));if(kind===0)edge++;else if(kind===2)darken++;else if(b[s.outlineSprites+4+sprite*82+62])add++;else normal++;}
    const metadata={formatVersion:9,width,height,occluders:b.readUInt32LE(s.outlineOccluders),sprites,commands,normalDraws:normal,
      outlineDraws:edge,glowAddDraws:add,glowDarkenDraws:darken,omittedGlowCommands:0,flushGroups:groups.size,sourceBorderCovered:border,colourRows:b.readUInt32LE(s.colourRows)};
    Object.assign(run.replay,{metadata,resolutionCounts:resolutions,pathCounts:paths,outlineMode:b[outline+2],
      backgroundOnly:sprites===0});
    check(replay.schema===1&&replay.executionPassed===true&&!replay.error,'Captured replay execution failed');
    check(replay.visualAccepted===false&&replay.gate2Accepted===false,'Visual/Gate 2 acceptance must remain false');
    for(const [key,value] of Object.entries(metadata))check(replay[key]===value,`Capture/replay ${key} differs`);
    check(JSON.stringify(replay.resolutionCounts)===JSON.stringify(resolutions)&&JSON.stringify(replay.pathCounts)===JSON.stringify(paths),'Captured resolution/path counts disagree');
    assert.deepEqual(replay.cases.map(c=>c.background),['flat','patterned'],'Missing native background comparisons');
    let totalBytes=0,totalPixels=0,totalDifferences=0;
    for(const c of replay.cases) {
      const raw=await pair(run.directory,'enhanced-replay-'+c.background,pixels*4,true),d=raw.metrics;
      check(c.comparedPixels===pixels&&c.differingRgbBytes===d.differingRgbBytes&&c.differingAlphaBytes===d.differingAlphaBytes
        &&c.maximumByteDelta===d.maximumByteDelta,`Raw captured comparison disagrees: ${c.background}`);
      totalBytes+=d.comparedBytes;totalPixels+=pixels;totalDifferences+=d.differingBytes;
      if(!d.differingBytes)replayExactBytes+=d.comparedBytes;
      run.replay.cases.push({...c,recomputed:true,raw});
    }
    run.replay.comparedBytes=totalBytes;run.replay.comparedPixels=totalPixels;run.replay.differingBytes=totalDifferences;
    check(totalPixels===replay.comparedPixels&&totalDifferences===replay.differingBytes,'Captured comparison aggregate counts disagree');
    check(replay.passed===(replay.executionPassed===true&&!replay.error&&totalDifferences===0)&&replay.reviewRequired===!replay.passed,'Captured exact/review status contradicts differences');
  } catch(error){run.errors.push(`Captured raw comparisons: ${error.message}`);}
  run.executionPassed=run.errors.length===0;
  run.exactComparison=run.executionPassed&&o?.passed===true&&(!captured||replay?.passed===true);
  if(run.executionPassed){run.qualifiedOperatorRawExactBytes=operatorExactBytes;run.qualifiedReplayExactBytes=replayExactBytes;}
  if(run.executionPassed&&!run.exactComparison)run.warnings.push('Nonzero cross-API byte differences retained for review; no tolerance accepted');
  return run;
}
async function main() {
  const args=process.argv.slice(2),at=args.indexOf('--output');assert.ok(at>0&&at===args.length-2,'Usage: node enhanced-sprite-evidence.mjs <run-dir> [...] --output <summary.json>');
  const runs=[],seen=new Set();for(const directory of args.slice(0,at)) {
    const resolved=path.resolve(directory).toLowerCase();assert.ok(!seen.has(resolved),'Duplicate selected run');seen.add(resolved);
    runs.push(await inspectEnhancedSpriteRun(directory));
  }
  const summary={schema:1,scope:'Captured GL9 sprite/glow/outline RGBA on declared backgrounds and terrain depth; operator-only runs separate, no terrain colour/full-world acceptance',
    executionPassed:runs.every(r=>r.executionPassed),exactRuns:runs.filter(r=>r.exactComparison).length,
    exactBackgroundOnlyReplayRuns:runs.filter(r=>r.exactComparison&&r.replay.backgroundOnly).length,visualAccepted:false,gate2Accepted:false,
    qualifiedOperatorRawExactBytes:runs.reduce((n,r)=>n+r.qualifiedOperatorRawExactBytes,0),qualifiedReplayExactBytes:runs.reduce((n,r)=>n+r.qualifiedReplayExactBytes,0),runs};
  const output=path.resolve(args[at+1]);await mkdir(path.dirname(output),{recursive:true});await writeFile(output,JSON.stringify(summary,null,2)+'\n');
  console.log(JSON.stringify({output,executionPassed:summary.executionPassed,exactRuns:summary.exactRuns,runs:runs.length,
    qualifiedOperatorRawExactBytes:summary.qualifiedOperatorRawExactBytes,qualifiedReplayExactBytes:summary.qualifiedReplayExactBytes}));
  if(!summary.executionPassed)process.exitCode=1;
}
if(process.argv[1]&&import.meta.url===pathToFileURL(path.resolve(process.argv[1])).href)await main();
