// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import {readFile,writeFile} from 'node:fs/promises';
import {join} from 'node:path';
const [originalDir,hdDir,destination]=process.argv.slice(2);
assert.ok(originalDir&&hdDir&&destination);
const read=async dir=>JSON.parse(await readFile(join(dir,'fixture-report.json'),'utf8'));
const original=await read(originalDir),hd=await read(hdDir);
assert.equal(original.passed,true);assert.equal(hd.passed,true);
assert.equal(original.variant,'Original');assert.equal(hd.variant,'HD');
assert.deepEqual(original.models,hd.models);assert.deepEqual(original.visibleModels,hd.visibleModels);
const before=r=>r.stages.find(s=>s.id==='crowd-before');
const layout=r=>before(r).creatures.map(c=>({model:c.model,position:c.simulationPosition,size:c.size})).sort((a,b)=>JSON.stringify(a).localeCompare(JSON.stringify(b)));
assert.deepEqual(layout(original),layout(hd),'Crowd positions/models/world sizes must match before timing');
assert.deepEqual(before(original).camera,before(hd).camera);
for(const run of [original,hd]) {assert.equal(run.benchmark.debug,false);assert.equal(run.benchmark.gpuValidation,false);assert.equal(run.finalStats.rejected,0);}
const result={passed:true,original:originalDir,hd:hdDir,creatures:168,models:14,matchedInitialScene:true,limitation:'Both runs retain every HD archive loaded. This compares rendering and simulation of original/HD mappings, not cold load or total memory savings.',timings:Object.fromEntries(Object.keys(original.timings).map(key=>[key,{original:original.timings[key],hd:hd.timings[key],medianRatio:hd.timings[key].median/original.timings[key].median}]))};
await writeFile(destination,JSON.stringify(result,null,2)+'\n');console.log(JSON.stringify({passed:true,destination}));
