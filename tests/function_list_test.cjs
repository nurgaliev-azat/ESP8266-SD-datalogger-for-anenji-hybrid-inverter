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
 if(url==='/api/tuya/functions'){
  if(options?.method==='POST'){job=JSON.parse(options.body);return {ok:true,json:async()=>({jobId:++jobId})};}
  return {ok:true,json:async()=>({status:'done',jobId,uid:job.uid,device:job.device,result:{functions:[{code:'switch_1',name:'Switch',type:'Boolean',values:'{}'},{code:'temp_set',name:'Temperature',type:'Integer',values:'{"min":10,"max":30,"step":2,"scale":1}'},{code:'mode',type:'Enum',values:'{"range":["auto","eco"]}'},{code:'raw',type:'Raw',values:'{}'}]}})};
 }
 if(url==='/api/tuya/devices'){
  if(options?.method==='POST'){job=JSON.parse(options.body);pageNumber=job.cursor?2:1;return {ok:true,json:async()=>({status:'queued',jobId:++jobId})};}
  return {ok:true,json:async()=>job?{status:fail?'error':'done',jobId,error:fail?'api_error':'',uid:'account_uid',page:pageNumber===1?first:second}:{status:'idle',jobId:0,uid:'account_uid',page:first}};
 }
 if(options?.method==='POST'){if(url.endsWith('/config'))configPosts++;return {ok:true,json:async()=>({status:'saved'})};}
 return {ok:true,json:async()=>({language:'ru',enabled:false,region:3,clientId:'test-client',hasSecret:true,status:'disabled',sdReady:true,hasCA:true,fresh:true,rules:[]})};
}};vm.createContext(context);vm.runInContext(html.match(/<script>([\s\S]*)<\/script>/)[1],context);
const settle=()=>new Promise(r=>setImmediate(r));const $=id=>document.getElementById(id);
(async()=>{
 await settle();assert.equal($('accountUid').value,'account_uid');assert.equal(document.querySelectorAll('#deviceList tbody tr').length,3);assert($('deviceMessage').textContent.includes('с SD'));assert.equal(document.querySelectorAll('#deviceList img').length,0,'device names must render as text');
 $('add').onclick();let input=document.querySelector('[data-field="action"]');input.value='SET("old-device", "switch_1", false)';const picker=document.querySelector('[data-device-picker]');picker.value='relay1';Array.from(document.querySelectorAll('button')).find(b=>b.textContent==='Подставить устройство в SET').onclick();assert.equal(input.value,'SET("relay1", "switch_1", false)');
 const area=picker.parentElement;
 Array.from(area.querySelectorAll('button')).find(b=>b.textContent==='Загрузить функции').onclick();await settle();await settle();
 const commands=area.querySelector('[data-function-picker]'),value=area.querySelector('input');assert.equal(commands.options.length,5);
 const insert=Array.from(area.querySelectorAll('button')).find(b=>b.textContent==='Вставить SET');
 commands.value='0';commands.dispatchEvent(new window.Event('change'));insert.onclick();assert.equal(input.value,'SET("relay1", "switch_1", true)');
 commands.value='1';commands.dispatchEvent(new window.Event('change'));value.value='11';insert.onclick();assert(input.value.includes('switch_1'),'reject invalid step');value.value='12';insert.onclick();assert.equal(input.value,'SET("relay1", "temp_set", 12)');
 commands.value='2';commands.dispatchEvent(new window.Event('change'));value.value='"invalid"';insert.onclick();assert(input.value.includes('temp_set'));value.value='"eco"';insert.onclick();assert.equal(input.value,'SET("relay1", "mode", "eco")');
 commands.value='3';commands.dispatchEvent(new window.Event('change'));insert.onclick();assert(input.value.includes('mode'),'Raw unsupported');
 picker.value='relay2';picker.dispatchEvent(new window.Event('change'));insert.onclick();assert(input.value.includes('relay1'),'must not reuse functions after device change');
 assert(requests.every(r=>!r.url.includes('/commands')));
 vm.runInContext('showStatus({status:"low_memory",sdReady:true,hasCA:false,fresh:true,heap:31000,maxBlock:22000,tlsHeap:30000,tlsMaxBlock:21000,tlsRxBytes:16384,tlsError:0})',context);
 assert($('status').textContent.includes('31000 B'));assert($('status').textContent.includes('22000 B'));assert($('status').textContent.includes('Свободная RAM'));
 vm.runInContext('lang="en";showStatus({heap:12345,maxBlock:9999})',context);assert($('status').textContent.includes('Free RAM: 12345 B'));
 vm.runInContext('lang="ru";showStatus({ruleErrors:[{name:"Бойлер <script>",device:"relay1",code:"command_rejected",paused:true}]})',context);
 assert($('ruleAlerts').textContent.includes('Бойлер <script>'));assert.equal($('ruleAlerts').querySelectorAll('script').length,0);assert($('ruleAlerts').textContent.includes('Команда')||$('ruleAlerts').textContent.includes('команд'));assert($('ruleAlerts').textContent.includes('Попытки остановлены'));
 vm.runInContext('showStatus({ruleErrors:[]})',context);assert.equal($('ruleAlerts').textContent,'');
 console.log('PASS: function loading, Boolean/Integer/Enum values, range steps, unsupported types, device changes and read-only requests');
})().catch(e=>{console.error(e);process.exit(1)});
