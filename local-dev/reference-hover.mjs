// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';

// Called only after the complete capture and sidecar frame identity validate.
export function inspectHoverTrace(bytes, parsed, metadata, expected, failures) {
  assert.ok(parsed.version >= 11 && parsed.glWorld && parsed.composition, 'Hover controls require owned GL composition');
  const p=metadata.hover;
  assert.ok(p && /^[1-9][0-9]*$/.test(p.targetIncarnation), 'Missing bound hover target identity');
  assert.ok(/^[0-9a-f]{512}$/.test(p.whiteRemap) && /^[0-9a-f]{512}$/.test(p.redRemap), 'Missing original hover/hit remap tables');
  const check=(ok,message)=>{if(!ok)failures.push(`hover: ${message}`);};
  check(p.active===1,'private draw scope inactive');
  for(const field of ['hovered','blinkOn','actualHit','owner','model'])
    if(expected[field]!==undefined)check(p[field]===expected[field],`${field}: expected ${expected[field]}, got ${p[field]}`);
  check(p.actualHit===Number(p.lastTurnDamaged===metadata.turn),'actual hit does not match the captured simulation turn');
  const {offsets}=parsed;
  const records=(offset,stride,read)=>Array.from({length:bytes.readUInt32LE(offset)},(_,i)=>read(offset+4+i*stride,i));
  const objects=records(offsets.objects,56,q=>({slot:bytes.readUInt32LE(q+8),incarnation:bytes.readBigUInt64LE(q).toString(),
    model:bytes.readUInt16LE(q+40),owner:bytes[q+50],renderFlags:bytes[q+54]}));
  const targetObjects=objects.filter(o=>o.slot===p.targetSlot);
  const draws=records(offsets.draws,327,(q,index)=>({index,slot:bytes.readUInt32LE(q+8),incarnation:bytes.readBigUInt64LE(q).toString(),
    flags:bytes.readUInt32LE(q+64),hasRemap:bytes[q+69],remap:bytes.subarray(q+71,q+327)}));
  const targetDraws=draws.filter(d=>d.slot===p.targetSlot);
  const sprites=records(offsets.outlineSprites,82,(q,index)=>({index,draw:bytes.readUInt32LE(q),alpha:bytes.readFloatLE(q+44),path:bytes[q+65]}));
  const phases=records(offsets.outlineCommands,9,(q,order)=>({order,kind:bytes[q],sprite:bytes.readUInt32LE(q+1),group:bytes.readUInt32LE(q+5)}));
  const bindings=records(offsets.colourBindings,5,q=>({row:bytes.readUInt32LE(q),resolution:bytes[q+4]}));
  const white=Buffer.from(p.whiteRemap,'hex'),red=Buffer.from(p.redRemap,'hex');
  check(!white.equals(red),'white and red production controls are indistinguishable');
  const actualDecision=p.white ? 'white' : p.red ? 'red' : p.normal ? 'normal' : 'none';
  check(actualDecision===expected.decision,`decision: expected ${expected.decision}, got ${actualDecision}`);
  const visible=expected.visible===true;
  check(targetObjects.length===(visible?1:0),'target visibility differs from owned objects');
  check(targetDraws.length===(visible?1:0),'target must have exactly one actual draw when visible and none when hidden');
  check(p.considered===(visible?1:0)&&p.white+p.red+p.normal===p.considered,'original sprite decision count differs');
  for(const o of targetObjects)check(o.incarnation===p.targetIncarnation&&o.owner===p.owner&&o.model===p.model&&o.renderFlags===p.renderFlags,'owned target identity or visibility flags differ');
  const execution=[];
  for(const draw of targetDraws) {
    check(draw.incarnation===p.targetIncarnation,'draw target incarnation differs');
    const wanted=expected.decision==='white'?white:expected.decision==='red'?red:null;
    if(wanted)check(!!draw.hasRemap&&!!(draw.flags&0x800)&&draw.remap.equals(wanted),'actual base remap does not equal the original selected table');
    else check(!draw.hasRemap||(!draw.remap.equals(white)&&!draw.remap.equals(red)),'normal control unexpectedly uses hover/hit remap');
    const matches=sprites.filter(s=>s.draw===draw.index);
    check(matches.length===1,'missing nonempty executed GL target');
    for(const s of matches) {
      check(s.alpha===Math.fround(expected.alpha),'actual source opacity differs');
      const ownPhases=phases.filter(c=>c.sprite===s.index);
      check(ownPhases.filter(c=>c.kind===1).length===1&&!ownPhases.some(c=>c.kind===2),'target main/glow phase ownership differs');
      const binding=bindings[s.index];
      check(binding&&binding.row<bytes.readUInt32LE(offsets.colourRows),'missing effective lookup row');
      if(binding&&binding.row<bytes.readUInt32LE(offsets.colourRows)) {
        const expectedResolution=s.path===2 ? (draw.hasRemap?5:4) : (draw.hasRemap?2:1);
        check(binding.resolution===expectedResolution,'lookup path silently lost the requested remap');
        const row=bytes.subarray(offsets.colourRows+4+binding.row*1024,offsets.colourRows+4+(binding.row+1)*1024);
        for(let i=0;i<256;i++) {
          const source=draw.hasRemap?draw.remap[i]:i;
          for(let c=0;c<3;c++) {
            // Instanced/atlas CLUT is built from captured display6; standalone
            // texture fallback consumes the actually uploaded global palette.
            const channel=s.path===2 ? bytes[offsets.worldPalette+source*4+c]
              : Math.floor(bytes[offsets.palettes+768+source*3+c]*255/63);
            check(row[i*4+c]===channel,`effective lookup byte ${i*4+c} differs from actual palette/remap`);
          }
          if(s.path!==2)check(row[i*4+3]===255,'effective CLUT alpha differs');
        }
        execution.push({...s,binding,phases:ownPhases,lookupSha256:createHash('sha256').update(row).digest('hex')});
      }
    }
  }
  return {targetSlot:p.targetSlot,targetIncarnation:p.targetIncarnation,visible,decision:actualDecision,
    requested:{hovered:p.hovered,blinkOn:p.blinkOn},actualHit:p.actualHit,objects:targetObjects.length,draws:targetDraws.length,execution,
    whiteRemapSha256:createHash('sha256').update(white).digest('hex'),redRemapSha256:createHash('sha256').update(red).digest('hex'),
    scope:'Render-time remap priority and visibility only; no mouse picking, status, hand or gameplay interaction qualification',visualReviewRequired:true};
}
