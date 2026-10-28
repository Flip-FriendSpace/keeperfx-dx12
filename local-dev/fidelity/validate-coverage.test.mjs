// SPDX-License-Identifier: GPL-2.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp, writeFile, readFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';

test('metadata-only invocation never establishes gate acceptance or overwrites evidence', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'keeperfx-coverage-'));
  try {
    const plan = join(directory, 'coverage.json'), output = join(directory, 'report.json');
    await writeFile(plan, JSON.stringify({gates: [{id: 7, status: 'not_started'}], pendingReferences: [
      {id: 'save-load', validationPlan: {gate: 7, status: 'not_started', remaining: 'Run same-turn continuation',
        tools: ['local-dev/fidelity/compare.mjs'], baselineCases: ['same-save']}}]}));
    const args = [fileURLToPath(new URL('./validate-coverage.mjs', import.meta.url)),
      '--plan', plan, '--workspace', directory, '--output', output];
    const first = spawnSync(process.execPath, args, {encoding: 'utf8'});
    assert.equal(first.status, 0, first.stderr);
    const bytes = await readFile(output), report = JSON.parse(bytes);
    assert.equal(report.metadataValid, true);
    assert.equal(report.establishesNewAcceptance, false);
    assert.equal(report.gate1Passed, false);
    assert.deepEqual(report.acceptanceDecisionsVerified, []);
    const second = spawnSync(process.execPath, args, {encoding: 'utf8'});
    assert.notEqual(second.status, 0);
    assert.deepEqual(await readFile(output), bytes);
  } finally { await rm(directory, {recursive: true, force: true}); }
});
