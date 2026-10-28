// SPDX-License-Identifier: GPL-2.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import {evidencePaths, evidencePath, validateAcceptancePlan} from './coverage-validation.mjs';
const hash = 'a'.repeat(64);
const gate = {id:8,status:'passed',acceptanceEvidence:{path:'accept.json',sha256:hash,status:'accepted_for_gre_single_player_preview'}};
const ref = {id:'stress',validationPlan:{gate:8,status:'passed',remaining:'Wider hardware unqualified',tools:['test'],baselineCases:['arena'],evidence:['progress.json','accept.json']}};
const read = async path => ({sha256:hash,document:path==='accept.json'?{gate:8,status:'accepted_for_gre_single_player_preview',scope:'GRE only'}:{gate:8,status:'failed'}});
test('supporting failure plus pinned scoped acceptance is valid',async()=>assert.equal((await validateAcceptancePlan(ref,[gate],read)).scope,'GRE only'));
test('single evidence path is accepted',async()=>assert.ok(await validateAcceptancePlan({...ref,validationPlan:{...ref.validationPlan,evidence:'accept.json'}},[gate],read)));
test('wrong acceptance hash fails closed',async()=>await assert.rejects(validateAcceptancePlan(ref,[gate],async p=>({...await read(p),sha256:'b'.repeat(64)})),/hash changed/));
test('wrong gate cannot qualify a plan',async()=>await assert.rejects(validateAcceptancePlan(ref,[gate],async p=>({...await read(p),document:{gate:7,status:gate.acceptanceEvidence.status}})),/gate differs/));
test('failed decision cannot pass',async()=>await assert.rejects(validateAcceptancePlan(ref,[gate],async p=>({...await read(p),document:{gate:8,status:'failed'}})),/status differs/));
test('missing supporting report is not ignored',async()=>await assert.rejects(validateAcceptancePlan(ref,[gate],async p=>{if(p==='progress.json')throw new Error('missing');return read(p)}),/missing/));
test('passed status requires explicit remaining scope',async()=>await assert.rejects(validateAcceptancePlan({...ref,validationPlan:{...ref.validationPlan,remaining:''}},[gate],read),/remaining scope/));
test('unstarted network plan never becomes passed from parent',async()=>assert.equal(await validateAcceptancePlan({...ref,validationPlan:{...ref.validationPlan,status:'not_started',evidence:undefined}},[gate],read),null));
test('evidence rejects duplicate and escaping paths',()=>{assert.throws(()=>evidencePaths(['x','x'],'case'));assert.throws(()=>evidencePath(process.cwd(),'../outside'));assert.throws(()=>evidencePath(process.cwd(),process.cwd()));});
