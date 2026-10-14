// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Pure file/buffer tests; no game, renderer, GPU or compilation.
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {captureCrc,frozenSchemes,parseSceneCapture,sceneFingerprint,verifyRecordedFingerprint} from './reference-fingerprint.mjs';

const directory=process.argv[2] ?? fileURLToPath(new URL('../../artifacts/scene-assets/',import.meta.url));
const helper=fileURLToPath(new URL('./reference-fingerprint.mjs',import.meta.url));
const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
const repair=bytes=>{
  bytes.writeUInt32LE(bytes.length-24,8); bytes.writeUInt32LE(captureCrc(bytes.subarray(24)),12); return bytes;
};
const variants=new Map();
let rejections=0;
const reject=bytes=>{assert.throws(()=>sceneFingerprint(bytes)); rejections++;};
for(const version of [2,3]) {
  const suffix=version === 3 ? '.v3' : '';
  const bytes=await readFile(join(directory,`fixture-x86_64.kfxscene${suffix}`));
  const other=await readFile(join(directory,`fixture-i686.kfxscene${suffix}`));
  const fingerprint=sceneFingerprint(bytes), parsed=parseSceneCapture(bytes);
  variants.set(version,{bytes,fingerprint,parsed});
  assert.deepEqual(bytes,other); assert.deepEqual(fingerprint,sceneFingerprint(other));
  assert.equal(fingerprint.version,version); assert.equal(fingerprint.frozenScheme,frozenSchemes[version]);
  if(version === 2) {
    const historical=Buffer.from(bytes); historical.fill(0,12,16); historical.fill(0,32,40);
    assert.equal(fingerprint.frozenSha256,hash(historical));
    assert.equal(fingerprint.frozenSha256,'408d7e04defdcfdf3acc58b6cb62fac1d4750e4d37c332460402d661d5f7a20b');
    assert.equal(parsed.inputEnd,bytes.length);
  } else {
    assert.equal(parsed.inputEnd,parsed.offsets.additive+4+65536);
    assert.equal(fingerprint.frozenSha256,'203138a721cb7b03c1202069da89f576b8baf2d21809494ab3cde5616d44e622');
  }
  const changed=(position,same)=>{
    const copy=Buffer.from(bytes); copy[position]^=1; repair(copy);
    assert.notEqual(hash(copy),hash(bytes));
    const actual=sceneFingerprint(copy);
    if(same) assert.deepEqual(actual,fingerprint); else assert.notEqual(actual.frozenSha256,fingerprint.frozenSha256);
  };
  changed(32,true); // Sequence and the resulting checksum are normalized.
  for(const position of [24,40,parsed.offsets.objects+4+12,parsed.offsets.palettes,parsed.offsets.textures+4,
    parsed.offsets.sprites+4+24+4]) changed(position,false); // Epoch, turn, scene, palette, texture and sprite pixels.
  for(const count of [parsed.offsets.columns,parsed.offsets.objects,parsed.offsets.textures,parsed.offsets.sprites,
    parsed.offsets.sprites+4+24]) {
    const copy=Buffer.from(bytes); copy.writeUInt32LE(0xffffffff,count); reject(repair(copy));
  }
  for(const cut of [0,23,24,40,parsed.offsets.columns+2,parsed.offsets.textures+6,parsed.offsets.sprites+30,bytes.length-1]) {
    const copy=Buffer.from(bytes.subarray(0,cut));
    reject(copy); if(cut >= 24) reject(repair(copy));
  }
  for(const position of [0,4,8,12,16,bytes.length-1]) {
    const copy=Buffer.from(bytes); copy[position]^=128; reject(copy);
  }
  reject(repair(Buffer.concat([bytes,Buffer.from([0])])));
  const cli=spawnSync(process.execPath,[helper,join(directory,`fixture-x86_64.kfxscene${suffix}`),'--record'],
    {input:JSON.stringify(fingerprint),encoding:'utf8'});
  assert.equal(cli.status,0,cli.stderr); assert.deepEqual(JSON.parse(cli.stdout),fingerprint);
}
const {bytes,fingerprint,parsed}=variants.get(3);
for(const position of [parsed.offsets.trace+1,parsed.offsets.trace+2,parsed.offsets.draws+4+20]) {
  const copy=Buffer.from(bytes); copy[position]^=1; repair(copy);
  assert.deepEqual(sceneFingerprint(copy),fingerprint); assert.notEqual(hash(copy),hash(bytes));
}
// A longer, structurally valid trace changes the full size/hash, not frozen inputs.
const {offsets}=parsed;
const entry=Buffer.alloc(5); entry[4]=255;
const extraEntry=Buffer.concat([bytes.subarray(0,offsets.draws),entry,bytes.subarray(offsets.draws)]);
extraEntry.writeUInt32LE(bytes.readUInt32LE(offsets.entries)+1,offsets.entries); repair(extraEntry);
assert.deepEqual(sceneFingerprint(extraEntry),fingerprint); assert.notEqual(hash(extraEntry),hash(bytes));
// Composite layers can own frames without animation bindings: every owned sprite stays hash-bound.
const firstSprite=offsets.sprites+4, firstPixels=bytes.readUInt32LE(firstSprite+24);
const sprite=Buffer.from(bytes.subarray(firstSprite,firstSprite+28+firstPixels)); sprite.writeUInt32LE(901,0);
const spriteEnd=firstSprite+28+firstPixels;
const extraSprite=Buffer.concat([bytes.subarray(0,spriteEnd),sprite,bytes.subarray(spriteEnd)]);
extraSprite.writeUInt32LE(bytes.readUInt32LE(offsets.sprites)+1,offsets.sprites); repair(extraSprite);
assert.notEqual(sceneFingerprint(extraSprite).frozenSha256,fingerprint.frozenSha256);
const additive=Buffer.from(bytes); additive[parsed.inputEnd-1]^=1; repair(additive);
assert.notEqual(sceneFingerprint(additive).frozenSha256,fingerprint.frozenSha256);
for(const [position,value] of [[offsets.additive,0],[offsets.additive,65535],[offsets.additive,65537],
  [offsets.entries,262145],[offsets.draws,65537]]) {
  const copy=Buffer.from(bytes); copy.writeUInt32LE(value,position); reject(repair(copy));
}
for(const [position,value] of [[offsets.trace,0],[offsets.trace,2],[offsets.trace+1,2],[offsets.draws+4+68,2]]) {
  const copy=Buffer.from(bytes); copy[position]=value; reject(repair(copy));
}
for(const cut of [offsets.additive+2,parsed.inputEnd-1,parsed.inputEnd,offsets.entries+2,offsets.draws+2])
  reject(repair(Buffer.from(bytes.subarray(0,cut))));
const old=variants.get(2).fingerprint;
assert.equal(verifyRecordedFingerprint({frozenSha256:old.frozenSha256},old),frozenSchemes[2]);
assert.equal(verifyRecordedFingerprint({},old),frozenSchemes[2]); // Historical v2 reports predate the scheme/hash fields.
for(const [record,actual] of [[{},fingerprint],[{frozenSha256:fingerprint.frozenSha256},fingerprint],
  [{frozenScheme:frozenSchemes[3]},fingerprint],
  [{frozenScheme:null},old],[{frozenScheme:frozenSchemes[2]},fingerprint],
  [{frozenScheme:frozenSchemes[3]},old],[{frozenScheme:'unknown'},old],
  [{...fingerprint,frozenSha256:'0'.repeat(64)},fingerprint]]) assert.throws(()=>verifyRecordedFingerprint(record,actual));
assert.notEqual(old.frozenScheme,fingerprint.frozenScheme);
// Independent v4 fixture contains both actual rasterizers and interleaves
// sprites with terrain; appending per-kind lists would give a different order.
const v4=await readFile(join(directory,'fixture-x86_64.kfxscene.v4'));
assert.deepEqual(v4,await readFile(join(directory,'fixture-i686.kfxscene.v4')));
const p4=parseSceneCapture(v4), f4=sceneFingerprint(v4), o4=p4.offsets;
assert.equal(f4.frozenScheme,frozenSchemes[4]);
assert.equal(p4.inputEnd,o4.additive+4+65536);
assert.equal(v4.readUInt32LE(o4.triangles),4);
assert.equal(v4.readUInt32LE(o4.commands),6);
const order=Array.from({length:6},(_,index)=>{
  const start=o4.commands+4+index*5;
  return [v4[start],v4.readUInt32LE(start+1)];
});
assert.deepEqual(order,[[1,0],[0,0],[1,1],[0,1],[1,2],[1,3]]);
assert.equal(v4.readInt32LE(o4.triangles+4+11+8),-1234567);
assert.equal(v4.readInt32LE(o4.triangles+4+71+11+16),0); // Trig7's ignored S is canonical.
// Construct a second fixture independently of native EncodeSceneCapture.
// It keeps wire order and payloads, expands only the owned texture bank, and
// selects colour32's actual md02 dispatch requiring full 256*256 addressing.
function trigTextureFixture(textureBytes,colour) {
  const oldBytes=v4.readUInt32LE(o4.textures), end=o4.textures+4+oldBytes;
  assert.ok(textureBytes >= oldBytes);
  const delta=textureBytes-oldBytes;
  const result=Buffer.concat([v4.subarray(0,end),Buffer.alloc(delta),v4.subarray(end)]);
  result.writeUInt32LE(textureBytes,o4.textures);
  result[o4.triangles+delta+4+71+6]=colour;
  return repair(result);
}
const fullUv=trigTextureFixture(65536,32);
assert.equal(parseSceneCapture(fullUv).version,4);
assert.notEqual(sceneFingerprint(fullUv).frozenSha256,f4.frozenSha256); // Owned bank growth is frozen-content evidence.
reject(trigTextureFixture(65535,32));
reject(trigTextureFixture(8192,32));
assert.equal(parseSceneCapture(trigTextureFixture(8192,31)).version,4); // Ordinary mode7 needs only 8-bit U/5-bit V.
for(const position of [32,o4.draws+4+20,o4.triangles+4+11,o4.triangles+4+11+8]) {
  const copy=Buffer.from(v4); copy[position]^=1; repair(copy);
  assert.deepEqual(sceneFingerprint(copy),f4); assert.notEqual(hash(copy),hash(v4));
}
for(const position of [24,40,o4.palettes,o4.additive+4,o4.sprites+4+28]) {
  const copy=Buffer.from(v4); copy[position]^=1; repair(copy);
  assert.notEqual(sceneFingerprint(copy).frozenSha256,f4.frozenSha256);
}
for(const [position,value,byte] of [
  [o4.trace+1,1,true],[o4.triangles,262145],[o4.commands,327681],
  [o4.triangles+4,5],[o4.triangles+4+4,2,true],[o4.triangles+4+5,7,true],
  [o4.triangles+4+6,1,true],[o4.triangles+4+7,1],
  [o4.triangles+4+11+16,4194304],[o4.triangles+4+71+11+16,1],
  [o4.triangles+4+71+6,64,true],[o4.triangles+4+11,16777217],
  [o4.commands+4,2,true],[o4.commands+4+1,1],[o4.commands+4+5*5+1,2]]) {
  const copy=Buffer.from(v4);
  if(byte) copy[position]=value; else copy.writeUInt32LE(value,position);
  reject(repair(copy));
}
const reordered=Buffer.from(v4);
v4.copy(reordered,o4.commands+4,o4.commands+9,o4.commands+14);
v4.copy(reordered,o4.commands+9,o4.commands+4,o4.commands+9);
reject(repair(reordered)); // Per-kind payload order is intact, but painter entries regress.
for(const cut of [o4.triangles,o4.triangles+17,o4.commands+2,v4.length-1])
  reject(repair(Buffer.from(v4.subarray(0,cut))));
