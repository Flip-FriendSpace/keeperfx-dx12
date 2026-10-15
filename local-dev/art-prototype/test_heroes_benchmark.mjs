// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import net from 'node:net';
import {setTimeout as delay} from 'node:timers/promises';
import {readFile,writeFile,readdir,rm} from 'node:fs/promises';
import {join} from 'node:path';
const [out,mode='Benchmark',variant='HD']=process.argv.slice(2);assert.ok(out);
const report={mode,variant,scope:'Resolution-aware 4x imp integration at unchanged creature size; original/native/HD controls and real-creature transitions.',stages:[],passed:false};
let socket,pending,buffer='',sequence=0,failure;
async function request(action,fields={}) {
  if(failure)throw failure;const ack=++sequence;
  const response=await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(new Error('API timeout: '+(fields.command??action))),10000);pending={ack,resolve:r=>{clearTimeout(timer);resolve(r);},reject:e=>{clearTimeout(timer);reject(e);}};socket.write(JSON.stringify({action,ack,...fields}));});pending=undefined;
  assert.equal(response.success,true,JSON.stringify(response));return response.data;
}
const command=text=>request('console_command',{command:text});
const setup=lua=>command('scenecheck reference setup '+lua);
async function waitPaused(){for(let n=0;n<100;n++){try{await setup('assert(true)');return;}catch(e){if(failure||e.code!=='ERR_ASSERTION')throw e;await delay(50);}}throw new Error('Pause timed out');}
async function step(){const before=(await request('get_current_game_info')).game_turn;await command('step');for(let n=0;n<200;n++){const turn=(await request('get_current_game_info')).game_turn;assert.ok(turn<=before+1);if(turn===before+1){await waitPaused();return turn;}await delay(25);}throw new Error('Step timed out');}
async function stats(){const file=join(out,'renderer-stats.json');await rm(file,{force:true});await command('scenecheck reference renderer-stats');for(let n=0;n<100;n++){try{const s=JSON.parse(await readFile(file,'utf8'));assert.equal(s.backend,'DX12');assert.equal(s.rejected,0);assert.equal(s.validation.errors,0);assert.equal(s.validation.warnings,0);assert.ok(!s.failed);return s;}catch(e){if(e.code!=='ENOENT'&&!(e instanceof SyntaxError))throw e;await delay(50);}}throw new Error('No renderer stats');}
async function receipt(id,fixture=true){const before=new Set(await readdir(out));await command('scenecheck receipt');for(let n=0;n<400;n++){const names=(await readdir(out)).filter(f=>!before.has(f)&&/^temporal-\d+-\d+\.png\.ready\.json$/.test(f));if(names.length){const r=JSON.parse(await readFile(join(out,names[0]),'utf8'));assert.equal(r.backend,'DX12');const stage={id,receipt:names[0],image:names[0].slice(0,-11),turn:r.turn,objects:r.objects.filter(o=>o.class===1&&o.model===3&&o.simulationPosition[0]>=47500&&o.simulationPosition[0]<=52000&&o.simulationPosition[1]>=47500&&o.simulationPosition[1]<=52000)};if(fixture)assert.equal(stage.objects.length,9);stage.creatures=r.objects.filter(o=>o.class===5&&o.simulationPosition[0]>=47000&&o.simulationPosition[0]<=52500&&o.simulationPosition[1]>=47000&&o.simulationPosition[1]<=52500);stage.camera=r.camera;stage.lighting=r.lighting;report.stages.push(stage);return stage;}await delay(50);}throw new Error('No sprite presentation receipt');}
try {
  socket=await new Promise((resolve,reject)=>{const s=net.createConnection({host:'127.0.0.1',port:5599});s.once('error',reject);s.once('connect',()=>resolve(s));});
  socket.on('error',e=>{failure=e;pending?.reject(e);});socket.on('data',d=>{buffer+=d.toString().replaceAll('\0','');let r;try{r=JSON.parse(buffer);}catch{return;}buffer='';if(r.ack===pending?.ack)pending.resolve(r);});
  for(let i=0;i<120;i++){const level=await request('get_level_info'),game=await request('get_current_game_info');if(level.level_number===1&&game.game_turn>0){assert.equal(level.is_multiplayer,false);report.startTurn=game.game_turn;break;}await delay(250);}
  assert.ok(report.startTurn);await command('scenecheck overhead');await command('zoomto.subtilecoord 193 193');await delay(700);await command('pause');await waitPaused();
  const models=['WIZARD','BARBARIAN','ARCHER','MONK','DWARFA','KNIGHT','AVATAR','TUNNELLER','WITCH','GIANT','FAIRY','THIEF','SAMURAI','IMP'];
  report.models=models;report.expectedCrowd=168;report.scope='Matched 168-creature original versus HD scene. Both variants retain all HD archives loaded; measures drawing/simulation cost, not cold loading or total-memory savings.';
  await setup(`for _,t in ipairs(GetThingsOfClass("CREATURE")) do t:delete() end; for x=59,69 do for y=59,69 do ChangeSlabType(x,y,11); ChangeSlabOwner(x,y,PLAYER0); if x==59 or x==69 or y==59 or y==69 then ChangeSlabType(x,y,"HARD") end end end; RevealMapRect(PLAYER0,193,193,40,40); artModels={${models.map(m=>'"'+m+'"').join(',')}}; artCrowd={}; for i=0,167 do local t=AddCreatureToLevel(PLAYER0,artModels[(i%14)+1],{stl_x=186+(i%14),stl_y=186+math.floor(i/14)},1,0); assert(t); t.state="ManualControl"; t.orientation=(i%8)*256; table.insert(artCrowd,t) end; assert(#artCrowd==168); for _,x in ipairs({185,193,201}) do for _,y in ipairs({185,193,201}) do assert(AddObjectToLevelAtPos("CANDLESTCK",x,y,0,PLAYER0,0)) end end`);
  await command('scenecheck reference camera 49536 49152 0 0 -266 4200 94');await command('scenecheck reference clear-ui');await command('scenecheck reference short-walls 0');
  const before=await receipt('crowd-before',false);assert.equal(before.creatures.length,168,'Every crowd member must be visible');
  const counts={};for(const c of before.creatures){assert.equal(c.size,300);counts[c.model]=(counts[c.model]??0)+1;}assert.equal(Object.keys(counts).length,14);assert.ok(Object.values(counts).every(n=>n===12));report.visibleModels=counts;report.beforeStats=await stats();
  await command('pause');await command('renderbench live');let completed;
  for(let n=0;n<160;n++){try{completed=JSON.parse(await readFile(join(out,'dx12-live.json'),'utf8'));break;}catch(e){if(e.code!=='ENOENT'&&!(e instanceof SyntaxError))throw e;}await delay(250);}
  assert.ok(completed?.passed);assert.equal(completed.debug,false);assert.equal(completed.gpuValidation,false);report.benchmark=completed;
  const lines=(await readFile(join(out,'dx12-live.csv'),'utf8')).trim().split(/\r?\n/),headers=lines.shift().split(',');const rows=lines.map(line=>Object.fromEntries(line.split(',').map((v,i)=>[headers[i],Number(v)])));
  assert.equal(rows.length,completed.rows);assert.ok(rows.every(r=>r.diagnostic===0&&r.palette_replay===0));
  function summary(key){const values=rows.map(r=>r[key]).sort((a,b)=>a-b);assert.ok(values.every(Number.isFinite));return {median:values[Math.floor(values.length/2)],p95:values[Math.floor(values.length*.95)],mean:values.reduce((a,b)=>a+b,0)/values.length};}
  report.timings=Object.fromEntries(['producer_wall_ms','preparation_submit_cpu_ms','gpu_upload_render_ms','admission_interval_ms'].map(k=>[k,summary(k)]));
  await command('pause');await waitPaused();const after=await receipt('crowd-after',false);assert.equal(after.creatures.length,168);const afterCounts={};for(const c of after.creatures){assert.equal(c.size,300);afterCounts[c.model]=(afterCounts[c.model]??0)+1;}assert.deepEqual(afterCounts,counts);report.finalStats=await stats();report.passed=true;await command('quit');
} catch(e){report.error=e.message;throw e;}
finally{socket?.destroy();await writeFile(join(out,'fixture-report.json'),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify({passed:report.passed,error:report.error,stages:report.stages.length}));}
