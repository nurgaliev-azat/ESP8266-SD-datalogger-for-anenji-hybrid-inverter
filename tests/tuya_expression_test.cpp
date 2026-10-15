#include <cassert>
#include <initializer_list>
#include <cstring>
#include "../ANENJI_Dataloger/tuya_rules.h"
using TuyaExpression::evaluate;
int main(){
 float v[10]={2500,500,0,96,52};
 auto r=evaluate("pvPower >= 2000 AND batteryPercent >= 95",v);assert(r.valid&&r.value);
 r=evaluate("NOT (pvPower < 2000 OR batteryPercent < 95)",v);assert(r.valid&&r.value);
 r=evaluate("true XOR true OR true AND false",v);assert(r.valid&&!r.value);
 r=evaluate("true OR false XOR true",v);assert(r.valid&&r.value);
 r=evaluate("hour == 22 AND weekday == 1 AND minuteOfDay >= 1320",v,0,1335);assert(r.valid&&r.value);
 r=evaluate("pvPower >= 2000 HYST 200",v);assert(r.valid&&r.value);v[0]=1900;
 auto kept=evaluate("pvPower >= 2000 HYST 200",v,0,0,r.states);assert(kept.valid&&kept.value);
 assert(!evaluate("pvPower >= 2000 HYST 200",v).value);
 for(const char*text:{"pvPower","pvPower >", "pvPower > 1 AND", "(true", "true)", "NOT", "unknown > 1", "1", "true true", "pvPower = 2", "pvPower == 2 HYST 1", "pvPower > 0 HYST -1", "pvPower > 1e999", "pvPower > 0x20", "pvPower > nan", "pvPower > 1; true", "NOT NOT NOT NOT NOT NOT NOT NOT NOT true"})assert(!evaluate(text,v).valid);
 assert(evaluate("pvPower > -1.5e2",v).valid);
 char longText[194];memset(longText,' ',193);longText[193]=0;assert(!evaluate(longText,v).valid);
 v[0]=NAN;assert(!evaluate("NOT (pvPower > 1)",v).valid);
 TuyaRules::Rule rule;rule.enabled=true;strcpy(rule.expression,"NOT (pvPower >= 2000 HYST 200)");TuyaRules::Runtime runtime;
 v[0]=2500;assert(!TuyaRules::update(rule,runtime,v,true,true,0,0,0));v[0]=1900;assert(!TuyaRules::update(rule,runtime,v,true,true,0,0,1000));v[0]=1700;assert(TuyaRules::update(rule,runtime,v,true,true,0,0,2000));
}
