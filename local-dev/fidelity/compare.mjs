// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFile,writeFile} from 'node:fs/promises';
import {pathToFileURL} from 'node:url';

export function canonical(value) {
  if (Array.isArray(value)) return '['+value.map(canonical).join(',')+']';
  if (value && typeof value==='object') return '{'+Object.keys(value).sort().map(k=>JSON.stringify(k)+':'+canonical(value[k])).join(',')+'}';
  assert.ok(typeof value==='string' || typeof value==='boolean' || value===null || Number.isSafeInteger(value), 'Noncanonical diagnostic value');
  return JSON.stringify(value);
}
export const digest=value=>createHash('sha256').update(canonical(value)).digest('hex');
export function validateSnapshot(value) {
  assert.equal(value.schema,1,'Unknown snapshot schema');
  assert.equal(value.scope,'selected-gameplay-v1','Unknown snapshot scope');
  assert.ok(Number.isSafeInteger(value.turn)&&value.turn>=0,'Invalid turn');
  assert.ok(Number.isSafeInteger(value.level)&&value.level>0,'Invalid level');
  assert.deepEqual(Object.keys(value.state).sort(),['armageddon','dungeons','players','rng','rooms','scripts','slabs','things']);
  for(const domain of ['dungeons','players','rooms','slabs','things','armageddon']) assert.ok(Array.isArray(value.state[domain]),`Invalid ${domain}`);
  for(const domain of ['dungeons','players','rooms','things']) {
    const ids=value.state[domain].map(v=>v.id);
    assert.ok(ids.every((v,i)=>Number.isInteger(v)&&v>=0&&(i===0||v>ids[i-1])),`${domain}: IDs must be unique and ascending`);
  }
  canonical(value);
  return value;
}
export function compareSnapshots(reference,candidate) {
  validateSnapshot(reference);validateSnapshot(candidate);
  assert.equal(candidate.level,reference.level,'Comparisons require the same level');
  assert.equal(candidate.turn,reference.turn,'Comparisons require the exact same simulation turn');
  const differences=[];
  function walk(a,b,path) {
    if(canonical(a)===canonical(b))return;
    if(a&&b&&typeof a==='object'&&typeof b==='object'&&Array.isArray(a)===Array.isArray(b)) {
      for(const key of [...new Set([...Object.keys(a),...Object.keys(b)])].sort()) {
        if(!(key in a)||!(key in b))differences.push({path:path+'.'+key,reference:a[key]??null,candidate:b[key]??null});
        else walk(a[key],b[key],path+'.'+key);
      }
    } else differences.push({path,reference:a,candidate:b});
  }
  walk(reference.state,candidate.state,'state');
  const domains=Object.keys(reference.state).map(name=>({name,reference:digest(reference.state[name]),candidate:digest(candidate.state[name])}));
  return {schema:1,passed:differences.length===0,scope:reference.scope,turn:reference.turn,level:reference.level,
    referenceSha256:digest(reference),candidateSha256:digest(candidate),domains,
    differenceCount:differences.length,differences:differences.slice(0,100),differencesTruncated:differences.length>100,
    limits:['Selected observed fields only; no full simulation or arbitrary Lua VM equivalence claim.','Same turn alone does not prove identical initial state, input stream, campaign, mods or configuration; bind those in the run manifest.']};
}
if(process.argv[1]&&import.meta.url===pathToFileURL(process.argv[1]).href){
  const [reference,candidate,output]=process.argv.slice(2);assert.ok(reference&&candidate,'Usage: node compare.mjs reference.json candidate.json [report.json]');
  const result=compareSnapshots(JSON.parse(await readFile(reference,'utf8')),JSON.parse(await readFile(candidate,'utf8')));
  if(output)await writeFile(output,JSON.stringify(result,null,2)+'\n');
  console.log(JSON.stringify(result,null,2));if(!result.passed)process.exitCode=1;
}
