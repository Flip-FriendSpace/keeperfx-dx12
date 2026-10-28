// SPDX-License-Identifier: GPL-2.0-or-later
// Connect only to an already-owned, disposable, paused local test process.
// This tool never starts games, changes settings, advances turns or pauses play.
import assert from 'node:assert/strict';
import net from 'node:net';
import {readFile,writeFile} from 'node:fs/promises';
import {join} from 'node:path';
import {digest,validateSnapshot} from './compare.mjs';
const [directory,label,turnText,portText='5599']=process.argv.slice(2),turn=Number(turnText),port=Number(portText);
assert.ok(directory&&/^[a-zA-Z0-9_-]{1,64}$/.test(label)&&Number.isSafeInteger(turn)&&turn>=0,'Usage: node capture.mjs KFX_FIDELITY_DIR label exactTurn [port]');
assert.ok(Number.isInteger(port)&&port>0&&port<65536);
const socket=net.createConnection({host:'127.0.0.1',port});
try {
  await new Promise((resolve,reject)=>{socket.once('connect',resolve);socket.once('error',reject);socket.setTimeout(10000,()=>socket.destroy(new Error('API connection timeout')))});
  const response=await new Promise((resolve,reject)=>{
    let buffer='';socket.once('error',reject);
    socket.on('data',chunk=>{buffer+=chunk.toString().replaceAll('\0','');let value;try{value=JSON.parse(buffer)}catch{return}if(value.ack===1)resolve(value)});
    socket.write(JSON.stringify({action:'console_command',ack:1,command:`statecheck fingerprint ${label}`}));
  });
  assert.equal(response.success,true,'Guarded capture refused; require KFX_STATE_TEST=1, matching KFX_FIDELITY_DIR, local single-player and paused state');
  const file=join(directory,label+'.json'),snapshot=validateSnapshot(JSON.parse(await readFile(file,'utf8')));
  assert.equal(snapshot.turn,turn,'Unexpected simulation turn; do not compare this sample');
  const receipt={schema:1,file,turn,level:snapshot.level,sha256:digest(snapshot),scope:snapshot.scope,capturedAt:new Date().toISOString()};
  await writeFile(join(directory,label+'.receipt.json'),JSON.stringify(receipt,null,2)+'\n',{flag:'wx'});
  console.log(JSON.stringify(receipt,null,2));
}finally{socket.destroy()}
