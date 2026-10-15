#ifndef ANENJI_TUYA_RULES_H
#define ANENJI_TUYA_RULES_H
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
namespace TuyaRules {
constexpr size_t MAX_RULES=4,MAX_CONDITIONS=3,SENSOR_COUNT=10;
struct Condition {
 uint8_t sensor,op; float threshold,hysteresis;
 Condition(uint8_t s=0,uint8_t o=0,float t=0,float h=0):sensor(s),op(o),threshold(t),hysteresis(h){}
};
// Sensors: PV W, load W, grid W, battery %, battery V, grid V,
// PV V, load V, battery charge W, battery discharge W.
struct Rule {
 char name[41]={},device[65]={},code[41]={},value[67]={};
 bool enabled=false,any=false;
 uint8_t conditionCount=0,weekdays=127;
 uint16_t startMinute=0,endMinute=0;
 uint32_t holdSeconds=0,delaySeconds=0,cooldownSeconds=60,repeatSeconds=0;
 Condition conditions[MAX_CONDITIONS];
};
struct Runtime {
 bool conditionOn[MAX_CONDITIONS]={};
 bool active=false,timing=false,fired=false,hasRun=false;
 uint32_t since=0,lastRun=0;
};
inline bool inWindow(const Rule&r,uint8_t weekday,uint16_t minute){
 if(weekday>6||minute>1439)return false;
 // Monday is bit 0. Overnight schedules belong to the starting day.
 if(r.startMinute<r.endMinute){
  if(minute<r.startMinute||minute>=r.endMinute)return false;
 }else if(r.startMinute>r.endMinute){
  if(minute>=r.endMinute&&minute<r.startMinute)return false;
  if(minute<r.endMinute)weekday=(weekday+6)%7;
 }
 return (r.weekdays&(1U<<weekday))!=0;
}
inline bool compare(const Condition&c,float value,bool wasOn){
 if(!isfinite(value))return false;
 float h=wasOn?c.hysteresis:0;
 switch(c.op){
  case 0:return value>=c.threshold-h;
  case 1:return value<=c.threshold+h;
  case 2:return value>c.threshold-h;
  case 3:return value<c.threshold+h;
  default:return false;
 }
}
inline bool sameTarget(const Rule&a,const Rule&b){
 return strcmp(a.device,b.device)==0&&strcmp(a.code,b.code)==0;
}
inline void resetPending(Runtime&s){
 s.active=false;s.timing=false;s.fired=false;
 for(size_t c=0;c<MAX_CONDITIONS;++c)s.conditionOn[c]=false;
}
inline bool update(const Rule&r,Runtime&s,const float values[SENSOR_COUNT],
 bool fresh,bool clockValid,uint8_t weekday,uint16_t minute,uint32_t now,
 bool blocked=false){
 bool match=r.enabled&&fresh&&clockValid&&inWindow(r,weekday,minute)&&!blocked;
 if(match){
  bool combined=!r.any;
  for(size_t c=0;c<r.conditionCount;++c){
   const Condition&cond=r.conditions[c];
   bool on=cond.sensor<SENSOR_COUNT&&compare(cond,values[cond.sensor],s.conditionOn[c]);
   s.conditionOn[c]=on;
   combined=r.any?(combined||on):(combined&&on);
  }
  // No numeric conditions means a schedule-only rule.
  match=r.conditionCount?combined:true;
 }
 if(!match){resetPending(s);return false;}
 s.active=true;
 if(!s.timing){s.timing=true;s.since=now;}
 const uint32_t wait=r.holdSeconds+r.delaySeconds;
 if(uint32_t(now-s.since)<wait*1000UL)return false;
 if(s.hasRun&&uint32_t(now-s.lastRun)<r.cooldownSeconds*1000UL)return false;
 if(s.fired){
  if(!r.repeatSeconds||uint32_t(now-s.lastRun)<r.repeatSeconds*1000UL)return false;
 }
 return true;
}
inline void evaluate(const Rule*rules,Runtime*runtime,size_t count,
 const float values[SENSOR_COUNT],bool fresh,bool clockValid,
 uint8_t weekday,uint16_t minute,uint32_t now,bool*due){
 for(size_t i=0;i<count;++i){
  bool blocked=false;
  for(size_t j=0;j<i;++j)if(runtime[j].active&&sameTarget(rules[j],rules[i]))blocked=true;
  due[i]=update(rules[i],runtime[i],values,fresh,clockValid,weekday,minute,now,blocked);
 }
 for(size_t i=0;i<count;++i)if(due[i]){
  for(size_t j=0;j<count;++j)if(runtime[j].hasRun&&sameTarget(rules[j],rules[i])&&
    uint32_t(now-runtime[j].lastRun)<rules[i].cooldownSeconds*1000UL)due[i]=false;
 }
}
inline void acknowledge(Runtime&s,uint32_t now){s.fired=true;s.hasRun=true;s.lastRun=now;}
}
#endif
