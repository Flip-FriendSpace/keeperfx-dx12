// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import net from 'node:net';
import {setTimeout as delay} from 'node:timers/promises';
import {readFile,writeFile,readdir,rm} from 'node:fs/promises';
import {join} from 'node:path';
const [out,variant='HD',tileCount='9',fullTerrain='0']=process.argv.slice(2);assert.ok(out);
const report={variant,scope:`${tileCount} source-matched 4x STANDARD terrain replacements at unchanged world size; matched camera and terrain under even lighting, plus a separate normal-lighting observation.`,stages:[],passed:false};
let socket,pending,buffer='',sequence=0,failure;
async function request(action,fields={}){
 if(failure)throw failure;const ack=++sequence;
 const response=await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(new Error('API timeout: '+(fields.command??action))),10000);pending={ack,resolve:r=>{clearTimeout(timer);resolve(r);},reject:e=>{clearTimeout(timer);reject(e);}};socket.write(JSON.stringify({action,ack,...fields}));});pending=undefined;
 assert.equal(response.success,true,JSON.stringify(response));return response.data;
}
const command=text=>request('console_command',{command:text});
const setup=lua=>command('scenecheck reference setup '+lua);
async function paused(){for(let i=0;i<100;i++){try{await setup('assert(true)');return;}catch(e){if(failure||e.code!=='ERR_ASSERTION')throw e;await delay(50);}}throw new Error('Pause timed out');}
async function stats(){await rm(join(out,'renderer-stats.json'),{force:true});await command('scenecheck reference renderer-stats');for(let i=0;i<100;i++){try{const s=JSON.parse(await readFile(join(out,'renderer-stats.json'),'utf8'));assert.equal(s.backend,'DX12');assert.equal(s.rejected,0);assert.equal(s.validation.errors,0);assert.equal(s.validation.warnings,0);assert.ok(!s.failed);return s;}catch(e){if(e.code!=='ENOENT'&&!(e instanceof SyntaxError))throw e;await delay(50);}}throw new Error('No renderer stats');}
async function receipt(id){const before=new Set(await readdir(out));await command('scenecheck receipt');for(let i=0;i<400;i++){const names=(await readdir(out)).filter(f=>!before.has(f)&&/^temporal-\d+-\d+\.png\.ready\.json$/.test(f));if(names.length){const r=JSON.parse(await readFile(join(out,names[0]),'utf8'));assert.equal(r.backend,'DX12');const patch=r.columns.filter(c=>c.x>=180&&c.x<=206&&c.y>=180&&c.y<=206);assert.ok(patch.length>0);const s={id,image:names[0].slice(0,-11),receipt:names[0],turn:r.turn,camera:r.camera,lighting:r.lighting,shading:id==='normal-lighting'?'normal':'fullbright',columns:patch};report.stages.push(s);return s;}await delay(50);}throw new Error('No terrain receipt');}
const overhead='scenecheck reference camera 49536 49536 0 0 -266 6500 94';
try{
 socket=await new Promise((resolve,reject)=>{const s=net.createConnection({host:'127.0.0.1',port:5599});s.once('error',reject);s.once('connect',()=>resolve(s));});
 socket.on('error',e=>{failure=e;pending?.reject(e);});socket.on('data',d=>{buffer+=d.toString().replaceAll('\0','');let r;try{r=JSON.parse(buffer);}catch{return;}buffer='';if(r.ack===pending?.ack)pending.resolve(r);});
 for(let i=0;i<120;i++){const level=await request('get_level_info');const game=await request('get_current_game_info');if(level.level_number===1&&game.game_turn>0){assert.equal(level.is_multiplayer,false);report.startTurn=game.game_turn;break;}await delay(250);}assert.ok(report.startTurn);
 await command('scenecheck overhead');await command('zoomto.subtilecoord 193 193');await delay(500);await command('pause');await paused();
 await setup('Map.default_texture=0; SetTexture(PLAYER0,1); for _,kind in ipairs({"CREATURE","TRAP","OBJECT","EFFECTELEM","EFFECT","EFFECTGEN","DEADCREATURE","SHOT","DOOR"}) do for _,t in ipairs(GetThingsOfClass(kind)) do local p=t.pos; if p.val_x>=45000 and p.val_x<54000 and p.val_y>=45000 and p.val_y<54000 then t:delete() end end end; for x=59,69 do for y=59,69 do ChangeSlabType(x,y,"PATH"); ChangeSlabOwner(x,y,PLAYER_NEUTRAL) end end; local kinds={"DRAPE_WALL","TORCH_WALL","TWINS_WALL","WOMAN_WALL","PAIR_WALL","DAMAGED_WALL"}; for i,k in ipairs(kinds) do for y=62,64 do ChangeSlabType(60+i,y,k); ChangeSlabOwner(60+i,y,PLAYER0) end end; RevealMapRect(PLAYER0,193,193,48,48)');
 await command('scenecheck reference setting fullbright 1');await command('scenecheck reference clear-ui');await command('scenecheck reference short-walls 0');await command(overhead);await receipt('overhead');report.beforeStats=await stats();
 await command('scenecheck reference camera 49536 49536 0 512 -266 6500 94');await receipt('rotated');
 await command('scenecheck reference short-walls 1');await command(overhead);await receipt('short-walls');
 await command('scenecheck reference short-walls 0');await command('pause');await command('scenecheck front');await delay(150);await command('pause');await paused();await receipt('front');
 await command('pause');await command('scenecheck overhead');await delay(150);await command('pause');await paused();await command(overhead);await receipt('restored-overhead');
 await setup('for x=61,66 do for y=62,64 do ChangeSlabType(x,y,"PATH"); ChangeSlabOwner(x,y,PLAYER_NEUTRAL) end end');await command(overhead);await receipt('dig-to-path');
 await setup('for x=61,66 do for y=62,64 do ChangeSlabType(x,y,"DAMAGED_WALL"); ChangeSlabOwner(x,y,PLAYER0) end end');await command(overhead);await receipt('rebuild-wall');
 await command('scenecheck reference setting fullbright 0');await receipt('normal-lighting');
 if(fullTerrain==='1'){
  await command('scenecheck reference setting fullbright 1');
  // Real material transitions precede the artificial source-coverage gallery.
  const groups=[['HARD','GOLD','DIRT','GEMS','WATER','LAVA','DAMAGED_WALL','TORCH_WALL'],['LAIR_AREA','TREASURY_AREA','BOOK_SHELVES','TRAINING_AREA','WORKSHOP_AREA','SCAVENGE_AREA','TEMPLE_POOL','GRAVE_AREA']];
  for(let g=0;g<groups.length;g++){
   await setup('for x=59,69 do for y=59,69 do ChangeSlabType(x,y,"PATH"); ChangeSlabOwner(x,y,PLAYER_NEUTRAL) end end');
   for(let n=0;n<groups[g].length;n++){const x=60+(n%4)*2,y=61+Math.floor(n/4)*3;await setup(`for x=${x},${x+1} do for y=${y},${y+1} do ChangeSlabType(x,y,"${groups[g][n]}"); ChangeSlabOwner(x,y,PLAYER0) end end; RevealMapRect(PLAYER0,193,193,48,48)`);}
   await command(overhead);await receipt('materials-'+g);await command('scenecheck reference camera 49536 49536 0 512 -266 6500 94');await receipt('materials-'+g+'-rotated');
  }
  await setup('for x=59,69 do for y=59,69 do ChangeSlabType(x,y,"PATH"); ChangeSlabOwner(x,y,PLAYER_NEUTRAL) end end; RevealMapRect(PLAYER0,193,193,48,48)');
  for(const bad of ['-1 1','0 0','0 65','692 2','693 1','0 1 junk']){let rejected=false;try{await command('scenecheck reference terrain-gallery '+bad);}catch(e){if(e.code!=='ERR_ASSERTION')throw e;rejected=true;}assert.ok(rejected,'Malformed gallery accepted: '+bad);}
  async function step(){const before=(await request('get_current_game_info')).game_turn;await command('step');for(let n=0;n<200;n++){const turn=(await request('get_current_game_info')).game_turn;assert.ok(turn<=before+1);if(turn===before+1){await paused();return turn;}await delay(25);}throw new Error('Step timed out');}
  // Align the native eight-phase texture clock between independent sessions.
  while((await request('get_current_game_info')).game_turn%16!==0)await step();
  await setup('Map.default_texture=0; SetTexture(PLAYER0,1)');await command('scenecheck reference terrain-gallery 632 61');await command(overhead);
  for(let phase=0;phase<8;phase++){const r=await receipt('animation-'+phase);assert.equal(r.turn%16,phase*2);await step();await step();await command(overhead);}
  await receipt('animation-loop');
  report.galleryPages=[];
  for(let start=0;start<632;start+=64){const count=Math.min(64,632-start);await command(`scenecheck reference terrain-gallery ${start} ${count}`);await command(overhead);await receipt('gallery-'+start);report.galleryPages.push({start,count});}
  await command('scenecheck reference terrain-gallery 0 64');await command(overhead);await receipt('gallery-revisit');
  report.coverage={physicalSources:632,animatedAliases:61,phases:8,malformedCommandsRejected:6,scope:'Every physical A/B source presented in a repeated floor gallery; real material scenes and existing wall/camera transitions. Not a full campaign or door-gameplay qualification.'};
 }
 report.finalStats=await stats();report.passed=true;await command('quit');
}catch(e){report.error=e.message;throw e;}
finally{socket?.destroy();await writeFile(join(out,'fixture-report.json'),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify({passed:report.passed,error:report.error,stages:report.stages.length}));}
