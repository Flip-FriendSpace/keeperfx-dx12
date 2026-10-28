// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import {readFile,writeFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {resolve} from 'node:path';
import {evidencePath} from './coverage-validation.mjs';
const args=process.argv.slice(2),runtime=args.shift();
assert.ok(runtime&&!runtime.startsWith('--'),'Usage: node campaign-check.mjs runtime [new-report.json] [--matrix run-matrix.json --evidence-root directory]');
let output=null,matrixPath=new URL('./original-campaign.json',import.meta.url),evidenceRoot=resolve(runtime);
const seen=new Set();
while(args.length){
  const value=args.shift();
  if(value==='--matrix'||value==='--evidence-root'){
    assert.ok(args.length&&!seen.has(value),`Missing or duplicate ${value}`);seen.add(value);
    const path=resolve(args.shift());if(value==='--matrix')matrixPath=path;else evidenceRoot=path;
  }else{assert.ok(!value.startsWith('--')&&!output,'Unexpected argument');output=value;}
}
const matrix=JSON.parse((await readFile(matrixPath,'utf8')).replace(/^\uFEFF/,''));
assert.equal(matrix.schema,1);assert.equal(matrix.campaign,'keeporig');
assert.deepEqual(matrix.maps.map(m=>m.level),[...Array.from({length:20},(_,i)=>i+1),100,101,102,103,104,105]);
const sha=b=>createHash('sha256').update(b).digest('hex'),files=[matrix.campaignConfig,...matrix.maps.map(m=>({path:m.script,sha256:m.scriptSha256}))];
for(const file of files)assert.equal(sha(await readFile(evidencePath(resolve(runtime),file.path))),file.sha256,`Campaign content differs: ${file.path}`);
for(const entry of [...matrix.maps,...matrix.domains]){
  assert.ok(['not_run','in_progress','passed','failed'].includes(entry.status));
  const records=[];
  for(const proof of entry.evidence){
      assert.ok(proof.path&&proof.sha256&&proof.check);const bytes=await readFile(evidencePath(evidenceRoot,proof.path));
      assert.equal(sha(bytes),proof.sha256,'Changed acceptance evidence');const record=JSON.parse(bytes.toString('utf8'));
      assert.equal(record.passed,true,'Failed evidence cannot accept an entry');
      if(entry.level!==undefined)assert.equal(record.level,entry.level,'Evidence belongs to another map');
    records.push(proof.check);
  }
  if(entry.status==='passed'){
    assert.ok(entry.evidence.length,'Passing entry needs evidence');
    for(const check of entry.requiredChecks??[entry.id])assert.ok(records.includes(check),`Missing acceptance check: ${check}`);
  }
}
const report={schema:1,inventoryPassed:true,scriptCount:matrix.maps.length,configurationAndScriptHashes:files.length,
  campaignCompleted:matrix.maps.every(m=>m.status==='passed'),domainsCompleted:matrix.domains.every(d=>d.status==='passed'),
  pendingMaps:matrix.maps.filter(m=>m.status!=='passed').map(m=>({level:m.level,name:m.name,status:m.status})),
  pendingDomains:matrix.domains.filter(d=>d.status!=='passed').map(d=>d.id)};
if(output)await writeFile(output,JSON.stringify(report,null,2)+'\n',{flag:'wx'});
console.log(JSON.stringify(report,null,2));