for(const position of [o4.triangles+15,o4.commands+5]) {
  const copy=Buffer.from(v4); copy[position]^=1; reject(copy); // Raster evidence is CRC-bound even though frozen inputs exclude it.
}
for(const version of [2,3]) {
  const copy=Buffer.from(v4); copy.writeUInt32LE(version,4); reject(repair(copy));
}
assert.throws(()=>verifyRecordedFingerprint({},f4));
assert.throws(()=>verifyRecordedFingerprint({...f4,frozenScheme:frozenSchemes[3]},f4));
assert.equal(verifyRecordedFingerprint(f4,f4),frozenSchemes[4]);
const cli4=spawnSync(process.execPath,[helper,join(directory,'fixture-x86_64.kfxscene.v4'),'--record'],{input:JSON.stringify(f4),encoding:'utf8'});
assert.equal(cli4.status,0,cli4.stderr); assert.deepEqual(JSON.parse(cli4.stdout),f4);
const v5=await readFile(join(directory,'fixture-x86_64.kfxscene.v5'));
assert.deepEqual(v5,await readFile(join(directory,'fixture-i686.kfxscene.v5')));
const p5=parseSceneCapture(v5),f5=sceneFingerprint(v5),o5=p5.offsets;
assert.equal(f5.frozenScheme,frozenSchemes[5]);
assert.equal(p5.inputEnd,o5.additive+4+65536);
assert.equal(v5.readUInt32LE(o5.excludedShadows),1);
assert.equal(v5.readUInt32LE(o5.shadowMasks),1);assert.equal(v5.readUInt32LE(o5.shadows),2);
assert.deepEqual(Array.from(v5.subarray(o5.shadowMasks+4+8,o5.shadowMasks+4+8+12)),[0,255,0,255,255,0,255,0,0,255,255,0]);
assert.deepEqual(Array.from({length:8},(_,index)=>{
  const start=o5.commands+4+index*5;return [v5[start],v5.readUInt32LE(start+1)];
}),[[1,0],[0,0],[1,1],[2,0],[2,1],[0,1],[1,2],[1,3]]);
for(const field of [32,o5.shadowMasks+4+8]) {
  const copy=Buffer.from(v5);copy[field]^=field === 32 ? 1 : 255;repair(copy);
  assert.deepEqual(sceneFingerprint(copy),f5);assert.notEqual(hash(copy),hash(v5));
}
const movedShadow=Buffer.from(v5);
movedShadow.writeInt32LE(-6,o5.shadows+4+21);movedShadow.writeInt32LE(-6,o5.shadows+4+81+21);repair(movedShadow);
assert.deepEqual(sceneFingerprint(movedShadow),f5);
for(const field of [24,40,o5.palettes,o5.sprites+4+28]) {
  const copy=Buffer.from(v5);copy[field]^=1;repair(copy);assert.notEqual(sceneFingerprint(copy).frozenSha256,f5.frozenSha256);
}
for(const [position,value,type] of [
  [o5.excludedShadows,0],[o5.shadowMasks,32769],[o5.shadowMasks+4,0,'u16'],[o5.shadowMasks+4,257,'u16'],
  [o5.shadowMasks+6,257,'u16'],[o5.shadowMasks+8,11],[o5.shadowMasks+12,1,'byte'],
  [o5.shadows,65537],[o5.shadows+4,0],[o5.shadows+4+12,7],[o5.shadows+4+16,1],
  [o5.shadows+4+20,15,'byte'],[o5.shadows+4+20,32,'byte'],[o5.shadows+4+81+20,24,'byte'],
  [o5.shadows+4+21+16,1],[o5.shadows+4+81+21,100],
  [o5.shadows+4+21+20+8,0xffffffff],[o5.shadows+4+21+20+8,3*65536+1],
  [o5.shadows+4+21+20+12,2*65536+1],[o5.shadows+4+21,16777217],
  [o5.commands+4+3*5,3,'byte'],[o5.commands+4+4*5+1,0],
  [o5.objects+4+51,4,'byte']]) {
  const copy=Buffer.from(v5);
  if(type === 'byte')copy[position]=value;else if(type === 'u16')copy.writeUInt16LE(value,position);else copy.writeUInt32LE(value,position);
  reject(repair(copy));
}
// Valid binary mask dimensions/counts alone can exceed the shared coverage
// budget. Verify that bound specifically, before later payload references.
const maskBlock=8+65536,maskBytes=Buffer.alloc(maskBlock*1024);
for(let mask=0;mask<1024;mask++) {
  maskBytes.writeUInt16LE(256,mask*maskBlock);maskBytes.writeUInt16LE(256,mask*maskBlock+2);
  maskBytes.writeUInt32LE(65536,mask*maskBlock+4);
}
const overBudget=Buffer.concat([v5.subarray(0,o5.shadowMasks+4),maskBytes,v5.subarray(o5.shadows)]);
overBudget.writeUInt32LE(1024,o5.shadowMasks);repair(overBudget);
assert.throws(()=>sceneFingerprint(overBudget),/Combined sprite\/shadow coverage budget exceeded/);rejections++;
for(const cut of [o5.excludedShadows,o5.shadowMasks+9,o5.shadows+40,v5.length-1])reject(repair(Buffer.from(v5.subarray(0,cut))));
for(const version of [3,4]) {const copy=Buffer.from(v5);copy.writeUInt32LE(version,4);reject(repair(copy));}
assert.equal(verifyRecordedFingerprint(f5,f5),frozenSchemes[5]);
assert.throws(()=>verifyRecordedFingerprint({...f5,frozenScheme:frozenSchemes[4]},f5));
const cli5=spawnSync(process.execPath,[helper,join(directory,'fixture-x86_64.kfxscene.v5'),'--record'],{input:JSON.stringify(f5),encoding:'utf8'});
assert.equal(cli5.status,0,cli5.stderr);assert.deepEqual(JSON.parse(cli5.stdout),f5);
const v6=await readFile(join(directory,'fixture-x86_64.kfxscene.v6'));
assert.deepEqual(v6,await readFile(join(directory,'fixture-i686.kfxscene.v6')));
const p6=parseSceneCapture(v6),f6=sceneFingerprint(v6),o6=p6.offsets;
assert.equal(f6.frozenScheme,frozenSchemes[6]);assert.equal(p6.inputEnd,o6.additive+4+65536);
assert.equal(v6[o6.entries+4+5+4],9);assert.equal(v6[o6.objects+4+56+52],6);
assert.equal(v6.readUInt32LE(o6.draws+4+8),3);assert.equal(v6.readUInt32LE(o6.draws+4+327+8),2);
assert.deepEqual(Array.from({length:8},(_,index)=>{
  const start=o6.commands+4+index*5;return [v6[start],v6.readUInt32LE(start+1)];
}),[[1,0],[0,0],[1,1],[2,0],[2,1],[0,1],[1,2],[1,3]]);
for(const [view,mode] of [[0,2],[0,8],[1,5]]) {
  const copy=Buffer.from(v6);copy[o6.camera]=view;copy[o6.camera+61]=mode;repair(copy);
  assert.equal(parseSceneCapture(copy).version,6);
}
for(const [view,mode] of [[2,6],[0,5],[1,2],[1,8]]) {
  const copy=Buffer.from(v6);copy[o6.camera]=view;copy[o6.camera+61]=mode;reject(repair(copy));
}
for(const [position,value,byte] of [[o6.entries+4+5+4,2,true],[o6.objects+4+56+52,2,true],
  [o6.objects+4+56+52,7,true],[o6.draws+4,0],[o6.draws+4+8,99],
  [o6.draws+4+327+12,0],[o6.commands+4+5*5+1,0],[o6.commands+4+5+1,1],[o6.commands,7],[4,10]]) {
  const copy=Buffer.from(v6);if(byte)copy[position]=value;else copy.writeUInt32LE(value,position);reject(repair(copy));
}
// V5's exact same wire shape must reject V6's semantic key permission.
const olderKey=Buffer.from(v6);olderKey.writeUInt32LE(5,4);reject(repair(olderKey));
for(const fixture of [variants.get(3).bytes,v4,v5]) {
  const parsed=parseSceneCapture(fixture),copy=Buffer.from(fixture);
  const entry=fixture.readUInt32LE(parsed.offsets.draws+4+12);
  copy[parsed.offsets.objects+4+52]=6;copy[parsed.offsets.entries+4+entry*5+4]=9;
  copy[parsed.offsets.camera]=0;copy[parsed.offsets.camera+61]=2;repair(copy);
  assert.throws(()=>sceneFingerprint(copy),/Invalid KFXC sprite object\/bucket pair/);rejections++;
}
const crossKindOrder=Buffer.from(v6);
v6.copy(crossKindOrder,o6.commands+9,o6.commands+14,o6.commands+19);
v6.copy(crossKindOrder,o6.commands+14,o6.commands+9,o6.commands+14);
reject(repair(crossKindOrder)); // Sprite and triangle indices stay monotonic, but bucket order regresses.
const missingCommand=Buffer.concat([v6.subarray(0,o6.commands+4+7*5),v6.subarray(o6.commands+4+8*5)]);
missingCommand.writeUInt32LE(7,o6.commands);repair(missingCommand);
assert.throws(()=>sceneFingerprint(missingCommand),/Missing\/duplicate unified raster payload/);rejections++;
for(const field of [32,o6.draws+4+20,o6.shadowMasks+4+8]) {
  const copy=Buffer.from(v6);copy[field]^=field === o6.shadowMasks+4+8 ? 255 : 1;repair(copy);
  assert.deepEqual(sceneFingerprint(copy),f6);
}
const changedOwner=Buffer.from(v6);changedOwner[o6.objects+4+56+12]^=1;repair(changedOwner);
assert.notEqual(sceneFingerprint(changedOwner).frozenSha256,f6.frozenSha256);
assert.equal(verifyRecordedFingerprint(f6,f6),frozenSchemes[6]);
assert.throws(()=>verifyRecordedFingerprint({...f6,frozenScheme:frozenSchemes[5]},f6));
const cli6=spawnSync(process.execPath,[helper,join(directory,'fixture-x86_64.kfxscene.v6'),'--record'],{input:JSON.stringify(f6),encoding:'utf8'});
assert.equal(cli6.status,0,cli6.stderr);assert.deepEqual(JSON.parse(cli6.stdout),f6);
assert.equal(f4.frozenSha256,'67a02a39c82cf3932face769287badb9169320ea0d0a9463c0a4ff0883521e7e');
assert.equal(f5.frozenSha256,'602d68c0066b6f6b336947a6c03877f13da45c499395df203fe3d3cb8ebcd6d3');
assert.equal(f6.frozenSha256,'1f654cbeddecfaffb6e3468971d8914cbea7135d72ac9468f442acac04206388');
const v7=await readFile(join(directory,'fixture-x86_64.kfxscene.v7'));
assert.deepEqual(v7,await readFile(join(directory,'fixture-i686.kfxscene.v7')));
const p7=parseSceneCapture(v7),f7=sceneFingerprint(v7),o7=p7.offsets;
assert.equal(f7.frozenScheme,frozenSchemes[7]);assert.equal(p7.inputEnd,o7.additive+4+65536);
assert.equal(v7[o7.trace+1],1);assert.equal(v7.readUInt32LE(o7.triangles),0);assert.equal(v7.readUInt32LE(o7.commands),0);
assert.equal(v7[o7.outline],1);assert.equal(v7[o7.outline+1],1);assert.equal(v7[o7.outline+2],2);
assert.equal(v7.readUInt32LE(o7.outlineOccluders),3);assert.equal(v7.readUInt32LE(o7.outlineSprites),3);
const firstOccluder=o7.outlineOccluders+4,firstState=o7.outlineSprites+4,firstCommand=o7.outlineCommands+4;
assert.deepEqual(Array.from({length:3},(_,i)=>v7.readFloatLE(firstOccluder+4+i*16+12)),[1,257,65536]);
assert.deepEqual(Array.from({length:8},(_,i)=>v7.readInt32LE(firstState+4+i*4)),[11,21,4,4,0,0,131072,131072]);
assert.equal(v7.readInt32LE(firstState+82+16),2); // Clipped height still retains full-source border coverage.
assert.deepEqual(Array.from({length:6},(_,i)=>{
  const start=firstCommand+i*9;return [v7[start],v7.readUInt32LE(start+1),v7.readUInt32LE(start+5)];
}),[[0,0,0],[1,0,0],[0,1,3],[1,1,3],[1,2,7],[2,2,7]]);
const mutate7=(position,value,type='u32')=>{
  const copy=Buffer.from(v7);
  if(type === 'byte')copy[position]=value;else if(type === 'f32')copy.writeFloatLE(value,position);else copy.writeUInt32LE(value,position);
  reject(repair(copy));
};
for(const [position,value,type] of [
  [4,10],[o7.trace+1,0,'byte'],[o7.outline,0,'byte'],[o7.outline,2,'byte'],[o7.outline+1,0,'byte'],[o7.outline+1,2,'byte'],
  [o7.outline+2,3,'byte'],[o7.outline+3,-.1,'f32'],[o7.outline+3,NaN,'f32'],[o7.outline+7,1025,'f32'],[o7.outline+7,Infinity,'f32'],
  [o7.outlineOccluders,262145],[firstOccluder,1],[firstOccluder+4,67108864,'f32'],[firstOccluder+4,NaN,'f32'],
  [firstOccluder+12,0,'f32'],[firstOccluder+16,0,'f32'],[firstOccluder+16,1.5,'f32'],[firstOccluder+16,4294967296,'f32'],
  [firstOccluder+16,Infinity,'f32'],[o7.outlineSprites,65537],[firstState,3],[firstState+82,0],
  [firstState+36,.1,'f32'],[firstState+40,0,'f32'],[firstState+82+44,1,'f32'],[firstState+52,2048],
  [firstState+56,1],[firstState+60,255,'byte'],[firstState+61,2,'byte'],[firstState+62,1,'byte'],
  [firstState+64,0,'byte'],[firstState+65,3,'byte'],[firstState+66+12,.5,'f32'],[firstState+66,-.1,'f32'],
  [firstState+2*82+66,.5,'f32'],[firstState+2*82+63,0,'byte'],[firstState+2*82+65,0,'byte'],
  [o7.outlineCommands,196609],[firstCommand,3,'byte'],[firstCommand+1,3],[firstCommand+9,0,'byte'],
  [firstCommand+9+5,1],[firstCommand+5*9,1,'byte'],[o7.excludedShadows,1],
  [o7.draws+4,0],[o7.objects+4+50,1,'byte'],[o7.camera,2,'byte'],[o7.draws+4+36,65536]])mutate7(position,value,type);
