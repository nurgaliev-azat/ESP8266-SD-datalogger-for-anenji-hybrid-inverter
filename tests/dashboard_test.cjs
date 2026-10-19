const fs=require('fs'),vm=require('vm'),assert=require('assert'),path=require('path');
const source=fs.readFileSync(path.join(__dirname,'../ANENJI_Dataloger/web_server.h'),'utf8');
const html=source.split('R"WEB(')[1].split(')WEB"')[0];
let script=html.match(/<script>([\s\S]*?)<\/script>/)[1];
script=script.slice(0,script.indexOf('async function startDashboard()'));
const nodes=[],ids={};
function element(attrs={}){const el={...attrs,textContent:'',checked:true,clientWidth:800,clientHeight:350,disabled:false,style:{setProperty(){}},classList:{toggle(){}},setAttribute(k,v){this[k]=v},addEventListener(){},getContext(){return new Proxy({}, {get:(o,k)=>o[k]||(()=>{})})}};nodes.push(el);return el;}
for(const m of html.matchAll(/<[^/!][^>]*>/g)){
 const tag=m[0],id=tag.match(/\bid="([^"]+)"/),i18n=tag.match(/data-i18n="([^"]+)"/),title=tag.match(/data-i18n-title="([^"]+)"/),scale=tag.match(/data-scale="([^"]+)"/);
 if(id||i18n||title||scale){let e=element({dataset:{}});if(id)ids[id[1]]=e;if(i18n)e.dataset.i18n=i18n[1];if(title)e.dataset.i18nTitle=title[1];if(scale)e.dataset.scale=scale[1];}
}
const langGroup=element();let persisted='en',postCount=0,fail=false;
const context={console:{error(){}},URLSearchParams,Date,Number,String,Math,Promise,document:{documentElement:{},getElementById:id=>ids[id],querySelector:()=>langGroup,querySelectorAll:sel=>sel==='[data-i18n]'?nodes.filter(e=>e.dataset?.i18n):sel==='[data-i18n-title]'?nodes.filter(e=>e.dataset?.i18nTitle):sel==='[data-scale]'?nodes.filter(e=>e.dataset?.scale):[]},window:{devicePixelRatio:1,addEventListener(){}},getComputedStyle:()=>({getPropertyValue:()=> '#123456'}),fetch:async (url,opt)=>{
 if(opt?.method==='POST'){postCount++;if(fail)return {ok:false,status:500};persisted=opt.body.get('language');return {ok:true,json:async()=>({language:persisted,status:persisted==='ru'?'Данные обновлены':'Data updated',mode:persisted==='ru'?'Зарядка':'Charging',faults:persisted==='ru'?'Неполадки: Перегрев DCDC [1]':'Faults: DC-DC overtemperature [1]',warnings:persisted==='ru'?'Предупреждений нет':'No warnings',updateTime:'2026-10-12 22:10:00'})};}
 const payload=url==='/api/live'?{language:persisted,status:persisted==='ru'?'Данные обновлены':'Data updated',mode:persisted==='ru'?'Зарядка':'Charging',faults:persisted==='ru'?'Неполадки: Перегрев DCDC [1]':'Faults: DC-DC overtemperature [1]',warnings:persisted==='ru'?'Предупреждений нет':'No warnings',updateTime:'2026-10-12 22:10:00',valid:true}:url.startsWith('/api/history')||url==='/api/samples'?[]:{};
 return {ok:true,json:async()=>payload};
}};vm.createContext(context);vm.runInContext(script,context);
(async()=>{
 vm.runInContext("applyLanguage('en')",context);assert.equal(context.document.documentElement.lang,'en');
 await vm.runInContext("changeLanguage('ru')",context);assert.equal(persisted,'ru');assert.equal(ids.mode.textContent,'Режим: Зарядка');assert(ids.faults.textContent.includes('Перегрев'));assert.equal(ids.langRu['aria-pressed'],'true');
 for(const e of nodes.filter(e=>e.dataset?.i18n))assert(e.textContent.length);
 assert.equal(ids.prevPeriod.title,'Предыдущий период');
 await vm.runInContext("changeLanguage('ru')",context);assert.equal(postCount,1,'same language must not write flash');
 await vm.runInContext("changeLanguage('en')",context);assert.equal(ids.mode.textContent,'Mode: Charging');assert.equal(ids.prevPeriod.title,'Previous period');
 for(const e of nodes.filter(e=>e.dataset?.i18n))assert(!/[А-Яа-яЁё]/.test(e.textContent),'English label contains Cyrillic: '+e.textContent);
 fail=true;await vm.runInContext("changeLanguage('ru')",context);assert.equal(context.document.documentElement.lang,'en');assert.equal(persisted,'en');assert(ids.languageMessage.textContent.includes('Could not save'));assert(!ids.langEn.disabled);
 assert(!source.includes("<select name='language'"));
 const range=vm.runInContext('timeAxisRange([{minute:600},{minute:660}],800)',context);
 assert(range.start<=600&&range.end>=660&&range.end-range.start<1440);
 const single=vm.runInContext('timeAxisRange([{minute:1439}],320)',context);
 assert(single.start<single.end&&single.end<=1440&&single.start<=1439);
 const empty=vm.runInContext('timeAxisRange([],800)',context);assert.equal(empty.start,0);assert.equal(empty.end,1440);
 const narrow=vm.runInContext('timeAxisRange([{minute:600},{minute:660}],320)',context);
 assert(narrow.step>=range.step);
 vm.runInContext("selected={year:2026,month:10,day:12};scale='hour';hours=[{year:2026,month:10,day:12,hour:8,hasData:true,grid:0},{year:2026,month:10,day:12,hour:10,hasData:true,grid:1}]",context);
 let cols=vm.runInContext('historyAxisColumns(columns())',context);
 assert.equal(cols.length,3);assert.equal(cols[0].index,8);assert.equal(cols[2].index,10);assert.equal(cols[1].hasData,false);
 vm.runInContext("scale='day';hours=[{year:2026,month:10,day:12,hasData:true,grid:0}]",context);
 cols=vm.runInContext('historyAxisColumns(columns())',context);assert.equal(cols.length,1);assert.equal(cols[0].index,11);
 vm.runInContext("scale='month';hours=[{year:2026,month:3,day:1,hasData:true},{year:2026,month:5,day:1,hasData:true}]",context);
 cols=vm.runInContext('historyAxisColumns(columns())',context);assert.equal(cols.length,3);assert.equal(cols[0].index,2);assert.equal(cols[2].index,4);
 vm.runInContext("scale='hour';hours=[]",context);assert.equal(vm.runInContext('historyAxisColumns(columns()).length',context),24);
 vm.runInContext('tuyaAlerts=[{name:"<img src=x>",device:"relay1",code:"api_error",paused:true}];applyLanguage("ru")',context);
 assert(ids.tuyaAlerts.textContent.includes('Ошибка сценария'));assert(ids.tuyaAlerts.textContent.includes('relay1'));assert(ids.tuyaAlerts.textContent.includes('Попытки остановлены'));
 vm.runInContext('applyLanguage("en")',context);assert(ids.tuyaAlerts.textContent.includes('Scenario error'));assert(!ids.tuyaAlerts.textContent.includes('Ошибка'));
 vm.runInContext('tuyaAlerts=[];renderTuyaAlerts()',context);assert.equal(ids.tuyaAlerts.textContent,'');
 vm.runInContext('tuyaAlerts=[{name:"Boiler",device:"relay1",code:"confirmation_timeout"}];applyLanguage("en")',context);assert(ids.tuyaAlerts.textContent.includes('60 seconds'));
 vm.runInContext('applyLanguage("ru")',context);assert(ids.tuyaAlerts.textContent.includes('60 секунд'));
 console.log('PASS: X ranges, single point, empty history, zero measurements, gaps, hour/day/month labels and responsive tick spacing');
 console.log('PASS: EN/RU labels, titles, units, diagnostics, same-language no-op and save-failure rollback');
})().catch(e=>{console.error(e);process.exit(1)});
