const fs=require('fs'),vm=require('vm'),assert=require('assert'),path=require('path');
const {parseHTML}=require('linkedom');
const html=fs.readFileSync(path.join(__dirname,'../ANENJI_Dataloger/tuya_page.h'),'utf8').split('R"TUYA(')[1].split(')TUYA"')[0];
const {document,window}=parseHTML(html);const proto=window.HTMLSelectElement.prototype;
if(!Object.getOwnPropertyDescriptor(proto,'value')?.set)Object.defineProperty(proto,'value',{get(){const value=this._testValue;return Array.from(this.options).some(o=>o.value===value)?value:this.options[0]?.value||''},set(v){this._testValue=String(v)}});
let jobId=0,job=null,pageNumber=0,requests=[],fail=false,configPosts=0;
const first={uid:'account_uid',requestCursor:'',nextCursor:'next+/=',hasMore:true,total:4,devices:[{id:'relay1',name:'Реле бойлера',category:'cz',online:true},{id:'relay2',name:'<img src=x onerror=alert(1)>',category:'cz',online:false},{id:'relay3',name:'Термостат',category:'wk',online:null}]};
const second={uid:'account_uid',requestCursor:'next+/=',nextCursor:'',hasMore:false,total:4,devices:[{id:'relay4',name:'Насос',category:'cz',online:true}]};
const context={document,console,JSON,Array,Number,String,Object,Error,Promise,TextEncoder,Date,setInterval(){},setTimeout(fn){fn()},fetch:async(url,options)=>{
 requests.push({url,method:options?.method||'GET'});
 if(url.split('?')[0]==='/api/tuya/devices'){
  if(options?.method==='POST'){job=JSON.parse(options.body);pageNumber=2;return {ok:true,json:async()=>({status:'queued',jobId:++jobId})};}
  return {ok:true,json:async()=>job?{status:fail?'error':'done',jobId,error:fail?'api_error':'',uid:'account_uid',page:{...first,hasMore:false,nextCursor:'',devices:[...first.devices,...second.devices]}}:{status:'idle',jobId:0,uid:'account_uid',page:first}};
 }
 if(options?.method==='POST'){if(url.endsWith('/config'))configPosts++;return {ok:true,json:async()=>({status:'saved'})};}
 return {ok:true,json:async()=>({language:'ru',enabled:false,region:3,clientId:'test-client',hasSecret:true,status:'disabled',sdReady:true,hasCA:true,fresh:true,rules:[]})};
}};vm.createContext(context);vm.runInContext(html.match(/<script>([\s\S]*)<\/script>/)[1],context);
const settle=()=>new Promise(r=>setImmediate(r));const $=id=>document.getElementById(id);
(async()=>{
 await settle();assert.equal($('accountUid').value,'account_uid');assert.equal(document.querySelectorAll('#deviceList tbody tr').length,3);assert($('deviceMessage').textContent.includes('с SD'));assert.equal(document.querySelectorAll('#deviceList img').length,0,'device names must render as text');
 $('add').onclick();let input=document.querySelector('[data-field="action"]');input.value='SET("old-device", "switch_1", false)';const picker=document.querySelector('[data-device-picker]');picker.value='relay1';Array.from(document.querySelectorAll('button')).find(b=>b.textContent==='Подставить устройство в SET').onclick();assert.equal(input.value,'SET("relay1", "switch_1", false)');
 await vm.runInContext('loadDevices(false)',context);assert.equal(document.querySelectorAll('#deviceList tbody tr').length,4);assert($('deviceMessage').textContent.includes('Загружено из Tuya'));assert.equal(job.initial,false);
 assert.equal(input.value,'SET("relay1", "switch_1", false)','background refresh must preserve unsaved action edits');
 assert($('nextDevices').disabled);
 await vm.runInContext('loadDevices(false)',context);assert.equal(document.querySelectorAll('#deviceList tbody tr').length,4,'refresh must replace old catalog, not duplicate entries');
 fail=true;await assert.rejects(()=>vm.runInContext('loadDevices(false)',context));assert.equal(document.querySelectorAll('#deviceList tbody tr').length,4);
 $('accountUid').value='other_uid';$('accountUid').dispatchEvent(new window.Event('input'));assert.equal(document.querySelectorAll('[data-device-picker] option').length,1);assert($('nextDevices').disabled);
 assert.equal(configPosts,0);assert(requests.every(r=>!r.url.includes('/commands')));
 console.log('PASS: SD catalogue restore, full background refresh, safe names, SET edits preserved, account change and cloud failure preserving old list');
})().catch(e=>{console.error(e);process.exit(1)});