for(let field=0;field<8;field++)mutate7(firstState+4+field*4,v7.readUInt32LE(firstState+4+field*4)+1);
for(const [a,b] of [[firstCommand,firstCommand+9],[firstCommand+4*9,firstCommand+5*9]]) {
  const copy=Buffer.from(v7);v7.copy(copy,a,b,b+9);v7.copy(copy,b,a,a+9);reject(repair(copy));
}
const removedOutline=Buffer.concat([v7.subarray(0,firstCommand),v7.subarray(firstCommand+9)]);
removedOutline.writeUInt32LE(5,o7.outlineCommands);reject(repair(removedOutline));
const removedDarken=Buffer.from(v7.subarray(0,v7.length-9));removedDarken.writeUInt32LE(5,o7.outlineCommands);reject(repair(removedDarken));
const missingOccluder=Buffer.concat([v7.subarray(0,firstOccluder+2*52),v7.subarray(firstOccluder+3*52)]);
missingOccluder.writeUInt32LE(2,o7.outlineOccluders);reject(repair(missingOccluder));
const wrongGroup=Buffer.from(v7);wrongGroup.writeUInt32LE(0,firstCommand+2*9+5);wrongGroup.writeUInt32LE(0,firstCommand+3*9+5);reject(repair(wrongGroup));
const reversedStates=Buffer.from(v7);v7.copy(reversedStates,firstState,firstState+82,firstState+164);
v7.copy(reversedStates,firstState+82,firstState,firstState+82);reject(repair(reversedStates));
for(const cut of [o7.outline,o7.outline+2,firstOccluder+23,firstState+30,firstCommand+7,v7.length-1])reject(repair(Buffer.from(v7.subarray(0,cut))));
for(const position of [firstOccluder+16,firstState+4,firstCommand+5]) {const copy=Buffer.from(v7);copy[position]^=1;reject(copy);}
for(const version of [3,4,5,6]) {const copy=Buffer.from(v7);copy.writeUInt32LE(version,4);reject(repair(copy));}
// Optional software extension adds only its canonical absence byte to V6.
const software7=Buffer.concat([v6,Buffer.from([0])]);software7.writeUInt32LE(7,4);repair(software7);
assert.equal(parseSceneCapture(software7).version,7);
// Exercise full-source palette-code eligibility rather than only clipped rows.
const instancedGlow=Buffer.from(v7.subarray(0,v7.length-9));instancedGlow.writeUInt32LE(5,o7.outlineCommands);
instancedGlow[firstState+2*82+65]=0;instancedGlow[firstState+2*82+63]=0;repair(instancedGlow);
assert.equal(parseSceneCapture(instancedGlow).version,7);
const clippedGlow=Buffer.from(v7);clippedGlow[firstState+2*82+65]=0;
clippedGlow[o7.sprites+4+28+6]=50;clippedGlow.writeInt32LE(1,o7.draws+4+2*327+60);clippedGlow.writeInt32LE(2,firstState+2*82+16);repair(clippedGlow);
assert.equal(parseSceneCapture(clippedGlow).version,7);
const uncoveredGlow=Buffer.from(clippedGlow);uncoveredGlow[o7.sprites+4+28+7]=0;reject(repair(uncoveredGlow));
const noBorder=Buffer.from(v7);for(const pixel of [1,2,3])noBorder[o7.sprites+4+28+pixel*2+1]=0;
for(let state=0;state<3;state++)noBorder[firstState+state*82+64]=0;repair(noBorder);
assert.equal(parseSceneCapture(noBorder).version,7);
const none=Buffer.concat([v7.subarray(0,firstCommand),v7.subarray(firstCommand+9,firstCommand+18),v7.subarray(firstCommand+27)]);
none[o7.outline+2]=0;none.writeUInt32LE(4,o7.outlineCommands);
for(let state=0;state<3;state++)none.fill(0,firstState+state*82+66,firstState+state*82+82);repair(none);
assert.equal(parseSceneCapture(none).version,7);
// A batch emits all outlines first, then monotonically ordered mains.
const batched=Buffer.from(instancedGlow);batched[firstState+82+65]=0;
for(const [i,[kind,sprite]] of [[0,[0,0]],[1,[0,1]],[2,[1,0]],[3,[1,1]],[4,[1,2]]]) {
  batched[firstCommand+i*9]=kind;batched.writeUInt32LE(sprite,firstCommand+i*9+1);batched.writeUInt32LE(0,firstCommand+i*9+5);
}
repair(batched);assert.equal(parseSceneCapture(batched).version,7);
for(const [a,b] of [[0,1],[2,3]]) {
  const copy=Buffer.from(batched);batched.copy(copy,firstCommand+a*9,firstCommand+b*9,firstCommand+(b+1)*9);
  batched.copy(copy,firstCommand+b*9,firstCommand+a*9,firstCommand+(a+1)*9);reject(repair(copy));
}
for(const [position,value,type] of [[32,20,'u32'],[firstOccluder+4,.25,'f32'],[firstOccluder+16,512,'f32'],
  [o7.outline+7,.5,'f32'],[firstState+66,.25,'f32'],[firstState+48,100,'u32']]) {
  const copy=Buffer.from(v7);if(type === 'f32')copy.writeFloatLE(value,position);else copy.writeUInt32LE(value,position);repair(copy);
  assert.deepEqual(sceneFingerprint(copy),f7);assert.notEqual(hash(copy),hash(v7));
}
for(const position of [24,40,o7.palettes,o7.sprites+4+28]) {
  const copy=Buffer.from(v7);copy[position]^=1;repair(copy);assert.notEqual(sceneFingerprint(copy).frozenSha256,f7.frozenSha256);
}
assert.equal(verifyRecordedFingerprint(f7,f7),frozenSchemes[7]);
assert.throws(()=>verifyRecordedFingerprint({...f7,frozenScheme:frozenSchemes[6]},f7));
const cli7=spawnSync(process.execPath,[helper,join(directory,'fixture-x86_64.kfxscene.v7'),'--record'],{input:JSON.stringify(f7),encoding:'utf8'});
assert.equal(cli7.status,0,cli7.stderr);assert.deepEqual(JSON.parse(cli7.stdout),f7);
// Materialize exact expected IEEE-754 values independently of the parser's
// candidate calculations: round an integer rational to nearest, ties to even.
const floatBits=new DataView(new ArrayBuffer(4));
const fromBits=bits=>{floatBits.setUint32(0,bits);return floatBits.getFloat32(0);};
const toBits=value=>{floatBits.setFloat32(0,value);return floatBits.getUint32(0);};
function rationalFloat(numerator,denominator) {
  if(numerator === 0n)return 0;
  const sign=numerator < 0n ? 0x80000000 : 0;let n=numerator < 0n ? -numerator : numerator,d=denominator,exponent=0;
  while(n < d) {n*=2n;exponent--;}
  while(n >= d*2n) {d*=2n;exponent++;}
  const scaled=n*8388608n,quotient=scaled/d,remainder=scaled%d;
  let significand=quotient+((remainder*2n > d || (remainder*2n === d && quotient%2n !== 0n)) ? 1n : 0n);
  if(significand === 16777216n) {significand/=2n;exponent++;}
  return fromBits((sign|((exponent+127)<<23)|Number(significand-8388608n))>>>0);
}
function floatRational(value) {
  const bits=toBits(value),sign=bits>>>31 ? -1n : 1n,exponent=((bits>>>23)&255)-127-23;
  const mantissa=BigInt((bits&0x7fffff)|0x800000)*sign;
  if(value === 0)return [0n,1n];
  return exponent >= 0 ? [mantissa<<BigInt(exponent),1n] : [mantissa,1n<<BigInt(-exponent)];
}
function subtractOne(value) {const [n,d]=floatRational(value);return rationalFloat(n-d,d);}
function addRational(value,n,d) {const [vn,vd]=floatRational(value);return rationalFloat(vn*d+n*vd,vd*d);}
function adjacentThird(first,second) {
  let value=first;
  do {
    const bits=toBits(value);
    value=value === 0 ? fromBits(1) : fromBits(value > 0 ? bits+1 : bits-1);
  } while(value === first || value === second);
  return value;
}
const bias32=floatRational(Math.fround(.002));
let terrainPolicyDifferences=0,spritePolicyDifferences=0,biasPolicyDifferences=0;
for(let bucket=0;bucket<=4097;bucket++) {
  const n=BigInt(bucket),terrain=[subtractOne(rationalFloat(2n*n,4097n)),rationalFloat(2n*n-4097n,4097n)];
  const sprite=[subtractOne(rationalFloat(2n*n-1n,4097n)),rationalFloat(2n*n-4098n,4097n)];
  terrainPolicyDifferences+=terrain[0] !== terrain[1];spritePolicyDifferences+=sprite[0] !== sprite[1];
  for(let policy=0;policy<2;policy++) {
    const copy=Buffer.from(v7);
    for(let entry=0;entry<5;entry++)copy.writeUInt32LE(bucket,o7.entries+4+entry*5);
    for(let triangle=0;triangle<3;triangle++)for(let vertex=0;vertex<3;vertex++)copy.writeFloatLE(terrain[policy],firstOccluder+triangle*52+4+vertex*16+8);
    const biases=[addRational(sprite[policy],...bias32),addRational(sprite[policy],1n,500n)];
    if(policy === 1)biasPolicyDifferences+=biases[0] !== biases[1];
    for(let state=0;state<3;state++) {
      copy.writeFloatLE(sprite[policy],firstState+state*82+36);copy.writeFloatLE(biases[policy],firstState+state*82+40);
      copy.writeUInt32LE(bucket,firstState+state*82+52);
    }
    repair(copy);assert.equal(parseSceneCapture(copy).version,7,`Rejected exact policy ${policy} at bucket ${bucket}`);
    // The less common alternate bias remains coupled to the captured stored Z.
    if(biases[0] !== biases[1]) {
      const alternate=Buffer.from(copy);for(let state=0;state<3;state++)alternate.writeFloatLE(biases[1-policy],firstState+state*82+40);
      repair(alternate);assert.equal(parseSceneCapture(alternate).version,7);
    }
    if(policy === 1)for(const [offset,a,b] of [[firstOccluder+12,...terrain],[firstState+36,...sprite],[firstState+40,...biases]]) {
      const invalid=Buffer.from(copy);invalid.writeFloatLE(adjacentThird(a,b),offset);reject(repair(invalid));
    }
  }
}
assert.equal(terrainPolicyDifferences,2048);assert.equal(spritePolicyDifferences,3584);assert.equal(biasPolicyDifferences,13);
const targetFixtures=[];
for(const suffix of ['v8','v8-lens']) {
  const bytes8=await readFile(join(directory,`fixture-x86_64.kfxscene.${suffix}`));
  assert.deepEqual(bytes8,await readFile(join(directory,`fixture-i686.kfxscene.${suffix}`)));
  const parsed8=parseSceneCapture(bytes8),fingerprint8=sceneFingerprint(bytes8),o8=parsed8.offsets;
  assert.equal(parsed8.version,8);assert.equal(fingerprint8.frozenScheme,frozenSchemes[8]);
  assert.equal(o8.target,bytes8.length-32);assert.equal(bytes8[o8.outline],0);
  assert.deepEqual(parsed8.target,suffix === 'v8' ? {kind:1,clearIndex:173,lensId:0,width:4096,height:2160,pitch:4112,window:[753,0,3087,2160]}
    : {kind:2,clearIndex:0,lensId:18,width:3072,height:2048,pitch:3072,window:[16,12,3000,2000]});
  const normalized=Buffer.from(bytes8.subarray(0,parsed8.inputEnd));normalized.fill(0,8,16);normalized.fill(0,32,40);
  const expected=createHash('sha256').update(frozenSchemes[8]+'\0').update(normalized).update(bytes8.subarray(o8.target)).digest('hex');
  assert.equal(fingerprint8.frozenSha256,expected);
  assert.notEqual(fingerprint8.frozenSha256,createHash('sha256').update(frozenSchemes[8]+'\0').update(normalized).digest('hex'));
  for(const offset of [32,o8.trace+2,o8.draws+4+20,o8.shadowMasks+12]) {
    const copy=Buffer.from(bytes8);copy[offset]^=offset === o8.shadowMasks+12 ? 255 : 1;repair(copy);
    assert.deepEqual(sceneFingerprint(copy),fingerprint8);assert.notEqual(hash(copy),hash(bytes8));
  }
  for(const [relative,value] of suffix === 'v8' ? [[1,174],[4,4097],[8,2161],[12,4113]] : [[2,1],[2,255]]) {
    const copy=Buffer.from(bytes8);
    if(relative === 1)copy[o8.target+relative]=value;else if(relative === 2)copy.writeUInt16LE(value,o8.target+relative);else copy.writeUInt32LE(value,o8.target+relative);
    repair(copy);assert.notEqual(sceneFingerprint(copy).frozenSha256,fingerprint8.frozenSha256);
  }
  for(const cut of [o8.target,o8.target+1,o8.target+3,o8.target+17,bytes8.length-1])reject(repair(Buffer.from(bytes8.subarray(0,cut))));
  for(const offset of [o8.target,o8.target+1,o8.target+12,bytes8.length-1]) {const copy=Buffer.from(bytes8);copy[offset]^=1;reject(copy);}
  for(const [relative,value,type] of [[0,0,'byte'],[0,3,'byte'],[4,0],[4,16385],[8,0],[8,16385],
    [12,parsed8.target.width-1],[12,16385],[16,0xffffffff],[20,0xffffffff],[24,0],[28,0]]) {
    const copy=Buffer.from(bytes8);if(type === 'byte')copy[o8.target+relative]=value;else copy.writeUInt32LE(value,o8.target+relative);reject(repair(copy));
  }
  const noProof=Buffer.from(bytes8);noProof.fill(0,o8.target);reject(repair(noProof));
  const oldVersion=Buffer.from(bytes8);oldVersion.writeUInt32LE(7,4);reject(repair(oldVersion));
  const cameraMismatch=Buffer.from(bytes8);cameraMismatch.writeInt32LE(parsed8.target.window[0]+1,o8.camera+37);reject(repair(cameraMismatch));
  const cli8=spawnSync(process.execPath,[helper,join(directory,`fixture-x86_64.kfxscene.${suffix}`),'--record'],{input:JSON.stringify(fingerprint8),encoding:'utf8'});
  assert.equal(cli8.status,0,cli8.stderr);assert.deepEqual(JSON.parse(cli8.stdout),fingerprint8);
  assert.equal(verifyRecordedFingerprint(fingerprint8,fingerprint8),frozenSchemes[8]);
  assert.throws(()=>verifyRecordedFingerprint({...fingerprint8,frozenScheme:frozenSchemes[7]},fingerprint8));
  targetFixtures.push({bytes:bytes8,parsed:parsed8,fingerprint:fingerprint8});
}
const screen8=targetFixtures[0],lens8=targetFixtures[1],t8=screen8.parsed.offsets.target;
assert.deepEqual(Array.from(screen8.bytes.subarray(t8)),[1,173,0,0,0,16,0,0,112,8,0,0,16,16,0,0,241,2,0,0,0,0,0,0,15,12,0,0,112,8,0,0]);
const maximum=Buffer.from(screen8.bytes);maximum.writeUInt32LE(16384,t8+4);maximum.writeUInt32LE(16384,t8+12);maximum.writeUInt32LE(4096,t8+8);
assert.equal(parseSceneCapture(repair(maximum)).target.pitch*parseSceneCapture(maximum).target.height,64*1024*1024);
const tooLarge=Buffer.from(maximum);tooLarge.writeUInt32LE(4097,t8+8);reject(repair(tooLarge));
for(const [relative,value] of [[2,1],[16,1010],[20,1]]) {
  const copy=Buffer.from(screen8.bytes);
  if(relative === 2)copy.writeUInt16LE(value,t8+relative);
  else {copy.writeInt32LE(value,t8+relative);copy.writeInt32LE(value,screen8.parsed.offsets.camera+37+(relative-16));}
  reject(repair(copy)); // Window may not spill into pitch padding or outside target height.
}
for(const [relative,value,type] of [[1,1,'byte'],[2,0,'u16'],[2,256,'u16'],[12,3073]]) {
  const copy=Buffer.from(lens8.bytes),offset=lens8.parsed.offsets.target+relative;
  if(type === 'byte')copy[offset]=value;else if(type === 'u16')copy.writeUInt16LE(value,offset);else copy.writeUInt32LE(value,offset);reject(repair(copy));
}
for(const view of [0,1]) {const copy=Buffer.from(lens8.bytes);copy[lens8.parsed.offsets.camera]=view;reject(repair(copy));}
const noLensPossession=Buffer.from(lens8.bytes),noLensTarget=lens8.parsed.offsets.target;
noLensPossession[noLensTarget]=1;noLensPossession[noLensTarget+1]=144;noLensPossession.writeUInt16LE(0,noLensTarget+2);repair(noLensPossession);
assert.equal(parseSceneCapture(noLensPossession).target.kind,1);assert.notEqual(sceneFingerprint(noLensPossession).frozenSha256,lens8.fingerprint.frozenSha256);
const gl8=Buffer.concat([v7,screen8.bytes.subarray(t8)]);gl8.writeUInt32LE(8,4);reject(repair(gl8));
for(const version of [2,3,4,5,6,7]) {
  const previous=version === 2 ? variants.get(2).bytes : version === 3 ? variants.get(3).bytes : version === 4 ? v4 : version === 5 ? v5 : version === 6 ? v6 : v7;
  reject(repair(Buffer.concat([previous,screen8.bytes.subarray(t8)]))); // Earlier wire versions own no target metadata.
}
assert.equal(screen8.fingerprint.frozenSha256,'9e3c1088861b60d4a60afaeb0c061ee1bfff9215dd6b34b7079998506694291d');
assert.equal(lens8.fingerprint.frozenSha256,'efffdd5f8ca4191cdec9e583a882e0ad278324540724f8dab9f2d097e3eea392');
const v9=await readFile(join(directory,'fixture-x86_64.kfxscene.v9'));
assert.deepEqual(v9,await readFile(join(directory,'fixture-i686.kfxscene.v9')));
const p9=parseSceneCapture(v9),f9=sceneFingerprint(v9),o9=p9.offsets;
assert.equal(f9.frozenScheme,frozenSchemes[9]);assert.equal(p9.inputEnd,o9.additive+4+65536);
assert.ok(v9.subarray(o9.target,o9.target+32).every(value=>value === 0));
assert.equal(v9.readUInt32LE(o9.colourRows),2);assert.equal(v9.readUInt32LE(o9.colourBindings),6);
assert.deepEqual(Array.from({length:6},(_,i)=>{const start=o9.colourBindings+4+i*5;return [v9.readUInt32LE(start),v9[start+4]];}),
  [[0,1],[1,2],[0xffffffff,0],[0,3],[0,4],[1,5]]);
