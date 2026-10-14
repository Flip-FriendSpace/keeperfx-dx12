// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';
import {parseSceneCapture} from './reference-fingerprint.mjs';

// Inspect owned commands after the shared parser has checked their wire bounds,
// identity, camera eligibility and ordering. Metadata alone cannot prove a draw.
export function inspectKeyTrace(bytes, expected, failures, renderer) {
  return inspectParsedKeyTrace(bytes, parseSceneCapture(bytes), expected, failures, renderer);
}

// Separate the assertion logic from wire validation for focused CPU fixtures.
// Production callers must use inspectKeyTrace, which validates the whole file.
export function inspectParsedKeyTrace(bytes, parsed, expected, failures, renderer) {
  const {version, offsets} = parsed;
  assert.ok(['SOFTWARE','OPENGL'].includes(renderer),'Unknown key reference renderer');
  const gl = renderer === 'OPENGL';
  assert.ok(version >= (gl ? 11 : 6),'Key reference format lacks required owned trace');
  assert.equal(bytes[offsets.trace+1],gl ? 1 : 0,'Key reference backend differs from requested renderer');
  if(gl) assert.ok(parsed.glWorld && parsed.composition,'GL key reference requires complete owned world composition');
  const records=(offset,stride,read)=>Array.from({length:bytes.readUInt32LE(offset)},(_,i)=>read(offset+4+i*stride,i));
  const objects=records(offsets.objects,56,p=>({slot:bytes.readUInt32LE(p+8),
    position:[12,16,20].map(o=>bytes.readInt32LE(p+o)), model:bytes.readUInt16LE(p+40),
    owner:bytes[p+50],drawClass:bytes[p+52],frame:bytes[p+53],renderFlags:bytes[p+54]}));
  const keys=objects.filter(o=>o.drawClass===6);
  const entries=records(offsets.entries,5,p=>({bucket:bytes.readInt32LE(p),kind:bytes[p+4]}));
  const draws=records(offsets.draws,327,(p,index)=>({index,slot:bytes.readUInt32LE(p+8),
    entry:bytes.readUInt32LE(p+12),sprite:bytes.readUInt32LE(p+16),flags:bytes.readUInt32LE(p+64),
    alpha:bytes[p+68],blend:bytes[p+70],scale:Array.from({length:10},(_,i)=>bytes.readInt32LE(p+20+i*4))}));
  const commands=records(offsets.commands,5,(p,order)=>({order,kind:bytes[p],index:bytes.readUInt32LE(p+1)}));
  const keyDraws=draws.filter(d=>entries[d.entry].kind===9);
  const keyIndices=new Set(keyDraws.map(d=>d.index));
  const keyCommands=commands.filter(c=>c.kind===0&&keyIndices.has(c.index));
  const check=(ok,message)=>{if(!ok) failures.push(`keyTrace: ${message}`);};
  for(const [name,actual] of [['objects',keys.length],['entries',entries.filter(e=>e.kind===9).length],['draws',keyDraws.length]])
    check(actual===expected[name],`${name}: expected ${expected[name]}, got ${actual}`);
  if(!gl) check(keyCommands.length===keyDraws.length,'Each key draw must occur exactly once in the mixed stream');
  for(const key of keys) {
    check(key.model===44&&key.owner===0,`Unexpected key model/owner for slot ${key.slot}`);
    check(key.frame===expected.frame,`slot ${key.slot}: expected animation frame ${expected.frame}, got ${key.frame}`);
    check((key.renderFlags&0x21)===expected.renderFlags,`slot ${key.slot}: visibility/transparency flags differ`);
    if(expected.draws) check(keyDraws.filter(d=>d.slot===key.slot).length===1,`slot ${key.slot}: expected one actual sprite submission`);
  }
  for(const draw of keyDraws) {
    check(keys.some(k=>k.slot===draw.slot),`draw ${draw.index}: missing captured key identity`);
    if(expected.bucket!==undefined) check(entries[draw.entry].bucket===expected.bucket,`draw ${draw.index}: incorrect key bucket`);
    if(expected.blend!==undefined) check(draw.blend===expected.blend,`draw ${draw.index}: expected blend ${expected.blend}, got ${draw.blend}`);
    check(draw.alpha===0,`draw ${draw.index}: unexpected additive key`);
    check(draw.scale[4]>0&&draw.scale[5]>0,`draw ${draw.index}: empty destination scale`);
  }
  let glExecution;
  if(gl) {
    check(commands.length===0,'GL must not claim a software mixed painter stream');
    const sprites=records(offsets.outlineSprites,82,(p,index)=>({index,draw:bytes.readUInt32LE(p),
      alpha:bytes.readFloatLE(p+44),flags:bytes.readUInt32LE(p+56),additive:bytes[p+62],path:bytes[p+65]}));
    const phases=records(offsets.outlineCommands,9,(p,order)=>({order,kind:bytes[p],sprite:bytes.readUInt32LE(p+1),group:bytes.readUInt32LE(p+5)}));
    const bindings=records(offsets.colourBindings,5,p=>({row:bytes.readUInt32LE(p),resolution:bytes[p+4]}));
    const executed=[];
    for(const draw of keyDraws) {
      const matches=sprites.filter(sprite=>sprite.draw===draw.index);
      check(matches.length===1,`draw ${draw.index}: expected one nonempty actual GL sprite submission`);
      for(const sprite of matches) {
        const main=phases.filter(phase=>phase.sprite===sprite.index&&phase.kind===1);
        check(main.length===1,`draw ${draw.index}: expected exactly one actual GL main phase`);
        check(!sprite.additive&&!phases.some(phase=>phase.sprite===sprite.index&&phase.kind===2),`draw ${draw.index}: unexpected GL glow phase`);
        const alpha=draw.flags&0x8000 ? .75 : draw.flags&4 ? Math.fround(1/3) : draw.flags&8 ? Math.fround(2/3) : 1;
        check(sprite.flags===draw.flags&&sprite.alpha===alpha,`draw ${draw.index}: actual GL flags/opacity differ`);
        if(expected.renderFlags===32) check(sprite.alpha===Math.fround(1/3),`draw ${draw.index}: secret key must use GL one-third source opacity`);
        const binding=bindings[sprite.index];
        check(binding&&binding.row<bytes.readUInt32LE(offsets.colourRows)
          && (sprite.path===2 ? [4,5] : [1,2,3]).includes(binding.resolution),`draw ${draw.index}: missing effective GL colour row/path correspondence`);
        executed.push({...sprite,binding,phases:phases.filter(phase=>phase.sprite===sprite.index)});
      }
    }
    // GL executes its terrain/depth stage before keeper sprites. Preserve the
    // original bucket-entry relation too, without inventing a mixed GL stream.
    const terrain=records(offsets.outlineOccluders,52,p=>({entry:bytes.readUInt32LE(p)}));
    if(expected.terrainBefore) check(keyDraws.length>0&&terrain.some(t=>t.entry<keyDraws[0].entry)
      && parsed.glWorld.terrain>0,'Missing owned terrain before the key entry/stage');
    glExecution={orderScope:'Terrain/depth stage precedes keeper sprite phases; original bucket entry relation retained, not software mixed painter order',sprites:executed};
  } else if(expected.terrainBefore) check(keyCommands.length>0&&commands.some(c=>c.kind===1&&c.order<keyCommands[0].order),'Missing terrain before the key');
  return {version,backend:renderer,keys,keyEntryCount:entries.filter(e=>e.kind===9).length,draws:keyDraws,
    ...(glExecution ? {glExecution} : {}),
    order:keyCommands.map(c=>({...c,entry:draws[c.index].entry,bucket:entries[draws[c.index].entry].bucket,
      previous:commands[c.order-1]??null,next:commands[c.order+1]??null}))};
}
