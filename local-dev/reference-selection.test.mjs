// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';
import {inspectSelectionTrace} from './reference-selection.mjs';

const bytes = Buffer.alloc(4 + 2*5 + 4 + 2*81);
const parsed = {version:11,offsets:{entries:0,overlayDraws:14}};
bytes.writeUInt32LE(2,0); bytes[8]=4; bytes[13]=2;
bytes.writeUInt32LE(2,14); bytes[18]=0; bytes[19]=2; bytes[99]=2; bytes[100]=2;
const metadata={selector:{active:1,slabX:63,slabY:63,width:1,height:1,valid:1,invalid:0,colour:1}};
const expected={active:1,valid:1,invalid:0,selectorEntries:1,depthSolidQuads:1};
const check=(data=bytes,meta=metadata)=>{
  const failures=[]; const actual=inspectSelectionTrace(data,parsed,meta,expected,failures); return {actual,failures};
};
assert.deepEqual(check().failures,[]);
const wrongPhase=Buffer.from(bytes); wrongPhase[18]=1;
assert.ok(check(wrongPhase).failures.some(x=>x.includes('DepthWorld')));
const wrongMode=Buffer.from(bytes); wrongMode[19]=0;
assert.ok(check(wrongMode).failures.some(x=>x.includes('DepthWorld')));
const wrongBucket=Buffer.from(bytes); wrongBucket[8]=8;
assert.ok(check(wrongBucket).failures.some(x=>x.includes('QK_SlabSelector')));
assert.ok(check(bytes,{}).failures.length);
assert.throws(()=>inspectSelectionTrace(bytes,{...parsed,version:10},metadata,expected,[]));
const resetFailures=[];
inspectSelectionTrace(bytes,parsed,{selector:{active:0}},{active:0},resetFailures);
assert.ok(resetFailures.some(x=>x.includes('selector reset')));
const resetBytes=Buffer.from(bytes); resetBytes[8]=2;
const privateFields=['active','slabX','slabY','width','height','valid','invalid','colour'];
const zeroProof=Object.fromEntries(privateFields.map(name=>[name,0]));
const resetClean=[];
inspectSelectionTrace(resetBytes,parsed,{selector:zeroProof},{active:0},resetClean);
assert.deepEqual(resetClean,[]); // Unrelated depth solids are allowed.
const cursorFailures=[];
const cursor=inspectSelectionTrace(bytes,parsed,{selector:zeroProof},{active:0},cursorFailures);
assert.deepEqual(cursorFailures,[]); // Restored ordinary cursor buckets are allowed.
assert.equal(cursor.selectorEntries,1);assert.equal(cursor.depthSolidQuads,1); // Diagnostics remain visible.
for(const name of privateFields) {
  const stale=[];
  inspectSelectionTrace(bytes,parsed,{selector:{...zeroProof,[name]:1}},{active:0},stale);
  assert.ok(stale.some(message=>message.startsWith(`selector reset ${name}:`)),`Stale private ${name} must reject`);
  const missing=[];const proof={...zeroProof};delete proof[name];
  inspectSelectionTrace(bytes,parsed,{selector:proof},{active:0},missing);
  assert.ok(missing.some(message=>message.startsWith(`selector reset ${name}:`)),`Missing private ${name} must reject`);
}
for(const value of ['0',false,null,.5]) {
  const invalid=[];
  inspectSelectionTrace(bytes,parsed,{selector:{...zeroProof,colour:value}},{active:0},invalid);
  assert.ok(invalid.some(message=>message.startsWith('selector reset colour:')),'Private proof must be integer zero');
}
console.log('Selection trace assertions: 29 focused cases PASS');