assert.deepEqual(Array.from(v9.subarray(o9.colourRows+4,o9.colourRows+8)),[7,255,127,255]);
const normalized9=Buffer.from(v9.subarray(0,p9.inputEnd));normalized9.fill(0,8,16);normalized9.fill(0,32,40);
assert.equal(f9.frozenSha256,createHash('sha256').update(frozenSchemes[9]+'\0').update(normalized9)
  .update(v9.subarray(o9.target,o9.target+32)).update(v9.subarray(o9.colourRows)).digest('hex'));
for(const [offset,value,type] of [[o9.colourRows+4,8,'byte'],[o9.colourBindings+4+5,0],
  [o9.colourBindings+4+5+4,3,'byte']]) {
  const copy=Buffer.from(v9);if(type === 'byte')copy[offset]=value;else copy.writeUInt32LE(value,offset);repair(copy);
  assert.notEqual(sceneFingerprint(copy).frozenSha256,f9.frozenSha256);assert.notEqual(hash(copy),hash(v9));
}
const phase9=Buffer.from(v9),state9=o9.outlineSprites+4,draw9=o9.draws+4;
phase9.writeInt32LE(5,draw9+36);phase9.writeInt32LE(5,state9+12);phase9.writeInt32LE(16384,state9+20);phase9.writeInt32LE(163840,state9+28);
repair(phase9);assert.deepEqual(sceneFingerprint(phase9),f9);assert.notEqual(hash(phase9),hash(v9));
const stalePalette=Buffer.from(v9);stalePalette.fill(0,o9.palettes+768,o9.palettes+1536);
assert.equal(parseSceneCapture(repair(stalePalette)).version,9); // Effective rows need not equal current palette/remap output.
for(const [offset,value,type] of [[4,10],[o9.trace+1,0,'byte'],[o9.outline,0,'byte'],[o9.target,1,'byte'],[o9.target+1,1,'byte'],
  [o9.colourRows,16385],[o9.colourBindings,65537],[o9.colourBindings,5],[o9.colourBindings,7],
  [o9.colourBindings+4,1],[o9.colourBindings+4,2],[o9.colourBindings+4,0xffffffff],
  [o9.colourBindings+4+2*5,0],[o9.colourBindings+4+4,0,'byte'],[o9.colourBindings+4+2*5+4,4,'byte'],
  [o9.colourBindings+4+4,6,'byte'],[o9.colourBindings+4+4,2,'byte'],[o9.colourBindings+4+5+4,1,'byte'],
  [o9.colourBindings+4+5+4,5,'byte'],[o9.colourBindings+4+4*5+4,1,'byte'],[o9.colourBindings+4+5*5+4,3,'byte'],
  [o9.colourRows+4+3,254,'byte'],[o9.draws+4+327+69,0,'byte']]) {
  const copy=Buffer.from(v9);if(type === 'byte')copy[offset]=value;else copy.writeUInt32LE(value,offset);reject(repair(copy));
}
const duplicateRows=Buffer.from(v9);v9.copy(duplicateRows,o9.colourRows+4+1024,o9.colourRows+4,o9.colourRows+4+1024);reject(repair(duplicateRows));
const zeroTexture=Buffer.from(v9);zeroTexture.fill(0,o9.colourRows+4,o9.colourRows+4+1024);reject(repair(zeroTexture));
const zeroRemap=Buffer.from(v9);zeroRemap.fill(0,o9.colourRows+4+1024,o9.colourRows+4+2048);reject(repair(zeroRemap));
const zeroCapacity=Buffer.concat([v9.subarray(0,o9.colourBindings),Buffer.alloc(1024),v9.subarray(o9.colourBindings)]);
zeroCapacity.writeUInt32LE(3,o9.colourRows);zeroCapacity.writeUInt32LE(2,o9.colourBindings+1024+4+3*5);repair(zeroCapacity);
assert.equal(parseSceneCapture(zeroCapacity).version,9);
const partialZero=Buffer.from(zeroCapacity);partialZero[o9.colourBindings]=1;reject(repair(partialZero));
const unusedRow=Buffer.from(zeroCapacity);unusedRow.writeUInt32LE(0,o9.colourBindings+1024+4+3*5);reject(repair(unusedRow));
const zeroIdentity=Buffer.concat([v9.subarray(0,o9.colourBindings),v9.subarray(o9.colourRows+4,o9.colourRows+4+1024),v9.subarray(o9.colourBindings)]);
zeroIdentity.fill(0,o9.colourRows+4,o9.colourRows+4+1024);zeroIdentity.writeUInt32LE(3,o9.colourRows);
zeroIdentity.writeUInt32LE(2,o9.colourBindings+1024+4+4*5);repair(zeroIdentity);assert.equal(parseSceneCapture(zeroIdentity).version,9);
for(const cut of [o9.target+2,o9.colourRows,o9.colourRows+4+1023,o9.colourBindings+2,v9.length-1])reject(repair(Buffer.from(v9.subarray(0,cut))));
for(const position of [o9.colourRows+4,o9.colourBindings+4,v9.length-1]) {const copy=Buffer.from(v9);copy[position]^=1;reject(copy);}
for(const version of [7,8]) {const copy=Buffer.from(v9);copy.writeUInt32LE(version,4);reject(repair(copy));}
const sw9=Buffer.concat([screen8.bytes,Buffer.alloc(8)]);sw9.writeUInt32LE(9,4);reject(repair(sw9));
for(const previous of [variants.get(2).bytes,variants.get(3).bytes,v4,v5,v6,v7,screen8.bytes,lens8.bytes])
  reject(repair(Buffer.concat([previous,v9.subarray(o9.colourRows)]))); // Earlier wire versions own no effective lookup tail.
