const fs=require('fs'),vm=require('vm'),assert=require('assert'),path=require('path');
const {parseHTML}=require('linkedom');
const html=fs.readFileSync(path.join(__dirname,'../ANENJI_Dataloger/tuya_page.h'),'utf8').split('R"TUYA(')[1].split(')TUYA"')[0];
const {document,window}=parseHTML(html);const timers=[];let posts=0,polls=0,started=false;
const page={uid:'uid',generation:1,requestCursor:'',nextCursor:'',hasMore:false,total:1,devices:[{id:'old',name:'Old device',online:null}]};
const context={document,console,JSON,Array,Number,String,Object,Error,Promise,TextEncoder,Date,setInterval(){},setTimeout(fn){timers.push(fn)},fetch:async(url,options)=>{
 if(url.startsWith('/api/tuya/devices')){
  if(options?.method==='POST'){posts++;started=true;return {ok:true,json:async()=>({status:'queued',jobId:8})};}
  if(url.includes('progress')){polls++;return {ok:true,json:async()=>polls===1?{uid:'uid',status:'loading',loading:true,loaded:3,jobId:8}:{uid:'uid',status:'done',jobId:8,page:{...page,generation:2,total:4,devices:[1,2,3,4].map(i=>({id:'d'+i,name:'Device '+i}))}}};}
  return {ok:true,json:async()=>({uid:'uid',status:started?'loading':'idle',jobId:8,loading:started,page})};
 }
 return {ok:true,json:async()=>({language:'en',enabled:false,region:3,clientId:'client',hasSecret:true,status:'disabled',rules:[]})};
}};vm.createContext(context);vm.runInContext(html.match(/<script>([\s\S]*)<\/script>/)[1],context);
const settle=()=>new Promise(r=>setImmediate(r));const $=id=>document.getElementById(id);
(async()=>{
 await settle();assert.equal(posts,0,'opening with an SD catalogue must not request a cloud reload');assert.equal(document.querySelectorAll('#deviceList tbody tr').length,1);
 $('add').onclick();const action=document.querySelector('[data-field="action"]');action.value='SET("old", "switch_1", false)';
 const load=vm.runInContext('loadDevices(false)',context);await settle();
 assert.equal(vm.runInContext('busy',context),false,'catalogue fetch must not lock scenario editing');assert.equal($('add').disabled,false);assert.equal($('refreshDevices').disabled,true);assert.equal(posts,1);
 await vm.runInContext('loadDevices(false)',context);assert.equal(posts,1,'duplicate refresh ignored while loading');
 assert.equal(timers.length,1);timers.shift()();await settle();assert($('deviceMessage').textContent.includes('3'));assert.equal(action.value,'SET("old", "switch_1", false)');
 timers.shift()();await load;assert.equal(document.querySelectorAll('#deviceList tbody tr').length,4);assert.equal($('refreshDevices').disabled,false);assert.equal(action.value,'SET("old", "switch_1", false)');
 console.log('PASS: SD restore without cloud POST, background progress, responsive editor, duplicate refresh prevention and complete replacement');
})().catch(e=>{console.error(e);process.exit(1)});
