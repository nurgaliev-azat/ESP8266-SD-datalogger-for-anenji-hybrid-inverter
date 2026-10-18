const fs=require('fs'),vm=require('vm'),assert=require('assert'),path=require('path');
const {parseHTML}=require('linkedom');
const html=fs.readFileSync(path.join(__dirname,'../ANENJI_Dataloger/tuya_page.h'),'utf8').split('R"TUYA(')[1].split(')TUYA"')[0];
let savedUid='saved_uid',accountWrites=0,cloudPosts=0;
function openPage(){
 const {document}=parseHTML(html);
 const context={document,console,JSON,Array,Number,String,Object,Error,Promise,TextEncoder,Date,setInterval(){},fetch:async(url,options)=>{
  if(url==='/api/tuya/account'){
   if(options?.method==='POST'){savedUid=JSON.parse(options.body).uid;accountWrites++;return {ok:true,json:async()=>({status:'saved'})};}
   return {ok:true,json:async()=>({uid:savedUid})};
  }
  if(url.startsWith('/api/tuya/devices')){
   if(options?.method==='POST')cloudPosts++;
   return {ok:true,json:async()=>{throw SyntaxError("Expected ',' or ']' after array element")}};
  }
  return {ok:true,json:async()=>({language:'ru',enabled:false,region:3,clientId:'client',hasSecret:true,status:'disabled',rules:[]})};
 }};vm.createContext(context);vm.runInContext(html.match(/<script>([\s\S]*)<\/script>/)[1],context);return {document,context};
}
const settle=()=>new Promise(r=>setImmediate(r));
(async()=>{
 let page=openPage();await settle();assert.equal(page.document.getElementById('accountUid').value,'saved_uid');assert(page.document.getElementById('deviceMessage').textContent.includes('Expected'));
 page.document.getElementById('accountUid').value='new_uid';page.document.getElementById('saveUid').onclick();await settle();assert.equal(savedUid,'new_uid');assert.equal(accountWrites,1);assert(page.document.getElementById('deviceMessage').textContent.includes('сохранён'));
 page=openPage();await settle();assert.equal(page.document.getElementById('accountUid').value,'new_uid','UID must survive reload even if catalogue JSON fails');assert.equal(cloudPosts,0,'saving/restoring UID must not directly require a cloud refresh');
 console.log('PASS: independent UID restore despite broken catalogue JSON, explicit saving and browser reopen without cloud POST');
})().catch(e=>{console.error(e);process.exit(1)});
