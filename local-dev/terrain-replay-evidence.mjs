// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Consolidate explicitly selected diagnostic runs; this never accepts raster
// comparison, visual review or Gate 2 and never filters out a failed run.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFile,stat,realpath,mkdir,writeFile} from 'node:fs/promises';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {parseSceneCapture} from './reference-fingerprint.mjs';
const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
const shaPattern=/^[a-f0-9]{64}$/i;
const unquote=value=>value?.startsWith('"') && value.endsWith('"') ? value.slice(1,-1) : value;
async function load(file) {
  assert.ok((await stat(file)).size <= 128*1024*1024,`Oversized evidence: ${file}`);
  return readFile(file);
}
async function json(file) {
  const bytes=await load(file);
  return {value:JSON.parse(bytes.toString('utf8').replace(/^\uFEFF/,'')),binding:{path:file,sha256:hash(bytes),bytes:bytes.length}};
}
export async function inspectTerrainRun(directory) {
  const run={directory:path.resolve(directory),passed:false,errors:[],warnings:[],bindings:{},
    observedGpuIndexedChecks:0,qualifiedGpuIndexedChecks:0,comparisonPaths:{},variants:[],artifacts:[],failedCases:[],
    rasterComparisonAccepted:false,gate2Accepted:false};
  const check=(ok,message)=>{if(!ok)run.errors.push(message);};
  let manifest,foundation,replay,pixels;
  const shadowInputs=[],maskDimensions=[];
  for(const [filename,key] of [['manifest.json','manifest'],['report.json','foundation'],['terrain-replay.json','terrainReplay']]) {
    try {
      const file=await json(path.join(run.directory,filename)); run.bindings[key]=file.binding;
      if(key === 'manifest')manifest=file.value;
      else if(key === 'foundation')foundation=file.value;
      else replay=file.value;
    } catch(error) {run.errors.push(`${filename}: ${error.message}`);}
  }
  if(manifest) {
    run.source={revision:manifest.sourceRevision,status:manifest.sourceStatus,timestamp:manifest.timestamp};
    try {
      assert.ok(Array.isArray(manifest.arguments) && manifest.arguments.includes('--terrain-replay'),'Not a terrain replay manifest');
      assert.ok(manifest.arguments.includes('--gpu-validation'),'GPU validation not requested');
      const index=manifest.arguments.indexOf('--scene-capture');
      assert.ok(index >= 0 && typeof manifest.arguments[index+1] === 'string','Missing capture argument');
      const file=path.resolve(unquote(manifest.arguments[index+1])),bytes=await load(file),parsed=parseSceneCapture(bytes);
      assert.ok(parsed.version >= 4 && parsed.version <= 6,'Mixed terrain evidence requires KFXC v4-v6');
      assert.match(manifest.sceneCaptureSha256 ?? '',shaPattern,'Missing original capture hash');
      const sha256=hash(bytes),recordedSha256=manifest.sceneCaptureSha256.toLowerCase();
      run.bindings.capture={path:file,sha256,recordedSha256,bytes:bytes.length,verified:sha256 === recordedSha256};
      check(sha256 === recordedSha256,'Capture differs from manifest SHA256');
      const viewport=24+36+1+12+128*16+1+12+8*4;
      run.capture={width:bytes.readInt32LE(viewport),height:bytes.readInt32LE(viewport+4),
        triangles:bytes.readUInt32LE(parsed.offsets.triangles),sprites:bytes.readUInt32LE(parsed.offsets.draws),
        commands:bytes.readUInt32LE(parsed.offsets.commands),shadows:0,shadowMasks:0,excludedShadowEntries:0};
      if(parsed.version >= 5) {
        run.capture.shadows=bytes.readUInt32LE(parsed.offsets.shadows);
        run.capture.shadowMasks=bytes.readUInt32LE(parsed.offsets.shadowMasks);
        run.capture.excludedShadowEntries=bytes.readUInt32LE(parsed.offsets.excludedShadows);
        let offset=parsed.offsets.shadowMasks+4;
        for(let mask=0;mask<run.capture.shadowMasks;mask++) {
          maskDimensions.push({width:bytes.readUInt16LE(offset),height:bytes.readUInt16LE(offset+2)});
          offset+=8+bytes.readUInt32LE(offset+4);
        }
        for(let shadow=0;shadow<run.capture.shadows;shadow++) {
          const start=parsed.offsets.shadows+4+shadow*81;
          shadowInputs.push({mask:bytes.readUInt32LE(start+16),shade:bytes[start+20],
            vertices:Array.from({length:3},(_,vertex)=>({x:bytes.readInt32LE(start+21+vertex*20),y:bytes.readInt32LE(start+25+vertex*20)}))});
        }
      }
      pixels=run.capture.width*run.capture.height;
      assert.ok(Number.isSafeInteger(pixels) && pixels > 0,'Invalid native viewport');
      if(replay)check(await realpath(file) === await realpath(replay.scene),'Replay capture path differs from manifest');
    } catch(error) {run.errors.push(`Capture binding: ${error.message}`);}
    try {
      assert.match(manifest.executableSha256 ?? '',shaPattern,'Missing recorded executable hash');
      const file=path.resolve(manifest.executablePath ?? path.join(run.directory,'../bin/keeperfx-dx12.exe'));
      const binding={path:file,recordedSha256:manifest.executableSha256.toLowerCase(),verification:'historical_hash_only'};
      run.bindings.executable=binding;
      try {
        binding.currentSha256=hash(await load(file));
        if(binding.currentSha256 === binding.recordedSha256)binding.verification='matching_current_binary';
        else run.warnings.push('Current binary changed; recorded manifest hash remains the historical executable binding');
      } catch(error) {
        if(error.code !== 'ENOENT')throw error;
        run.warnings.push('Historical executable hash retained; executable bytes unavailable');
      }
    } catch(error) {run.errors.push(`Executable binding: ${error.message}`);}
  }
  if(foundation) {
    run.device={adapter:foundation.adapter,debugLayer:foundation.debugLayer,gpuValidation:foundation.gpuValidation,
      validationErrors:foundation.validationErrors,validationWarnings:foundation.validationWarnings,
      rovQuerySucceeded:foundation.rasterizerOrderedViewsQuerySucceeded,rovSupported:foundation.rasterizerOrderedViewsSupported};
    check(foundation.status === 'passed',`Foundation failed: ${foundation.error ?? foundation.status}`);
    check(foundation.debugLayer === true && foundation.gpuValidation === true,'Debug/GPU validation success missing');
    check(foundation.validationErrors === 0 && foundation.validationWarnings === 0,'Zero validation warning/error counts missing');
  }
  run.shadowOperator={recorded:false,passed:null,observedGpuIndexedChecks:0,qualifiedGpuIndexedChecks:0};
  try {
    const loaded=await json(path.join(run.directory,'shadow-operator.json')),operator=loaded.value;
    run.bindings.shadowOperator=loaded.binding;
    const result=run.shadowOperator={recorded:true,passed:false,scope:operator.scope,observedGpuIndexedChecks:0,
      qualifiedGpuIndexedChecks:0,rovVersusSerialChecks:0,caseCount:operator.cases?.length ?? 0,errors:[],failedCases:[],
      countPolicy:'Each serial/ROV record counts its pixels; successful schema-1 ROV cases also assert equality with serial and count those pixels separately'};
    const verify=(ok,message)=>{if(!ok)result.errors.push(message);};
    const rov=foundation?.rasterizerOrderedViewsQuerySucceeded === true && foundation?.rasterizerOrderedViewsSupported === true;
    verify(operator.schema === 1 && operator.passed === true,`Shadow operator failed/unsupported: ${operator.error ?? ''}`);
    const groups=new Map();
    for(const record of operator.cases ?? []) {
      const group=groups.get(record.name) ?? [];group.push(record);groups.set(record.name,group);
      verify(['serial','rov'].includes(record.path),`Unknown operator path: ${record.name}`);
      if(record.pixels !== undefined) {
        verify(Number.isSafeInteger(record.pixels) && record.pixels > 0 && record.differences === 0,`Shadow operator difference/count: ${record.name}/${record.path}`);
        if(Number.isSafeInteger(record.pixels) && record.pixels > 0) {
          result.observedGpuIndexedChecks+=record.pixels;
          if(record.path === 'rov') {result.rovVersusSerialChecks+=record.pixels;result.observedGpuIndexedChecks+=record.pixels;}
        }
        if(record.differences !== 0)result.failedCases.push(record);
      } else verify(record.rejected === true,`Shadow rejection failed: ${record.name}/${record.path}`);
    }
    for(const [name,group] of groups) {
      verify(group.filter(item=>item.path === 'serial').length === 1 && group.filter(item=>item.path === 'rov').length === Number(rov),`Shadow operator path coverage: ${name}`);
    }
    for(let shade=16;shade<=31;shade++)verify(groups.has('all-destinations-shade-'+shade),`Missing destination/shade fixture ${shade}`);
    for(const name of [...Array.from({length:7},(_,i)=>'invalid-shadow-'+i),'nonbinary-mask','sample-outside-initialized-mask',
      'mask-error-survives-opaque-sprite','mask-error-survives-opaque-terrain'])
      verify(groups.has(name) && groups.get(name).every(item=>item.rejected === true),`Missing shadow rejection paths: ${name}`);
    verify(operator.indexed_checks === result.observedGpuIndexedChecks,'Shadow operator count differs from explicit comparisons plus asserted ROV/serial checks');
    result.passed=result.errors.length === 0;
    for(const error of result.errors)run.errors.push('Shadow operator: '+error);
  } catch(error) {
    if(error.code !== 'ENOENT')run.errors.push('Shadow operator report: '+error.message);
  }
  run.shadowEdge={recorded:false,passed:null,observedGpuIndexedChecks:0,qualifiedGpuIndexedChecks:0};
  try {
    const loaded=await json(path.join(run.directory,'shadow-edge.json')),edge=loaded.value;
    run.bindings.shadowEdge=loaded.binding;
    const result=run.shadowEdge={recorded:true,passed:false,scope:'Exact standalone captured-edge regression; no raster/world acceptance',
      observedGpuIndexedChecks:Number.isSafeInteger(edge.indexed_checks) && edge.indexed_checks >= 0 ? edge.indexed_checks : 0,
      qualifiedGpuIndexedChecks:0,report:edge,errors:[],artifacts:[]};
    const verify=(ok,message)=>{if(!ok)result.errors.push(message);};
    const rov=foundation?.rasterizerOrderedViewsSupported === true && foundation?.rasterizerOrderedViewsQuerySucceeded === true;
    verify(edge.schema === 1 && edge.passed === true,`Shadow edge failed/unsupported: ${edge.error ?? ''}`);
    verify(JSON.stringify(edge.viewport) === '[3087,2160]' && JSON.stringify(edge.edge_pixel) === '[1943,745]','Shadow edge viewport/pixel changed');
    verify(JSON.stringify(edge.edge_geometry_uv) === '[255,0,15]' && edge.edge_output === 39 && edge.expected_edge_output === 39,'Shadow edge geometry/UV/fade regression');
    verify(edge.constant_bad_uv_rejected === true,'Shadow edge correction hid genuinely invalid constant UV');
    verify(edge.rov_supported === rov && edge.rov_tested === rov,'Shadow edge ROV support/test evidence disagrees');
    verify(edge.rasterComparisonAccepted === false,'Shadow edge report unexpectedly claims raster acceptance');
    const edgePixels=3087*2160,position=745*3087+1943;
    verify(edge.indexed_checks === edgePixels*(rov ? 5 : 1),'Shadow edge full-image comparison count differs');
    for(const [channel,expected] of [['geometry',255],['u',0],['v',15],['masked-fade',39]]) {
      try {
        const name='shadow-edge-'+channel+'.idx',bytes=await load(path.join(run.directory,name));
        verify(bytes.length === edgePixels,`Native shadow edge index size mismatch: ${name}`);
        verify(bytes[position] === expected,`Native shadow edge sample mismatch: ${name}`);
        const artifact={name,bytes:bytes.length,sha256:hash(bytes)};
        result.artifacts.push(artifact);run.artifacts.push(artifact);
      } catch(error) {result.errors.push('Shadow edge artifact: '+error.message);}
    }
    result.passed=result.errors.length === 0;
    for(const error of result.errors)run.errors.push('Shadow edge: '+error);
  } catch(error) {
    if(error.code !== 'ENOENT')run.errors.push('Shadow edge report: '+error.message);
  }
  if(replay) {
    run.scope=replay.scope;run.comparisonPolicy=replay.comparison_policy;run.spriteRle=replay.sprite_rle;
    run.visualReview=replay.visual_review;run.sourceRasterComparisonAccepted=replay.rasterComparisonAccepted;
    run.quantizationComparison=replay.quantization_comparison ?? null;
    check(replay.schema === 1,'Unsupported terrain evidence schema');
    check(replay.passed === true,`Terrain replay failed: ${replay.error ?? ''}`);
    check(replay.rasterComparisonAccepted === false,'Diagnostic report unexpectedly claims raster acceptance');
    check(typeof replay.visual_review === 'string','Visual review state missing');
    check(typeof replay.scope === 'string' && replay.scope.includes('full-world parity'),'Explicit world-parity exclusion missing');
    const rov=foundation?.rasterizerOrderedViewsSupported === true && foundation?.rasterizerOrderedViewsQuerySucceeded === true;
    check(replay.rov_supported === rov && replay.rov_tested === rov,'ROV support/test state differs from device evidence');
    if(run.capture)check(replay.triangles === run.capture.triangles && replay.sprites === run.capture.sprites
      && replay.gpu_primitives+replay.clipped_sprites === run.capture.commands,'Captured payload/primitive totals disagree');
    if(run.capture)check((replay.shadows ?? 0) === run.capture.shadows && (replay.shadow_masks ?? 0) === run.capture.shadowMasks
      && (replay.excluded_shadow_entries ?? 0) === run.capture.excludedShadowEntries,'Native shadow/mask/excluded counts disagree');
    const cases=Array.isArray(replay.cases) ? replay.cases : [],groups=new Map();
    check(cases.length > 0,'No diagnostic cases');
    for(const record of cases) {
      const group=groups.get(record.name) ?? [];group.push(record);groups.set(record.name,group);
      if(record.exact_differences !== undefined) {
        check(Number.isSafeInteger(record.exact_differences) && record.exact_differences === 0,`Exact GPU comparison failed: ${record.name}/${record.path}`);
        if(record.exact_differences !== 0)run.failedCases.push(record);
        check(['serial','rov','rov-versus-serial'].includes(record.path),`Unknown GPU comparison path: ${record.name}`);
        const count=record.name?.startsWith('synthetic-') ? 32*32 : pixels;
        if(Number.isSafeInteger(count)) {
          run.observedGpuIndexedChecks+=count;
          run.comparisonPaths[record.path]=(run.comparisonPaths[record.path] ?? 0)+count;
        }
      } else if(record.name?.startsWith('shadow-bounds-failure-')) {
        run.shadowBoundsFailures ??= [];run.shadowBoundsFailures.push(record);run.failedCases.push(record);
        check(false,'Captured shadow sampled outside initialized mask: '+record.name);
      } else if(record.name === 'shadow-selection') {
        run.shadowSelection=record;
        const candidates=[];
        for(let index=0;index<shadowInputs.length;index++) {
          const v=shadowInputs[index].vertices,area=(v[1].x-v[0].x)*(v[2].y-v[0].y)-(v[1].y-v[0].y)*(v[2].x-v[0].x);
          if(area > 0 && Math.max(0,Math.min(...v.map(p=>p.x))) < Math.min(run.capture.width,Math.max(...v.map(p=>p.x)))
            && Math.max(0,Math.min(...v.map(p=>p.y))) < Math.min(run.capture.height,Math.max(...v.map(p=>p.y))))candidates.push(index);
        }
        const selected=candidates.length ? [...new Set([candidates[0],candidates[Math.floor(candidates.length/2)],candidates.at(-1)])] : [];
        check(record.candidates === candidates.length && record.selected === selected.length,'Shadow selection differs from captured input geometry');
        run.shadowSelection.expectedIndices=selected;
      } else if(record.name?.startsWith('invalid-triangle-'))check(record.rejected === true,`Malformed input accepted: ${record.name}`);
      else if(record.original_modern_differences !== undefined || record.index_differences !== undefined) {
        const variant=record.vertex_attributes ?? 'raw-fraction';
        let group=run.variants.find(item=>item.name === variant);
        if(!group) {group={name:variant,rasterComparisonAccepted:false,comparisons:[]};run.variants.push(group);}
        group.comparisons.push(record);
        check(record.rasterComparisonAccepted === false,`Comparison unexpectedly accepted: ${record.name}`);
        if(record.same_samples_unexplained_index_differences !== undefined)
          check(record.same_samples_unexplained_index_differences === 0,`Unexplained palette differences: ${record.name}`);
        if(/^shadow-\d+$/.test(record.name)) {
          const input=shadowInputs[Number(record.name.slice(7))],mask=input && maskDimensions[input.mask];
          check(input && record.mask === input.mask && record.shade === input.shade && record.mask_width === mask.width
            && record.mask_height === mask.height,'Shadow diagnostic state differs from native mask/shade: '+record.name);
        }
      } else check(false,`Unknown diagnostic case record: ${record.name}`);
    }
    for(const [name,group] of groups) {
      const paths=group.filter(item=>item.path).map(item=>item.path);
      check(new Set(paths).size === paths.length,`Duplicate case/path: ${name}`);
      if(group.some(item=>item.exact_differences !== undefined)) {
        check(group.some(item=>item.path === 'rov-versus-serial') === rov,`ROV/serial comparison missing or unexpected: ${name}`);
        if(group.some(item=>item.path === 'serial'))check(group.some(item=>item.path === 'rov') === rov,`ROV exact fixture missing or unexpected: ${name}`);
      }
    }
    check(replay.indexed_checks === run.observedGpuIndexedChecks,'GPU comparison count differs from native-viewport/synthetic case counts');
    if(run.capture?.shadows)check(!!run.shadowSelection,'Captured shadows lack input-based selection evidence');
    if(run.shadowSelection) {
      const actual=run.variants.flatMap(variant=>variant.comparisons.filter(item=>/^shadow-\d+$/.test(item.name)).map(item=>Number(item.name.slice(7))));
      check(JSON.stringify(actual) === JSON.stringify(run.shadowSelection.expectedIndices),'Isolated shadow cases differ from geometry-based selection');
    }
    const declared=replay.vertex_attribute_variants ?? ['raw-fraction'];
    check(JSON.stringify(declared) === JSON.stringify(run.variants.map(item=>item.name)),'Declared vertex variants differ from retained comparisons');
    const required=new Set();
    for(const variant of run.variants) {
      const prefix=variant.name === 'vertex-quantized' ? 'vertex-quantized-' : '';
      for(const background of ['flat','pattern']) {
        const name=prefix+'captured-'+background;
        check(variant.comparisons.some(item=>item.name === name),`Full captured comparison missing: ${name}`);
        for(const suffix of ['modern','original','corrected-sprites'])for(const extension of ['idx','bmp'])required.add(`${name}-${suffix}.${extension}`);
        for(const suffix of ['original-modern','corrected-modern','original-corrected'])required.add(`${name}-${suffix}-diff.bmp`);
      }
      for(const comparison of variant.comparisons.filter(item=>item.name.includes('triangle-'))) {
        const name=comparison.name;
        for(const suffix of ['modern','original'])for(const extension of ['idx','bmp'])required.add(`${name}-${suffix}.${extension}`);
        required.add(`${name}-index-diff.bmp`);
        for(const channel of ['coverage','u','v','shade']) {
          for(const suffix of ['modern','original'])required.add(`${name}-${suffix}-${channel}.idx`);
          required.add(`${name}-${channel}-diff.bmp`);
        }
      }
      for(const comparison of variant.comparisons.filter(item=>/^shadow-\d+$/.test(item.name))) {
        const name=comparison.name;
        for(const suffix of ['modern','original'])for(const extension of ['idx','bmp'])required.add(`${name}-${suffix}.${extension}`);
        required.add(`${name}-index-diff.bmp`);
        for(const channel of ['geometric-coverage','u','v','masked-coverage']) {
          for(const suffix of ['modern','original'])required.add(`${name}-${suffix}-${channel}.idx`);
          required.add(`${name}-${channel}-diff.bmp`);
        }
      }
    }
    for(const name of required) {
      try {
        const file=path.join(run.directory,name),bytes=await load(file);
        if(name.endsWith('.idx'))assert.equal(bytes.length,pixels,`Native index size mismatch: ${name}`);
        else {
          assert.equal(bytes.toString('ascii',0,2),'BM',`Not a retained BMP: ${name}`);
          assert.equal(bytes.readInt32LE(18),run.capture?.width,`BMP viewport width mismatch: ${name}`);
          assert.equal(Math.abs(bytes.readInt32LE(22)),run.capture?.height,`BMP viewport height mismatch: ${name}`);
        }
        run.artifacts.push({name,bytes:bytes.length,sha256:hash(bytes)});
      } catch(error) {run.errors.push(`Artifact: ${error.message}`);}
    }
  }
  run.passed=run.errors.length === 0;
  if(run.passed)run.qualifiedGpuIndexedChecks=run.observedGpuIndexedChecks;
  if(run.passed && run.shadowOperator.passed)run.shadowOperator.qualifiedGpuIndexedChecks=run.shadowOperator.observedGpuIndexedChecks;
  if(run.passed && run.shadowEdge.passed)run.shadowEdge.qualifiedGpuIndexedChecks=run.shadowEdge.observedGpuIndexedChecks;
  return run;
}
export async function terrainEvidence(directories) {
  assert.ok(directories.length > 0,'Explicit run directories required');
  const selected=directories.map(item=>path.resolve(item));
  assert.equal(new Set(selected).size,selected.length,'Duplicate directories would inflate comparison counts');
  const runs=[];for(const directory of selected)runs.push(await inspectTerrainRun(directory));
  return {schema:'kfx-terrain-replay-evidence-1',generatedAt:new Date().toISOString(),passed:runs.every(run=>run.passed),
    scope:'Diagnostic compositor checks and retained original-C/hardware raster differences; no visual or Gate 2 acceptance',
    rasterComparisonAccepted:false,gate2Accepted:false,visualReviewAcceptancePerformed:false,
    runCount:runs.length,qualifiedRunCount:runs.filter(run=>run.passed).length,
    observedGpuIndexedChecks:runs.reduce((sum,run)=>sum+run.observedGpuIndexedChecks,0),
    qualifiedGpuIndexedChecks:runs.reduce((sum,run)=>sum+run.qualifiedGpuIndexedChecks,0),
    observedShadowOperatorGpuIndexedChecks:runs.reduce((sum,run)=>sum+run.shadowOperator.observedGpuIndexedChecks,0),
    qualifiedShadowOperatorGpuIndexedChecks:runs.reduce((sum,run)=>sum+run.shadowOperator.qualifiedGpuIndexedChecks,0),
    observedShadowEdgeGpuIndexedChecks:runs.reduce((sum,run)=>sum+run.shadowEdge.observedGpuIndexedChecks,0),
    qualifiedShadowEdgeGpuIndexedChecks:runs.reduce((sum,run)=>sum+run.shadowEdge.qualifiedGpuIndexedChecks,0),runs};
}
if(process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    const args=process.argv.slice(2),index=args.indexOf('--output');
    assert.ok(index > 0 && index === args.length-2 && !args.slice(0,index).some(item=>item.startsWith('--')),
      'Usage: terrain-replay-evidence.mjs <run-directory> [...] --output <output.json>');
    const output=path.resolve(args[index+1]),evidence=await terrainEvidence(args.slice(0,index));
    await mkdir(path.dirname(output),{recursive:true});await writeFile(output,JSON.stringify(evidence,null,2)+'\n');
    console.log(JSON.stringify({output,passed:evidence.passed,runs:evidence.runCount,qualifiedRuns:evidence.qualifiedRunCount,
      qualifiedGpuIndexedChecks:evidence.qualifiedGpuIndexedChecks,qualifiedShadowOperatorGpuIndexedChecks:evidence.qualifiedShadowOperatorGpuIndexedChecks,
      qualifiedShadowEdgeGpuIndexedChecks:evidence.qualifiedShadowEdgeGpuIndexedChecks,
      rasterComparisonAccepted:false,gate2Accepted:false}));
    process.exitCode=evidence.passed ? 0 : 1;
  } catch(error) {console.error(error.message);process.exitCode=1;}
}
