const fs=require('fs'),path=require('path'),vm=require('vm'),assert=require('assert');
const source=fs.readFileSync(path.join(__dirname,'../ANENJI_Dataloger/tuya_page.h'),'utf8');
const code=source.slice(source.indexOf('function expressionCheck('),source.indexOf('function legacyExpression('));
const context={TextEncoder};vm.createContext(context);vm.runInContext(code,context);
for(const text of ['true','false','NOT true','pvPower >= 2000 HYST 200 AND NOT (batteryPercent < 95 OR gridPower > 100)','true XOR true OR true AND false','hour == 22 AND weekday == 1','pvPower > -1.5e2','pvPower > loadPower'])assert(context.expressionCheck(text).valid,text);
for(const text of ['', 'pvPower','pvPower >', 'pvPower > 1 AND','(true','true)','NOT','unknown > 1','1','true true','pvPower = 2','pvPower == 2 HYST 1','pvPower > 0 HYST -1','pvPower > 1e999','pvPower > 0x20','pvPower > nan','true; false','NOT NOT NOT NOT NOT NOT NOT NOT NOT true',' '.repeat(193)])assert(!context.expressionCheck(text).valid,text);
for(const text of ['SET("dev","switch_1",true)','SET("dev","temp",21.5)','SET("dev","mode","eco")'])assert(context.actionCheck(text).valid,text);
for(const text of ['DELETE("dev")','SET("dev","switch_1",true,false)','SET("dev","mode",null)','SET("bad/path","mode",true)','SET("dev","mode",[])','SET("dev","mode",{})','SET("dev","mode",1e999)'])assert(!context.actionCheck(text).valid,text);
console.log('PASS: browser expression grammar, nesting limits, numeric validation and SET-only action syntax');
