// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';
import {inspectSelectionTrace} from './reference-selection.mjs';

// The complete capture and sidecar frame identity must validate first.
export function inspectRoomPreviewTrace(bytes, parsed, metadata, expected, failures) {
  const selection=inspectSelectionTrace(bytes,parsed,metadata,{active:1},failures);
  const proof=metadata.roomPreview;
  assert.ok(proof && proof.active===1, 'Missing private room preview proof');
  for(const [name,value] of Object.entries(expected)) {
    assert.ok(['roomKind','busy','valid','invalid','colour','processCalls','busyBranches','basicCalls','fancyCalls','hiddenSlabs'].includes(name),'Unknown room preview assertion');
    const range=typeof value==='number'?{min:value,max:value}:{min:value.min??0,max:value.max??Number.MAX_SAFE_INTEGER};
    assert.ok(Number.isSafeInteger(range.min)&&Number.isSafeInteger(range.max)&&range.min>=0&&range.max>=range.min,'Invalid room preview bounds');
    if(!Number.isSafeInteger(proof[name])||proof[name]<range.min||proof[name]>range.max)
      failures.push(`room preview ${name}: expected ${range.min}..${range.max}, got ${proof[name]}`);
  }
  if(proof.processCalls!==1 || proof.basicCalls+proof.fancyCalls!==1)
    failures.push('room preview: original geometry branch did not execute exactly once');
  if(proof.busyBranches!==proof.busy || (proof.busy && proof.colour!==5))
    failures.push('room preview: active-build feedback branch differs');
  if(!Number.isSafeInteger(proof.emittedLines)||proof.emittedLines<=0||proof.emittedLines!==selection.selectorEntries)
    failures.push('room preview: actual scoped emitted lines differ from owned selector buckets');
  // The original empty-room fallback recreates a 1x1 box (slab_count=1),
  // while actual can_build_roomspace remains zero. Preserve that behavior.
  if(proof.invalid!==metadata.selector.invalid||metadata.selector.colour!==(proof.valid>0?1:0))
    failures.push('room preview: actual build eligibility differs from the initial selector colour');
  return {...proof,selectorEntries:selection.selectorEntries,depthSolidQuads:selection.depthSolidQuads,
    scope:'Original room-footprint validity, geometry and active-build colour feedback only; no building, cost text, picking, packets or gameplay interaction qualification',visualReviewRequired:true};
}