assert.equal(verifyRecordedFingerprint(f9,f9),frozenSchemes[9]);
assert.throws(()=>verifyRecordedFingerprint({...f9,frozenScheme:frozenSchemes[8]},f9));
const cli9=spawnSync(process.execPath,[helper,join(directory,'fixture-x86_64.kfxscene.v9'),'--record'],{input:JSON.stringify(f9),encoding:'utf8'});
assert.equal(cli9.status,0,cli9.stderr);assert.deepEqual(JSON.parse(cli9.stdout),f9);
assert.equal(f9.frozenSha256,'1907982e137e35550e1d4316d0ebc37bbfba4b3057969dfbc4082a1b88d49ff0');
const v10=await readFile(join(directory,'fixture-x86_64.kfxscene.v10'));
assert.deepEqual(v10,await readFile(join(directory,'fixture-i686.kfxscene.v10')));
const p10=parseSceneCapture(v10),f10=sceneFingerprint(v10),o10=p10.offsets,t10=o10.glWorld;
// Derive the append layout independently of parser offsets from the fixed
// wire record lengths, then check concrete fixture bytes and references.
const light10=t10+17491,tiles10=light10+4+511*511*2,terrain10=tiles10+4+2*1024;
const sources10=terrain10+4+3*64,shadows10=sources10+4+8+12+8+1;
assert.equal(o10.worldSettings,t10+17451);assert.equal(o10.worldLightmap,light10);
assert.equal(o10.worldTiles,tiles10);assert.equal(o10.worldTerrain,terrain10);
assert.equal(o10.worldShadowSources,sources10);assert.equal(o10.worldShadows,shadows10);
assert.equal(v10.length,shadows10+4+3*92);assert.equal(v10.readUInt32LE(light10),261121);
assert.deepEqual(p10.glWorld,{targetKind:0,width:3840,height:2160,window:[0,0,3840,2160],clear:[.125,.25,.75,1],
  settings:[0,.25,1.5,.75,1,2,.5,.625,1],time:0,tiles:2,terrain:3,shadowSources:2,shadows:3});
