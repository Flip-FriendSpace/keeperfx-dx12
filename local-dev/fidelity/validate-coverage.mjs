// SPDX-License-Identifier: GPL-2.0-or-later
// Validate existing acceptance evidence; this does not establish new coverage.
import assert from 'node:assert/strict';
import {readFile, writeFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {resolve} from 'node:path';
import {evidencePath, validateAcceptancePlan} from './coverage-validation.mjs';

const args = process.argv.slice(2), options = {};
while (args.length) {
  const name = args.shift();
  assert.ok(['--plan', '--workspace', '--output'].includes(name) && args.length,
    'Usage: node validate-coverage.mjs --plan coverage.json --workspace evidence-root [--output new-report.json]');
  assert.ok(!(name in options), `Duplicate option: ${name}`);
  options[name] = args.shift();
}
assert.ok(options['--plan'] && options['--workspace'], '--plan and --workspace are required');
const workspace = resolve(options['--workspace']);
const coverage = JSON.parse((await readFile(resolve(options['--plan']), 'utf8')).replace(/^\uFEFF/, ''));
assert.ok(Array.isArray(coverage.gates) && Array.isArray(coverage.pendingReferences), 'Missing gates or pendingReferences');
const references = coverage.pendingReferences.filter(reference => reference.validationPlan);
assert.ok(references.length, 'No validation plans supplied');
assert.equal(new Set(references.map(reference => reference.id)).size, references.length, 'Duplicate reference ID');
const decisions = [];
for (const reference of references) {
  const decision = await validateAcceptancePlan(reference, coverage.gates, async path => {
    const bytes = await readFile(evidencePath(workspace, path));
    return {document: JSON.parse(bytes.toString('utf8').replace(/^\uFEFF/, '')),
      sha256: createHash('sha256').update(bytes).digest('hex')};
  });
  if (decision) decisions.push({id: reference.id, ...decision});
}
const report = {schema: 1, metadataValid: true, validationPlansChecked: references.length,
  acceptanceDecisionsVerified: decisions, establishesNewAcceptance: false, gate1Passed: false,
  scope: 'Existing hash-bound acceptance metadata only; no source, frame, gameplay or full coverage qualification.'};
if (options['--output']) await writeFile(resolve(options['--output']), JSON.stringify(report, null, 2) + '\n', {flag: 'wx'});
console.log(JSON.stringify(report, null, 2));
