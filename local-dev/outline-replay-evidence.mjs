// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Bind explicitly selected outline runs and recompute differences from raw
// pixels. Successful execution is distinct from exact cross-API agreement.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFile,writeFile,stat,mkdir,realpath} from 'node:fs/promises';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {parseSceneCapture} from './reference-fingerprint.mjs';
const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
async function load(file) {
  assert.ok((await stat(file)).size<=128*1024*1024,`Oversized evidence: ${file}`);
  const bytes=await readFile(file); return {bytes,binding:{path:file,bytes:bytes.length,sha256:hash(bytes)}};
}
async function json(file) {
  const loaded=await load(file);return {...loaded,value:JSON.parse(loaded.bytes.toString('utf8').replace(/^\uFEFF/,''))};
}
export async function inspectOutlineRun(directory) {
  const run={directory:path.resolve(directory),executionPassed:false,exactComparison:false,errors:[],warnings:[],bindings:{},cases:[],
    qualifiedStrictByteChecks:0,qualifiedExactPixels:0,rasterComparisonAccepted:false,gate2Accepted:false};
  try {
    const manifest=await json(path.join(run.directory,'manifest.json'));
    const main=await json(path.join(run.directory,'report.json'));
    const operator=await json(path.join(run.directory,'outline-operator.json'));
    const replay=await json(path.join(run.directory,'outline-replay.json'));
    Object.assign(run.bindings,{manifest:manifest.binding,main:main.binding,operator:operator.binding,replay:replay.binding});
    const m=manifest.value,f=main.value,o=operator.value,r=replay.value;
    assert.ok(m.arguments.includes('--outline-replay') && m.arguments.includes('--gpu-validation'),'Not a validated outline run');
    const index=m.arguments.indexOf('--scene-capture');assert.ok(index>=0,'Missing captured input');
    const input=path.resolve(m.arguments[index+1].replace(/^"|"$/g,''));
    assert.equal(await realpath(input),await realpath(r.capture),'Replay and manifest input differ');
    const capture=await load(input),parsed=parseSceneCapture(capture.bytes);
    assert.equal(parsed.version,7,'Outline input must be v7');
    assert.equal(capture.binding.sha256,m.sceneCaptureSha256.toLowerCase(),'Captured input hash changed');
    run.bindings.capture=capture.binding;
    assert.match(m.executableSha256,/^[a-f0-9]{64}$/i,'Missing executable binding');
    run.bindings.executable={path:m.executablePath,recordedSha256:m.executableSha256.toLowerCase(),verification:'historical_hash_only'};
    try {
      const exe=await load(m.executablePath);
      run.bindings.executable.currentSha256=exe.binding.sha256;
      if(exe.binding.sha256===m.executableSha256.toLowerCase())run.bindings.executable.verification='matching_current_binary';
      else run.warnings.push('Executable has changed; original manifest hash retained');
    } catch(error) {if(error.code!=='ENOENT')throw error;run.warnings.push('Historical executable unavailable');}
    assert.equal(f.status,'passed');assert.equal(f.debugLayer,true);assert.equal(f.gpuValidation,true);
    assert.equal(f.validationErrors,0);assert.equal(f.validationWarnings,0);
    assert.equal(o.strict_checks_passed,true);assert.equal(o.error,'');assert.equal(r.passed,true);assert.equal(r.error,'');
    assert.equal(r.rasterComparisonAccepted,false);assert.equal(r.gate2Accepted,false);
    assert.ok(Number.isSafeInteger(o.strict_byte_checks)&&o.strict_byte_checks>0);
    run.device={adapter:f.adapter,vendorId:f.vendorId,openglDevice:r.opengl_device};
    run.source={revision:m.sourceRevision,status:m.sourceStatus,timestamp:m.timestamp};
    run.mode=r.mode;run.viewport=r.viewport;run.occluders=r.occluders;run.sprites=r.sprites;
    run.outlineDraws=r.outline_draws;run.glowCommandsOmitted=r.glow_commands_omitted;
    run.sourcePaddingPolicy=r.outside_logical_source_coverage;
    const [width,height]=r.viewport,pixels=width*height;
    assert.ok(Number.isInteger(width)&&width>0&&Number.isInteger(height)&&height>0&&pixels<=16777216);
    assert.deepEqual(r.cases.map(c=>c.case),['outline-owner-colour','outline-ordered-coverage','outline-depth-coverage']);
    let compared=0,exact=0;
    for(const c of r.cases) {
      const a=await load(path.join(run.directory,c.case+'-dx12.rgba'));
      const b=await load(path.join(run.directory,c.case+'-opengl.rgba'));
      assert.equal(a.bytes.length,pixels*4);assert.equal(b.bytes.length,pixels*4);
      let differentPixels=0,differentChannels=0,maxDelta=0,coverageDisagreement=0;
      const background=c.case==='outline-depth-coverage'?[0,0,0,255]:[19,31,47,255];
      for(let p=0;p<a.bytes.length;p+=4) {
        let changed=false,aChanged=false,bChanged=false;
        for(let channel=0;channel<4;channel++) {
          const delta=Math.abs(a.bytes[p+channel]-b.bytes[p+channel]);
          maxDelta=Math.max(maxDelta,delta);differentChannels+=delta!==0;changed ||= delta!==0;
          aChanged ||= a.bytes[p+channel]!==background[channel];bChanged ||= b.bytes[p+channel]!==background[channel];
        }
        differentPixels+=changed;coverageDisagreement+=aChanged!==bChanged;
      }
      assert.equal(c.compared_pixels,pixels);assert.equal(c.different_pixels,differentPixels);
      assert.equal(c.different_channels,differentChannels);assert.equal(c.max_channel_delta,maxDelta);
      assert.equal(c.changed_coverage_disagreement,coverageDisagreement);
      run.bindings[c.case]={dx12:a.binding,opengl:b.binding};
      run.cases.push({...c,recomputed:true});compared+=pixels;if(!differentPixels)exact+=pixels;
    }
    assert.equal(compared,r.compared_pixels);
    assert.equal(o.blend_comparisons_exact,o.blend_difference_bytes===0);
    assert.equal(o.blend_review_required,o.blend_difference_bytes!==0);
    if(o.blend_difference_bytes)run.warnings.push('Operator blend bytes differ; not qualified as an exact cross-API comparison');
    if(exact!==compared)run.warnings.push('Captured RGBA pixels differ; retain per-channel and coverage evidence for review');
    run.executionPassed=true;run.qualifiedStrictByteChecks=o.strict_byte_checks;
    run.qualifiedExactPixels=exact;run.observedComparedPixels=compared;run.blendDifferenceBytes=o.blend_difference_bytes;
    run.exactComparison=exact===compared&&o.blend_difference_bytes===0;
  } catch(error) {run.errors.push(error.message);run.qualifiedStrictByteChecks=0;run.qualifiedExactPixels=0;}
  return run;
}
async function main() {
  const args=process.argv.slice(2),at=args.indexOf('--output');
  assert.ok(at>0&&at===args.length-2,'Usage: node outline-replay-evidence.mjs <run-dir> [...] --output <summary.json>');
  const runs=[];for(const directory of args.slice(0,at))runs.push(await inspectOutlineRun(directory));
  const summary={schema:1,scope:'Owned outline diagnostics; exact geometry and colour counters separated, no full-world or gate acceptance',
    executionPassed:runs.every(r=>r.executionPassed),exactRuns:runs.filter(r=>r.exactComparison).length,
    qualifiedStrictByteChecks:runs.reduce((n,r)=>n+r.qualifiedStrictByteChecks,0),
    qualifiedExactPixels:runs.reduce((n,r)=>n+r.qualifiedExactPixels,0),rasterComparisonAccepted:false,gate2Accepted:false,runs};
  const target=path.resolve(args[at+1]);await mkdir(path.dirname(target),{recursive:true});
  await writeFile(target,JSON.stringify(summary,null,2)+'\n');
  console.log(JSON.stringify({output:target,executionPassed:summary.executionPassed,exactRuns:summary.exactRuns,
    qualifiedStrictByteChecks:summary.qualifiedStrictByteChecks,qualifiedExactPixels:summary.qualifiedExactPixels}));
  if(!summary.executionPassed)process.exitCode=1;
}
if(process.argv[1]&&import.meta.url===pathToFileURL(path.resolve(process.argv[1])).href)await main();