assert.deepEqual(Array.from(v10.subarray(t10+43,t10+47)),[7,17,255,255]);
assert.equal(v10.readUInt16LE(light10+4+510*2),8670);
assert.deepEqual(Array.from({length:3},(_,i)=>v10.readUInt32LE(terrain10+4+i*64)),[0,0xffffffff,1]);
assert.deepEqual(Array.from({length:3},(_,i)=>[v10.readUInt32LE(shadows10+4+i*92+12),v10.readUInt32LE(shadows10+4+i*92+16)]),[[8,0],[9,1],[10,0]]);
assert.equal(v10.readFloatLE(shadows10+4+20+2*16+8),3/256);
assert.equal(v10.readFloatLE(shadows10+4+20+2*16+12),2/256);
// Independent wire regression for actual Rotable0 frame endpoints46,42 over
// logical45x38 artwork. The full initialized256x256 upload owns zero padding;
// retaining only logical artwork cannot prove the very same UV samples safe.
function paddedShadowWire(width,height) {
  const header=Buffer.alloc(8),coverage=Buffer.alloc(width*height);
  header.writeUInt16LE(width,0);header.writeUInt16LE(height,2);header.writeUInt32LE(coverage.length,4);
  for(let y=0;y<38;y++)for(let x=0;x<45;x++)coverage[y*width+x]=255;
  const copy=Buffer.concat([v10.subarray(0,sources10+4),header,coverage,v10.subarray(sources10+4+8+12)]);
  const shadows=sources10+4+8+coverage.length+8+1,vertices=shadows+4+20;
  for(const vertex of [1,2])copy.writeFloatLE(46/256,vertices+vertex*16+8);
  for(const vertex of [2,3])copy.writeFloatLE(42/256,vertices+vertex*16+12);
  return {bytes:repair(copy),shadows,coverage};
}
const paddedShadow=paddedShadowWire(256,256),paddedParsed=parseSceneCapture(paddedShadow.bytes);
assert.equal(paddedParsed.offsets.worldShadows,paddedShadow.shadows);
assert.equal(paddedShadow.coverage[37*256+44],255);assert.equal(paddedShadow.coverage[42*256+46],0);
assert.equal(paddedShadow.coverage.reduce((n,p)=>n+(p===255),0),45*38);
assert.equal(paddedParsed.glWorld.shadowSources,2);assert.equal(paddedParsed.glWorld.shadows,3);
assert.equal(sceneFingerprint(paddedShadow.bytes).frozenSha256,f10.frozenSha256);
assert.notEqual(sceneFingerprint(paddedShadow.bytes).effectiveWorldResourcesSha256,f10.effectiveWorldResourcesSha256);
reject(paddedShadowWire(45,38).bytes);
for(const field of [8,12]) {
  const outside=Buffer.from(paddedShadow.bytes);outside.writeFloatLE(256/256,paddedShadow.shadows+4+20+2*16+field);reject(repair(outside));
}
const norm10=Buffer.from(v10.subarray(0,p10.inputEnd));norm10.fill(0,8,16);norm10.fill(0,32,40);
assert.equal(f10.frozenSha256,createHash('sha256').update(frozenSchemes[10]+'\0').update(norm10)
  .update(v10.subarray(o10.target,o10.target+32)).update(v10.subarray(o10.colourRows,t10)).digest('hex'));
assert.equal(f10.effectiveWorldResourcesScheme,'kfxc-v10-effective-world-resources-1');
assert.equal(f10.appliedWorldStateScheme,'kfxc-v10-applied-world-state-1');
assert.equal(f10.effectiveWorldResourcesSha256,createHash('sha256').update(f10.effectiveWorldResourcesScheme+'\0')
  .update(v10.subarray(t10+2,t10+17451)).update(v10.subarray(light10,terrain10)).update(v10.subarray(sources10,shadows10)).digest('hex'));
assert.equal(f10.appliedWorldStateSha256,createHash('sha256').update(f10.appliedWorldStateScheme+'\0')
  .update(v10.subarray(t10+17451,light10)).digest('hex'));
const edit10=(offset,value,type='u32')=>{
  const copy=Buffer.from(v10);
  if(type === 'byte')copy[offset]=value;else if(type === 'u16')copy.writeUInt16LE(value,offset);
  else if(type === 'float')copy.writeFloatLE(value,offset);else copy.writeUInt32LE(value,offset);
  return repair(copy);
};
// No actual resource bytes vanish under the scene-identity exclusions.
for(const [offset,value,type] of [[t10+3,3841],[t10+27,.5,'float'],[t10+43,8,'byte'],[o10.worldFade,255,'byte'],
  [light10+4,65535,'u16'],[tiles10+4,1,'byte'],[sources10+12,255,'byte']]) {
  const copy=edit10(offset,value,type),fp=sceneFingerprint(copy);
  assert.equal(fp.frozenSha256,f10.frozenSha256);assert.equal(fp.appliedWorldStateSha256,f10.appliedWorldStateSha256);
  assert.notEqual(fp.effectiveWorldResourcesSha256,f10.effectiveWorldResourcesSha256);assert.notEqual(hash(copy),hash(v10));
  assert.throws(()=>verifyRecordedFingerprint(f10,fp));
}
const state10=sceneFingerprint(edit10(o10.worldSettings+4,.5,'float'));
assert.equal(state10.frozenSha256,f10.frozenSha256);assert.equal(state10.effectiveWorldResourcesSha256,f10.effectiveWorldResourcesSha256);
assert.notEqual(state10.appliedWorldStateSha256,f10.appliedWorldStateSha256);assert.throws(()=>verifyRecordedFingerprint(f10,state10));
for(const [offset,value,type] of [[terrain10+4+12,.75,'float'],[terrain10+4+64,1],[shadows10+4+88,.4,'float'],[32,20]]) {
  const copy=edit10(offset,value,type);assert.deepEqual(sceneFingerprint(copy),f10);assert.notEqual(hash(copy),hash(v10));
}
const colours10=sceneFingerprint(edit10(o10.colourRows+4,8,'byte'));
assert.notEqual(colours10.frozenSha256,f10.frozenSha256);
assert.equal(colours10.effectiveWorldResourcesSha256,f10.effectiveWorldResourcesSha256);
assert.equal(colours10.appliedWorldStateSha256,f10.appliedWorldStateSha256);
// A shadow-disabled execution retains the same owned scene but has no actual
// uploaded masks: the separate resource hash must expose that change.
const shadowOff10=Buffer.concat([v10.subarray(0,sources10),Buffer.alloc(8)]);
for(const entry of [8,9,10])shadowOff10[o10.entries+4+entry*5+4]=255;
repair(shadowOff10);const off10=sceneFingerprint(shadowOff10);
assert.equal(off10.frozenSha256,f10.frozenSha256);assert.equal(off10.appliedWorldStateSha256,f10.appliedWorldStateSha256);
assert.notEqual(off10.effectiveWorldResourcesSha256,f10.effectiveWorldResourcesSha256);
assert.equal(parseSceneCapture(shadowOff10).glWorld.shadows,0);
assert.equal(verifyRecordedFingerprint(f10,f10),frozenSchemes[10]);
for(const name of ['effectiveWorldResources','appliedWorldState']) {
  for(const field of ['Scheme','Sha256']) {const incomplete={...f10};delete incomplete[name+field];assert.throws(()=>verifyRecordedFingerprint(incomplete,f10));}
  assert.throws(()=>verifyRecordedFingerprint({...f10,[name+'Sha256']:'0'.repeat(64)},f10));
  assert.throws(()=>verifyRecordedFingerprint({...f10,[name+'Scheme']:'other-domain'},f10));
}
const bad10=[[4,11],[o10.trace+1,0,'byte'],[o10.target,1,'byte'],[t10,0,'byte'],[t10,2,'byte'],[t10+1,0,'byte'],[t10+1,2,'byte'],
  [t10+2,2,'byte'],[t10+2,1,'byte'],[t10+3,0],[t10+3,16385],[t10+7,16385],[t10+11,0xffffffff],[t10+19,0],[t10+19,3839],
  [t10+27,-.01,'float'],[t10+31,1.01,'float'],[t10+35,Infinity,'float'],[t10+39,0,'float'],[t10+46,254,'byte'],
  [o10.worldSettings+36,.5,'float'],[light10,261120],[light10,261122],[tiles10,65537],[terrain10,262145],[terrain10,2],
  [terrain10+4,1],[terrain10+4+2*64,2],[terrain10+4+2*64,0xffffffff],
  [terrain10+8,32769,'float'],[terrain10+12,-32769,'float'],[terrain10+16,1025,'float'],[terrain10+20,-1.01,'float'],[terrain10+24,511.01,'float'],
  [sources10,65537],[sources10+4,0,'u16'],[sources10+6,257,'u16'],[sources10+8,11],[sources10+8,65537],[sources10+12,1,'byte'],
  [shadows10,65537],[shadows10,2],[shadows10+4,0],[shadows10+12,99],[shadows10+16,7],[shadows10+16+92,8],
  [shadows10+20,1],[shadows10+20,2],[shadows10+88,0,'float'],[shadows10+92,.19,'float'],[shadows10+92,.56,'float'],
  [shadows10+24,16777218,'float'],[shadows10+28,NaN,'float'],[shadows10+32,-.001,'float'],[shadows10+36+2*16,3/256,'float'],
  [o10.objects+4+51,4,'byte']];
const badSettings10=[.5,-.01,.09,3.01,2,3,5.01,1.01,.5];
for(let field=0;field<9;field++)bad10.push([o10.worldSettings+field*4,badSettings10[field],'float'],[o10.worldSettings+field*4,NaN,'float']);
for(const offset of [terrain10+8,terrain10+12,terrain10+16,terrain10+20,terrain10+24])bad10.push([offset,Infinity,'float']);
for(const args of bad10)reject(edit10(...args));
const duplicateTile10=Buffer.from(v10);v10.copy(duplicateTile10,tiles10+4+1024,tiles10+4,tiles10+4+1024);reject(repair(duplicateTile10));
const reorderedShadow10=Buffer.from(v10);v10.copy(reorderedShadow10,shadows10+4,shadows10+4+92,shadows10+4+2*92);
v10.copy(reorderedShadow10,shadows10+4+92,shadows10+4,shadows10+4+92);reject(repair(reorderedShadow10));
const duplicateSource10=Buffer.concat([v10.subarray(0,sources10+4+8+12),v10.subarray(sources10+4,sources10+4+8+12),v10.subarray(shadows10)]);
reject(repair(duplicateSource10));
const overTarget10=edit10(t10+3,4097);overTarget10.writeUInt32LE(4097,t10+7);reject(repair(overTarget10));
// A legal old texture span can still exceed the combined v10 owned budget.
const overAssets10=Buffer.concat([v10.subarray(0,o10.textures),Buffer.alloc(4),Buffer.alloc(64*1024*1024),
  v10.subarray(o10.textures+4+v10.readUInt32LE(o10.textures))]);
overAssets10.writeUInt32LE(64*1024*1024,o10.textures);reject(repair(overAssets10));
const lens10=Buffer.from(v10);lens10[t10+2]=1;lens10[o10.camera]=2;
for(let i=0;i<v10.readUInt32LE(o10.outlineSprites);i++) {const start=o10.outlineSprites+4+i*82;lens10[start+61]=0;lens10.fill(0,start+66,start+82);}
const phases10=[];for(let i=0;i<v10.readUInt32LE(o10.outlineCommands);i++) {
  const start=o10.outlineCommands+4+i*9;if(lens10[start] !== 0)phases10.push(lens10.subarray(start,start+9));
}
const lensWire10=Buffer.concat([lens10.subarray(0,o10.outlineCommands),Buffer.alloc(4),...phases10,lens10.subarray(o10.outlineCommands+4+9*v10.readUInt32LE(o10.outlineCommands))]);
lensWire10.writeUInt32LE(phases10.length,o10.outlineCommands);assert.equal(parseSceneCapture(repair(lensWire10)).glWorld.targetKind,1);
for(const cut of [t10,t10+17490,light10+9,tiles10+7,terrain10+63,sources10+11,shadows10+91,v10.length-1])reject(repair(Buffer.from(v10.subarray(0,cut))));
for(const position of [t10+43,light10+4,tiles10+4,sources10+12,v10.length-1]) {const copy=Buffer.from(v10);copy[position]^=1;reject(copy);}
for(const previous of [variants.get(2).bytes,variants.get(3).bytes,v4,v5,v6,v7,screen8.bytes,lens8.bytes,v9])
  reject(repair(Buffer.concat([previous,v10.subarray(t10)])));
