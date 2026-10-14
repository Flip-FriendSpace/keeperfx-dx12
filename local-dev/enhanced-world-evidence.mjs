// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Explicit GL10 world, GL11 composition and operator evidence. Execution, byte equality and visual
// acceptance stay separate; no raster or colour tolerance is accepted here.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFile,stat,realpath,mkdir,writeFile} from 'node:fs/promises';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {parseSceneCapture,sceneFingerprint} from './reference-fingerprint.mjs';
const sha=bytes=>createHash('sha256').update(bytes).digest('hex'),count=n=>Number.isSafeInteger(n)&&n>=0;
const shaPattern=/^[a-f0-9]{64}$/i;
async function load(file) {
  assert.ok((await stat(file)).size<=128*1024*1024,`Oversized evidence: ${file}`);
  const bytes=await readFile(file);return {bytes,binding:{path:file,bytes:bytes.length,sha256:sha(bytes)}};
}
async function json(file) {const data=await load(file);return {...data,value:JSON.parse(data.bytes.toString('utf8').replace(/^\uFEFF/,''))};}
async function optional(file) {try{return await load(file);}catch(error){if(error.code==='ENOENT')return null;throw error;}}
function compare(a,b) {
  assert.equal(a.length,b.length);assert.equal(a.length%4,0);
  let rgb=0,alpha=0,pixels=0,max=0,one=0,larger=0;const signed=[0,0,0,0];
  for(let i=0;i<a.length;i+=4) {
    let changed=false;
    for(let c=0;c<4;c++) {const delta=a[i+c]-b[i+c],absolute=Math.abs(delta);signed[c]+=delta;max=Math.max(max,absolute);
      one+=absolute===1;larger+=absolute>1;changed||=delta!==0;if(c===3)alpha+=delta!==0;else rgb+=delta!==0;}
    pixels+=changed;
  }
  return {comparedPixels:a.length/4,comparedBytes:a.length,differingBytes:rgb+alpha,differingRgbBytes:rgb,
    alphaDifferingBytes:alpha,differentPixels:pixels,maximumByteDelta:max,oneByteDifferences:one,largerByteDifferences:larger,signedChannelSum:signed};
}
async function image(directory,stem,width,height) {
  const raw=await load(path.join(directory,stem+'.rgba')),bmp=await load(path.join(directory,stem+'.bmp'));
  assert.equal(raw.bytes.length,width*height*4,`${stem} native RGBA size`);
  assert.equal(bmp.bytes.length,54+raw.bytes.length,`${stem} native BMP size`);
  const b=bmp.bytes;assert.equal(b.toString('ascii',0,2),'BM');assert.equal(b.readUInt32LE(2),b.length);
  assert.equal(b.readUInt32LE(10),54);assert.equal(b.readUInt32LE(14),40);assert.equal(b.readUInt32LE(18),width);
  assert.equal(b.readUInt32LE(22),height);assert.equal(b.readUInt16LE(26),1);assert.equal(b.readUInt16LE(28),32);assert.equal(b.readUInt32LE(30),0);
  for(let y=0;y<height;y++)for(let x=0;x<width;x++) {
    const source=(y*width+x)*4,to=54+((height-y-1)*width+x)*4;
    assert.ok(b[to]===raw.bytes[source+2]&&b[to+1]===raw.bytes[source+1]&&b[to+2]===raw.bytes[source]&&b[to+3]===raw.bytes[source+3],`${stem} BMP/raw disagreement`);
  }
  const bindings={raw:raw.binding,bmp:bmp.binding},png=await optional(path.join(directory,stem+'.png'));
  if(png) {assert.equal(png.bytes.subarray(0,8).toString('hex'),'89504e470d0a1a0a');assert.equal(png.bytes.readUInt32BE(16),width);
    assert.equal(png.bytes.readUInt32BE(20),height);bindings.png=png.binding;}
  return {bytes:raw.bytes,bindings};
}
async function comparison(directory,prefix,width,height) {
  const gpu=await image(directory,prefix+'-gpu',width,height),gl=await image(directory,prefix+'-gl',width,height);
  const mask=await image(directory,prefix+'-difference',width,height),metrics=compare(gpu.bytes,gl.bytes);
  for(let i=0;i<gpu.bytes.length;i+=4) {
    const changed=gpu.bytes[i]!==gl.bytes[i]||gpu.bytes[i+1]!==gl.bytes[i+1]||gpu.bytes[i+2]!==gl.bytes[i+2]||gpu.bytes[i+3]!==gl.bytes[i+3];
    assert.ok(mask.bytes[i]===(changed?255:0)&&mask.bytes[i+1]===0&&mask.bytes[i+2]===(changed?255:0)&&mask.bytes[i+3]===255,'Difference mask disagrees with raw RGBA');
  }
  return {gpu,gl,record:{metrics,bindings:{gpu:gpu.bindings,gl:gl.bindings,difference:mask.bindings}}};
}
const analyticNames=new Set(['clear-native-origin','nearest-negative-uv-wrap','opaque-index-zero','constant-palette-row32',
  'terrain-always-overwrites-nearer','linear-gamma','lightmap-missing-coordinates','lightmap-zero-retains-face-shade',
  'missing-atlas-global-checkerboard','shadow-binary-holes-equal-depth','shadow-overlap-multiply','shadow-behind-terrain','shadow-before-normal-art']);
