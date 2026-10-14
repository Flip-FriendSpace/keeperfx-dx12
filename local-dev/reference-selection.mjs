// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
import assert from 'node:assert/strict';

// The caller has already validated the complete capture and sidecar identity.
// A configured preview alone is insufficient: require actual original bucket
// entries and executed solid quads in the world depth phase.
export function inspectSelectionTrace(bytes, parsed, metadata, expected, failures) {
  assert.ok(parsed.version >= 11, 'Selection references require owned V11 composition');
  const {offsets} = parsed;
  let selectorEntries = 0, depthSolidQuads = 0;
  const entries = bytes.readUInt32LE(offsets.entries);
  for (let i = 0; i < entries; ++i)
    if (bytes[offsets.entries + 4 + i * 5 + 4] === 4) ++selectorEntries;
  const draws = bytes.readUInt32LE(offsets.overlayDraws);
  for (let i = 0; i < draws; ++i) {
    const p = offsets.overlayDraws + 4 + i * 81;
    if (bytes[p] === 0 && bytes[p + 1] === 2) ++depthSolidQuads;
  }
  const actual = {...metadata.selector, selectorEntries, depthSolidQuads,
    proofScope:'Emission only: kind4 buckets and DepthWorld Solid counts are not an association proof; native visual review must verify colour, raised-terrain occlusion and concealed-cell placement',
    visualReviewRequired:true};
  for (const [name, range] of Object.entries(expected)) {
    assert.ok(['active','slabX','slabY','width','height','valid','invalid','colour','selectorEntries','depthSolidQuads'].includes(name), 'Unknown selector assertion');
    const {min, max} = typeof range === 'number' ? {min:range,max:range}
      : {min:range.min ?? 0,max:range.max ?? Number.MAX_SAFE_INTEGER};
    assert.ok(Number.isSafeInteger(min) && Number.isSafeInteger(max) && min >= 0 && max >= min, 'Invalid selector count bounds');
    if (!Number.isSafeInteger(actual[name]) || actual[name] < min || actual[name] > max)
      failures.push(`selector ${name}: expected ${min}..${max}, got ${actual[name]}`);
  }
  if (expected.active === 1) {
    if (!selectorEntries) failures.push('selector: no original QK_SlabSelector bucket was emitted');
    if (!depthSolidQuads) failures.push('selector: no executed DepthWorld Solid quad was captured');
  }
  if (expected.active === 0) {
    // Reset proves that the private reference preview state was cleared. The
    // restored ordinary cursor may still emit selector buckets and solids.
    for (const name of ['active','slabX','slabY','width','height','valid','invalid','colour'])
      if (!Number.isSafeInteger(actual[name]) || actual[name] !== 0)
        failures.push(`selector reset ${name}: expected integer zero, got ${actual[name]}`);
  }
  return actual;
}
