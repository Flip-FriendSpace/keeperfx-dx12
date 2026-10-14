// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Validate explicitly selected frozen-sprite replay runs. Failed inputs remain
// in the output; no directory discovery, best-run selection or world-parity gate.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFile, writeFile, mkdir, stat, realpath} from 'node:fs/promises';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {isDeepStrictEqual} from 'node:util';
import {parseSceneCapture} from './reference-fingerprint.mjs';

const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const hashPattern = /^[a-f0-9]{64}$/i;
const count = (value, label) => {
  assert.ok(Number.isSafeInteger(value) && value >= 0, `Invalid or missing ${label}`);
  return value;
};
const unquote = value => typeof value === 'string' && value.startsWith('"') && value.endsWith('"')
  ? value.slice(1,-1) : value;
async function bytes(file, limit=128*1024*1024) {
  assert.ok((await stat(file)).size <= limit, `Oversized evidence file: ${file}`);
  return readFile(file);
}
async function jsonFile(file) {
  const raw = await bytes(file,16*1024*1024);
  return {path:file, sha256:sha(raw), bytes:raw.length, value:JSON.parse(raw.toString('utf8').replace(/^\uFEFF/,''))};
}
function captureMetadata(raw) {
  const parsed = parseSceneCapture(raw);
  assert.ok(parsed.version >= 3,'Sprite replay evidence requires KFXC v3 or later');
  // SceneCaptureCodec camera: world header, 128 wibble records, camera kind,
  // position, then yaw/pitch/roll/zoom/fov/lens/viewport_x/y/w/h.
  const viewport = 24+36+1+12+128*16+1+12+8*4;
  const width=raw.readInt32LE(viewport), height=raw.readInt32LE(viewport+4);
  assert.ok(width > 0 && height > 0,'Invalid captured viewport');
  const draws = raw.readUInt32LE(parsed.offsets.draws);
  const commands = Array.from({length:draws},(_,ordinal) => {
    const start=parsed.offsets.draws+4+ordinal*327;
    return {ordinal, object_slot:raw.readUInt32LE(start+8), entry:raw.readUInt32LE(start+12),
      sprite:raw.readUInt32LE(start+16), content_height:raw.readInt32LE(start+60),
      flags:raw.readUInt32LE(start+64), alpha:!!raw[start+68], operator:raw[start+70]};
  });
  return {version:parsed.version,width,height,commands,triangles:parsed.offsets.triangles === undefined ? 0 : raw.readUInt32LE(parsed.offsets.triangles),
    unifiedCommands:parsed.offsets.commands === undefined ? null : raw.readUInt32LE(parsed.offsets.commands),producerBackend:raw[parsed.offsets.trace+1],
    excludedSpriteCalls:raw.readUInt32LE(parsed.offsets.trace+2)};
}
export async function inspectReplayRun(directory) {
  const run={directory:path.resolve(directory),passed:false,errors:[],warnings:[],bindings:{},
    observedGpuIndexedChecks:0,qualifiedGpuIndexedChecks:0,cpuIndexedChecks:0,
    paths:{serial:{cases:0,indexedChecks:0},rov:{cases:0,indexedChecks:0}},failedCases:[],originalQuirkCases:[]};
  const check=(condition,message) => { if(!condition) run.errors.push(message); };
  let manifest,report,replay,capture;
  for(const [name,key] of [['manifest.json','manifest'],['report.json','report'],['sprite-replay.json','spriteReplay']]) {
    try {
      const loaded=await jsonFile(path.join(run.directory,name));
      run.bindings[key]={path:loaded.path,sha256:loaded.sha256,bytes:loaded.bytes};
      if(key === 'manifest') manifest=loaded.value;
      if(key === 'report') report=loaded.value;
      if(key === 'spriteReplay') replay=loaded.value;
    } catch(error) { run.errors.push(`${name}: ${error.message}`); }
  }
  if(manifest) {
    run.source={revision:manifest.sourceRevision,status:manifest.sourceStatus,timestamp:manifest.timestamp};
    try {
      assert.ok(Array.isArray(manifest.arguments),'Missing manifest arguments');
      assert.ok(manifest.arguments.includes('--sprite-replay'),'Manifest is not a sprite replay invocation');
      assert.ok(manifest.arguments.includes('--gpu-validation'),'Manifest does not request GPU validation');
      const index=manifest.arguments.indexOf('--scene-capture');
      assert.ok(index >= 0 && typeof manifest.arguments[index+1] === 'string','Missing manifest capture path');
      const capturePath=path.resolve(unquote(manifest.arguments[index+1]));
      assert.match(manifest.sceneCaptureSha256 ?? '',hashPattern,'Missing recorded capture SHA256');
      const raw=await bytes(capturePath);
      const actualSha256=sha(raw);
      run.bindings.capture={path:capturePath,sha256:actualSha256,recordedSha256:manifest.sceneCaptureSha256.toLowerCase(),
        bytes:raw.length,verified:actualSha256 === manifest.sceneCaptureSha256.toLowerCase()};
      check(run.bindings.capture.verified,'Capture SHA256 differs from original manifest');
      capture=captureMetadata(raw);
      run.capture={version:capture.version,width:capture.width,height:capture.height,commands:capture.commands.length,
        triangles:capture.triangles,unifiedCommands:capture.unifiedCommands,
        producerBackend:capture.producerBackend,excludedSpriteCalls:capture.excludedSpriteCalls};
      if(replay) check(await realpath(capturePath) === await realpath(replay.scene),'Replay scene path differs from manifest capture path');
    } catch(error) { run.errors.push(`Capture binding: ${error.message}`); }
    try {
      assert.match(manifest.executableSha256 ?? '',hashPattern,'Missing recorded executable SHA256');
      const executablePath=path.resolve(manifest.executablePath ?? path.join(run.directory,'../bin/keeperfx-dx12.exe'));
      run.bindings.executable={path:executablePath,recordedSha256:manifest.executableSha256.toLowerCase(),
        pathSource:manifest.executablePath ? 'manifest' : 'shared-sibling-bin',verified:false,verification:'historical_hash_only'};
      try {
        const raw=await bytes(executablePath);
        const actual=sha(raw);
        Object.assign(run.bindings.executable,{currentSha256:actual,currentBytes:raw.length,verified:actual === manifest.executableSha256.toLowerCase()});
        if(run.bindings.executable.verified) run.bindings.executable.verification='matching_current_binary';
        else run.warnings.push('Current shared executable differs from historical run hash; original manifest remains the run-time executable binding');
      } catch(error) {
        if(error.code !== 'ENOENT') throw error;
        run.warnings.push('Recorded executable hash retained; executable bytes unavailable for independent verification');
      }
    } catch(error) { run.errors.push(`Executable binding: ${error.message}`); }
  }
  if(report) {
    run.foundationScope=report.scope;
    if(replay && typeof report.scope === 'string' && !report.scope.toLowerCase().includes('sprite'))
      run.warnings.push('Historical foundation report carries the contact-sheet scope label; scope is taken explicitly from the hash-bound sprite-replay.json, which declares sprites-only replay');
    run.device={adapter:report.adapter,vendorId:report.vendorId,debugLayer:report.debugLayer,
      gpuValidation:report.gpuValidation,validationErrors:report.validationErrors,validationWarnings:report.validationWarnings,
      rovQuerySucceeded:report.rasterizerOrderedViewsQuerySucceeded,rovSupported:report.rasterizerOrderedViewsSupported};
    check(report.status === 'passed',`Foundation status is ${report.status}: ${report.error ?? ''}`);
    check(report.debugLayer === true,'Debug layer success is not recorded');
    check(report.gpuValidation === true,'GPU validation success is not recorded');
    check(report.validationErrors === 0 && report.validationWarnings === 0,'Zero validation error/warning counts are not recorded');
  }
  if(replay) {
    run.scope=replay.scope; run.comparison=replay.comparison;
    run.excluded=replay.excluded; run.unsupported=replay.unsupported;
    run.originalQuirk={description:replay.original_row_copy_quirk,guardObservations:replay.original_guard_observations,
      differences:replay.original_corrected_differences,guardChangedBytes:replay.original_guard_changed_bytes,
      originalExactParity:replay.original_exact_parity};
    check(replay.passed === true,`Sprite replay failed: ${replay.error ?? ''}`);
    check(typeof replay.scope === 'string' && replay.scope.includes('no full world-frame parity'),'Missing sprites-only scope');
    check(Array.isArray(replay.excluded) && replay.excluded.length > 0,'Missing explicit rendering exclusions');
    if(capture) {
      check(isDeepStrictEqual(replay.source_commands,capture.commands),'Reported commands differ from captured submission order/state');
      check(replay.producer_backend === capture.producerBackend,'Producer backend differs from capture');
      check(replay.excluded_sprite_calls === capture.excludedSpriteCalls,'Excluded-call count differs from capture');
    }
    const records=Array.isArray(replay.results) ? replay.results : [];
    check(records.length > 0,'No replay case records');
    const groups=new Map();
    for(const record of records) {
      if(record.passed !== true) run.failedCases.push(record);
      const group=groups.get(record.case) ?? []; group.push(record); groups.set(record.case,group);
      const serial=record.path === 'serial' || record.path === 'fallback';
      const gpuPath=serial ? 'serial' : record.path === 'rov' ? 'rov' : null;
      try {
        if(record.path === 'cpu-empty-maps') {
          check(record.gpu_tested === false && record.indexed_checks === 0,`CPU-only case claims GPU checks: ${record.case}`);
          const checks=count(record.cpu_indexed_checks ?? 0,'CPU indexed checks');
          run.cpuIndexedChecks += checks;
          check(checks > 0 && checks === count(record.width,'CPU width')*count(record.height,'CPU height'),
            `CPU empty-map background comparison is not recorded for the full viewport: ${record.case}`);
          check(record.corrected_guard_changed_bytes === 0,`CPU corrected guard invariance missing: ${record.case}`);
          continue;
        }
        if(!gpuPath) { check(false,`Unknown or missing GPU path: ${record.case}`); continue; }
        const checks=count(record.indexed_checks ?? 0,'GPU indexed checks');
        run.paths[gpuPath].cases++; run.paths[gpuPath].indexedChecks += checks;
        run.observedGpuIndexedChecks += checks;
        if(checks) {
          check(checks === count(record.width,'width')*count(record.height,'height'),`Partial-viewport comparison count: ${record.case}/${record.path}`);
          check(record.gpu_corrected_differences === 0,`GPU/corrected mismatch: ${record.case}/${record.path}`);
          check(record.corrected_guard_changed_bytes === 0,`Corrected guard invariance missing: ${record.case}/${record.path}`);
        } else check(record.rejected_before_allocation === true,`Zero-pixel GPU record is not a validated rejection: ${record.case}`);
      } catch(error) { run.errors.push(`${record.case}: ${error.message}`); }
    }
    check(run.failedCases.length === 0,`${run.failedCases.length} replay case records failed`);
    check(replay.indexed_checks === run.observedGpuIndexedChecks,'Replay GPU comparison aggregate differs from per-path pixel counts');
    check(replay.gpu_corrected_differences === 0,'Nonzero GPU/corrected aggregate');
    const rovExpected=report?.rasterizerOrderedViewsQuerySucceeded === true && report?.rasterizerOrderedViewsSupported === true;
    check(replay.rov_supported === rovExpected,'ROV support disagrees with foundation diagnostics');
    check(replay.rov_tested === (run.paths.rov.indexedChecks > 0),'ROV tested flag disagrees with actual comparisons');
    let originalDifferences=0,originalGuards=0;
    for(const [name,group] of groups) {
      const first=group[0];
      for(const record of group) {
        check(record.original_corrected_differences === first.original_corrected_differences
          && record.original_guard_changed_bytes === first.original_guard_changed_bytes
          && isDeepStrictEqual(record.original_guard_changes,first.original_guard_changes),
          `Original observations differ between GPU paths: ${name}`);
      }
      const keys=group.map(record=>record.path);
      check(new Set(keys).size === keys.length,`Duplicate case/path records: ${name}`);
      if(group.some(record=>record.indexed_checks > 0)) {
        check(group.some(record=>record.path === 'serial' || record.path === 'fallback'),`Serial case missing: ${name}`);
        check(group.some(record=>record.path === 'rov') === rovExpected,`ROV case coverage disagrees with support: ${name}`);
      }
      if(first.original_corrected_differences !== undefined) originalDifferences += first.original_corrected_differences;
      if(first.original_guard_changed_bytes !== undefined) originalGuards += first.original_guard_changed_bytes;
      if(first.original_corrected_differences || first.original_guard_changed_bytes) {
        check(first.legacy_row_copy_quirk_commands > 0,`Unclassified original renderer artifact: ${name}`);
        check(Array.isArray(first.original_guard_changes) && first.original_guard_changes.length === first.original_guard_changed_bytes,`Guard observation count mismatch: ${name}`);
        for(const guard of first.original_guard_changes ?? [])
          check(guard.viewport_byte_offset === -1 && Number.isSafeInteger(guard.command)
            && guard.command >= 0 && guard.command < first.command_count
            && Number.isInteger(guard.before) && guard.before >= 0 && guard.before <= 255
            && Number.isInteger(guard.after) && guard.after >= 0 && guard.after <= 255 && guard.after !== guard.before,
            `Unclassified or invalid original guard observation: ${name}`);
        run.originalQuirkCases.push({case:name,differences:first.original_corrected_differences,
          guardChangedBytes:first.original_guard_changed_bytes,guardChanges:first.original_guard_changes,
          gpuTested:first.path !== 'cpu-empty-maps',artifactStem:first.artifact_stem});
      }
    }
    check(originalDifferences === replay.original_corrected_differences,'Original/corrected aggregate differs from unique cases');
    check(originalGuards === replay.original_guard_changed_bytes,'Original guard aggregate differs from unique cases');
    for(const name of ['captured/full-command-order-flat','captured/full-command-order-patterned']) {
      const group=groups.get(name) ?? [];
      for(const gpuPath of rovExpected ? ['serial','rov'] : ['serial']) {
        const record=group.find(item=>item.path === gpuPath || (gpuPath === 'serial' && item.path === 'fallback'));
        check(!!record && record.passed === true && capture && record.width === capture.width && record.height === capture.height
          && record.indexed_checks === capture.width*capture.height && record.command_count === capture.commands.length,
          `Complete captured viewport case missing or invalid: ${name}/${gpuPath}`);
      }
    }
    check(run.paths.serial.indexedChecks > 0,'No serial GPU comparisons');
  }
  run.passed=run.errors.length === 0;
  if(run.passed) run.qualifiedGpuIndexedChecks=run.observedGpuIndexedChecks;
  return run;
}
export async function replayEvidence(directories) {
  assert.ok(directories.length > 0,'At least one explicit run directory is required');
  const selected=directories.map(directory=>path.resolve(directory));
  assert.equal(new Set(selected).size,selected.length,'Duplicate run directories would inflate comparison counts');
  const runs=[];
  for(const directory of selected) runs.push(await inspectReplayRun(directory));
  return {schema:'kfx-sprite-replay-evidence-1',generatedAt:new Date().toISOString(),
    scope:'Sprites-only frozen command replay on declared backgrounds; corrected original C reference; no full world-frame or live gameplay parity',
    selection:'Every explicitly supplied run retained; no discovery or filtering by success',
    passed:runs.every(run=>run.passed),runCount:runs.length,qualifiedRunCount:runs.filter(run=>run.passed).length,
    qualifiedGpuIndexedChecks:runs.reduce((sum,run)=>sum+run.qualifiedGpuIndexedChecks,0),
    observedGpuIndexedChecks:runs.reduce((sum,run)=>sum+run.observedGpuIndexedChecks,0),
    cpuIndexedChecks:runs.reduce((sum,run)=>sum+run.cpuIndexedChecks,0),runs};
}
if(process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    const args=process.argv.slice(2), index=args.indexOf('--output');
    assert.ok(index > 0 && index === args.length-2 && !args.slice(0,index).some(arg=>arg.startsWith('--')),
      'Usage: sprite-replay-evidence.mjs <run-directory> [...] --output <output.json>');
    const output=path.resolve(args[index+1]);
    const evidence=await replayEvidence(args.slice(0,index));
    await mkdir(path.dirname(output),{recursive:true});
    await writeFile(output,JSON.stringify(evidence,null,2)+'\n');
    console.log(JSON.stringify({output,passed:evidence.passed,runs:evidence.runCount,
      qualifiedRuns:evidence.qualifiedRunCount,qualifiedGpuIndexedChecks:evidence.qualifiedGpuIndexedChecks}));
    process.exitCode=evidence.passed ? 0 : 1;
  } catch(error) { console.error(error.message); process.exitCode=1; }
}
