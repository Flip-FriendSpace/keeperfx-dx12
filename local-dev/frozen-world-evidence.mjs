// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Explicitly selected resident frozen-world runs. Exact palette/path checks
// are separate from original-C raster differences and pending visual review.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFile,stat,writeFile,mkdir} from 'node:fs/promises';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {parseSceneCapture,sceneFingerprint} from './reference-fingerprint.mjs';
const shaPattern=/^[a-f0-9]{64}$/i;
const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
const unquote=value=>value.replace(/^"|"$/g,'');
const count=value=>Number.isSafeInteger(value)&&value>=0;
async function load(file) {
  assert.ok((await stat(file)).size<=128*1024*1024,`Oversized evidence: ${file}`);
  const bytes=await readFile(file);return {bytes,binding:{path:file,bytes:bytes.length,sha256:hash(bytes)}};
}
async function json(file) {
  const loaded=await load(file);return {...loaded,value:JSON.parse(loaded.bytes.toString('utf8').replace(/^\uFEFF/,''))};
}
const differences=(a,b)=>{assert.equal(a.length,b.length,'Comparison lengths differ');let total=0;for(let i=0;i<a.length;i++)total+=a[i]!==b[i];return total;};
export async function inspectFrozenWorldRun(directory) {
  const run={directory:path.resolve(directory),passed:false,errors:[],warnings:[],bindings:{},artifacts:[],
    comparisons:{},qualifiedPaletteByteChecks:0,qualifiedRovSerialIndexChecks:0,qualifiedGpuChecks:0,
    visualAccepted:false,gate2Accepted:false};
  const check=(ok,message)=>{if(!ok)run.errors.push(message);};
  const documents={};
  for(const [key,name] of [['manifest','manifest.json'],['main','report.json'],['world','frozen-world.json']]) {
    try {const loaded=await json(path.join(run.directory,name));documents[key]=loaded.value;run.bindings[key]=loaded.binding;}
    catch(error){run.errors.push(`${name}: ${error.message}`);}
  }
  const {manifest:m,main:f,world:w}=documents;
  let capture,parsed,pixels;
  if(m) {
    run.source={revision:m.sourceRevision,status:m.sourceStatus,timestamp:m.timestamp};
    try {
      assert.ok(Array.isArray(m.arguments)&&m.arguments.includes('--frozen-world')&&m.arguments.includes('--gpu-validation'),'Not a GPU-validated frozen-world manifest');
      const at=m.arguments.indexOf('--scene-capture');assert.ok(at>=0&&typeof m.arguments[at+1]==='string','Missing capture argument');
      capture=await load(path.resolve(unquote(m.arguments[at+1])));parsed=parseSceneCapture(capture.bytes);
      assert.equal(parsed.version,8,'Frozen world requires v8 target/clear proof');
      assert.match(m.sceneCaptureSha256??'',shaPattern,'Missing captured input hash');
      run.bindings.capture={...capture.binding,recordedSha256:m.sceneCaptureSha256.toLowerCase()};
      assert.equal(capture.binding.sha256,run.bindings.capture.recordedSha256,'Capture changed since GPU run');
      run.frozenFingerprint=sceneFingerprint(capture.bytes);run.target=parsed.target;
      const {offsets,target}=parsed,b=capture.bytes;
      const triangles=b.readUInt32LE(offsets.triangles),shadows=b.readUInt32LE(offsets.shadows),sprites=b.readUInt32LE(offsets.draws),commands=b.readUInt32LE(offsets.commands);
      pixels=target.window[2]*target.window[3];assert.ok(count(pixels)&&pixels>0&&pixels<=16777216,'Invalid GPU viewport pixel count');
      const buckets=new Array(12).fill(0);
      for(let i=0;i<b.readUInt32LE(offsets.entries);i++) {const kind=b[offsets.entries+4+i*5+4];assert.ok(kind<buckets.length,'Unaccounted world bucket kind');buckets[kind]++;}
      run.capture={triangles,shadowTriangles:shadows,spriteDraws:sprites,commands,bucketCounts:buckets,
        excludedSpriteCalls:b.readUInt32LE(offsets.trace+2),excludedShadowEntries:b.readUInt32LE(offsets.excludedShadows)};
      if(w) {
        check(w.formatVersion===8&&w.targetKind===target.kind&&w.clearIndex===target.clearIndex&&w.lensId===target.lensId,'Report target kind/clear/lens differs from capture');
        check(w.targetWidth===target.width&&w.targetHeight===target.height&&w.targetPitch===target.pitch,'Report target dimensions/pitch differ from capture');
        check(JSON.stringify(w.viewport)===JSON.stringify(target.window)&&w.pixels===pixels,'Report native viewport/pixels differ from capture');
        for(const key of ['triangles','shadowTriangles','spriteDraws','excludedSpriteCalls','excludedShadowEntries'])check(w[key]===run.capture[key],`Report ${key} differs from capture`);
        check(count(w.clippedSprites)&&w.clippedSprites<=sprites&&w.commands===commands-w.clippedSprites,'Rendered/clipped command counts disagree');
        check(JSON.stringify(w.bucketCounts)===JSON.stringify(buckets),'Bucket counts differ from captured traversal');
      }
    } catch(error){run.errors.push(`Capture binding: ${error.message}`);}
    try {
      assert.match(m.executableSha256??'',shaPattern,'Missing recorded executable hash');
      const file=path.resolve(m.executablePath??path.join(run.directory,'../bin/keeperfx-dx12.exe'));
      const binding=run.bindings.executable={path:file,recordedSha256:m.executableSha256.toLowerCase(),verification:'historical_hash_only'};
      try {
        binding.currentSha256=(await load(file)).binding.sha256;
        if(binding.currentSha256===binding.recordedSha256)binding.verification='matching_current_binary';
        else run.warnings.push('Current executable changed; original manifest hash retained');
      } catch(error){if(error.code!=='ENOENT')throw error;run.warnings.push('Historical executable unavailable; recorded hash retained');}
    } catch(error){run.errors.push(`Executable binding: ${error.message}`);}
  }
  if(w) {
    run.preparationPassed=w.preparationPassed;run.scope=w.scope;run.sourceLimitations=w.sourceLimitations;
    run.exclusions={uiBucketKinds:w.unrenderedUiBucketKinds,excludedSpriteCalls:w.excludedSpriteCalls,
      excludedShadowEntries:w.excludedShadowEntries,clippedSprites:w.clippedSprites};
    run.originalGuardChanges=w.originalGuardChanges;
    run.preparation={preparations:w.preparations,diagnosticImageReadbacks:w.diagnosticImageReadbacks};
    check([1,2].includes(w.schema)&&w.preparationPassed===true,'Frozen world preparation did not pass');
    const available=w.schema===1?true:w.originalAvailable;
    run.originalReference={available,unavailableReason:w.schema===1?null:w.originalUnavailableReason,
      policy:w.schema===1?'Historical packed viewport stride; native pitch alias differences remain explicit':'Native target stride; unavailable reference has no qualified comparison'};
    check(typeof available==='boolean','Explicit original-C availability missing');
    if(w.schema===2) {
      run.originalReference.nativeStride=w.originalNativeStride;run.correctedNativeStride=w.correctedNativeStride;
      run.originalUnavailableReads=w.originalUnavailableReads;
      check(w.originalNativeStride===true&&w.correctedNativeStride===true,'Native target stride evidence missing');
      check(typeof w.originalUnavailableReason==='string'&&(available?w.originalUnavailableReason.length===0:w.originalUnavailableReason.length>0),'Original-C availability reason contradicts status');
      if(!available) {
        check(w.originalDifferingIndices===null&&w.originalCorrectionDifferingIndices===null,'Unavailable original-C metrics must be null');
        run.warnings.push(`Original-C reference unavailable: ${w.originalUnavailableReason}`);
      }
    }
    check(w.visualAccepted===false&&w.gate2Accepted===false,'Pending visual/Gate 2 acceptance missing');
    check(typeof w.scope==='string'&&typeof w.sourceLimitations==='string','Comparison scope/limitations missing');
    check(JSON.stringify(w.unrenderedUiBucketKinds)===JSON.stringify([4,5,7,8,10]),'UI exclusions changed');
  }
  if(f) {
    run.device={adapter:f.adapter,debugLayer:f.debugLayer,gpuValidation:f.gpuValidation,
      validationErrors:f.validationErrors,validationWarnings:f.validationWarnings,rovQuerySucceeded:f.rasterizerOrderedViewsQuerySucceeded,rovSupported:f.rasterizerOrderedViewsSupported};
    run.presentation={frames:f.frames,presentations:f.frozenPresentations,preparations:f.frozenPreparations,imageReadbacks:f.frozenImageReadbacks,interactive:f.interactive};
    check(f.status==='passed','Overall execution did not pass');
    check(f.debugLayer===true&&f.gpuValidation===true&&f.validationErrors===0&&f.validationWarnings===0,'Zero debug/GPU validation counts missing');
    check(count(f.frozenPresentations)&&f.frozenPresentations>=6&&count(f.frames)&&f.frames>=6,'Fewer than six resident presentations');
    check(f.comparisonReviewRequired===true,'Original-C raster comparison must remain explicit/pending');
    if(w)check(f.frozenPreparations===w.preparations&&f.frozenImageReadbacks===w.diagnosticImageReadbacks,'Extra preparations/readbacks occurred after world preparation');
  }
  if(capture&&parsed&&pixels&&w&&f)try {
    const buffers={};
    const originalAvailable=run.originalReference.available===true;
    const inputs=[['gpu','frozen-gpu.idx',pixels],['rgba','frozen-gpu.rgba',pixels*4],['corrected','frozen-corrected.idx',pixels]];
    if(originalAvailable)inputs.push(['original','frozen-original.idx',pixels]);
    for(const [key,name,size] of inputs) {
      const loaded=await load(path.join(run.directory,name));run.artifacts.push(loaded.binding);
      assert.equal(loaded.bytes.length,size,`Wrong native byte count: ${name}`);buffers[key]=loaded.bytes;
    }
    const bitmaps=['frozen-gpu.bmp','frozen-corrected.bmp','frozen-difference.bmp'];
    if(originalAvailable)bitmaps.push('frozen-original.bmp');
    for(const name of bitmaps) {
      const loaded=await load(path.join(run.directory,name));run.artifacts.push(loaded.binding);
      assert.equal(loaded.bytes.length,54+pixels*4,`Wrong native BMP size: ${name}`);
      assert.equal(loaded.bytes.toString('ascii',0,2),'BM');assert.equal(loaded.bytes.readInt32LE(18),parsed.target.window[2]);assert.equal(loaded.bytes.readInt32LE(22),parsed.target.window[3]);
    }
    const metrics=run.comparisons={pixels,originalDifferingIndices:originalAvailable?differences(buffers.gpu,buffers.original):null,
      correctedDifferingIndices:differences(buffers.gpu,buffers.corrected),originalCorrectionDifferingIndices:originalAvailable?differences(buffers.original,buffers.corrected):null,paletteDifferentBytes:0,rovSerialDifferentIndices:null};
    for(const key of ['originalDifferingIndices','correctedDifferingIndices','originalCorrectionDifferingIndices'])check(metrics[key]===w[key],`Raw ${key} differs from report`);
    const palette=capture.bytes.subarray(parsed.offsets.palettes+768,parsed.offsets.palettes+1536);
    assert.ok(palette.every(value=>value<=63),'Invalid captured display palette');
    for(let i=0;i<pixels;i++) {
      for(let c=0;c<3;c++)metrics.paletteDifferentBytes+=buffers.rgba[i*4+c]!==Math.floor(palette[buffers.gpu[i]*3+c]*255/63);
      metrics.paletteDifferentBytes+=buffers.rgba[i*4+3]!==255;
    }
    check(metrics.paletteDifferentBytes===0&&w.exactPaletteByteChecks===pixels*4,'Integer palette RGBA differs or reported check count is wrong');
    const rov=f.rasterizerOrderedViewsSupported===true;
    check(typeof f.rasterizerOrderedViewsSupported==='boolean'&&(!rov||f.rasterizerOrderedViewsQuerySucceeded===true),'ROV capability provenance missing');
    check(w.preparations===(rov?2:1)&&w.diagnosticImageReadbacks===(rov?3:2),'Preparation/readback totals disagree with serial/ROV paths');
    check(w.exactRovSerialIndexChecks===(rov?pixels:0),'Reported ROV/serial checks disagree with actual path');
    run.paths={serial:true,rov,reason:rov?'supported and retained':'unsupported; serial only'};
    if(rov) {
      const loaded=await load(path.join(run.directory,'frozen-rov.idx'));run.artifacts.push(loaded.binding);
      assert.equal(loaded.bytes.length,pixels,'Wrong native ROV byte count');metrics.rovSerialDifferentIndices=differences(buffers.gpu,loaded.bytes);
      check(metrics.rovSerialDifferentIndices===0,'ROV differs from serial painter order');
    }
    assert.ok(Array.isArray(w.originalGuardChanges),'Original guard evidence missing');
    const copyCandidate=commandIndex=>{
      assert.ok(count(commandIndex)&&commandIndex<run.capture.commands,'Invalid original-C sprite command');
      const command=parsed.offsets.commands+4+commandIndex*5;
      assert.equal(capture.bytes[command],0,'Original-C diagnostic did not originate from a sprite command');
      const draw=parsed.offsets.draws+4+capture.bytes.readUInt32LE(command+1)*327,b=capture.bytes,flags=b.readUInt32LE(draw+64);
      assert.ok(!b[draw+68]&&(flags&1)&&[0,1].includes(b[draw+70])
        &&(b.readInt32LE(draw+36)>b.readInt32LE(draw+28)||b.readInt32LE(draw+40)>b.readInt32LE(draw+32)), 'Original-C diagnostic is not the declared mirrored upscale row-copy quirk');
    };
    for(const change of w.originalGuardChanges) {
      const row=(change.offset+1)/parsed.target.pitch;
      const knownOffset=w.schema===1?change.offset===-1:Number.isInteger(row)&&row>=0&&row<parsed.target.window[3];
      assert.ok(Number.isSafeInteger(change.offset)&&knownOffset
        &&count(change.before)&&change.before<=255&&count(change.after)&&change.after<=255&&change.before!==change.after,'Invalid original guard change');
      copyCandidate(change.command);
    }
    if(w.schema===2) {
      assert.ok(Array.isArray(w.originalUnavailableReads),'Original read-exclusion evidence missing');
      if(originalAvailable)assert.equal(w.originalUnavailableReads.length,0,'Available original reference has blocked reads');
      for(const read of w.originalUnavailableReads) {
        assert.ok(Number.isSafeInteger(read.sourceOffset)&&Number.isSafeInteger(read.destinationOffset)&&count(read.length)&&read.length>0,'Invalid original read-exclusion range');
        const pitch=parsed.target.pitch,[,,width,height]=parsed.target.window,first=read.sourceOffset,last=first+read.length-1;
        assert.ok(Number.isSafeInteger(last),'Original read-exclusion range overflow');
        const firstRow=Math.floor(first/pitch),lastRow=Math.floor(last/pitch),firstX=first-firstRow*pitch,lastX=last-lastRow*pitch;
        const inside=firstRow>=0&&lastRow<height&&firstX<width&&lastX<width&&(firstRow===lastRow||pitch===width);
        assert.ok(!inside,'Claimed unavailable source read stays inside owned viewport');copyCandidate(read.command);
      }
    }
  } catch(error){run.errors.push(`Raw comparisons: ${error.message}`);}
  run.passed=run.errors.length===0;
  if(run.passed) {
    run.qualifiedPaletteByteChecks=pixels*4;run.qualifiedRovSerialIndexChecks=w.exactRovSerialIndexChecks;
    run.qualifiedGpuChecks=run.qualifiedPaletteByteChecks+run.qualifiedRovSerialIndexChecks;
  }
  return run;
}
async function main() {
  const args=process.argv.slice(2),at=args.indexOf('--output');
  assert.ok(at>0&&at===args.length-2,'Usage: node frozen-world-evidence.mjs <run-dir> [...] --output <summary.json>');
  const runs=[];for(const directory of args.slice(0,at))runs.push(await inspectFrozenWorldRun(directory));
  const result={schema:1,scope:'Resident v8 software-world viewport evidence; exact GPU palette/path checks separated from original-C raster diagnostics',
    passed:runs.every(run=>run.passed),visualAccepted:false,gate2Accepted:false,
    qualifiedPaletteByteChecks:runs.reduce((sum,run)=>sum+run.qualifiedPaletteByteChecks,0),
    qualifiedRovSerialIndexChecks:runs.reduce((sum,run)=>sum+run.qualifiedRovSerialIndexChecks,0),runs};
  const file=path.resolve(args[at+1]);await mkdir(path.dirname(file),{recursive:true});await writeFile(file,JSON.stringify(result,null,2)+'\n');
  console.log(JSON.stringify({output:file,passed:result.passed,runs:runs.length,qualifiedPaletteByteChecks:result.qualifiedPaletteByteChecks,qualifiedRovSerialIndexChecks:result.qualifiedRovSerialIndexChecks}));
  if(!result.passed)process.exitCode=1;
}
if(process.argv[1]&&import.meta.url===pathToFileURL(path.resolve(process.argv[1])).href)await main();