const historicalNames=new Set([...analyticNames,'lightmap-ratio-bilinear','terrain-shadow-outline-normal-glow',
  ...Array.from({length:3},(_,mode)=>[0,1].map(filter=>`perspective-darkness${mode}-filter${filter}`)).flat()]);
const fogNames=['active-fog-native-origin','active-fog-equivalent-global-origin'];
const fogSuiteNames=new Set([...historicalNames,...fogNames]);
const nativeNames=['native-target-fog-crop','native-target-checkerboard-crop'];
const operatorNames=new Set([...fogSuiteNames,...nativeNames]);
export async function inspectEnhancedWorldRun(directory) {
  const run={directory:path.resolve(directory),executionPassed:false,exactComparison:false,errors:[],warnings:[],bindings:{},
    qualifiedReplayExactBytes:0,qualifiedOperatorExactBytes:0,visualAccepted:false,gate2Accepted:false};
  const check=(ok,message)=>{if(!ok)run.errors.push(message);},docs={};
  for(const [key,name] of [['manifest','manifest.json'],['main','report.json']])try {
    const data=await json(path.join(run.directory,name));docs[key]=data.value;run.bindings[key]=data.binding;
  } catch(error){run.errors.push(`${name}: ${error.message}`);}
  const {manifest:m,main:f}=docs;let replay=false,nativeOrigin=false,capture,parsed,report,qualified=0;
  if(m) {
    run.source={revision:m.sourceRevision,status:m.sourceStatus,timestamp:m.timestamp};
    try {
      assert.ok(Array.isArray(m.arguments)&&m.arguments.includes('--gpu-validation'),'GPU validation was not requested');
      replay=m.arguments.includes('--enhanced-world');const operator=m.arguments.includes('--enhanced-world-test');
      nativeOrigin=m.arguments.includes('--enhanced-world-native-origin');assert.ok(!nativeOrigin||replay,'Native-origin flag requires captured world mode');
      assert.ok(replay!==operator,'Not exactly one enhanced-world mode');run.kind=replay?'captured-replay':'operator-only';
      const name=replay?'enhanced-world-replay.json':'enhanced-world-operator.json',data=await json(path.join(run.directory,name));
      report=data.value;run.bindings.comparisonReport=data.binding;
      run.reported={...report};
      if(replay) {
        const at=m.arguments.indexOf('--scene-capture');assert.ok(at>=0&&typeof m.arguments[at+1]==='string','Missing capture argument');
        const file=path.resolve(m.arguments[at+1].replace(/^"|"$/g,''));assert.equal(await realpath(file),await realpath(report.capture),'Capture paths disagree');
        capture=await load(file);parsed=parseSceneCapture(capture.bytes);assert.ok([10,11].includes(parsed.version),'World replay requires GL10/11 owned inputs');
        nativeOrigin ||= parsed.version===11; // Full composition executes on the captured native framebuffer.
        assert.match(m.sceneCaptureSha256??'',shaPattern,'Missing capture hash');run.bindings.capture={...capture.binding,recordedSha256:m.sceneCaptureSha256.toLowerCase()};
        assert.equal(capture.binding.sha256,run.bindings.capture.recordedSha256,'Capture changed since GPU execution');
        run.fingerprint=sceneFingerprint(capture.bytes);run.world=parsed.glWorld;
        if(parsed.version===11)run.composition=parsed.composition;
      }
    } catch(error){run.errors.push(`Input binding: ${error.message}`);}
    try {
      assert.match(m.executableSha256??'',shaPattern,'Missing recorded executable hash');
      const file=path.resolve(m.executablePath??path.join(run.directory,'../bin/keeperfx-dx12.exe'));
      const binding=run.bindings.executable={path:file,recordedSha256:m.executableSha256.toLowerCase(),verification:'historical_hash_only'},current=await optional(file);
      if(current) {binding.currentSha256=current.binding.sha256;if(binding.currentSha256===binding.recordedSha256)binding.verification='matching_current_binary';
        else run.warnings.push('Current executable rebuilt; recorded run hash retained');}
      else run.warnings.push('Historical executable unavailable; recorded run hash retained');
    } catch(error){run.errors.push(`Executable binding: ${error.message}`);}
  }
  if(f) {
    run.executionError=f.error??null;run.mainScope=f.scope;
    run.device={adapter:f.adapter,vendorId:f.vendorId,debugLayer:f.debugLayer,gpuValidation:f.gpuValidation,validationErrors:f.validationErrors,validationWarnings:f.validationWarnings};
    run.presentation={frames:f.frames,preparations:f.frozenPreparations,presentations:f.frozenPresentations,imageReadbacks:f.frozenImageReadbacks};
    check(f.status==='passed','Overall execution failed');check(f.debugLayer===true&&f.gpuValidation===true&&f.validationErrors===0&&f.validationWarnings===0,'Zero debug/GPU validation issues missing');
  }
  if(report) {
    check((report.schema===1||replay&&report.schema===2)&&report.executionPassed===true&&!report.error,'Comparison execution failed');
    check(report.gate2Accepted===false&&report.visualAccepted!==true,'Visual/Gate2 acceptance must remain pending');
    check(typeof report.passed==='boolean','Explicit exact comparison status missing');
  }
  if(replay&&report&&capture&&parsed)try {
    const b=capture.bytes,o=parsed.offsets,w=parsed.glWorld,whole=parsed.version===11;let normals=0,outlines=0,adds=0,darkens=0;const groups=new Set();
    check(report.schema===(whole?2:1),'Replay schema differs from capture version');
    for(let i=0;i<b.readUInt32LE(o.outlineCommands);i++) {
      const p=o.outlineCommands+4+i*9,kind=b[p],sprite=b.readUInt32LE(p+1);groups.add(b.readUInt32LE(p+5));
      if(kind===0)outlines++;else if(kind===2)darkens++;else if(b[o.outlineSprites+4+sprite*82+62])adds++;else normals++;
    }
    const metadata={formatVersion:parsed.version,nativeWidth:w.width,nativeHeight:w.height,targetKind:w.targetKind,terrainTriangles:w.terrain,
      tileVersions:w.tiles,shadowQuads:w.shadows,normalDraws:normals,outlineDraws:outlines,glowAddDraws:adds,glowDarkenDraws:darkens,
      spritePhases:b.readUInt32LE(o.outlineCommands),groups:groups.size};run.metadata=metadata;
    for(const [key,value] of Object.entries(metadata))check(report[key]===value,`Capture/report ${key} differs`);
    check(JSON.stringify(report.viewport)===JSON.stringify(w.window),'Native viewport differs from capture');
    if(whole) {
      const c=parsed.composition,l=c.lens;
      const composition={wholeComposition:true,outputWidth:w.width,outputHeight:w.height,completedPhases:15,
        overlayDraws:c.overlays.draws,lensRequestedActive:l.requestedActive,lensCaptureBegan:l.captureBegan,
        lensOperation:l.operation,lensRequestedType:l.requestedType,lensFallbackCopy:l.fallbackCopy};
      for(const [key,value] of Object.entries(composition))check(report[key]===value,`Composition/report ${key} differs`);
      check(JSON.stringify(report.overlayPhaseCounts)===JSON.stringify(c.phaseCounts),'Composition phase counts differ');
      if(typeof f?.scope==='string'&&/v10|no overlays|no.*lens composition/i.test(f.scope))
        run.warnings.push('Historical main report scope label excludes composition; validated schema2 comparison report supplies the actual full-native V11 scope');
    } else check(report.wholeComposition!==true,'GL10 cannot claim whole composition');
    const originFields=['rasterOrigin','workingTargetWidth','workingTargetHeight','gpuViewportCrop'];
    const present=originFields.filter(key=>Object.hasOwn(report,key));
    const extent=nativeOrigin?[w.width,w.height]:w.window.slice(2);
    const crop=nativeOrigin&&!whole;
    run.raster={origin:nativeOrigin?'native':'viewport-local',workingTargetExtent:extent,gpuViewportCrop:crop,
      metadataSource:present.length?'reported':'historical-implicit'};
    if(present.length)check(present.length===4&&report.rasterOrigin===run.raster.origin&&report.workingTargetWidth===extent[0]
      &&report.workingTargetHeight===extent[1]&&report.gpuViewportCrop===crop,'Raster origin/working target/crop metadata contradicts manifest mode');
    else check(!nativeOrigin,'Native-origin manifest requires explicit raster/crop metadata');
    check(report.residentGpuFrame===true&&report.preparationImageReadbacks===0&&report.diagnosticImageReadbacks===1,'Resident preparation/readback evidence missing');
    if(f)check(count(f.frames)&&f.frames>=6&&count(f.frozenPresentations)&&f.frozenPresentations>=6&&f.frozenPreparations===1
      &&f.frozenImageReadbacks===1&&f.comparisonReviewRequired===true,'Resident presentations or unchanged preparation/readback totals missing');
    const outputExtent=whole?[w.width,w.height]:w.window.slice(2);
    const compared=await comparison(run.directory,whole?'enhanced-world-native':'enhanced-world-viewport',...outputExtent);run.recomputed=compared.record;
    const d=compared.record.metrics;
    for(const key of ['comparedPixels','differentPixels','differingBytes','alphaDifferingBytes','maximumByteDelta'])check(report[key]===d[key],`Raw replay ${key} disagrees`);
    check(report.passed===(report.executionPassed===true&&!report.error&&d.differingBytes===0)&&report.reviewRequired===!report.passed,'Replay exact/review flags contradict differences');
    check(typeof report.scope==='string'&&typeof report.sourceLimitations==='string','Replay scope/limitations missing');
    if(!d.differingBytes)qualified=d.comparedBytes;
  } catch(error){run.errors.push(`World replay: ${error.message}`);}
  if(!replay&&report) {
    run.operatorCases=[];let pixels=0,differences=0;const names=new Set(),fogImages=new Map();
    for(const c of report.cases??[])try {
      assert.ok(operatorNames.has(c.name)&&!names.has(c.name),'Unknown/duplicate world operator case');names.add(c.name);
      assert.equal(c.pixels,64*48);assert.ok(count(c.analyticDifferingBytes),'Invalid analytic difference count');
      const compared=await comparison(run.directory,'world-'+c.name,64,48),d=compared.record.metrics;
      if([...fogNames,nativeNames[0]].includes(c.name))fogImages.set(c.name,compared);
      for(const key of ['differingBytes','alphaDifferingBytes','maximumByteDelta'])check(c[key]===d[key],`Raw operator ${key}: ${c.name}`);
      let analytic=0,expectedBinding;
      if(analyticNames.has(c.name)||c.name===nativeNames[1]) {
        const expected=await image(run.directory,'world-'+c.name+'-expected',64,48);expectedBinding=expected.bindings;
        analytic=compare(compared.gpu.bytes,expected.bytes).differingBytes+compare(compared.gl.bytes,expected.bytes).differingBytes;
      }
      check(analytic===c.analyticDifferingBytes&&analytic===0,`Analytic comparison differs: ${c.name}`);
      pixels+=d.comparedPixels;differences+=d.differingBytes;if(!d.differingBytes)qualified+=d.comparedBytes;
      run.operatorCases.push({...c,raw:compared.record,...(expectedBinding?{expected:expectedBinding}:{}),analyticChecksQualified:false});
    } catch(error){run.errors.push(`World operator ${c.name}: ${error.message}`);run.operatorCases.push({...c,error:error.message});}
    check(report.comparedPixels===pixels&&report.differingBytes===differences,'Operator aggregate counts disagree');
    check(report.passed===(report.executionPassed===true&&!report.error&&differences===0),'Operator exact status contradicts differences');
    const complete=set=>names.size===set.size&&[...set].every(name=>names.has(name));
    const historical=complete(historicalNames),expanded=complete(fogSuiteNames),native=complete(operatorNames);
    run.operatorCoverage={profile:historical?'historical-21':expanded?'active-fog-23':native?'native-crop-25':'incomplete',
      activeFogOperatorCovered:false,nativeOriginTranslationCovered:false,nativeCropControlsCovered:false};
    if(report.executionPassed)check((historical||expanded||native)&&report.invalidCases===(native?10:9),'Operator named coverage/rejection set incomplete');
    if(expanded||native)try {
      const first=fogImages.get(fogNames[0]),translated=fogImages.get(fogNames[1]);assert.ok(first&&translated,'Missing active fog raw images');
      const changed=[],originDifferences=[],lower=[15,25,15],upper=[213,42,142];
      for(const api of ['gpu','gl']) {
        let active=0;const bytes=first[api].bytes;
        for(let p=0;p<bytes.length;p+=4) {
          for(let c=0;c<3;c++){assert.ok(bytes[p+c]>=lower[c]&&bytes[p+c]<=upper[c],'Active fog escaped analytic blend endpoints');active+=bytes[p+c]!==upper[c];}
          assert.equal(bytes[p+3],255,'Active fog changed opaque alpha');
        }
        assert.ok(active>0,'Active fog fixture remained inactive');changed.push(active);
        const difference=compare(bytes,translated[api].bytes).differingBytes;assert.equal(difference,0,'Equivalent global fog coordinates differ');originDifferences.push(difference);
      }
      run.operatorCoverage.activeFogDiagnostics={changedRgbComponents:changed,equivalentOriginDifferingBytes:originDifferences,
        apiOrder:['gpu','gl'],rgbLower:lower,rgbUpper:upper};
      if(native) {
        const cropped=fogImages.get(nativeNames[0]);assert.ok(cropped,'Missing native target fog crop');
        const differences=['gpu','gl'].map(api=>compare(translated[api].bytes,cropped[api].bytes).differingBytes);
        assert.deepEqual(differences,[0,0],'Native target fog crop differs from equivalent viewport-local global coordinates');
        run.operatorCoverage.nativeCropFogInvariantDifferingBytes=differences;
      }
    } catch(error){run.errors.push(`Active fog coverage: ${error.message}`);}
    if(f)check(f.frozenPreparations===(report.cases??[]).length&&f.frozenImageReadbacks===(report.cases??[]).length,'Operator preparation/readback totals disagree');
  }
  run.executionPassed=run.errors.length===0;run.exactComparison=run.executionPassed&&report?.passed===true;
  if(run.executionPassed&&['active-fog-23','native-crop-25'].includes(run.operatorCoverage?.profile)) {
    run.operatorCoverage.activeFogOperatorCovered=true;run.operatorCoverage.nativeOriginTranslationCovered=true;
    run.operatorCoverage.nativeCropControlsCovered=run.operatorCoverage.profile==='native-crop-25';
  }
  if(run.executionPassed)run[replay?'qualifiedReplayExactBytes':'qualifiedOperatorExactBytes']=qualified;
  if(run.executionPassed&&!run.exactComparison)run.warnings.push('Nonzero cross-API differences retained; no tolerance or visual acceptance');
  return run;
}
async function main() {
  const args=process.argv.slice(2),at=args.indexOf('--output');assert.ok(at>0&&at===args.length-2,'Usage: node enhanced-world-evidence.mjs <run-dir> [...] --output <summary.json>');
  const runs=[],seen=new Set();for(const directory of args.slice(0,at)) {
    const key=path.resolve(directory).toLowerCase();assert.ok(!seen.has(key),'Duplicate selected run');seen.add(key);runs.push(await inspectEnhancedWorldRun(directory));
  }
  const summary={schema:1,scope:'Explicit GL10 pre-overlay viewport comparisons, GL11 full-native world/overlay/swipe/lens composition comparisons and separate operators; scene identity and effective world/composition resource/state hashes remain separate. Ordinary UI, gameplay and visual/Gate2 acceptance remain excluded',
    executionPassed:runs.every(r=>r.executionPassed),exactRuns:runs.filter(r=>r.exactComparison).length,visualAccepted:false,gate2Accepted:false,
    qualifiedReplayExactBytes:runs.reduce((n,r)=>n+r.qualifiedReplayExactBytes,0),qualifiedOperatorExactBytes:runs.reduce((n,r)=>n+r.qualifiedOperatorExactBytes,0),runs};
  const output=path.resolve(args[at+1]);await mkdir(path.dirname(output),{recursive:true});await writeFile(output,JSON.stringify(summary,null,2)+'\n');
  console.log(JSON.stringify({output,executionPassed:summary.executionPassed,exactRuns:summary.exactRuns,runs:runs.length,
    qualifiedReplayExactBytes:summary.qualifiedReplayExactBytes,qualifiedOperatorExactBytes:summary.qualifiedOperatorExactBytes}));
  if(!summary.executionPassed)process.exitCode=1;
}
if(process.argv[1]&&import.meta.url===pathToFileURL(path.resolve(process.argv[1])).href)await main();
