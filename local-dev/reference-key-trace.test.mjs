// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';
import {test} from 'node:test';
import {inspectParsedKeyTrace} from './reference-key-trace.mjs';

// These exercise reference assertions, not the separately tested wire decoder.
function fixture({gl=true,secret=false,suppressed=false,version=gl?11:8}={}) {
  const bytes=Buffer.alloc(4096),offsets={trace:0};let cursor=8;
  bytes[1]=Number(gl);
  function records(name,count,stride) {
    offsets[name]=cursor;bytes.writeUInt32LE(count,cursor);cursor+=4;
    const p=cursor;cursor+=count*stride;return p;
  }
  let p=records('objects',1,56);
  bytes.writeUInt32LE(7,p+8);bytes.writeUInt16LE(44,p+40);bytes[p+52]=6;bytes[p+54]=secret?32:0;
  p=records('entries',2,5);bytes.writeInt32LE(20,p);bytes[p+4]=0;bytes.writeInt32LE(1,p+5);bytes[p+9]=9;
  p=records('draws',suppressed?0:1,327);
  if(!suppressed){bytes.writeUInt32LE(7,p+8);bytes.writeUInt32LE(1,p+12);bytes.writeInt32LE(17,p+36);bytes.writeInt32LE(19,p+40);bytes.writeUInt32LE(secret?4:0,p+64);bytes[p+70]=secret?2:0;}
  p=records('commands',gl?0:suppressed?1:2,5);
  if(!gl){bytes[p]=1;if(!suppressed){bytes[p+5]=0;bytes.writeUInt32LE(0,p+6);}}
  p=records('outlineOccluders',gl?1:0,52);
  p=records('outlineSprites',gl&&!suppressed?1:0,82);
  if(gl&&!suppressed){bytes.writeFloatLE(secret?Math.fround(1/3):1,p+44);bytes.writeUInt32LE(secret?4:0,p+56);}
  p=records('outlineCommands',gl&&!suppressed?1:0,9);if(gl&&!suppressed)bytes[p]=1;
  records('colourRows',gl&&!suppressed?1:0,1024);
  p=records('colourBindings',gl&&!suppressed?1:0,5);if(gl&&!suppressed)bytes[p+4]=1;
  const expected={objects:1,entries:1,draws:suppressed?0:1,frame:0,renderFlags:secret?32:0,
    ...(suppressed?{}:{bucket:1,terrainBefore:true}),...(secret?{blend:2}:{})};
  return {bytes,parsed:{version,offsets,...(gl?{glWorld:{terrain:1},composition:{}}:{})},expected,renderer:gl?'OPENGL':'SOFTWARE'};
}
function inspect(f) {const failures=[];const result=inspectParsedKeyTrace(f.bytes,f.parsed,f.expected,failures,f.renderer);return {failures,result};}
for(const [name,options] of Object.entries({softwareV6:{gl:false,version:6},softwareV8:{gl:false},softwareSecret:{gl:false,secret:true},softwareSuppressed:{gl:false,suppressed:true},glOpaque:{},glSecret:{secret:true},glSuppressed:{suppressed:true}}))
  test(name,()=>assert.deepEqual(inspect(fixture(options)).failures,[]));
for(const [name,change,pattern] of [
  ['missing GL sprite',f=>f.bytes.writeUInt32LE(0,f.parsed.offsets.outlineSprites),/actual GL sprite/],
  ['wrong draw binding',f=>f.bytes.writeUInt32LE(1,f.parsed.offsets.outlineSprites+4),/actual GL sprite/],
  ['missing main phase',f=>f.bytes[f.parsed.offsets.outlineCommands+4]=0,/main phase/],
  ['glow phase',f=>f.bytes[f.parsed.offsets.outlineCommands+4]=2,/glow phase/],
  ['wrong opacity',f=>f.bytes.writeFloatLE(.5,f.parsed.offsets.outlineSprites+4+44),/opacity/],
  ['missing effective row',f=>f.bytes.writeUInt32LE(1,f.parsed.offsets.colourBindings+4),/effective GL colour/],
  ['wrong fallback binding',f=>f.bytes[f.parsed.offsets.colourBindings+8]=4,/effective GL colour/],
  ['missing terrain stage',f=>f.parsed.glWorld.terrain=0,/terrain/],
  ['terrain follows original key entry',f=>f.bytes.writeUInt32LE(1,f.parsed.offsets.outlineOccluders+4),/terrain/],
  ['suppression unexpectedly draws',f=>f.expected.draws=0,/draws/],
  ['software missing mixed main',f=>f.bytes.writeUInt32LE(1,f.parsed.offsets.commands),/mixed stream/],
]) test(name,()=>{const f=fixture({gl:!name.startsWith('software'),secret:true});change(f);assert.ok(inspect(f).failures.some(message=>pattern.test(message)));});
test('reject wrong backend and obsolete GL format',()=>{
  const f=fixture();f.bytes[1]=0;assert.throws(()=>inspect(f),/backend/);
  f.bytes[1]=1;f.parsed.version=9;assert.throws(()=>inspect(f),/format/);
  f.parsed.version=11;delete f.parsed.composition;assert.throws(()=>inspect(f),/composition/);
});
test('GL execution report distinguishes stage order',()=>{
  const {result}=inspect(fixture({secret:true}));
  assert.equal(result.glExecution.sprites[0].alpha,Math.fround(1/3));
  assert.match(result.glExecution.orderScope,/not software mixed painter order/);
  assert.deepEqual(result.order,[]);
});
