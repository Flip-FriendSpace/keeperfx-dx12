// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Frozen input identity, independent of the settings-dependent v3 draw trace.
// This parses the codec's wire layout and checks bounds/CRC, not engine semantics.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFile, stat} from 'node:fs/promises';
import {pathToFileURL} from 'node:url';

const maxFile = 128 * 1024 * 1024;
export const frozenSchemes = Object.freeze({2:'kfxc-v2-frozen-1', 3:'kfxc-v3-scene-assets-1',4:'kfxc-v4-scene-assets-1',5:'kfxc-v5-scene-assets-1',6:'kfxc-v6-scene-assets-1',7:'kfxc-v7-scene-assets-1',8:'kfxc-v8-scene-assets-target-1',9:'kfxc-v9-scene-assets-colour-1',10:'kfxc-v10-scene-assets-colour-1',11:'kfxc-v10-scene-assets-colour-1'});
const worldResourceScheme='kfxc-v10-effective-world-resources-1',worldStateScheme='kfxc-v10-applied-world-state-1';
const compositionResourceScheme='kfxc-v11-composition-resources-1',compositionStateScheme='kfxc-v11-composition-state-1';
const crcTable = Uint32Array.from({length:256}, (_, value) => {
  for (let bit=0;bit<8;bit++) value = (value >>> 1) ^ ((value & 1) ? 0xedb88320 : 0);
  return value >>> 0;
});
export function captureCrc(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) crc = (crc >>> 8) ^ crcTable[(crc ^ byte) & 255];
  return (crc ^ 0xffffffff) >>> 0;
}
class Reader {
  constructor(bytes) { this.bytes=bytes; this.position=24; }
  skip(size) {
    assert.ok(Number.isSafeInteger(size) && size >= 0 && size <= this.bytes.length-this.position, 'Truncated KFXC field');
    this.position += size;
  }
  u8() { this.skip(1); return this.bytes[this.position-1]; }
  bool() { const value=this.u8(); assert.ok(value <= 1, 'Noncanonical KFXC bool'); return value; }
  u32() { this.skip(4); return this.bytes.readUInt32LE(this.position-4); }
  u16() { this.skip(2); return this.bytes.readUInt16LE(this.position-2); }
  u64() { this.skip(8); return this.bytes.readBigUInt64LE(this.position-8); }
  i32() { this.skip(4); return this.bytes.readInt32LE(this.position-4); }
  f32() { this.skip(4); return this.bytes.readFloatLE(this.position-4); }
  count(cap, stride=1) {
    const count=this.u32();
    assert.ok(count <= cap && count*stride <= this.bytes.length-this.position, 'Invalid KFXC vector length');
    return count;
  }
  scalars(cap, stride) { const count=this.count(cap,stride); this.skip(count*stride); return count; }
  records(cap, stride, boolOffsets=[]) {
    const count=this.count(cap,stride);
    for(let index=0;index<count;index++) {
      for(const offset of boolOffsets) assert.ok(this.bytes[this.position+offset] <= 1, 'Noncanonical KFXC bool');
      this.skip(stride);
    }
    return count;
  }
}
function outlineAxis(origin,src,dst,window,offset,count) {
  assert.ok(src > 0 && src <= 65535 && dst >= 0 && dst <= 16777216 && window > 0 && window <= 16384
    && Math.abs(origin) <= 16777216 && Math.abs(offset) <= 65535,'Invalid outline scale');
  const empty=[0,0,0,1];
  if(!dst)return empty;
  const step=BigInt(dst)*65536n/BigInt(src);
  assert.ok(step <= 2147483647n,'Outline sampling step overflow');
  if(!step)return empty;
  const base=step/2n+BigInt(origin)*65536n+BigInt(offset)*step;
  let start=base>>16n; const end=(base+BigInt(count)*step)>>16n;
  if(!offset && (origin < 0 || dst+src+origin >= window))start=start < BigInt(Math.max(origin,0)) ? start : BigInt(Math.max(origin,0));
  if(end <= start)return empty;
  const result=[start,end-start,base-start*65536n,step];
  assert.ok(result.every(value=>value >= -2147483648n && value <= 2147483647n),'Outline sampling coordinate overflow');
  return result.map(Number);
}
function parseOutline(r,offsets,backend,cameraView,kinds,buckets,draws,sources) {
  offsets.outline=r.position;
  const present=r.bool();
  if(!present) { assert.equal(backend,0,'GL v7 requires owned outline trace'); return; }
  assert.equal(backend,1,'Outline extension requires GL backend');
  assert.equal(r.bool(),1,'Incomplete outline trace'); const mode=r.u8(),alpha=r.f32();
  assert.ok(mode <= 2 && Number.isFinite(alpha) && alpha >= 0 && alpha <= 1,'Invalid outline mode/alpha');
  for(let i=0;i<9;i++) {const value=r.f32();assert.ok(Number.isFinite(value) && Math.abs(value) <= 1024,'Invalid outline palette transform');}
  offsets.outlineOccluders=r.position;
  const occluders=r.count(262144,52),counts=new Uint32Array(kinds.length); let previousEntry=0;
  const f=Math.fround;
  for(let i=0;i<occluders;i++) {
    const entry=r.u32();assert.ok(entry < kinds.length && entry >= previousEntry && [0,1,6].includes(kinds[entry]),'Invalid outline occluder entry');previousEntry=entry;
    const depths=[f(f(2*buckets[entry]/4097)-1),f(2*buckets[entry]/4097-1)];
    for(let j=0;j<3;j++) {
      const x=r.f32(),y=r.f32(),z=r.f32(),w=r.f32();
      assert.ok(Number.isFinite(x) && Number.isFinite(y) && Math.abs(x) <= 33554432 && Math.abs(y) <= 33554432
        && depths.includes(z) && Number.isFinite(w) && Number.isInteger(w) && w >= 1 && w <= 2147483648,'Invalid outline occluder vertex/depth/W');
    }
    counts[entry]++;
  }
  for(let i=0;i<kinds.length;i++)if([0,1,6].includes(kinds[i]))assert.equal(counts[i],kinds[i] === 6 ? 2 : 1,'Missing outline terrain occluder');
  const mappings=draws.map(draw=>{
    const source=sources[draw.sprite],s=draw.scale;
    assert.ok(source.width <= 256 && source.height <= 256 && draw.contentHeight > 0 && draw.contentHeight <= source.height,'Invalid outline source dimensions/clip');
    return [outlineAxis(s[0],s[2],s[4],s[8],s[6],source.width),outlineAxis(s[1],s[3],s[5],s[9],s[7],draw.contentHeight)];
  });
  offsets.outlineSprites=r.position;
  const spriteCount=r.count(65536,82),states=[],seen=new Uint8Array(draws.length);let previousDraw=0;
  for(let i=0;i<spriteCount;i++) {
    const drawIndex=r.u32(),sampling=Array.from({length:8},()=>r.i32()),z=r.f32(),outlineZ=r.f32(),sourceAlpha=r.f32();
    r.skip(8);const sort=r.bytes.readBigUInt64LE(r.position-8),flags=r.u32();r.skip(1);const owner=r.bytes.readInt8(r.position-1);
    const wants=r.bool(),additive=r.bool(),darkens=r.bool(),border=r.bool(),path=r.u8(),rgba=Array.from({length:4},()=>r.f32());
    assert.ok(drawIndex < draws.length && drawIndex >= previousDraw && !seen[drawIndex]++ && path <= 2,'Invalid outline sprite reference/order/path');previousDraw=drawIndex;
    const draw=draws[drawIndex],source=sources[draw.sprite],[x,y]=mappings[drawIndex];
    assert.ok(x[1] > 0 && y[1] > 0,'Outline references empty mapping');
    assert.deepEqual(sampling,[x[0],y[0],x[1],y[1],x[2],y[2],x[3],y[3]],'Outline sampling differs from owned draw');
    const depths=[f(f((2*buckets[draw.entry]-1)/4097)-1),f((2*buckets[draw.entry]-1)/4097-1)];
    const expectedAlpha=draw.flags&0x8000 ? .75 : draw.flags&4 ? f(1/3) : draw.flags&8 ? f(2/3) : 1;
    assert.ok(flags === draw.flags && depths.includes(z) && [f(z+f(.002)),f(z+.002)].includes(outlineZ) && sourceAlpha === expectedAlpha
      && sort>>32n === BigInt(buckets[draw.entry]) && owner === draw.owner && additive === Number(!!(flags&0x4000))
      && (!darkens || additive) && (!wants || cameraView !== 2),'Invalid outline sprite ownership/depth/alpha/state');
    let expectedBorder=false,glow=false;
    for(let p=0;p<source.width*source.height;p++)if(source.pixels[p*2+1]) {
      if(p%source.width === source.width-1 || Math.floor(p/source.width) === source.height-1)expectedBorder=true;
      if(source.pixels[p*2] >= 50 && source.pixels[p*2] <= 56)glow=true;
    }
    assert.equal(border,Number(expectedBorder),'Incorrect full-source border coverage');
    if(additive)assert.equal(darkens,Number(path ? true : glow),'Incorrect glow darken eligibility');
    const outlined=!!(mode && wants && !additive);
    assert.ok(rgba.every(value=>Number.isFinite(value) && value >= 0 && value <= 1 && (outlined || value === 0))
      && (!outlined || rgba[3] === alpha),'Invalid outline owner RGBA');
    states.push({draw:drawIndex,path,outlined,darkens,additive});
  }
  for(let i=0;i<draws.length;i++)assert.equal(!!seen[i],mappings[i][0][1] > 0 && mappings[i][1][1] > 0,'Missing outline sprite mapping');
  offsets.outlineCommands=r.position;
  const commands=r.count(196608,9),phases=states.map(()=>[0,0,0]),groups=new Map();
  let previous,group=-1,groupSprite,path,mainPhase=false,previousMain=0,previousOutline=0;
  for(let i=0;i<commands;i++) {
    const kind=r.u8(),sprite=r.u32(),nextGroup=r.u32();assert.ok(kind <= 2 && sprite < states.length,'Invalid outline command reference');
    const state=states[sprite];assert.ok(!groups.has(sprite) || groups.get(sprite) === nextGroup,'Sprite phases crossed flush groups');groups.set(sprite,nextGroup);
    if(nextGroup !== group) {assert.ok(nextGroup > group,'Outline flush groups regressed');group=nextGroup;groupSprite=sprite;path=state.path;mainPhase=false;}
    assert.ok(path === state.path && (!path || sprite === groupSprite),'Invalid fallback/instanced group ownership');
    assert.equal(phases[sprite][kind]++,0,'Duplicate outline phase');
    if(kind === 0) {assert.ok(!mainPhase && state.outlined && state.draw >= previousOutline,'Invalid outline phase/order');previousOutline=state.draw;}
    else if(kind === 1) {assert.ok(state.draw >= previousMain,'Main sprite order regressed');previousMain=state.draw;mainPhase=true;}
    else assert.ok(state.darkens && previous?.kind === 1 && previous.sprite === sprite && previous.group === group,'Glow darken must follow its main sprite');
    previous={kind,sprite,group};
  }
  for(let i=0;i<states.length;i++)assert.deepEqual(phases[i],[Number(states[i].outlined),1,states[i].darkens],'Missing required outline phase');
  return states;
}
function parseOutlineColours(r,offsets,states,draws) {
  offsets.colourRows=r.position;
  const count=r.count(16384,1024),rows=[],unique=new Set();
  for(let i=0;i<count;i++) {
    const row=r.bytes.subarray(r.position,r.position+1024);r.skip(1024);const key=hashRow(row);
    assert.ok(!unique.has(key),'Duplicate effective colour row');unique.add(key);
    rows.push({opaque:row.every((value,index)=>index%4 !== 3 || value === 255),zero:row.every(value=>value === 0)});
  }
  offsets.colourBindings=r.position;
  const bindings=r.count(65536,5);assert.equal(bindings,states.length,'Effective colour bindings must parallel outline sprites');
  let used=0;
  for(let i=0;i<bindings;i++) {
    const row=r.u32(),resolution=r.u8(),state=states[i],draw=draws[state.draw];
    assert.equal(draw.hasRemap,Number(!!(draw.flags&0x0800)),'Requested remap flag/presence disagree');
    if(state.additive) {assert.ok(resolution === 0 && row === 0xffffffff,'Glow must bypass effective CLUT rows');continue;}
    assert.ok(row < count && row <= used,'Invalid effective colour row reference/first use');if(row === used)used++;
    assert.ok(state.path === 2 ? resolution === (draw.hasRemap ? 5 : 4) : draw.hasRemap ? [2,3].includes(resolution) : resolution === 1,
      'Effective colour resolution contradicts path/requested remap');
    assert.ok(rows[row].opaque || ([1,3].includes(resolution)&&rows[row].zero),'Invalid effective CLUT/texture alpha');
  }
  assert.equal(used,count,'Unused effective colour row');
}
// Full bytes, rather than palette-derived values, define row deduplication.
const hashRow=row=>row.toString('hex');
function parseGLWorld(r,offsets,backend,cameraView,kinds,buckets,objects,ownedBytes) {
  offsets.glWorld=r.position;
  assert.equal(r.bool(),1,'V10 requires an owned GL world pass');assert.equal(r.bool(),1,'Incomplete GL world pass');
  const targetKind=r.u8(),width=r.u32(),height=r.u32(),window=Array.from({length:4},()=>r.i32());
  assert.ok(backend === 1 && targetKind <= 1 && (targetKind === 0 || cameraView === 2),'Invalid GL world target/backend');
  assert.ok(width > 0 && height > 0 && width <= 16384 && height <= 16384 && width*height*4 <= 64*1024*1024,'Invalid GL native target bounds');
  const [x,y,w,h]=window;
  assert.ok(x >= 0 && y >= 0 && w > 0 && h > 0 && x+w <= width && y+h <= height,'GL viewport outside target');
  assert.deepEqual(window,Array.from({length:4},(_,i)=>r.bytes.readInt32LE(offsets.camera+37+i*4)),'GL viewport differs from captured camera');
  const clear=Array.from({length:4},()=>r.f32());
  assert.ok(clear.every(value=>Number.isFinite(value)&&value >= 0&&value <= 1)&&clear[3] === 1,'Invalid actual GL clear colour');
  offsets.worldPalette=r.position;const palette=r.bytes.subarray(r.position,r.position+1024);r.skip(1024);
  assert.ok(palette.every((value,index)=>index%4 !== 3 || value === 255),'Invalid applied GL palette alpha');
  offsets.worldFade=r.position;r.skip(16384);offsets.worldSettings=r.position;
  const settings=Array.from({length:9},()=>r.f32()),time=r.f32(),f=Math.fround;
  assert.ok(settings.every(Number.isFinite)&&[0,1].includes(settings[0])&&settings[1] >= 0&&settings[1] <= 1
    &&settings[2] >= f(.1)&&settings[2] <= 3&&settings[3] >= f(.1)&&settings[3] <= 3
    &&[0,1].includes(settings[4])&&[0,1,2].includes(settings[5])&&settings[6] >= f(.1)&&settings[6] <= 5
    &&settings[7] >= 0&&settings[7] <= 1&&[0,1].includes(settings[8])&&time === 0,'Invalid applied GL shade settings/time');
  offsets.worldLightmap=r.position;assert.equal(r.scalars(511*511,2),511*511,'Invalid applied lightmap length');
  offsets.worldTiles=r.position;const tileCount=r.count(65536,1024),tiles=new Set();
  for(let i=0;i<tileCount;i++) {
    const key=hashRow(r.bytes.subarray(r.position,r.position+1024));r.skip(1024);
    assert.ok(!tiles.has(key),'Duplicate GL tile version');tiles.add(key);
  }
  offsets.worldTerrain=r.position;const terrainCount=r.count(262144,64);
  assert.equal(terrainCount,r.bytes.readUInt32LE(offsets.outlineOccluders),'GL terrain must parallel owned occluders');let usedTiles=0;
  for(let i=0;i<terrainCount;i++) {
    const tile=r.u32();
    if(tile !== 0xffffffff) {assert.ok(tile < tileCount&&tile <= usedTiles,'Invalid GL tile reference/first use');if(tile === usedTiles)usedTiles++;}
    for(let vertex=0;vertex<3;vertex++) {
      const u=r.f32(),v=r.f32(),shade=r.f32(),stlX=r.f32(),stlY=r.f32();
      assert.ok([u,v,shade,stlX,stlY].every(Number.isFinite)&&Math.abs(u) <= 32768&&Math.abs(v) <= 32768&&Math.abs(shade) <= 1024
        &&stlX >= -1&&stlY >= -1&&stlX <= 511&&stlY <= 511,'Invalid GL terrain sampling/shade/lightmap coordinate');
    }
  }
  assert.equal(usedTiles,tileCount,'Unused GL tile version');
  offsets.worldShadowSources=r.position;const sourceCount=r.count(65536,8),sources=[],unique=new Set();let maskBytes=0;
  for(let i=0;i<sourceCount;i++) {
    const start=r.position;r.skip(4);const width=r.bytes.readUInt16LE(start),height=r.bytes.readUInt16LE(start+2),length=r.count(65536);
    assert.ok(width > 0&&height > 0&&width <= 256&&height <= 256&&length === width*height,'Invalid GL shadow source dimensions/length');
    const coverage=r.bytes.subarray(r.position,r.position+length);r.skip(length);maskBytes+=length;
    assert.ok(coverage.every(value=>value === 0||value === 255),'Nonbinary GL shadow source coverage');
    const key=width+','+height+','+hashRow(coverage);assert.ok(!unique.has(key),'Duplicate GL shadow source');unique.add(key);sources.push({width,height});
  }
  const resourceBytes=ownedBytes+r.bytes.readUInt32LE(offsets.colourRows)*1024+1024+16384+511*511*2+tileCount*1024+maskBytes;
  assert.ok(resourceBytes <= 64*1024*1024,'Combined GL owned assets exceed 64 MiB');
  offsets.worldShadows=r.position;const shadows=r.count(65536,92),seen=new Set();let previousEntry=0,usedSources=0;
  for(let i=0;i<shadows;i++) {
    const incarnation=r.bytes.readBigUInt64LE(r.position);r.skip(8);const slot=r.u32(),entry=r.u32(),source=r.u32(),object=objects.get(slot);
    assert.ok(object&&object.incarnation === incarnation&&object.classId === 5&&object.drawClass === 2,'Invalid GL shadow caster identity/class');
    assert.ok(entry < kinds.length&&entry >= previousEntry&&!seen.has(entry)&&kinds[entry] === 3&&source < sourceCount&&source <= usedSources,'Invalid GL shadow entry/source/order');
    previousEntry=entry;seen.add(entry);if(source === usedSources)usedSources++;
    for(let vertex=0;vertex<4;vertex++) {
      const x=r.f32(),y=r.f32(),u=r.f32(),v=r.f32();
      assert.ok([x,y,u,v].every(Number.isFinite)&&Math.abs(x) <= 16777216&&Math.abs(y) <= 16777216
        &&u >= 0&&v >= 0&&u <= (sources[source].width-1)/256&&v <= (sources[source].height-1)/256,'Invalid GL shadow geometry/UV');
    }
    const z=r.f32(),darken=r.f32();
    assert.ok([f(f(2*buckets[entry]/4097)-1),f(2*buckets[entry]/4097-1)].includes(z)
      &&Number.isFinite(darken)&&darken >= f(.2)&&darken <= f(.55),'Invalid GL shadow depth/darken');
  }
  assert.equal(usedSources,sourceCount,'Unused GL shadow source');
  assert.equal(seen.size,kinds.filter(kind=>kind === 3).length,'Missing GL shadow bucket');
  return {resourceBytes,metadata:{targetKind,width,height,window,clear,settings,time,tiles:tileCount,terrain:terrainCount,shadowSources:sourceCount,shadows}};
}
const adjacent32=(value,up)=>{
  if(value===0)return up?2**-149:-(2**-149);
  const b=Buffer.allocUnsafe(4);b.writeFloatLE(value);b.writeUInt32LE(b.readUInt32LE()+(up?1:-1));return b.readFloatLE();
};
function parseComposition(r,o,world,worldResourceBytes) {
  o.composition=r.position;assert.equal(r.bool(),1,'V11 requires owned composition');assert.equal(r.bool(),1,'Incomplete composition');
  assert.equal(r.u8(),15,'Missing completed composition phases');const phaseCounts=Array.from({length:3},()=>r.u32()),clear=Array.from({length:4},()=>r.f32());
  assert.ok(clear.every(v=>Number.isFinite(v)&&v>=0&&v<=1)&&clear[3]===1,'Invalid screen clear');
  if(world.targetKind===0)assert.ok(clear.every((v,i)=>v===world.clear[i]),'Screen/world clear mismatch');
  o.overlayNative=r.position;const native=[r.u32(),r.u32()];assert.deepEqual(native,[world.width,world.height],'Overlay/world target mismatch');
  o.overlaySources=r.position;o.overlaySourceRecords=[];const sourceCount=r.count(256,16),sources=[],uniqueSources=new Set();let resourceBytes=0;
  for(let i=0;i<sourceCount;i++) {
    o.overlaySourceRecords.push(r.position);const width=r.u32(),height=r.u32(),length=r.count(64*1024*1024);
    assert.ok(width>0&&height>0&&width<=16384&&height<=16384&&length===width*height,'Invalid overlay source dimensions/indices');
    const indices=r.bytes.subarray(r.position,r.position+length);r.skip(length);assert.equal(r.count(16384),height,'Invalid initialized-row proof length');
    const known=r.bytes.subarray(r.position,r.position+height);r.skip(height);resourceBytes+=length+height;
    assert.ok(resourceBytes<=64*1024*1024&&known.every(v=>v<=1),'Invalid overlay row proof/resource budget');
    for(let row=0;row<height;row++)if(!known[row])assert.ok(indices.subarray(row*width,(row+1)*width).every(v=>v===0),'Unknown row contains data');
    const key=width+','+height+','+hashRow(indices)+','+hashRow(known);assert.ok(!uniqueSources.has(key),'Duplicate overlay source');uniqueSources.add(key);
    sources.push({width,height,known});
  }
  o.overlayColourRows=r.position;const rows=r.count(16384,1024),uniqueRows=new Set();
  for(let i=0;i<rows;i++){const key=hashRow(r.bytes.subarray(r.position,r.position+1024));r.skip(1024);assert.ok(!uniqueRows.has(key),'Duplicate overlay colour row');uniqueRows.add(key);}
  o.overlayFade=r.position;const fadeBytes=r.scalars(16384,1);resourceBytes+=rows*1024+fadeBytes;
  assert.ok(resourceBytes<=64*1024*1024,'Overlay owned-resource budget exceeded');
  o.overlayDraws=r.position;const drawCount=r.count(262144,81),actual=[0,0,0];let priorPhase=0,usedSources=0,usedRows=0,hasFade=false;
  for(let i=0;i<drawCount;i++) {
    const phase=r.u8(),mode=r.u8(),depth=r.bool(),scissor=r.bool(),rect=Array.from({length:4},()=>r.i32());
    const xy=Array.from({length:4},()=>r.f32()),uv=Array.from({length:4},()=>r.f32()),z=r.f32(),colour=Array.from({length:4},()=>r.f32());
    const source=r.u32(),row=r.u32(),fadeRow=r.u8();
    assert.ok(phase<=2&&phase>=priorPhase&&mode<=4&&(phase!==0||depth)&&(phase!==1||!depth),'Invalid overlay phase/mode/depth');priorPhase=phase;actual[phase]++;
    assert.ok(xy.every(v=>Number.isFinite(v)&&Math.abs(v)<=32768)&&xy[2]>xy[0]&&xy[3]>xy[1]&&Number.isFinite(z)&&Math.abs(z)<=2,'Invalid overlay geometry/depth');
    assert.ok(colour.every(v=>Number.isFinite(v)&&v>=0&&v<=1),'Invalid overlay vertex colour');
    assert.ok(scissor?rect[0]>=-32768&&rect[0]<=32768&&rect[1]>=-32768&&rect[1]<=32768&&rect[2]>=0&&rect[2]<=32768&&rect[3]>=0&&rect[3]<=32768:rect.every(v=>v===0),'Invalid overlay scissor');
    assert.ok(mode===3?fadeRow<64:fadeRow===0,'Invalid/unused overlay fade row');hasFade||=mode===3;
    assert.ok(mode===1||mode===2?row===0xffffffff:row<rows,'Invalid/unused overlay colour row');
    if(row!==0xffffffff){assert.ok(row<=usedRows,'Overlay row first-use order');if(row===usedRows)usedRows++;}
    if(mode===2){assert.ok(source===0xffffffff&&uv.every(v=>v===0),'Unused solid source/UV');continue;}
    assert.ok(source<sourceCount&&source<=usedSources,'Invalid overlay source reference/first use');if(source===usedSources)usedSources++;
    assert.ok(uv.every(v=>Number.isFinite(v)&&v>=0&&v<=1),'Invalid normalized overlay UV');
    const s=sources[source],low=adjacent32(Math.min(uv[1],uv[3]),false),high=adjacent32(Math.max(uv[1],uv[3]),true);
    const first=Math.max(0,Math.min(s.height-1,Math.floor(low*s.height))),last=Math.max(0,Math.min(s.height-1,Math.floor(high*s.height)));
    for(let row=first;row<=last;row++)assert.equal(s.known[row],1,'Overlay sample reaches uninitialized row');
  }
  assert.deepEqual(actual,phaseCounts,'Composition phase counts disagree');assert.equal(usedSources,sourceCount,'Unused overlay source');assert.equal(usedRows,rows,'Unused overlay row');
  assert.equal(fadeBytes,hasFade?16384:0,'Missing/unused overlay fade table');
  o.lens=r.position;const active=r.bool(),began=r.bool(),operation=r.u8(),requestedType=r.u8(),fallback=r.bool();
  assert.ok(operation<=5&&requestedType<=4&&began===Number(world.targetKind===1),'Invalid lens resolution/capture target');
  o.lensStamps=r.position;const stamps=Array.from({length:6},()=>({generation:r.u64(),revision:r.u64()}));
  o.lensPass=r.position;const kind=r.u8(),sourceExtent=[r.u32(),r.u32()],sourceOrigin=[r.i32(),r.i32()],targetExtent=[r.u32(),r.u32()],destination=[r.i32(),r.i32()],viewport=[r.u32(),r.u32()];
  o.lensMist=r.position;const mistOffsets=Array.from({length:4},()=>r.f32()),lightness=r.u8();
  o.lensRemapDims=r.position;const remapExtent=[r.u32(),r.u32()];o.lensRemap=r.position;const remapPixels=r.scalars(16*1024*1024,4);
  o.lensOverlayDims=r.position;const overlayExtent=[r.u32(),r.u32()];o.lensOverlayAlpha=r.position;const overlayAlpha=r.u16();
  o.lensOverlay=r.position;const overlayBytes=r.scalars(64*1024*1024,1);
  o.lensDensity=r.position;const densityBytes=r.scalars(65536,1);o.lensPalette=r.position;const paletteBytes=r.scalars(1024,1);
  o.lensFade=r.position;const lensFadeBytes=r.scalars(16384,1);o.lensRgb6Lookup=r.position;const rgb6Bytes=r.scalars(512*512,1);o.compositionEnd=r.position;
  const passDefault=kind===0&&[...sourceExtent,...sourceOrigin,...targetExtent,...destination,...viewport,...mistOffsets,lightness,...remapExtent,remapPixels,...overlayExtent,overlayAlpha,overlayBytes,densityBytes,paletteBytes,lensFadeBytes,rgb6Bytes].every(v=>v===0);
  const stampsDefault=stamps.every(s=>s.generation===0n&&s.revision===0n);
  if(!active)assert.ok(!began&&operation===0&&requestedType===0&&!fallback&&passDefault&&stampsDefault,'Inactive lens must retain defaults');
  else if(operation===5)assert.ok(!fallback&&passDefault&&stampsDefault,'NotDrawn lens must retain no pass/resources');
  else {
    assert.ok(operation>=1&&operation<=4&&began&&kind===operation-1&&fallback===Number(operation===1&&requestedType!==0),'Invalid resolved lens branch/fallback');
    assert.ok(operation===2?[2,3].includes(requestedType):operation===3?requestedType===1:operation===4?requestedType===4:true,'Requested/resolved lens operation mismatch');
    const image=([w,h])=>w>0&&h>0&&w<=16384&&h<=16384&&w*h*4<=64*1024*1024;
    assert.ok(image(sourceExtent)&&image(targetExtent)&&sourceOrigin[0]>=0&&sourceOrigin[0]<sourceExtent[0]&&sourceOrigin[1]===0,'Invalid lens native source/target/origin');
    assert.deepEqual(sourceExtent,[world.width,world.height],'Lens/world source mismatch');assert.deepEqual(targetExtent,sourceExtent,'Lens/world target mismatch');
    assert.ok(sourceOrigin[0]===destination[0]&&destination.every(v=>v>=0)&&viewport.every(v=>v>0)&&destination[0]+viewport[0]<=targetExtent[0]&&destination[1]+viewport[1]<=targetExtent[1],'Invalid lens destination rectangle');
    const mist=operation===3,indexed=operation===4,remap=operation===2;
    assert.ok(paletteBytes===(mist||indexed?1024:0)&&rgb6Bytes===(mist||indexed?262144:0)&&lensFadeBytes===(mist?16384:0)&&densityBytes===(mist?65536:0),'Missing/unused lens resource');
    assert.ok(mistOffsets.every(v=>Number.isFinite(v)&&(mist?v>=0&&v<256:v===0))&&(mist?lightness<=63:lightness===0),'Invalid/unused mist state');
    assert.ok(remap?image(remapExtent)&&remapPixels===remapExtent[0]*remapExtent[1]:remapExtent.every(v=>v===0)&&remapPixels===0,'Invalid/unused remap');
    assert.ok(indexed?overlayExtent.every(v=>v>0&&v<=16384)&&overlayBytes===overlayExtent[0]*overlayExtent[1]&&overlayAlpha<=256:overlayExtent.every(v=>v===0)&&overlayAlpha===0&&overlayBytes===0,'Invalid/unused indexed overlay');
    const consumed=[mist||indexed,mist,mist||indexed,mist,remap,indexed];
    for(let i=0;i<6;i++)assert.ok(consumed[i]?stamps[i].generation>0n&&stamps[i].revision>0n:stamps[i].generation===0n&&stamps[i].revision===0n,'Missing/unused lens upload stamp');
  }
  const lensResourceBytes=remapPixels*4+overlayBytes+densityBytes+paletteBytes+lensFadeBytes+rgb6Bytes;
  assert.ok(lensResourceBytes<=64*1024*1024,'Lens auxiliary-resource budget exceeded');
  // V11 alone allows the measured combination of native world, swipe atlas
  // and lens inputs. Each child remains <=64 MiB; the wire stays <=128 MiB.
  assert.ok(worldResourceBytes+resourceBytes+lensResourceBytes<=96*1024*1024,'Shared V11 composition owned-resource budget exceeded');
  o.compositionResourceRanges=[[o.overlaySources,o.overlayDraws],[o.lensRemapDims,o.lensOverlayDims],[o.lensOverlayDims,o.lensOverlayAlpha],[o.lensOverlay,o.compositionEnd]];
  o.compositionStateRanges=[[o.composition,o.overlaySources],[o.overlayDraws,o.lensRemapDims],[o.lensOverlayAlpha,o.lensOverlay]];
  return {phaseCounts,screenClear:clear,overlays:{native,sources:sourceCount,colourRows:rows,fadeBytes,draws:drawCount},
    lens:{requestedActive:!!active,captureBegan:!!began,operation,requestedType,fallbackCopy:!!fallback,kind,sourceExtent,sourceOrigin,targetExtent,destination,viewport,mistOffsets,lightness,remapExtent,remapPixels,overlayExtent,overlayAlpha,overlayBytes,
      densityBytes,paletteBytes,fadeBytes:lensFadeBytes,rgb6Bytes,resources:stamps.map(s=>({generation:s.generation.toString(),revision:s.revision.toString()}))}};
}
export function parseSceneCapture(bytes) {
  assert.ok(Buffer.isBuffer(bytes) && bytes.length >= 24 && bytes.length <= maxFile, 'Invalid KFXC size');
  assert.equal(bytes.readUInt32LE(0),0x4358464b,'Invalid KFXC magic');
  const version=bytes.readUInt32LE(4);
  assert.ok(Object.hasOwn(frozenSchemes,version),'Unsupported frozen capture version');
  assert.equal(bytes.readUInt32LE(8),bytes.length-24,'Invalid KFXC payload size');
  assert.equal(bytes.readBigUInt64LE(16),0n,'Unsupported KFXC flags');
  assert.equal(captureCrc(bytes.subarray(24)),bytes.readUInt32LE(12),'Invalid KFXC checksum');
  const r=new Reader(bytes), offsets={};let target,glWorld,composition,worldResourceBytes;
  // WorldScene fields, wibble and camera, in SceneCaptureCodec.cpp order.
  r.skip(36); r.bool(); r.skip(12 + 128*16);
  offsets.camera=r.position;
  const cameraView=r.u8(),cameraMode=bytes[offsets.camera+1+12+12*4];
  assert.ok(cameraView <= 2,'Invalid KFXC camera view');
  r.skip(12 + 12*4 + 3); r.bool(); r.skip(12*4);
  offsets.columns=r.position; r.records(512*512,44,[34,35]);
  offsets.objects=r.position; r.records(65536,56);
  r.records(65536,53,[44]); r.records(65536,13);
  r.scalars(65536,2); r.scalars(65536,2);
  const frameStart=r.position;
  assert.equal(r.scalars(32*1544,4),32*1544,'Invalid texture frame count');
  const textureDimension=r.u32(), textureStride=r.u32();
  offsets.palettes=r.position; r.skip(768*2 + 64*256 + 256*256);
  offsets.textures=r.position; const textureBytes=r.scalars(64*1024*1024,1);
  offsets.sprites=r.position;
  const sprites=r.count(65536,28);
  let spriteBytes=0; const sources=[];
  for(let index=0;index<sprites;index++) {
    const start=r.position;
    r.skip(24);
    const width=bytes.readUInt16LE(start+4), height=bytes.readUInt16LE(start+6);
    assert.ok(width <= 1024 && height <= 1024,'Invalid KFXC sprite dimensions');
    const pixels=r.count(1024*1024*2);
    assert.equal(pixels,width*height*2,'Invalid KFXC sprite pixel count');
    spriteBytes += pixels; assert.ok(spriteBytes <= 64*1024*1024,'Excessive KFXC sprite pixels');
    sources.push({width,height,pixels:bytes.subarray(r.position,r.position+pixels)});r.skip(pixels);
  }
  r.records(65536*8,17,[8]); r.records(65536,17,[16]); r.records(4096,21,[20]);
  if(version >= 3) {
    offsets.additive=r.position;
    assert.equal(r.scalars(65536,1),65536,'Invalid additive table length');
  }
  const inputEnd=r.position;
  if(version >= 3) {
    offsets.trace=r.position;
    assert.equal(r.bool(),1,'Incomplete KFXC trace'); const backend=r.u8(); assert.ok(backend <= 1,'Invalid KFXC trace backend'); r.skip(4);
    offsets.entries=r.position;
    const entries=r.count(262144,5);
    let previousBucket=4097; const kinds=[],buckets=[];
    for(let index=0;index<entries;index++) {
      const bucket=r.u32(); assert.ok(bucket <= previousBucket,'Invalid KFXC bucket order'); previousBucket=bucket; kinds.push(r.u8());buckets.push(bucket);
    }
    offsets.draws=r.position;
    const draws=r.count(65536,327);
    const objects=new Map();
    for(let object=0;object<bytes.readUInt32LE(offsets.objects);object++) {
      const start=offsets.objects+4+object*56;
      objects.set(bytes.readUInt32LE(start+8),{incarnation:bytes.readBigUInt64LE(start),classId:bytes[start+51],drawClass:bytes[start+52],owner:bytes.readInt8(start+50)});
    }
    let previousEntry=0; const spriteEntries=[],drawValues=[];
    for(let index=0;index<draws;index++) {
      const incarnation=bytes.readBigUInt64LE(r.position),slot=bytes.readUInt32LE(r.position+8);
      r.skip(12); const entry=r.u32(), sprite=r.u32();
      assert.ok(entry < entries && entry >= previousEntry && sprite < sprites,'Invalid KFXC draw reference'); previousEntry=entry;
      const object=objects.get(slot);
      assert.ok(object && object.incarnation === incarnation,'Invalid KFXC sprite owner identity');
      const ordinary=object.drawClass === 2 && kinds[entry] === 2;
      const key=version >= 6 && object.drawClass === 6 && kinds[entry] === 9;
      assert.ok(ordinary || key,'Invalid KFXC sprite object/bucket pair');
      if(key)assert.ok((cameraView === 0 && [2,8].includes(cameraMode)) || (cameraView === 1 && cameraMode === 5),'Spinning key submitted for unsupported camera');
      spriteEntries.push(entry);
      drawValues.push({entry,sprite,owner:object.owner,scale:Array.from({length:10},(_,i)=>bytes.readInt32LE(r.position+i*4)),
        contentHeight:bytes.readInt32LE(r.position+40),flags:bytes.readUInt32LE(r.position+44),hasRemap:bytes[r.position+49]});
      r.skip(40+4+4); r.bool(); r.bool(); assert.ok(r.u8() <= 4,'Invalid KFXC blend'); r.skip(256);
    }
    if(version >= 4) {
      const glOutline=version >= 7 && backend === 1;
      assert.ok(backend === 0 || glOutline,'KFXC v4-v6 requires software raster trace');
      if(!glOutline)assert.equal(textureStride,256,'KFXC v4 requires original texture row stride');
      assert.ok(textureDimension > 0,'Invalid texture dimension');
      const textures=new Set();
      for(let index=0;index<32*1544;index++) {
        const offset=bytes.readInt32LE(frameStart+4+index*4); if(offset >= 0) textures.add(offset);
      }
      offsets.triangles=r.position;
      const triangles=r.count(262144,71), triangleEntries=[], triangleCounts=new Uint32Array(entries);
      if(glOutline)assert.equal(triangles,0,'GL outline trace cannot contain indexed terrain');
      previousEntry=0;
      for(let index=0;index<triangles;index++) {
        const entry=r.u32(), rasterizer=r.u8(), mode=r.u8(), colour=r.u8(), texture=r.u32();
        assert.ok(entry < entries && entry >= previousEntry,'Invalid KFXC triangle entry'); previousEntry=entry;
        const kind=kinds[entry]; assert.ok([0,1,6].includes(kind),'Invalid KFXC triangle producer kind');
        const gpoly=rasterizer === 0 && mode === 5, trig=rasterizer === 1 && mode === 7 && kind === 1;
        assert.ok(gpoly || trig,'Unsupported KFXC rasterizer mode');
        assert.ok(gpoly ? colour === 0 : colour < 64,'Invalid ignored or constant triangle colour');
        // Trig7 colour32 dispatches md02, bypassing fade and retaining both
        // full 8-bit U/V addresses; other Trig7 colours mask V to five bits.
        const textureSpan=gpoly ? 7968 : colour === 32 ? 65536 : 8192;
        assert.ok(textures.has(texture) && texture+textureSpan <= textureBytes,'Invalid KFXC texture address/span');
        for(let vertex=0;vertex<3;vertex++) {
          const start=r.position; r.skip(20);
          const x=bytes.readInt32LE(start), y=bytes.readInt32LE(start+4), shade=bytes.readInt32LE(start+16);
          assert.ok(Math.abs(x) <= 16777216 && Math.abs(y) <= 16777216,'KFXC raster coordinate outside bounds');
          assert.ok(gpoly ? shade >= 0 && shade < 4194304 : shade === 0,'Invalid KFXC vertex shade/ignored S');
        }
        triangleEntries.push(entry); triangleCounts[entry]++;
      }
      for(let entry=0;!glOutline && entry<entries;entry++) {
        const kind=kinds[entry], count=triangleCounts[entry];
        if(kind === 0) assert.equal(count,1,'Standard entry must own one triangle');
        if(kind === 6) assert.equal(count,2,'Front entry must own two triangles');
        if(kind === 1) assert.ok([1,2,4,6,16].includes(count),'Invalid near-surface subdivision count');
      }
      offsets.commands=r.position;
      const commands=r.count(327680,5), commandValues=[];
      if(glOutline)assert.equal(commands,0,'GL outline trace cannot contain indexed commands');
      for(let command=0;command<commands;command++) commandValues.push({kind:r.u8(),index:r.u32()});
      const shadowEntries=[];
      if(version >= 5) {
        offsets.excludedShadows=r.position; const excluded=r.u32();
        offsets.shadowMasks=r.position; const masks=r.count(32768,8), dimensions=[];
        if(glOutline)assert.ok(excluded === 0 && masks === 0,'GL outline trace cannot contain indexed shadows');
        for(let mask=0;mask<masks;mask++) {
          const start=r.position; r.skip(4);
          const width=bytes.readUInt16LE(start), height=bytes.readUInt16LE(start+2), length=r.count(65536);
          assert.ok(width > 0 && width <= 256 && height > 0 && height <= 256 && length === width*height,'Invalid shadow mask dimensions/coverage length');
          spriteBytes += length; assert.ok(spriteBytes <= 64*1024*1024,'Combined sprite/shadow coverage budget exceeded');
          const coverage=bytes.subarray(r.position,r.position+length);
          assert.ok(coverage.every(value=>value === 0 || value === 255),'Noncanonical shadow mask coverage'); r.skip(length);
          dimensions.push({width,height});
        }
        offsets.shadows=r.position; const shadows=r.count(65536,81);
        assert.equal(shadows,masks*2,'One shadow pair per mask required');
        let first,previousPair=-1;
        for(let index=0;index<shadows;index++) {
          const incarnation=bytes.readBigUInt64LE(r.position); r.skip(8);
          const slot=r.u32(),entry=r.u32(),mask=r.u32(),shade=r.u8(),object=objects.get(slot),vertices=[];
          assert.ok(object && object.incarnation === incarnation && object.classId === 5 && object.drawClass === 2,'Invalid shadow caster identity/class');
          assert.ok(entry < entries && kinds[entry] === 3 && mask === Math.floor(index/2) && mask < masks,'Invalid shadow entry/mask reference');
          assert.ok(shade >= 16 && shade <= 31,'Invalid shadow shade');
          for(let vertex=0;vertex<3;vertex++) {
            const start=r.position; r.skip(20);
            const values=Array.from({length:5},(_,field)=>bytes.readInt32LE(start+field*4));
            const [x,y,u,v,s]=values,dimension=dimensions[mask];
            assert.ok(Math.abs(x) <= 16777216 && Math.abs(y) <= 16777216 && s === 0,'Invalid shadow XY/ignored S');
            assert.ok(u >= 0 && u <= (dimension.width-1)*65536 && v >= 0 && v <= (dimension.height-1)*65536,'Shadow UV outside owned mask');
            vertices.push(values);
          }
          if(index%2 === 0) {
            assert.ok(entry > previousPair,'Shadow pair bucket order must increase'); previousPair=entry;
            first={incarnation,slot,entry,mask,shade,vertices};
          } else {
            assert.ok(first.incarnation === incarnation && first.slot === slot && first.entry === entry && first.mask === mask && first.shade === shade,'Shadow pair state disagrees');
            assert.deepEqual(vertices[0],first.vertices[0],'Shadow pair origin disagrees');
            assert.deepEqual(vertices[1],first.vertices[2],'Shadow pair shared edge disagrees');
          }
          shadowEntries.push(entry);
        }
        if(!glOutline)assert.equal(excluded,kinds.filter(kind=>kind === 3).length-masks,'Excluded shadow count does not match empty shadow buckets');
      }
      if(!glOutline)assert.equal(commands,draws+triangles+shadowEntries.length,'Missing/duplicate unified raster payload');
      let nextSprite=0,nextTriangle=0,nextShadow=0; previousEntry=0;
      for(const {kind,index} of commandValues) {
        let entry;
        if(kind === 0) { assert.ok(index === nextSprite && nextSprite < draws,'Invalid ordered sprite command'); entry=spriteEntries[nextSprite++]; }
        else if(kind === 1) { assert.ok(index === nextTriangle && nextTriangle < triangles,'Invalid ordered triangle command'); entry=triangleEntries[nextTriangle++]; }
        else { assert.ok(version >= 5 && kind === 2 && index === nextShadow && nextShadow < shadowEntries.length,'Invalid ordered shadow command'); entry=shadowEntries[nextShadow++]; }
        assert.ok(entry >= previousEntry,'Unified command entry order regressed'); previousEntry=entry;
      }
      if(!glOutline)assert.ok(nextSprite === draws && nextTriangle === triangles && nextShadow === shadowEntries.length,'Unreferenced unified payload');
      const outlineStates=version >= 7?parseOutline(r,offsets,backend,cameraView,kinds,buckets,drawValues,sources):undefined;
      if(version >= 8) {
        offsets.target=r.position;
        const kind=r.u8(),clearIndex=r.u8();r.skip(2);const lensId=bytes.readUInt16LE(r.position-2);
        const width=r.u32(),height=r.u32(),pitch=r.u32(),window=Array.from({length:4},()=>r.i32());
        if(version >= 9) {
          assert.ok(backend === 1 && outlineStates && [kind,clearIndex,lensId,width,height,pitch,...window].every(value=>value === 0),'V9 requires GL outline capture and canonical absent indexed target');
        } else {
          assert.ok(backend === 0 && [1,2].includes(kind),'V8 requires a proven software indexed target');
          assert.ok(width > 0 && width <= 16384 && height > 0 && height <= 16384 && pitch >= width && pitch <= 16384
            && pitch*height <= 64*1024*1024,'Invalid indexed target dimensions/pitch/allocation bounds');
          const [x,y,w,h]=window;
          assert.ok(x >= 0 && y >= 0 && w > 0 && h > 0 && x+w <= width && y+h <= height,'Indexed target window outside logical pixels');
          assert.deepEqual(window,Array.from({length:4},(_,i)=>bytes.readInt32LE(offsets.camera+37+i*4)),'Indexed target window differs from captured camera viewport');
          assert.ok(kind === 1 ? lensId === 0 : clearIndex === 0 && pitch === width && lensId >= 1 && lensId <= 255 && cameraView === 2,
            'Invalid screen/lens-source target semantics');
        }
        target={kind,clearIndex,lensId,width,height,pitch,window};
      }
      if(version >= 9)parseOutlineColours(r,offsets,outlineStates,drawValues);
      if(version >= 10) {
        const parsed=parseGLWorld(r,offsets,backend,cameraView,kinds,buckets,objects,textureBytes+spriteBytes+65536+768*2+16384+65536);
        glWorld=parsed.metadata;worldResourceBytes=parsed.resourceBytes;
      }
      if(version>=11)composition=parseComposition(r,offsets,glWorld,worldResourceBytes);
    }
  }
  assert.equal(r.position,bytes.length,'Trailing KFXC fields');
  return {version,inputEnd,offsets,...(target ? {target} : {}),...(glWorld ? {glWorld} : {}),...(composition ? {composition} : {})};
}
export function sceneFingerprint(bytes) {
  const {version,inputEnd,offsets}=parseSceneCapture(bytes);
  const normalized=Buffer.from(bytes.subarray(0,inputEnd));
  // v2 must retain its historical whole-file fingerprint exactly.
  // v3 excludes commands, normalizes the full-file size and uses a domain tag.
  normalized.fill(0,version >= 3 ? 8 : 12,16); normalized.fill(0,32,40);
  if(version>=11)normalized.writeUInt32LE(10,4); // V11 preserves the V10 scene/assets identity.
  const hash=createHash('sha256');
  if(version >= 3) hash.update(frozenSchemes[version]+'\0');
  hash.update(normalized);
  // V8 clear/target identity affects the initialized frame. Hash its fixed
  // tail even though the settings-dependent draw trace remains excluded.
  if(version >= 8)hash.update(bytes.subarray(offsets.target,offsets.target+32));
  // Effective GL lookup resources are owned inputs even when they are stale
  // or capacity fallbacks. V9 binds them while still excluding phase traces.
  if(version >= 9)hash.update(bytes.subarray(offsets.colourRows,offsets.glWorld??bytes.length));
  const result={version,frozenScheme:frozenSchemes[version],frozenSha256:hash.digest('hex')};
  if(version >= 10) {
    // Executed geometry is full-file/CRC-bound. Settings A/B retains the same
    // scene identity while these separate hashes expose actual resources and
    // applied uniforms; equal scene identity never implies equal world inputs.
    const resources=createHash('sha256').update(worldResourceScheme+'\0')
      .update(bytes.subarray(offsets.glWorld+2,offsets.worldSettings))
      .update(bytes.subarray(offsets.worldLightmap,offsets.worldTerrain))
      .update(bytes.subarray(offsets.worldShadowSources,offsets.worldShadows));
    const state=createHash('sha256').update(worldStateScheme+'\0').update(bytes.subarray(offsets.worldSettings,offsets.worldLightmap));
    Object.assign(result,{effectiveWorldResourcesScheme:worldResourceScheme,effectiveWorldResourcesSha256:resources.digest('hex'),
      appliedWorldStateScheme:worldStateScheme,appliedWorldStateSha256:state.digest('hex')});
  }
  if(version>=11)for(const [name,scheme,ranges] of [['compositionResources',compositionResourceScheme,offsets.compositionResourceRanges],['compositionState',compositionStateScheme,offsets.compositionStateRanges]]) {
    const hash=createHash('sha256').update(scheme+'\0');for(const [start,end] of ranges)hash.update(bytes.subarray(start,end));
    result[name+'Scheme']=scheme;result[name+'Sha256']=hash.digest('hex');
  }
  return result;
}
export function verifyRecordedFingerprint(frame, fingerprint) {
  const scheme=frame.frozenScheme === undefined ? (fingerprint.version === 2 ? frozenSchemes[2] : undefined) : frame.frozenScheme;
  assert.equal(scheme,fingerprint.frozenScheme,'Missing or incompatible frozen fingerprint scheme');
  if(fingerprint.version >= 3) assert.match(frame.frozenSha256 ?? '',/^[a-f0-9]{64}$/,'Missing versioned frozen fingerprint');
  if(frame.frozenSha256 !== undefined) assert.equal(frame.frozenSha256,fingerprint.frozenSha256,'Changed frozen scene/assets');
  if(fingerprint.version >= 10)for(const name of ['effectiveWorldResources','appliedWorldState']) {
    assert.equal(frame[name+'Scheme'],fingerprint[name+'Scheme'],'Missing/incompatible '+name+' scheme');
    assert.match(frame[name+'Sha256']??'',/^[a-f0-9]{64}$/,'Missing '+name+' fingerprint');
    assert.equal(frame[name+'Sha256'],fingerprint[name+'Sha256'],'Changed '+name);
  }
  if(fingerprint.version>=11)for(const name of ['compositionResources','compositionState']) {
    assert.equal(frame[name+'Scheme'],fingerprint[name+'Scheme'],'Missing/incompatible '+name+' scheme');
    assert.match(frame[name+'Sha256']??'',/^[a-f0-9]{64}$/,'Missing '+name+' fingerprint');
    assert.equal(frame[name+'Sha256'],fingerprint[name+'Sha256'],'Changed '+name);
  }
  return scheme;
}
if(process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [path,mode,...extra]=process.argv.slice(2);
  assert.ok(path && (!mode || mode === '--record') && !extra.length,'Usage: reference-fingerprint.mjs capture [--record]');
  const size=(await stat(path)).size; assert.ok(size >= 24 && size <= maxFile,'Invalid KFXC size');
  const fingerprint=sceneFingerprint(await readFile(path));
  if(mode === '--record') {
    let record='';
    for await(const chunk of process.stdin) { record += chunk; assert.ok(record.length <= 256*1024,'Oversized fingerprint record'); }
    verifyRecordedFingerprint(JSON.parse(record),fingerprint);
  }
  console.log(JSON.stringify(fingerprint));
}
