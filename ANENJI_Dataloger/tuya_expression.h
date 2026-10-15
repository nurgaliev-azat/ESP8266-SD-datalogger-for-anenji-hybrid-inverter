#ifndef ANENJI_TUYA_EXPRESSION_H
#define ANENJI_TUYA_EXPRESSION_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
namespace TuyaExpression {
constexpr size_t MAX_LENGTH=192,MAX_COMPARISONS=12,MAX_TOKENS=96,MAX_DEPTH=8;
struct Result { bool valid=false,value=false;uint16_t states=0,errorAt=0; };
// Bounded recursive descent, no heap, scripts, loops, functions or implicit truthiness.
class Parser {
 const char*text;size_t pos=0,tokens=0,depth=0,comparisons=0;
 const float*values;uint8_t weekday;uint16_t minute,oldStates,newStates=0;
 bool ok=true;uint16_t error=0;
 void fail(){if(ok){ok=false;error=pos;}}
 void space(){while(text[pos]==' '||text[pos]=='\t'||text[pos]=='\r'||text[pos]=='\n')++pos;}
 bool boundary(char c){return !((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_');}
 void token(){if(++tokens>MAX_TOKENS)fail();}
 bool word(const char*w){space();size_t n=strlen(w);if(strncmp(text+pos,w,n)||!boundary(text[pos+n]))return false;pos+=n;token();return true;}
 bool symbol(const char*w){space();size_t n=strlen(w);if(strncmp(text+pos,w,n))return false;pos+=n;token();return true;}
 bool number(float&out){
  space();size_t start=pos;
  if(text[pos]=='+'||text[pos]=='-')++pos;
  bool digit=false;while(text[pos]>='0'&&text[pos]<='9'){digit=true;++pos;}
  if(text[pos]=='.'){++pos;while(text[pos]>='0'&&text[pos]<='9'){digit=true;++pos;}}
  if(!digit){pos=start;fail();return false;}
  if(text[pos]=='e'||text[pos]=='E'){
   ++pos;if(text[pos]=='+'||text[pos]=='-')++pos;size_t exponent=pos;
   while(text[pos]>='0'&&text[pos]<='9')++pos;
   if(pos==exponent){fail();return false;}
  }
  char*end=nullptr;out=strtof(text+start,&end);token();
  if(end!=text+pos||!isfinite(out)||fabs(out)>1000000){fail();return false;}
  return true;
 }
 float value(){
  space();char c=text[pos];
  if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'){
   size_t start=pos;while(!boundary(text[pos]))++pos;token();
   const char*const names[]={"pvPower","loadPower","gridPower","batteryPercent","batteryVoltage","gridVoltage","pvVoltage","loadVoltage","batteryChargePower","batteryDischargePower","hour","minute","minuteOfDay","weekday"};
   for(size_t i=0;i<14;++i)if(strlen(names[i])==pos-start&&!strncmp(text+start,names[i],pos-start)){
    if(i<10){if(!isfinite(values[i]))fail();return values[i];}
    if(i==10)return minute/60;
    if(i==11)return minute%60;
    if(i==12)return minute;
    return weekday+1;
   }
   fail();return 0;
  }
  float n=0;number(n);return n;
 }
 bool primary(){
  if(!ok)return false;
  if(symbol("(")){
   if(++depth>MAX_DEPTH){fail();return false;}
   bool result=logicalOr();if(!symbol(")"))fail();--depth;return result;
  }
  if(word("true"))return true;
  if(word("false"))return false;
  float left=value();int op=-1;
  if(symbol(">="))op=0;else if(symbol("<="))op=1;
  else if(symbol("=="))op=4;else if(symbol("!="))op=5;
  else if(symbol(">"))op=2;else if(symbol("<"))op=3;
  if(op<0){fail();return false;}
  float right=value(),hysteresis=0;
  if(word("HYST")){number(hysteresis);if(hysteresis<0||(op>=4&&hysteresis!=0))fail();}
  if(comparisons>=MAX_COMPARISONS){fail();return false;}
  const uint16_t bit=uint16_t(1U<<comparisons++);
  const float h=(oldStates&bit)?hysteresis:0;
  bool result=false;
  switch(op){case 0:result=left>=right-h;break;case 1:result=left<=right+h;break;
   case 2:result=left>right-h;break;case 3:result=left<right+h;break;
   case 4:result=left==right;break;case 5:result=left!=right;break;}
  if(result)newStates|=bit;
  return result;
 }
 bool logicalNot(){
  if(!ok)return false;
  if(word("NOT")){
   if(++depth>MAX_DEPTH){fail();return false;}
   bool result=!logicalNot();--depth;return result;
  }
  return primary();
 }
 bool logicalAnd(){bool result=logicalNot();while(ok&&word("AND")){bool right=logicalNot();result=result&&right;}return result;}
 bool logicalXor(){bool result=logicalAnd();while(ok&&word("XOR")){bool right=logicalAnd();result=result!=right;}return result;}
 bool logicalOr(){bool result=logicalXor();while(ok&&word("OR")){bool right=logicalXor();result=result||right;}return result;}
 public:
 Parser(const char*input,const float*v,uint8_t day,uint16_t time,uint16_t previous):text(input),values(v),weekday(day),minute(time),oldStates(previous){}
 Result run(){
  Result result;if(!text||strlen(text)>MAX_LENGTH){result.errorAt=MAX_LENGTH;return result;}
  result.value=logicalOr();space();if(text[pos])fail();
  result.valid=ok;result.errorAt=error;result.states=ok?newStates:oldStates;
  if(!ok)result.value=false;
  return result;
 }
};
inline Result evaluate(const char*text,const float*values,uint8_t weekday=0,uint16_t minute=0,uint16_t previous=0){return Parser(text,values,weekday,minute,previous).run();}
}
#endif
