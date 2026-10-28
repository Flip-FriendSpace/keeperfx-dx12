// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import {isAbsolute, relative, resolve} from 'node:path';

export function evidencePaths(value, label) {
  const paths = typeof value === 'string' ? [value] : value;
  assert.ok(Array.isArray(paths) && paths.length > 0, `${label}: missing evidence`);
  assert.ok(paths.every(path => typeof path === 'string' && path.trim()), `${label}: evidence must contain paths`);
  assert.equal(new Set(paths).size, paths.length, `${label}: duplicate evidence`);
  return paths;
}

export function evidencePath(root, path) {
  assert.ok(!isAbsolute(path), `Evidence must be workspace-relative: ${path}`);
  const full = resolve(root, path), local = relative(root, full);
  assert.ok(local && !local.startsWith('..') && !isAbsolute(local), `Evidence escapes workspace: ${path}`);
  return full;
}

// Supporting reports may contain retained failures. Only the named, hash-bound
// acceptance decision establishes that this deliberately limited gate passed.
export async function validateAcceptancePlan(reference, gates, readEvidence) {
  const plan = reference.validationPlan, label = reference.id;
  assert.ok(plan && Number.isInteger(plan.gate) && plan.gate >= 3 && plan.gate <= 8, label);
  assert.ok(['not_started', 'in_progress', 'passed'].includes(plan.status), label);
  assert.ok(typeof plan.remaining === 'string' && plan.remaining.trim(), `${label}: missing remaining scope`);
  assert.ok(plan.tools?.length && plan.baselineCases?.length, `${label}: missing validation recipe`);
  if (plan.status !== 'passed') return null;
  const gate = gates.find(gate => gate.id === plan.gate);
  assert.equal(gate?.status, 'passed', `${label}: parent gate has not passed`);
  const supporting = evidencePaths(plan.evidence, label);
  const acceptance = plan.acceptanceEvidence ?? gate.acceptanceEvidence;
  assert.ok(acceptance?.path && /^[a-f0-9]{64}$/i.test(acceptance.sha256)
    && typeof acceptance.status === 'string', `${label}: missing pinned acceptance decision`);
  assert.ok(supporting.includes(acceptance.path), `${label}: acceptance absent from evidence`);
  for (const path of supporting) await readEvidence(path);
  const {document, sha256} = await readEvidence(acceptance.path);
  assert.equal(sha256.toLowerCase(), acceptance.sha256.toLowerCase(), `${label}: acceptance hash changed`);
  assert.equal(document.gate, plan.gate, `${label}: acceptance gate differs`);
  assert.equal(document.status, acceptance.status, `${label}: acceptance status differs`);
  const accepted = plan.gate === 7 ? ['passed', 'accepted_for_single_player_preview']
    : plan.gate === 8 ? ['passed', 'accepted_for_gre_single_player_preview'] : ['passed'];
  assert.ok(accepted.includes(document.status),
    `${label}: decision does not accept this gate`);
  return {path: acceptance.path, sha256, status: document.status, scope: document.scope ?? null};
}