const cli10=spawnSync(process.execPath,[helper,join(directory,'fixture-x86_64.kfxscene.v10'),'--record'],{input:JSON.stringify(f10),encoding:'utf8'});
assert.equal(cli10.status,0,cli10.stderr);assert.deepEqual(JSON.parse(cli10.stdout),f10);
const v11=await readFile(join(directory,'fixture-x86_64.kfxscene.v11'));
assert.deepEqual(v11,await readFile(join(directory,'fixture-i686.kfxscene.v11')));
const p11=parseSceneCapture(v11),f11=sceneFingerprint(v11),o11=p11.offsets,c11=o11.composition;
assert.equal(c11,v10.length);assert.deepEqual(v11.subarray(24,c11),v10.subarray(24));
assert.equal(f11.version,11);assert.equal(f11.frozenScheme,f10.frozenScheme);assert.equal(f11.frozenSha256,f10.frozenSha256);
for(const field of ['effectiveWorldResourcesScheme','effectiveWorldResourcesSha256','appliedWorldStateScheme','appliedWorldStateSha256'])assert.equal(f11[field],f10[field]);
assert.deepEqual(p11.composition.phaseCounts,[3,1,1]);assert.deepEqual(p11.composition.overlays,{native:[3840,2160],sources:2,colourRows:2,fadeBytes:16384,draws:5});
assert.equal(p11.composition.lens.operation,0);assert.equal(p11.composition.lens.requestedActive,false);
assert.deepEqual(o11.overlaySourceRecords,[c11+43,c11+79]);
assert.deepEqual(Array.from(v11.subarray(c11+55,c11+71)),[0,1,2,3,4,5,6,7,8,9,10,11,0,0,0,0]);
assert.deepEqual(Array.from(v11.subarray(c11+75,c11+79)),[1,1,1,0]);
assert.deepEqual(Array.from({length:5},(_,i)=>[v11[o11.overlayDraws+4+i*81],v11[o11.overlayDraws+5+i*81],v11[o11.overlayDraws+6+i*81]]),[[0,0,1],[0,1,1],[0,2,1],[1,3,0],[2,4,1]]);
assert.equal(v11.readFloatLE(o11.overlayDraws+4+4*81+40),Math.fround(.49));
assert.equal(v11.readUInt32LE(o11.overlayDraws+4+2*81+72),0xffffffff);
// Hash construction is independent of the parser's range arrays. Dynamic UVs,
// ordering, lens state and upload stamps stay distinct from owned resources.
const resourceRanges11=[[o11.overlaySources,o11.overlayDraws],[o11.lensRemapDims,o11.lensOverlayDims],
  [o11.lensOverlayDims,o11.lensOverlayAlpha],[o11.lensOverlay,v11.length]];
const stateRanges11=[[c11,o11.overlaySources],[o11.overlayDraws,o11.lensRemapDims],[o11.lensOverlayAlpha,o11.lensOverlay]];
const rangeHash=(scheme,bytes,ranges)=>{const h=createHash('sha256').update(scheme+'\0');for(const [a,b] of ranges)h.update(bytes.subarray(a,b));return h.digest('hex');};
assert.equal(f11.compositionResourcesScheme,'kfxc-v11-composition-resources-1');
assert.equal(f11.compositionStateScheme,'kfxc-v11-composition-state-1');
assert.equal(f11.compositionResourcesSha256,rangeHash(f11.compositionResourcesScheme,v11,resourceRanges11));
assert.equal(f11.compositionStateSha256,rangeHash(f11.compositionStateScheme,v11,stateRanges11));
const edit11=(bytes,offset,value,type='u32')=>{const copy=Buffer.from(bytes);
  if(type==='byte')copy[offset]=value;else if(type==='float')copy.writeFloatLE(value,offset);else if(type==='u16')copy.writeUInt16LE(value,offset);
  else if(type==='u64')copy.writeBigUInt64LE(BigInt(value),offset);else copy.writeUInt32LE(value,offset);return repair(copy);};
const assertScope11=(bytes,baseline,scope)=>{const fp=sceneFingerprint(bytes);assert.equal(fp.frozenSha256,baseline.frozenSha256);
  assert.equal(fp.effectiveWorldResourcesSha256,baseline.effectiveWorldResourcesSha256);assert.equal(fp.appliedWorldStateSha256,baseline.appliedWorldStateSha256);
  assert.notEqual(fp[scope+'Sha256'],baseline[scope+'Sha256']);assert.equal(fp[(scope==='compositionResources'?'compositionState':'compositionResources')+'Sha256'],baseline[(scope==='compositionResources'?'compositionState':'compositionResources')+'Sha256']);
  assert.throws(()=>verifyRecordedFingerprint(baseline,fp));};
for(const [offset,value] of [[c11+55,255],[o11.overlayColourRows+4,19],[o11.overlayFade+4,201]])assertScope11(edit11(v11,offset,value,'byte'),f11,'compositionResources');
for(const [offset,value,type] of [[o11.overlayDraws+4+20,-1.25,'float'],[o11.overlayDraws+4+56,.125,'float'],
  [o11.overlayDraws+4+4*81+2,0,'byte'],[o11.overlayDraws+4+4*81+76,1]]) {
  assertScope11(edit11(v11,offset,value,type),f11,'compositionState');
}
const phaseEndpoint11=edit11(v11,o11.overlayDraws+4+44,Math.fround(.48),'float');assertScope11(phaseEndpoint11,f11,'compositionState');
assert.equal(verifyRecordedFingerprint(f11,f11),f10.frozenScheme);
for(const name of ['compositionResources','compositionState']) {
  for(const suffix of ['Scheme','Sha256']) {const incomplete={...f11};delete incomplete[name+suffix];assert.throws(()=>verifyRecordedFingerprint(incomplete,f11));}
  assert.throws(()=>verifyRecordedFingerprint({...f11,[name+'Sha256']:'0'.repeat(64)},f11));
  assert.throws(()=>verifyRecordedFingerprint({...f11,[name+'Scheme']:'incorrect-domain'},f11));
}
const q11=o11.overlayDraws+4,s11=o11.overlaySourceRecords[0];
const invalid11=[[4,12],[c11,0,'byte'],[c11,2,'byte'],[c11+1,0,'byte'],[c11+1,2,'byte'],[c11+2,7,'byte'],[c11+2,31,'byte'],[c11+3,4],
  [c11+15,0,'float'],[c11+27,.5,'float'],[o11.overlayNative,3839],[o11.overlaySources,257],[s11,0],[s11+4,16385],
  [s11+8,67108865],[s11+28,16385],[s11+35,2,'byte'],[s11+27,1,'byte'],[o11.overlayColourRows,16385],[o11.overlayFade,16385],
  [o11.overlayDraws,262145],[q11,3,'byte'],[q11+1,5,'byte'],[q11+2,0,'byte'],[q11+2,2,'byte'],[q11+3,2,'byte'],[q11+4,1],
  [q11+20,32769,'float'],[q11+28,-2.5,'float'],[q11+24,NaN,'float'],[q11+52,2.01,'float'],[q11+56,-.01,'float'],[q11+68,1.01,'float'],
  [q11+36,Infinity,'float'],[q11+48,.75,'float'],[q11+72,1],[q11+72,2],[q11+76,1],[q11+76,2],[q11+80,1,'byte'],
  [q11+81+76,0],[q11+2*81+72,0],[q11+2*81+40,.25,'float'],[q11+3*81+2,1,'byte'],[q11+3*81+12,0xffffffff],[q11+3*81+80,64,'byte'],
  [q11+4*81+40,1,'float'],[o11.lens,1,'byte'],[o11.lens+1,1,'byte'],[o11.lens+2,6,'byte'],[o11.lens+3,1,'byte'],[o11.lens+4,1,'byte'],
  [o11.lensStamps,1,'u64'],[o11.lensPass+1,1],[o11.lensMist,.5,'float'],[o11.lensRemapDims,1],[o11.lensOverlayAlpha,1,'u16']];
