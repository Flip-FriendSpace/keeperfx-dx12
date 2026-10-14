// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';
import {test} from 'node:test';
import {inspectHoverTrace} from './reference-hover.mjs';
import {readFileSync} from 'node:fs';

function fixture({decision='white',visible=true,hit=false,hovered=1,blinkOn=1,concealed=false}={}) {
  const bytes=Buffer.alloc(8192),offsets={};let cursor=0;
  const records=(name,count,stride)=>{offsets[name]=cursor;bytes.writeUInt32LE(count,cursor);cursor+=4;const p=cursor;cursor+=count*stride;return p;};
  const white=Buffer.from(Array.from({length:256},(_,i)=>(i+40)%256));
  const red=Buffer.from(Array.from({length:256},(_,i)=>(i+80)%256));
  const remap=decision==='white'?white:decision==='red'?red:Buffer.from(Array.from({length:256},(_,i)=>i));
  const mapped=decision==='white'||decision==='red',alpha=concealed?Math.fround(1/3):1;
  let p=records('objects',visible?1:0,56);
  if(visible){bytes.writeBigUInt64LE(19n,p);bytes.writeUInt32LE(7,p+8);bytes.writeUInt16LE(16,p+40);bytes[p+54]=concealed?32:0;}
  p=records('draws',visible?1:0,327);
  if(visible){bytes.writeBigUInt64LE(19n,p);bytes.writeUInt32LE(7,p+8);bytes.writeUInt32LE((mapped?0x800:0)|(concealed?4:0),p+64);bytes[p+69]=Number(mapped);remap.copy(bytes,p+71);}
  p=records('outlineSprites',visible?1:0,82);if(visible)bytes.writeFloatLE(alpha,p+44);
  p=records('outlineCommands',visible?1:0,9);if(visible)bytes[p]=1;
  p=records('colourBindings',visible?1:0,5);if(visible)bytes[p+4]=mapped?2:1;
  const row=records('colourRows',visible?1:0,1024);
  offsets.palettes=cursor;cursor+=1536;offsets.worldPalette=cursor;cursor+=1024;
  for(let i=0;i<256;i++)for(let c=0;c<3;c++) {
    bytes[offsets.palettes+768+i*3+c]=(i+c*7)%64;
    bytes[offsets.worldPalette+i*4+c]=Math.floor(((i+c*7)%64)*255/63);
    if(visible)bytes[row+i*4+c]=Math.floor(((remap[i]+c*7)%64)*255/63);
  }
  if(visible)for(let i=0;i<256;i++)bytes[row+i*4+3]=255;
  const metadata={turn:80,hover:{active:1,targetSlot:7,targetIncarnation:'19',hovered,blinkOn,actualHit:Number(hit),lastTurnDamaged:hit?80:79,
    owner:0,model:16,renderFlags:visible?(concealed?32:0):1,whiteRemap:white.toString('hex'),redRemap:red.toString('hex'),
    considered:Number(visible),white:Number(visible&&decision==='white'),red:Number(visible&&decision==='red'),normal:Number(visible&&decision==='normal')}};
  return {bytes,parsed:{version:11,offsets,glWorld:{},composition:{}},metadata,
    expected:{visible,hovered,blinkOn,actualHit:Number(hit),owner:0,model:16,decision:visible?decision:'none',alpha}};
}
function inspect(f){const failures=[];const result=inspectHoverTrace(f.bytes,f.parsed,f.metadata,f.expected,failures);return {failures,result};}
for(const [name,options] of Object.entries({baseline:{decision:'normal',hovered:0},bright:{},hit:{decision:'red',hit:true,hovered:0},priority:{hit:true},dimHit:{decision:'red',hit:true,blinkOn:0},concealedOwned:{concealed:true},concealedEnemy:{visible:false}}))
  test(name,()=>assert.deepEqual(inspect(fixture(options)).failures,[]));
