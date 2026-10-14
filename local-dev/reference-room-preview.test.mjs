// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';
import {test} from 'node:test';
import {readFileSync} from 'node:fs';
import {inspectRoomPreviewTrace} from './reference-room-preview.mjs';

function fixture({busy=0,valid=1,invalid=0,fancy=0,hidden=0}={}) {
  const bytes=Buffer.alloc(1024),offsets={entries:0,overlayDraws:128};
  bytes.writeUInt32LE(8,0);for(let i=0;i<8;i++)bytes[8+i*5]=4;
  bytes.writeUInt32LE(1,128);bytes[132]=0;bytes[133]=2;
  const colour=busy?5:valid?1:0;
  const metadata={selector:{active:1,slabX:63,slabY:63,width:1,height:1,valid,invalid,colour:valid?1:0},
    roomPreview:{active:1,roomKind:2,busy,valid,invalid,colour,processCalls:1,busyBranches:busy,basicCalls:1-fancy,fancyCalls:fancy,hiddenSlabs:hidden,emittedLines:8}};
  return {bytes,parsed:{version:11,offsets},metadata,expected:{roomKind:2,busy,valid,invalid,colour,processCalls:1,busyBranches:busy,basicCalls:1-fancy,fancyCalls:fancy,hiddenSlabs:hidden}};
}
function inspect(f){const failures=[];inspectRoomPreviewTrace(f.bytes,f.parsed,f.metadata,f.expected,failures);return failures;}
for(const [name,options] of Object.entries({valid:{},invalid:{valid:0,invalid:1},concealed:{valid:6,invalid:3,fancy:1,hidden:3},busy:{busy:1}}))
  test(name,()=>assert.deepEqual(inspect(fixture(options)),[]));
for(const [name,mutate,pattern] of [
  ['wrong validity',f=>f.metadata.roomPreview.valid=0,/valid/],
  ['no original process',f=>f.metadata.roomPreview.processCalls=0,/geometry branch/],
  ['no actual geometry',f=>f.metadata.roomPreview.emittedLines=0,/emitted lines/],
  ['unbound geometry',f=>f.metadata.roomPreview.emittedLines=7,/emitted lines/],
  ['busy branch absent',f=>{f.metadata.roomPreview.busy=1;f.expected.busy=1;},/feedback branch/],
  ['wrong busy colour',f=>{f.metadata.roomPreview.busy=1;f.metadata.roomPreview.busyBranches=1;f.expected.busy=1;f.expected.busyBranches=1;},/feedback branch/],
  ['no depth geometry',f=>f.bytes[133]=0,/Solid/],
])test(name,()=>{const f=fixture();mutate(f);assert.ok(inspect(f).some(s=>pattern.test(s)));});
test('missing proof rejects',()=>{const f=fixture();delete f.metadata.roomPreview;assert.throws(()=>inspect(f),/proof/);});
test('legacy invalid box count does not invent build eligibility',()=>{
  const f=fixture({valid:0,invalid:1});f.metadata.selector.valid=1;
  assert.deepEqual(inspect(f),[]);
});
test('four recipes retain real setup and final scoped command',()=>{
  const recipes=JSON.parse(readFileSync(new URL('./reference-scenes.json',import.meta.url),'utf8')).scenes.filter(s=>s.expectRoomPreview);
  assert.deepEqual(recipes.map(s=>s.id),['room-preview-valid','room-preview-invalid','room-preview-concealed','room-preview-busy']);
  for(const s of recipes) {
    const p=s.expectRoomPreview, selector=s.expectSelector;
    assert.equal(s.suite,'selection');assert.equal(s.expect.view,0);
    assert.equal(s.pausedCommands.at(-1),`scenecheck reference room-preview 2 64 64 ${selector.width} ${selector.height} ${p.busy}`);
    assert.match(s.pausedCommands[0],/ADD_GOLD_TO_PLAYER/);
    assert.equal(s.pausedCommands[0].includes('ConcealMapRect'),!!p.hiddenSlabs);
    assert.equal(s.pausedCommands[0].includes('ChangeSlabType(64,64,2)'),s.id==='room-preview-invalid');
    assert.equal(p.colour,p.busy?5:p.valid?1:0);
    assert.doesNotMatch(s.pausedCommands.join('\n'),/work_state|roomspace|chosen_room|packet/i);
  }
});