for(const args of invalid11) {assert.throws(()=>sceneFingerprint(edit11(v11,...args)),undefined,`V11 mutation ${JSON.stringify(args)}`);rejections++;}
const duplicateRow11=Buffer.from(v11);v11.copy(duplicateRow11,o11.overlayColourRows+4+1024,o11.overlayColourRows+4,o11.overlayColourRows+4+1024);reject(repair(duplicateRow11));
const duplicateSource11=Buffer.concat([v11.subarray(0,o11.overlaySourceRecords[1]),v11.subarray(s11,s11+36),v11.subarray(o11.overlayColourRows)]);
reject(repair(duplicateSource11));
const unusedSource11=Buffer.from(v11);unusedSource11.writeUInt32LE(0,q11+3*81+72);reject(repair(unusedSource11));
const unusedRow11=Buffer.from(v11);unusedRow11.writeUInt32LE(0,q11+3*81+76);reject(repair(unusedRow11));
const reordered11=Buffer.from(v11);v11.copy(reordered11,q11+3*81,q11+4*81,q11+5*81);v11.copy(reordered11,q11+4*81,q11+3*81,q11+4*81);reject(repair(reordered11));
const edgeProof11=Buffer.from(v11);edgeProof11.fill(0,s11+20,s11+24);edgeProof11[s11+34]=0;
const adjacentHalf11=Buffer.alloc(4);adjacentHalf11.writeFloatLE(.5);adjacentHalf11.writeUInt32LE(adjacentHalf11.readUInt32LE()-1);
edgeProof11.writeFloatLE(adjacentHalf11.readFloatLE(),q11+48);reject(repair(edgeProof11));
adjacentHalf11.writeUInt32LE(adjacentHalf11.readUInt32LE()-1);edgeProof11.writeFloatLE(adjacentHalf11.readFloatLE(),q11+48);
assert.equal(parseSceneCapture(repair(edgeProof11)).composition.overlays.draws,5);
const signedZero11=Buffer.from(v11);signedZero11.writeFloatLE(0,o11.glWorld+27);signedZero11.writeFloatLE(-0,c11+15);
assert.equal(parseSceneCapture(repair(signedZero11)).composition.screenClear[0],-0);
for(const cut of [c11,c11+54,s11+19,o11.overlayColourRows+1023,o11.overlayFade+16383,q11+80,o11.lens+100,v11.length-1])reject(repair(Buffer.from(v11.subarray(0,cut))));
for(const pos of [c11+15,s11+12,o11.overlayColourRows+4,o11.overlayFade+4,v11.length-1]) {const bad=Buffer.from(v11);bad[pos]^=1;reject(bad);}
const expectedBranches11=[['empty',0,0,false],['copy',1,0,true],['fallback',1,2,true],['remap',2,3,true],['mist',3,1,true],['overlay',4,4,true],['not-drawn',5,4,false],['not-drawn-source',5,0,true]];
for(const [suffix,operation,requested,began] of expectedBranches11) {
  const bytes=await readFile(join(directory,'fixture-x86_64.kfxscene.v11-'+suffix));
  assert.deepEqual(bytes,await readFile(join(directory,'fixture-i686.kfxscene.v11-'+suffix)));
  const parsed=parseSceneCapture(bytes),fp=sceneFingerprint(bytes),o=parsed.offsets,l=parsed.composition.lens;
  assert.equal(l.operation,operation);assert.equal(l.requestedType,requested);assert.equal(l.captureBegan,began);
  assert.equal(l.fallbackCopy,suffix==='fallback');assert.equal(verifyRecordedFingerprint(fp,fp),f10.frozenScheme);
  if(operation===0||operation===5) {
    assert.deepEqual(l.sourceExtent,[0,0]);assert.ok(l.resources.every(s=>s.generation==='0'&&s.revision==='0'));
    reject(edit11(bytes,o.lensPass+1,1));reject(edit11(bytes,o.lensStamps,1,'u64'));reject(edit11(bytes,o.lens+4,1,'byte'));
    continue;
  }
  assert.deepEqual(l.sourceExtent,[3840,2160]);assert.deepEqual(l.sourceOrigin,[7,0]);assert.deepEqual(l.destination,[7,11]);assert.deepEqual(l.viewport,[64,48]);
  const bad=[[o.lens,0,'byte'],[o.lens+1,0,'byte'],[o.lens+2,6,'byte'],[o.lens+3,5,'byte'],[o.lens+4,Number(!l.fallbackCopy),'byte'],
    [o.lensPass,4,'byte'],[o.lensPass+1,3839],[o.lensPass+5,16385],[o.lensPass+9,8],[o.lensPass+13,1],
    [o.lensPass+21,2159],[o.lensPass+29,0xffffffff],[o.lensPass+33,0],[o.lensPass+37,2160],
    [o.lensRemap,16777217],[o.lensOverlay,67108865],[o.lensDensity,65537],[o.lensPalette,1025],[o.lensFade,16385],[o.lensRgb6Lookup,262145]];
  for(let i=0;i<6;i++)for(const [field,off] of [['generation',0],['revision',8]])bad.push([o.lensStamps+i*16+off,l.resources[i][field]==='0'?1:0,'u64']);
  const largeStamp11=Buffer.from(bytes);
  for(let i=0;i<6;i++)if(l.resources[i].generation!=='0') {
    largeStamp11.writeBigUInt64LE(0xffffffffffffffffn,o.lensStamps+i*16);largeStamp11.writeBigUInt64LE(0xffffffffffffffffn,o.lensStamps+i*16+8);
  }
  const largeResources11=parseSceneCapture(repair(largeStamp11)).composition.lens.resources;
  assert.ok(largeResources11.every((stamp,i)=>stamp.generation===(l.resources[i].generation==='0'?'0':'18446744073709551615')));
  if(operation===1)for(let request=0;request<=4;request++) {
    const actual=edit11(bytes,o.lens+3,request,'byte');actual[o.lens+4]=Number(request!==0);
    assert.equal(parseSceneCapture(repair(actual)).composition.lens.fallbackCopy,request!==0);
  }
  if(operation===2) {
    assert.deepEqual(l.remapExtent,[2,2]);assert.equal(l.remapPixels,4);assert.equal(bytes.readUInt16LE(o.lensRemap+8),65535);
    bad.push([o.lens+3,1,'byte'],[o.lensRemapDims,0],[o.lensRemap,3],[o.lensRemap,16777217]);
    assert.equal(parseSceneCapture(edit11(bytes,o.lens+3,2,'byte')).composition.lens.operation,2);
    assertScope11(edit11(bytes,o.lensRemap+4,1234,'u16'),fp,'compositionResources');
    assertScope11(edit11(bytes,o.lensStamps+4*16+8,20,'u64'),fp,'compositionState');
  } else bad.push([o.lensRemapDims,1]);
  if(operation===3) {
    assert.deepEqual(l.mistOffsets,[255.75,0,.5,128.125]);assert.equal(l.lightness,63);assert.equal(l.densityBytes,65536);
    bad.push([o.lens+3,2,'byte'],[o.lensMist,256,'float'],[o.lensMist+4,-.01,'float'],[o.lensMist+8,NaN,'float'],[o.lensMist+16,64,'byte'],
      [o.lensDensity,65535],[o.lensFade,16383]);
    assertScope11(edit11(bytes,o.lensMist,.5,'float'),fp,'compositionState');
    for(const offset of [o.lensDensity+4,o.lensPalette+4,o.lensFade+4,o.lensRgb6Lookup+4])assertScope11(edit11(bytes,offset,201,'byte'),fp,'compositionResources');
  } else bad.push([o.lensMist,.5,'float'],[o.lensMist+16,1,'byte']);
  if(operation===4) {
    assert.deepEqual(l.overlayExtent,[3,2]);assert.equal(l.overlayAlpha,256);assert.equal(l.overlayBytes,6);
    bad.push([o.lens+3,3,'byte'],[o.lensOverlayDims,16385],[o.lensOverlay,5],[o.lensOverlayAlpha,257,'u16']);
    assertScope11(edit11(bytes,o.lensOverlayAlpha,128,'u16'),fp,'compositionState');
    assertScope11(edit11(bytes,o.lensOverlay+4,201,'byte'),fp,'compositionResources');
    assert.equal(parseSceneCapture(edit11(bytes,o.lensOverlayAlpha,0,'u16')).composition.lens.overlayAlpha,0);
  } else bad.push([o.lensOverlayAlpha,1,'u16']);
  if(operation===3||operation===4)bad.push([o.lensPalette,1023],[o.lensRgb6Lookup,262143]);else bad.push([o.lensPalette,1]);
  for(const args of bad)reject(edit11(bytes,...args));
}
const empty11=await readFile(join(directory,'fixture-x86_64.kfxscene.v11-empty'));
assert.equal(empty11.length,c11+256);assert.deepEqual(parseSceneCapture(empty11).composition.phaseCounts,[0,0,0]);
{
  const combined=await readFile(join(directory,'fixture-x86_64.kfxscene.v11-budget'));
  assert.deepEqual(combined,await readFile(join(directory,'fixture-i686.kfxscene.v11-budget')));
  const parsed=parseSceneCapture(combined),offsets=parsed.offsets,fp=sceneFingerprint(combined);
  assert.ok(combined.length>64*1024*1024&&combined.length<96*1024*1024);
  assert.equal(combined.readUInt32LE(offsets.overlaySourceRecords[0]+8),32*1024*1024);
  assert.deepEqual(parsed.composition.lens.remapExtent,[3072,3072]);assert.equal(parsed.composition.lens.remapPixels*4,36*1024*1024);
  const standard=sceneFingerprint(await readFile(join(directory,'fixture-x86_64.kfxscene.v11-remap')));
  assert.equal(fp.frozenSha256,standard.frozenSha256);assert.equal(fp.compositionStateSha256,standard.compositionStateSha256);
  assert.notEqual(fp.compositionResourcesSha256,standard.compositionResourcesSha256);
  // Both 48 MiB children are individually valid; mandatory world/resources
  // push their actual aggregate above96 while the wire remains below128.
  const sourceHeader=Buffer.alloc(12);sourceHeader.writeUInt32LE(12288,0);sourceHeader.writeUInt32LE(4096,4);sourceHeader.writeUInt32LE(48*1024*1024,8);
  const proofCount=Buffer.alloc(4);proofCount.writeUInt32LE(4096);
  const remapHeader=Buffer.alloc(12);remapHeader.writeUInt32LE(12288,0);remapHeader.writeUInt32LE(1024,4);remapHeader.writeUInt32LE(12288*1024,8);
  const over96=Buffer.concat([combined.subarray(0,offsets.overlaySourceRecords[0]),sourceHeader,Buffer.alloc(48*1024*1024),proofCount,Buffer.alloc(4096,1),
    combined.subarray(offsets.overlaySourceRecords[1],offsets.lensRemapDims),remapHeader,Buffer.alloc(48*1024*1024),combined.subarray(offsets.lensOverlayDims)]);
  assert.ok(over96.length<128*1024*1024);assert.throws(()=>parseSceneCapture(repair(over96)),/Shared V11 composition owned-resource budget exceeded/);rejections++;
}
{
  const indexed=await readFile(join(directory,'fixture-x86_64.kfxscene.v11-overlay')),offsets=parseSceneCapture(indexed).offsets;
  const header=Buffer.alloc(14);header.writeUInt32LE(8192,0);header.writeUInt32LE(8192,4);header.writeUInt16LE(128,8);header.writeUInt32LE(64*1024*1024,10);
  const overLens=Buffer.concat([indexed.subarray(0,offsets.lensOverlayDims),header,Buffer.alloc(64*1024*1024),indexed.subarray(offsets.lensDensity)]);
  // 64 MiB image plus mandatory palette/RGB6 crosses the lens CHILD limit,
  // although the complete composition would still fit96.
  assert.ok(overLens.length<96*1024*1024);assert.throws(()=>parseSceneCapture(repair(overLens)),/Lens auxiliary-resource budget exceeded/);rejections++;
}
const overAssets11=Buffer.concat([v11.subarray(0,o11.textures),Buffer.alloc(4),Buffer.alloc(64*1024*1024),v11.subarray(o11.textures+4+v11.readUInt32LE(o11.textures))]);
overAssets11.writeUInt32LE(64*1024*1024,o11.textures);reject(repair(overAssets11));
for(const previous of [v9,v10])reject(repair(Buffer.concat([previous,v11.subarray(c11)])));
const cli11=spawnSync(process.execPath,[helper,join(directory,'fixture-x86_64.kfxscene.v11'),'--record'],{input:JSON.stringify(f11),encoding:'utf8'});
assert.equal(cli11.status,0,cli11.stderr);assert.deepEqual(JSON.parse(cli11.stdout),f11);
console.log(`Frozen fingerprint tests passed: historical identities, v3-v11 phase/target/effective-colour/composition isolation, GL world and composition resource/state scopes, exact x87/SSE rules, canonical bounds, cross-architecture/CLI and ${rejections} malformed captures.`);
