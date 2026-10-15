// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import net from 'node:net';
import {setTimeout as delay} from 'node:timers/promises';
import {readFile,writeFile,readdir,rm} from 'node:fs/promises';
import {join} from 'node:path';
const [out,mode='Validation',variant='HD']=process.argv.slice(2);assert.ok(out);
const report={mode,variant,scope:'Remaining imp actions: full forced heading/frame coverage, real creature instances, slap, collapse, death and pickup. No natural hauling claim.',stages:[],passed:false};
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
async function receipt(id,fixture=true){const before=new Set(await readdir(out));await command('scenecheck receipt');for(let n=0;n<400;n++){const names=(await readdir(out)).filter(f=>!before.has(f)&&/^temporal-\d+-\d+\.png\.ready\.json$/.test(f));if(names.length){const r=JSON.parse(await readFile(join(out,names[0]),'utf8'));assert.equal(r.backend,'DX12');const stage={id,receipt:names[0],image:names[0].slice(0,-11),turn:r.turn,objects:r.objects.filter(o=>o.class===1&&o.model===3&&o.simulationPosition[0]>=46000&&o.simulationPosition[0]<=54000&&o.simulationPosition[1]>=46000&&o.simulationPosition[1]<=54000)};if(fixture)assert.equal(stage.objects.length,18);stage.creatures=r.objects.filter(o=>o.class===5);stage.allObjects=r.objects;stage.camera=r.camera;stage.lighting=r.lighting;report.stages.push(stage);return stage;}await delay(50);}throw new Error('No sprite presentation receipt');}
try {
  socket=await new Promise((resolve,reject)=>{const s=net.createConnection({host:'127.0.0.1',port:5599});s.once('error',reject);s.once('connect',()=>resolve(s));});
  socket.on('error',e=>{failure=e;pending?.reject(e);});socket.on('data',d=>{buffer+=d.toString().replaceAll('\0','');let r;try{r=JSON.parse(buffer);}catch{return;}buffer='';if(r.ack===pending?.ack)pending.resolve(r);});
  for(let i=0;i<120;i++){const level=await request('get_level_info'),game=await request('get_current_game_info');if(level.level_number===1&&game.game_turn>0){assert.equal(level.is_multiplayer,false);report.startTurn=game.game_turn;break;}await delay(250);}
  assert.ok(report.startTurn);await command('scenecheck overhead');await command('zoomto.subtilecoord 193 193');await delay(700);await command('pause');await waitPaused();
  await setup('for x=59,69 do for y=59,69 do ChangeSlabType(x,y,11); ChangeSlabOwner(x,y,PLAYER0) end end; RevealMapRect(PLAYER0,193,193,40,40)');
  const names=['hit','relax','claim','drag','smoke','scream','death','grab','slap'];
  const counts=[1,2,6,6,8,4,2,6,1],ids={};
  await setup('artFixtures={}; artNames={"HIT","RELAX","CLAIM","DRAG","SMOKE","SCREAM","DEATH","GRAB","SLAP"}; artOriginal={561,563,565,567,569,571,573,575,577}; for a=1,9 do for v=1,2 do local x=183+((a-1)%3)*8+(v-1)*3; local y=186+math.floor((a-1)/3)*6; local t=AddObjectToLevelAtPos("GOLD_CHEST",x,y,0,PLAYER0,0); assert(t); t.anim_sprite=v==1 and artOriginal[a] or ("ARTIMP_HD_"..artNames[a]); t.anim_speed=256; t.sprite_size=300; t.sprite_size_min=300; t.sprite_size_max=300; t.pos={val_x=x*256+128,val_y=y*256+128,val_z=256}; table.insert(artFixtures,t) end end');
  await command('scenecheck reference short-walls 0');await command('scenecheck reference clear-ui');
  const camera='scenecheck reference camera 49536 49536 0 0 -266 5800 94';
  const coverage=Object.fromEntries(names.map(n=>[n,[]]));
  for(const [d,angle] of [1024,768,512,256,0,1792,1536,1280].entries()) {
    await setup(`for _,t in ipairs(artFixtures) do t.orientation=${angle} end`);
    const seen=names.map(()=>new Set());
    for(let frame=0;frame<8;frame++) {
      await step();await command(camera);const r=await receipt(`heading-${d}-tick-${frame}`);
      for(let a=0;a<9;a++) {
        const x=(186+(a%3)*8)*256+128,y=(186+Math.floor(a/3)*6)*256+128;
        const o=r.objects.find(o=>o.simulationPosition[0]===x&&o.simulationPosition[1]===y);
        assert.ok(o,`Missing ${names[a]}`);assert.equal(o.size,300);ids[names[a]]=o.animation;seen[a].add(o.frame);
      }
    }
    for(let a=0;a<9;a++){assert.equal(seen[a].size,counts[a],`${names[a]} heading ${d} frame coverage`);coverage[names[a]].push([...seen[a]]);}
  }
  report.forcedCoverage=coverage;report.animationIds=ids;
  await setup('for _,t in ipairs(artFixtures) do t:delete() end; artFixtures={}; for _,t in ipairs(GetThingsOfClass("CREATURE")) do if t.model=="IMP" then t:delete() end end; artImp=AddCreatureToLevel(PLAYER0,"IMP",{stl_x=193,stl_y=193},1,0); assert(artImp); artImp.state="ManualControl"; artImp.orientation=1024; artImp.pos={val_x=49536,val_y=49536,val_z=256}; artSize=artImp.sprite_size; assert(artSize==300); assert(AddObjectToLevelAtPos("CANDLESTCK",191,191,0,PLAYER0,0)); assert(AddObjectToLevelAtPos("TORCH",195,191,0,PLAYER0,0))');
  async function real(id,action){await command('scenecheck reference camera 49536 49536 0 0 -266 13000 94');const r=await receipt(id,false);assert.equal(r.creatures.length,1);assert.equal(r.creatures[0].size,300);if(action)assert.equal(r.creatures[0].animation,ids[action],id);return r;}
  await step();await real('real-idle');
  for(const [instance,action] of [['PRETTY_PATH','claim'],['CELEBRATE_SHORT','claim'],['REINFORCE','claim'],['TOKING','smoke'],['RELAXING','relax'],['MOAN','scream']]) {
    await setup(`artImp.instance="NULL"; artImp.state="ManualControl"; artImp.instance="${instance}"`);
    await step();await real('real-instance-'+instance,action);
  }
  await setup('artImp.instance="NULL"; artImp.state="ManualControl"; UsePowerOnCreature(artImp,"POWER_SLAP",0,PLAYER0,true)');
  await step();await real('real-slap-initial');
  let slapSeen=false;
  for(let i=0;i<30;i++){await step();const r=await real('real-slap-recovery-'+i);if(r.creatures[0].animation===ids.slap){slapSeen=true;break;}}
  assert.ok(slapSeen,'Slap cower was not observed after the initial hit freeze');
  await setup('artImp.instance="NULL"; artImp.state="ManualControl"; artImp:stun(100)');
  await step();await real('real-collapse','death');
  for(let i=0;i<5;i++)await step();await real('real-collapse-settled','death');
  await setup('artImp.state="ManualControl"; artImp.health=1000; artImp:kill()');
  await command('scenecheck reference camera 49536 49536 0 0 -266 13000 94');
  const dead=await receipt('real-death-start',false);assert.ok(dead.allObjects.some(o=>o.class===4&&o.animation===ids.scream),'No HD dying imp');
  let corpseSeen=false;
  for(let i=0;i<32;i++){await step();await command('scenecheck reference camera 49536 49536 0 0 -266 13000 94');const r=await receipt('real-death-progress-'+i,false);if(r.allObjects.some(o=>o.class===4&&o.animation===ids.death)){corpseSeen=true;break;}}
  assert.ok(corpseSeen,'No settled HD imp corpse');report.realDeath=true;
  await setup('artImp=AddCreatureToLevel(PLAYER0,"IMP",{stl_x=193,stl_y=193},1,0); assert(artImp); artImp.state="ManualControl"; artImp.pos={val_x=49536,val_y=49536,val_z=256}; artImp.orientation=1024');
  await step();await setup('UsePowerOnCreature(artImp,"POWER_HAND",0,PLAYER0,true); assert(artImp.state=="InPowerHand",artImp.state); assert(artImp.sprite_size==300); artHeldAnimation=artImp.anim_sprite');
  report.handAnimation=await setup('assert(artHeldAnimation>1000)');
  for(let i=0;i<6;i++){await step();await command('scenecheck reference camera 49536 49536 0 0 -266 13000 94');await receipt('real-held-'+i,false);}
  await setup('assert(artImp.state=="InPowerHand"); artImp.anim_sprite=574');
  await receipt('original-held-size-control',false);
  await setup('artImp.anim_sprite=artHeldAnimation; assert(artImp.sprite_size==300)');
  await receipt('hd-held-restored',false);report.realPickup=true;
  report.finalStats=await stats();report.passed=true;await command('quit');
} catch(e){report.error=e.message;throw e;}
finally{socket?.destroy();await writeFile(join(out,'fixture-report.json'),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify({passed:report.passed,error:report.error,stages:report.stages.length}));}
