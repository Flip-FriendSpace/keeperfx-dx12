// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';
import net from 'node:net';
import {setTimeout as delay} from 'node:timers/promises';
import {readFile,writeFile,readdir,stat,rename} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {join,resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {parseSceneCapture,sceneFingerprint} from './reference-fingerprint.mjs';

const [out,readerDir,renderer='OPENGL',pythonExecutable]=process.argv.slice(2);
assert.ok(out&&readerDir&&['OPENGL','DX12'].includes(renderer),'Usage: node indexed-fallback-live-test.mjs output reader-directory [OPENGL|DX12]');
assert.ok(renderer!=='DX12'||pythonExecutable,'DX12 displayed-marker checks require Python with Pillow');
const digest=b=>createHash('sha256').update(b).digest('hex');
const report={schema:1,renderer,passed:false,scope:'Actual custom Lua lens callbacks and immutable last-completed INDEX8 snapshots; DX12 additionally verifies two displayed marker pixels per paused callback, not full-frame parity or snapshot/screenshot sequence pairing',cases:[]};
const deadline=Date.now()+30000;
let socket;
while(!socket&&Date.now()<deadline) {
  try {socket=await new Promise((resolve,reject)=>{
    const candidate=net.createConnection({host:'127.0.0.1',port:5599});
    const timer=setTimeout(()=>{const e=new Error('Startup connection timeout');e.code='ETIMEDOUT';candidate.destroy(e);},Math.min(2000,deadline-Date.now()));
    candidate.once('error',e=>{clearTimeout(timer);candidate.destroy();reject(e);});
    candidate.once('connect',()=>{clearTimeout(timer);resolve(candidate);});
  });} catch(e) {if(!['ECONNREFUSED','ETIMEDOUT'].includes(e.code))throw e;await delay(250);}
}
assert.ok(socket,'API startup exceeded 30 seconds');
let ack=0,pending,buffer='',connectionError,closing=false;
socket.on('error',e=>{connectionError=e;pending?.reject(e);});
socket.on('close',()=>{if(!closing){connectionError??=new Error('Established API connection closed');pending?.reject(connectionError);}});
socket.on('data',data=>{
  buffer+=data.toString('utf8').replaceAll('\0','');let response;
  try{response=JSON.parse(buffer);}catch{return;}buffer='';
  if(response.ack===pending?.ack)pending.resolve(response);
});
async function request(action,fields={}) {
  if(connectionError)throw connectionError;
  const number=++ack;
  const response=await new Promise((resolve,reject)=>{
    const timer=setTimeout(()=>reject(new Error(`API timeout: ${fields.command??action}`)),30000);
    pending={ack:number,resolve:v=>{clearTimeout(timer);resolve(v);},reject:e=>{clearTimeout(timer);reject(e);}};
    socket.write(JSON.stringify({action,ack:number,...fields}));
  });
  assert.equal(response.success,true,JSON.stringify(response));return response.data;
}
const command=command=>request('console_command',{command});
async function captureDX12Presentation(file) {
  const turn=(await request('get_current_game_info')).game_turn;
  await command('scenecheck reference screenshot');
  const path=join(out,'presentation.png'),imageDeadline=Date.now()+30000;let png;
  while(Date.now()<imageDeadline) {
    try {
      const bytes=await readFile(path);
      if(bytes.length>=33&&bytes.subarray(0,8).toString('hex')==='89504e470d0a1a0a'&&bytes.subarray(-12).toString('hex')==='0000000049454e44ae426082'){png=bytes;break;}
    } catch(error) {if(!['ENOENT','EBUSY'].includes(error.code))throw error;}
    await delay(100);
  }
  assert.ok(png,'A complete DX12 presentation was not written within 30 seconds');
  const dimensions=[png.readUInt32BE(16),png.readUInt32BE(20)];
  assert.deepEqual(dimensions,[3840,2160]);
  const turnAfter=(await request('get_current_game_info')).game_turn;
  assert.equal(turnAfter,turn,'Simulation advanced during a paused presentation screenshot');
  if(file!=='presentation.png') {
    const renameDeadline=Date.now()+5000;
    while(true) {
      try {await rename(path,join(out,file));break;}
      catch(error) {if(!['EBUSY','EPERM','EACCES'].includes(error.code)||Date.now()>=renameDeadline)throw error;await delay(100);}
    }
  }
  return {file,dimensions,turnBefore:turn,turnAfter};
}
async function inspectSnapshot(file,recipe,previousSequence) {
  const path=join(out,file);assert.ok((await stat(path)).size<=16778312);
  const b=await readFile(path);
  assert.equal(b.toString('ascii',0,4),'KFXI');assert.equal(b.readUInt32LE(4),1);
  const native=[b.readUInt32LE(8),b.readUInt32LE(12)],viewport=[16,20,24,28].map(p=>b.readUInt32LE(p));
  assert.ok(native.every(n=>n>0&&n<=16384)&&native[0]*native[1]*4<=64*1024*1024);
  assert.ok(viewport[2]>33&&viewport[3]>64&&viewport[0]+viewport[2]<=native[0]&&viewport[1]+viewport[3]<=native[1]);
  assert.equal(b[32],0);assert.equal(b[34],1);assert.equal(b[35],0);assert.equal(b.readUInt32LE(44),0);
  assert.equal(b.readUInt32LE(36),1024);assert.equal(b.readUInt32LE(40),viewport[2]*viewport[3]);
  assert.equal(b.length,1096+viewport[2]*viewport[3]);
  const sequence=b.readBigUInt64LE(48),before=b.readBigUInt64LE(56),after=b.readBigUInt64LE(64);
  assert.ok(sequence>previousSequence);assert.equal(file,`indexed-fallback-${sequence}.kfxidx`);
  assert.ok(before<0xffffffffffffffffn);assert.equal(after,before+1n);
  const palette=b.subarray(72,1096),indices=b.subarray(1096);
  for(let i=3;i<1024;i+=4)assert.equal(palette[i],255);
  const offsets=[64*viewport[2]+32,64*viewport[2]+33];
  assert.deepEqual(offsets.map(p=>indices[p]),recipe.expected,`${recipe.id}: actual output markers`);
  let untouchedDifferingIndices=null;
  if(recipe.partial){untouchedDifferingIndices=0;for(let i=0;i<indices.length;i++)if(!offsets.includes(i)&&indices[i]!==b[33])untouchedDifferingIndices++;assert.equal(untouchedDifferingIndices,0,'True callback must retain untouched destination clear');}
  const readers=[];
  for(const architecture of ['i686','x86_64']) {
    const executable=resolve(readerDir,`indexed-fallback-codec-${architecture}.exe`);
    const result=JSON.parse(execFileSync(executable,['--inspect',resolve(path)],{encoding:'utf8'}));
    assert.equal(result.canonical,true);assert.equal(result.restored,true);assert.deepEqual(result.viewport,viewport);
    assert.deepEqual(result.native,native);assert.equal(result.sequence,String(sequence));
    assert.equal(result.before,String(before));assert.equal(result.after,String(after));
    assert.equal(result.indexBytes,indices.length);assert.equal(result.clearIndex,b[33]);
    readers.push({architecture,executableSha256:digest(await readFile(executable)),...result});
  }
  return {id:recipe.id,callback:recipe.body,file,sha256:digest(b),native,viewport,clearIndex:b[33],sequence:String(sequence),attemptsBefore:String(before),attemptsAfter:String(after),attemptDelta:1,rendererStateRestored:true,paletteSha256:digest(palette),indicesSha256:digest(indices),indexBytes:indices.length,expectedMarkers:recipe.expected,actualMarkers:offsets.map(p=>indices[p]),expectedMarkerRGBA:recipe.expected.map(index=>[...palette.subarray(index*4,index*4+4)]),untouchedDifferingIndices,readers};
}
try {
  report.versionInfo=await request('get_kfx_info');report.levelInfo=await request('get_level_info');
  assert.equal(report.levelInfo.level_number,1);assert.equal(report.levelInfo.is_multiplayer,false);
  await command('scenecheck overhead');await command('scenecheck possession IMP');await delay(600);await command('pause');await delay(300);
  await command('scenecheck reference setup assert(SetActiveLens(0))');
  await command('scenecheck reference clear-ui');await command('scenecheck reference short-walls 0');
  await command('scenecheck reference camera 28800 33408 769 512 0 * 94');
  const recipes=[
    {id:'true-partial',partial:true,expected:[199,200],body:'SetPixel(ctx.srcbuf,32,64,11); SetPixel(ctx.dstbuf,32,64,199); SetPixel(ctx.dstbuf,33,64,200); return true'},
    {id:'false-mutated-source',expected:[123,124],body:'SetPixel(ctx.srcbuf,32,64,123); SetPixel(ctx.srcbuf,33,64,124); SetPixel(ctx.dstbuf,32,64,221); SetPixel(ctx.dstbuf,33,64,222); return false'},
    {id:'error-mutated-source',expected:[125,126],body:"SetPixel(ctx.srcbuf,32,64,125); SetPixel(ctx.srcbuf,33,64,126); SetPixel(ctx.dstbuf,32,64,223); SetPixel(ctx.dstbuf,33,64,224); error('intentional fallback live test')"}
  ];
  let previousSequence=0n;
  for(const recipe of recipes) {
    const name=recipe.id.replaceAll('-','_');
    await command(`scenecheck reference setup assert(SetActiveLens(0)); assert(CreateLens('${name}')); assert(SetLensDrawCallback('${name}',function(ctx) ${recipe.body} end)); assert(SetActiveLens('${name}'))`);
    await delay(400);
    const previous=new Set(await readdir(out));await command('scenecheck reference fallback');
    const file=(await readdir(out)).find(f=>f.endsWith('.kfxidx')&&!previous.has(f));assert.ok(file,'Exporter did not publish a new snapshot');
    const checked=await inspectSnapshot(file,recipe,previousSequence);previousSequence=BigInt(checked.sequence);report.cases.push(checked);
    if(renderer==='DX12') {
      checked.gpuPresentation=await captureDX12Presentation(`${recipe.id}.png`);
      const positions=[[checked.viewport[0]+32,checked.viewport[1]+64],[checked.viewport[0]+33,checked.viewport[1]+64]];
      const pixelReader=fileURLToPath(new URL('./indexed-fallback-pixels.py',import.meta.url));
      const pixels=JSON.parse(execFileSync(pythonExecutable,[pixelReader,resolve(out,checked.gpuPresentation.file),...positions.flat().map(String)],{encoding:'utf8',timeout:10000}));
      assert.deepEqual(pixels.dimensions,checked.native);
      checked.gpuPresentation.markers=positions.map((position,i)=>({position,index:recipe.expected[i],expectedRGBA:checked.expectedMarkerRGBA[i],actualRGBA:pixels.rgba[i]}));
      checked.gpuPresentation.scope='Two exact displayed pixels from the active paused callback; no full-frame parity claim';
      assert.deepEqual(pixels.rgba,checked.expectedMarkerRGBA,`${recipe.id}: actual GPU presentation markers`);
    }
    await writeFile(join(out,'report.json'),JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify({case:recipe.id,sequence:checked.sequence,attemptDelta:1,actualMarkers:checked.actualMarkers}));
  }
  await command('scenecheck reference setup assert(SetActiveLens(0))');await delay(300);
  if(renderer==='DX12') {
    report.normalDX12AfterRestore=await captureDX12Presentation('presentation.png');
  } else {
  const previous=new Set(await readdir(out));await command('scenecheck capture');let file;
  for(let i=0;i<160;i++){const files=await readdir(out);file=files.find(f=>f.endsWith('.kfxscene')&&!previous.has(f)&&files.includes(f+'.png.ready.json')&&files.includes(f+'.json'));if(file)break;await delay(250);}
  assert.ok(file,'Normal GL capture did not resume after disabling custom lens');
  const b=await readFile(join(out,file)),parsed=parseSceneCapture(b);
  assert.equal(parsed.version,11);assert.equal(b[parsed.offsets.trace+1],1);assert.equal(parsed.composition.lens.operation,0);
  report.normalOpenGLAfterRestore={file,sha256:digest(b),...sceneFingerprint(b),backend:1,lensOperation:0};
  }
  report.passed=true;await command('quit');
} catch(e){report.error=e.stack;throw e;}
finally{closing=true;socket.destroy();await writeFile(join(out,'report.json'),JSON.stringify(report,null,2)+'\n');}
