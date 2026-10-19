#include <cassert>
#include "../ANENJI_Dataloger/tuya_rules.h"
using namespace TuyaRules;
int main(){
 Rule r;r.enabled=true;r.conditionCount=1;r.conditions[0]={0,0,2000,200};r.holdSeconds=10;r.delaySeconds=5;r.cooldownSeconds=60;
 Runtime s;float v[SENSOR_COUNT]={2100};
 assert(!update(r,s,v,true,true,0,600,1000));assert(!update(r,s,v,true,true,0,600,15999));assert(update(r,s,v,true,true,0,600,16000));acknowledge(s,16000);assert(!update(r,s,v,true,true,0,600,80000));
 v[0]=1900;assert(!update(r,s,v,true,true,0,600,81000)&&s.active);v[0]=1799;assert(!update(r,s,v,true,true,0,600,82000)&&!s.active);
 v[0]=2100;assert(!update(r,s,v,true,true,0,600,83000));assert(update(r,s,v,true,true,0,600,98000));assert(!update(r,s,v,false,true,0,600,99000)&&!s.timing);
 Runtime wrap;assert(!update(r,wrap,v,true,true,0,600,0xfffffff0));assert(update(r,wrap,v,true,true,0,600,uint32_t(0xfffffff0+15000)));
 Rule night;night.enabled=true;night.weekdays=1;night.startMinute=22*60;night.endMinute=6*60;assert(inWindow(night,0,23*60));assert(inWindow(night,1,5*60));assert(!inWindow(night,1,6*60));assert(!inWindow(night,1,23*60));
 Runtime schedule;assert(update(night,schedule,v,true,true,0,23*60,1000));assert(!update(night,schedule,v,true,false,0,23*60,1000));
 r.any=true;r.conditionCount=2;r.conditions[1]={3,1,20,2};v[0]=0;v[3]=10;Runtime any;assert(!update(r,any,v,true,true,0,0,1000));assert(update(r,any,v,true,true,0,0,16000));assert(!update(r,any,v,true,true,0,0,16001,true));
 r.holdSeconds=r.delaySeconds=0;r.repeatSeconds=120;Runtime periodic;assert(update(r,periodic,v,true,true,0,0,1000));acknowledge(periodic,1000);assert(!update(r,periodic,v,true,true,0,0,120999));assert(update(r,periodic,v,true,true,0,0,121000));
 v[0]=NAN;Condition c;assert(!compare(c,v[0],true));
 Rule pair[2];Runtime rt[2];bool due[2];
 for(int i=0;i<2;++i){pair[i].cooldownSeconds=60;pair[i].enabled=true;pair[i].conditionCount=1;strcpy(pair[i].device,"relay");strcpy(pair[i].code,"switch_1");pair[i].conditions[0]=Condition(0,i,100,0);}
 v[0]=100;evaluate(pair,rt,2,v,true,true,0,0,1000,due);assert(due[0]&&!due[1]);
 acknowledge(rt[0],1000);v[0]=90;evaluate(pair,rt,2,v,true,true,0,0,2000,due);assert(!due[1]);
 evaluate(pair,rt,2,v,true,true,0,0,61000,due);assert(due[1]);
 evaluate(pair,rt,2,v,false,true,0,0,62000,due);assert(!due[0]&&!due[1]&&!rt[1].timing);
 Rule immediate;assert(immediate.cooldownSeconds==0);immediate.enabled=true;immediate.conditionCount=1;
 immediate.conditions[0]=Condition(0,0,100,10);Runtime quick;v[0]=110;
 assert(update(immediate,quick,v,true,true,0,0,1000));acknowledge(quick,1000);
 v[0]=89;assert(!update(immediate,quick,v,true,true,0,0,1001));v[0]=110;
 assert(update(immediate,quick,v,true,true,0,0,1002)); // no hidden 60s pause after a reset
 pair[1].cooldownSeconds=0;rt[0]=Runtime();rt[1]=Runtime();v[0]=100;
 evaluate(pair,rt,2,v,true,true,0,0,1000,due);acknowledge(rt[0],1000);v[0]=90;
 evaluate(pair,rt,2,v,true,true,0,0,1001,due);assert(due[1]); // opposite action can run without cross-rule cooldown
}
