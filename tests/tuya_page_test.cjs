const fs=require('fs'),vm=require('vm'),assert=require('assert'),path=require('path');
const {parseHTML}=require('linkedom');
const html=fs.readFileSync(path.join(__dirname,'../ANENJI_Dataloger/tuya_page.h'),'utf8').split('R"TUYA(')[1].split(')TUYA"')[0];
const script=html.match(/<script>([\s\S]*)<\/script>/)[1];
const {document,window}=parseHTML(html);const proto=window.HTMLSelectElement.prototype;
const desc=Object.getOwnPropertyDescriptor(proto,'value');
if(!desc?.set)Object.defineProperty(proto,'value',{get(){return Array.from(this.options).find(o=>o.selected)?.value||this.options[0]?.value||''},set(v){for(const o of this.options)o.selected=String(o.value)===String(v)}});
let config=null,savedRules=null,testCalls=0,lang='ru';
const context={document,console,JSON,Array,Number,String,Object,Error,Promise,setInterval(){},fetch:async(path,options)=>{
 if(options?.method==='POST'){
 const b=JSON.parse(options.body);if(path.endsWith('/config'))config=b;if(path.endsWith('/rules'))savedRules=b;if(path.endsWith('/test'))testCalls++;
 return {ok:true,json:async()=>({status:'saved'})};}
 return {ok:true,json:async()=>({language:lang,enabled:false,region:3,clientId:'test-client',hasSecret:true,status:'disabled',sdReady:true,hasCA:false,fresh:true,rules:[]})};
}};vm.createContext(context);vm.runInContext(script,context);
const $=id=>document.getElementById(id);const settle=()=>new Promise(r=>setImmediate(r));
(async()=>{
 await settle();assert.equal(document.documentElement.lang,'ru');assert.equal($('client').value,'test-client');
 $('add').onclick();assert.equal(document.querySelectorAll('.rule').length,1);assert.equal(document.querySelector('[data-field="enabled"]').checked,false);
 document.querySelector('[data-field="device"]').value='test-device';
 document.querySelector('[data-field="startMinute"]').value='22:00';document.querySelector('[data-field="endMinute"]').value='06:00';
 const addCond=Array.from(document.querySelectorAll('button')).find(b=>b.textContent==='Добавить условие');addCond.onclick();assert.equal(document.querySelectorAll('.condition').length,2);
 $('rulesForm').onsubmit({preventDefault(){}});await settle();assert(savedRules);assert.equal(savedRules.rules[0].startMinute,1320);assert.equal(savedRules.rules[0].endMinute,360);assert.equal(savedRules.rules[0].conditions.length,2);assert.equal(savedRules.rules[0].value,true);
 $('secret').value='test-secret';$('configForm').onsubmit({preventDefault(){}});await settle();assert.equal(config.secret,'test-secret');assert.equal($('secret').value,'');
 $('test').onclick();await settle();assert.equal(testCalls,1);
 vm.runInContext("lang='en';translate();renderRules()",context);assert.equal(document.documentElement.lang,'en');assert.equal($('add').textContent,'Add scenario');
 const prev=savedRules;document.querySelector('[data-field="value"]').value='not JSON';$('rulesForm').onsubmit({preventDefault(){}});await settle();assert.equal(savedRules,prev);assert($('message').textContent.includes('Invalid scenario'));
 console.log('PASS: Tuya page RU/EN, rule editor, overnight window, JSON serialization, secret clearing, authentication test and invalid action input');
})().catch(e=>{console.error(e);process.exit(1)});
