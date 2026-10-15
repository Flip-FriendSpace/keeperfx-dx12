// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import net from 'node:net';
import {setTimeout as delay} from 'node:timers/promises';
import {readFile,writeFile,readdir,rm} from 'node:fs/promises';
import {join} from 'node:path';
const [out,mode='Validation',variant='HD']=process.argv.slice(2);assert.ok(out);
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
async function receipt(id,fixture=true){const before=new Set(await readdir(out));await command('scenecheck receipt');for(let n=0;n<400;n++){const names=(await readdir(out)).filter(f=>!before.has(f)&&/^temporal-\d+-\d+\.png\.ready\.json$/.test(f));if(names.length){const r=JSON.parse(await readFile(join(out,names[0]),'utf8'));assert.equal(r.backend,'DX12');const stage={id,receipt:names[0],image:names[0].slice(0,-11),turn:r.turn,objects:r.objects.filter(o=>o.class===1&&o.model===3&&o.simulationPosition[0]>=47500&&o.simulationPosition[0]<=52000&&o.simulationPosition[1]>=47500&&o.simulationPosition[1]<=52000)};if(fixture)assert.equal(stage.objects.length,9);stage.creatures=r.objects.filter(o=>o.class===5);stage.camera=r.camera;stage.lighting=r.lighting;report.stages.push(stage);return stage;}await delay(50);}throw new Error('No sprite presentation receipt');}
try {
  socket=await new Promise((resolve,reject)=>{const s=net.createConnection({host:'127.0.0.1',port:5599});s.once('error',reject);s.once('connect',()=>resolve(s));});
  socket.on('error',e=>{failure=e;pending?.reject(e);});socket.on('data',d=>{buffer+=d.toString().replaceAll('\0','');let r;try{r=JSON.parse(buffer);}catch{return;}buffer='';if(r.ack===pending?.ack)pending.resolve(r);});
  for(let i=0;i<120;i++){const level=await request('get_level_info');const game=await request('get_current_game_info');if(level.level_number===1&&game.game_turn>0){assert.equal(level.is_multiplayer,false);report.startTurn=game.game_turn;break;}await delay(250);}
  assert.ok(report.startTurn);await command('scenecheck overhead');await command('zoomto.subtilecoord 193 193');await delay(700);await command('pause');await waitPaused();
  await setup('for x=60,68 do for y=60,68 do ChangeSlabType(x,y,11); ChangeSlabOwner(x,y,PLAYER0) end end; RevealMapRect(PLAYER0,193,193,36,36)');
  if(mode==='Benchmark') {
    await setup('for x=59,69 do for y=59,69 do if x==59 or x==69 or y==59 or y==69 then ChangeSlabType(x,y,"HARD") end end end; artCrowd={}; for i=0,143 do local t=AddCreatureToLevel(PLAYER0,"IMP",{stl_x=185+(i%12),stl_y=185+math.floor(i/12)},1,0); assert(t); table.insert(artCrowd,t) end; for _,x in ipairs({185,193,201}) do for _,y in ipairs({185,193,201}) do assert(AddObjectToLevelAtPos("CANDLESTCK",x,y,0,PLAYER0,0)) end end; assert(#artCrowd==144)');
    await command('scenecheck reference camera 49152 49152 0 0 -266 6500 94');
    await receipt('crowd-before',false);report.beforeStats=await stats();
    await command('pause');await command('renderbench live');
    let completed;
    for(let n=0;n<160;n++){try{completed=JSON.parse(await readFile(join(out,'dx12-live.json'),'utf8'));break;}catch(e){if(e.code!=='ENOENT'&&!(e instanceof SyntaxError))throw e;}await delay(250);}
    assert.ok(completed?.passed);assert.equal(completed.debug,false);assert.equal(completed.gpuValidation,false);report.benchmark=completed;
    const lines=(await readFile(join(out,'dx12-live.csv'),'utf8')).trim().split(/\r?\n/),headers=lines.shift().split(',');
    const rows=lines.map(line=>Object.fromEntries(line.split(',').map((v,i)=>[headers[i],Number(v)])));
    assert.equal(rows.length,completed.rows);assert.ok(rows.every(r=>r.diagnostic===0&&r.palette_replay===0));
    function summary(key){const values=rows.map(r=>r[key]).sort((a,b)=>a-b);assert.ok(values.every(Number.isFinite));return {median:values[Math.floor(values.length/2)],p95:values[Math.floor(values.length*.95)],mean:values.reduce((a,b)=>a+b,0)/values.length};}
    report.timings=Object.fromEntries(['producer_wall_ms','preparation_submit_cpu_ms','gpu_upload_render_ms','admission_interval_ms'].map(k=>[k,summary(k)]));
    await command('pause');await waitPaused();await receipt('crowd-after',false);report.afterStats=await stats();
    assert.ok(report.stages.at(-1).creatures.length>=100,'Crowd no longer visible');
  } else {
  await setup('artFixtures={}; artNames={"IDLE","WALK","DIG"}; artOriginal={557,555,559}; for v=1,3 do for a=1,3 do local x=188+(v-1)*5; local y=188+(a-1)*5; local t=AddObjectToLevelAtPos("GOLD_CHEST",x,y,0,PLAYER0,0); assert(t); t.anim_sprite=v==1 and artOriginal[a] or ("ARTIMP_"..(v==2 and "NATIVE_" or "HD_")..artNames[a]); t.anim_speed=256; t.sprite_size=300; t.sprite_size_min=t.sprite_size; t.sprite_size_max=t.sprite_size; t.pos={val_x=x*256+128,val_y=y*256+128,val_z=256}; table.insert(artFixtures,t) end end');
  await setup('local found=false; for _,t in ipairs(GetThingsOfClass("CREATURE")) do if t.model=="IMP" then for i=7,9 do if t.anim_sprite==artFixtures[i].anim_sprite then found=true end end end end; assert(found,"No normal imp is using the prototype HD animation")');report.hdCreatureOverrideVerified=true;
  await command('scenecheck reference short-walls 0');await command('scenecheck reference clear-ui');
  const camera='scenecheck reference camera 49536 49536 0 0 -266 6500 94';await command(camera);
  for(const [d,angle] of [1024,768,512,256,0].entries()){
    await setup(`for _,t in ipairs(artFixtures) do t.orientation=${angle} end`);
    await delay(150);await receipt('direction-'+d);
  }
  await setup('for _,t in ipairs(artFixtures) do t.orientation=1024 end');
  for(let n=0;n<6;n++){await step();await command(camera);await receipt('front-cycle-'+(n+1));}
  const cycle=report.stages.filter(s=>s.id.startsWith('front-cycle'));
  assert.ok(new Set(cycle.map(s=>JSON.stringify(s.objects.map(o=>o.frame)))).size>1,'Animation did not advance');
  const hdAnimations=cycle[0].objects.filter(o=>o.simulationPosition[0]===50816)
    .sort((a,b)=>a.simulationPosition[1]-b.simulationPosition[1]).map(o=>o.animation);
  assert.equal(hdAnimations.length,3);
  async function impReceipt(id,animation){
    await command('scenecheck reference camera 49536 49536 0 0 -266 13000 94');
    const s=await receipt(id,false);assert.equal(s.creatures.length,1);assert.equal(s.creatures[0].size,300);
    if(animation!==undefined)assert.equal(s.creatures[0].animation,animation);
    return s;
  }
  await setup('for _,t in ipairs(artFixtures) do t:delete() end; artFixtures={}; for _,t in ipairs(GetThingsOfClass("CREATURE")) do if t.model=="IMP" then t:delete() end end; artImp=AddCreatureToLevel(PLAYER0,"IMP",{stl_x=193,stl_y=193},1,0); assert(artImp); artImp.state="ManualControl"; artImp.orientation=1024; artImp.pos={val_x=49536,val_y=49536,val_z=256}; artSize=artImp.sprite_size; assert(artSize==300); assert(AddObjectToLevelAtPos("CANDLESTCK",191,191,0,PLAYER0,0)); assert(AddObjectToLevelAtPos("TORCH",195,191,0,PLAYER0,0))');
  await step();await command('scenecheck reference camera 49536 49536 0 0 -266 13000 94');
  await impReceipt('real-imp-idle',hdAnimations[0]);
  await setup('assert(artImp:walk_to(195,193))');
  for(let n=0;n<4;n++)await step();await impReceipt('real-imp-walk',hdAnimations[1]);
  await setup('artImp.state="ManualControl"; artImp.instance="DIG"');
  for(let n=0;n<3;n++)await step();await setup('artImp.orientation=1024');await impReceipt('real-imp-dig',hdAnimations[2]);
  await setup('artImp.instance="NULL"; artImp.anim_sprite=565; assert(artImp.sprite_size==artSize)');
  await impReceipt('original-action-fallback',565);
  await setup('artImp.anim_sprite="ARTIMP_HD_IDLE"; assert(artImp.sprite_size==artSize)');
  await impReceipt('return-to-hd',hdAnimations[0]);
  await command('pause');await command('scenecheck possession IMP');await delay(600);await command('pause');await waitPaused();
  await receipt('possession',false);
  await command('pause');await command('scenecheck overhead');await delay(200);await command('pause');await waitPaused();
  await command('scenecheck reference camera 49536 49536 0 0 -266 13000 94');
  await setup('assert(artImp.sprite_size==artSize)');await impReceipt('return-from-possession');
  assert.equal(report.stages.find(s=>s.id==='possession').camera.view,2);
  report.creatureSizeUnchanged=true;
  }
  report.finalStats=await stats();report.passed=true;await command('quit');
} catch(e){report.error=e.message;throw e;}
finally{socket?.destroy();await writeFile(join(out,'fixture-report.json'),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify({passed:report.passed,error:report.error,stages:report.stages.length}));}