for(const [name,mutate,pattern] of [
  ['stale identity',f=>f.metadata.hover.targetIncarnation='20',/incarnation|identity/],
  ['inactive scope',f=>f.metadata.hover.active=0,/inactive/],
  ['invented hit',f=>f.metadata.hover.lastTurnDamaged=80,/simulation turn/],
  ['wrong base remap',f=>f.bytes[f.parsed.offsets.draws+4+71]^=1,/base remap/],
  ['dropped actual sprite',f=>f.bytes.writeUInt32LE(0,f.parsed.offsets.outlineSprites),/nonempty executed/],
  ['missing main phase',f=>f.bytes[f.parsed.offsets.outlineCommands+4]=0,/phase ownership/],
  ['wrong opacity',f=>f.bytes.writeFloatLE(.5,f.parsed.offsets.outlineSprites+4+44),/opacity/],
  ['lost CLUT remap',f=>f.bytes[f.parsed.offsets.colourBindings+8]=3,/lost the requested remap/],
  ['wrong effective byte',f=>f.bytes[f.parsed.offsets.colourRows+4]^=1,/effective lookup byte/],
  ['hidden target leaked',f=>{f.expected.visible=false;f.expected.decision='none';},/visibility|actual draw/],
]) test(name,()=>{const f=fixture();mutate(f);assert.ok(inspect(f).failures.some(message=>pattern.test(message)));});
test('standalone texture fallback uses actual uploaded palette',()=>{
  const f=fixture();f.bytes[f.parsed.offsets.outlineSprites+4+65]=2;f.bytes[f.parsed.offsets.colourBindings+8]=5;
  assert.deepEqual(inspect(f).failures,[]);
  f.bytes[f.parsed.offsets.worldPalette+40*4]^=1;
  assert.ok(inspect(f).failures.some(message=>/effective lookup byte/.test(message)));
});
test('missing proof is rejected',()=>{const f=fixture();f.metadata.hover.targetIncarnation='0';assert.throws(()=>inspect(f),/identity/);});

test('bounded recipes preserve real damage, exact stepping and final target binding',()=>{
  const catalog=JSON.parse(readFileSync(new URL('./reference-scenes.json',import.meta.url),'utf8'));
  const recipes=catalog.scenes.filter(s=>s.expectHover);
  const cases=[['hover-baseline',0,1,0,'normal'],['hover-bright',1,1,0,'white'],
    ['hover-real-hit',0,1,1,'red'],['hover-hit-bright',1,1,1,'white'],
    ['hover-hit-dim',1,0,1,'red'],['hover-front-hit-bright',1,1,1,'white'],
    ['hover-owned-concealed',1,1,0,'white'],['hover-enemy-concealed',1,1,0,'none']];
  assert.deepEqual(recipes.map(s=>s.id),cases.map(c=>c[0]));
  for(const [id,hovered,blinkOn,actualHit,decision] of cases) {
    const s=recipes.find(r=>r.id===id),e=s.expectHover;
    assert.deepEqual([e.hovered,e.blinkOn,e.actualHit,e.decision],[hovered,blinkOn,actualHit,decision]);
    assert.equal(s.suite,'effects');assert.equal(s.setupStepTurns,1);
    assert.equal(s.setupSettleMs,undefined);assert.equal(s.setupPhases,undefined);
    assert.equal(s.pausedCommands.at(-1),`scenecheck reference hover ${e.owner} 6 199 193 ${hovered} ${blinkOn}`);
    const setup=s.setupCommands.join('\n'),all=[...s.commands,...s.setupCommands,...s.pausedCommands].join('\n');
    assert.equal(setup.includes('t.health=hp-20'),!!actualHit);
    assert.equal(setup.includes('UsePowerOnCreature'),id.includes('concealed'));
    assert.doesNotMatch(all,/last_turn_damaged|local_thing_under_hand|rendering_flags|game_turn\s*=/);
    assert.match(s.pausedCommands[0],/t.health>0/);assert.match(s.pausedCommands[0],/assert\(n==1/);
    assert.equal(e.visible,decision!=='none');
    assert.equal(e.alpha,id.includes('concealed')?1/3:1);
    assert.equal(s.expect.view,id.includes('front')?1:0);
  }
});
