#ifndef ANENJI_TUYA_CLOUD_H
#define ANENJI_TUYA_CLOUD_H
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>
#include <memory>
#include <algorithm>
#include <ctype.h>
#include <sys/time.h>
#include "tuya_rules.h"
#include "tuya_devices.h"

namespace TuyaCloud {
using namespace TuyaRules;
constexpr uint32_t MAGIC=0x54555941;
struct Credentials {
 uint32_t magic;uint8_t version,region,enabled,reserved;
 char clientId[33],secret[65];uint16_t crc;
};
constexpr size_t CREDENTIAL_OFFSET=sizeof(SettingsRecord);
static_assert(CREDENTIAL_OFFSET+sizeof(Credentials)<=SETTINGS_BYTES,
              "Tuya credentials overlap history EEPROM");
Credentials credentials={};
Rule rules[MAX_RULES];Runtime runtime[MAX_RULES];size_t ruleCount=0;
String accessToken,state="disabled";
time_t tokenExpires=0;
uint32_t bootMs=0,lastTick=0,lastAttempt=0;
bool attempted=false,testRequested=false,rulesLoaded=false;
uint8_t failures[MAX_RULES]={};
bool suspended[MAX_RULES]={};
struct DeviceJob {
 bool queued=false;uint32_t id=0,started=0;uint8_t attempts=0;
 char uid[65]={},cursor[TuyaDevices::MAX_CURSOR+1]={};
 const char*status="idle";String error;
} deviceJob;
int verifyRule=-1;uint8_t verifyAttempts=0;uint32_t verifyAfter=0;
const char* const HOSTS[]={"openapi.tuyacn.com","openapi.tuyaus.com",
 "openapi-ueaz.tuyaus.com","openapi.tuyaeu.com","openapi-weaz.tuyaeu.com",
 "openapi.tuyain.com","openapi-sg.iotbing.com"};
const char* const SENSORS[]={"pvPower","loadPower","gridPower","batteryPercent",
 "batteryVoltage","gridVoltage","pvVoltage","loadVoltage","batteryChargePower","batteryDischargePower"};
uint16_t credentialCrc(const Credentials&c){
 return crc16Modbus(reinterpret_cast<const uint8_t*>(&c),offsetof(Credentials,crc));
}
bool safeIdentifier(const char*s,size_t maximum){
 if(!s||!s[0]||strlen(s)>maximum)return false;
 for(const char*p=s;*p;++p)if(!isalnum(static_cast<unsigned char>(*p))&&*p!='_'&&*p!='-')return false;
 return true;
}
void loadCredentials(){
 EEPROM.get(CREDENTIAL_OFFSET,credentials);
 if(credentials.magic!=MAGIC||credentials.version!=1||credentials.region>6||
   credentials.enabled>1||credentials.crc!=credentialCrc(credentials)||
   !memchr(credentials.clientId,0,sizeof(credentials.clientId))||
   !memchr(credentials.secret,0,sizeof(credentials.secret))){credentials={};}
}
bool saveCredentials(const Credentials&next){
 if(memcmp(&next,&credentials,sizeof(next))==0)return true;
 Credentials old;EEPROM.get(CREDENTIAL_OFFSET,old);
 EEPROM.put(CREDENTIAL_OFFSET,next);
 if(!EEPROM.commit()){EEPROM.put(CREDENTIAL_OFFSET,old);return false;}
 deviceJob.queued=false;deviceJob.status="idle";deviceJob.error="";
 credentials=next;accessToken="";tokenExpires=0;verifyRule=-1;attempted=false;
 for(size_t i=0;i<MAX_RULES;++i){runtime[i]=Runtime();failures[i]=0;suspended[i]=false;}
 state=credentials.enabled?"ready":"disabled";
 return true;
}
bool writeFile(const char*path,const String&text){
 if(!sdReady)return false;
 const String temp=String(path)+".NEW",backup=String(path)+".BAK";
 SD.remove(temp.c_str());File file=SD.open(temp.c_str(),FILE_WRITE);
 if(!file)return false;
 bool ok=file.print(text)==text.length();file.flush();file.close();
 file=SD.open(temp.c_str(),FILE_READ);
 if(!file)return false;
 ok=ok&&file.size()==text.length();
 for(size_t i=0;ok&&i<text.length();++i)if(file.read()!=static_cast<uint8_t>(text[i]))ok=false;
 file.close();if(!ok){SD.remove(temp.c_str());return false;}
 SD.remove(backup.c_str());
 bool exists=SD.exists(path);
 if(exists&&!SD.rename(path,backup.c_str()))return false;
 if(!SD.rename(temp.c_str(),path)){
  if(exists)SD.rename(backup.c_str(),path);
  return false;
 }
 return true;
}
bool integer(JsonVariantConst v,uint32_t low,uint32_t high,uint32_t&out){
 if(!v.is<uint32_t>())return false;
 uint32_t n=v.as<uint32_t>();if(n<low||n>high)return false;out=n;return true;
}
bool parseRules(JsonVariantConst input,Rule*out,size_t&count){
 if(!input.is<JsonArrayConst>())return false;
 JsonArrayConst array=input.as<JsonArrayConst>();if(array.size()>MAX_RULES)return false;
 count=0;
 for(JsonObjectConst o:array){
  if(o.isNull())return false;
  Rule r;const char*name=o["name"]|"";
  if(!name[0]||strlen(name)>=sizeof(r.name))return false;
  strlcpy(r.name,name,sizeof(r.name));
  if(!o["enabled"].is<bool>())return false;r.enabled=o["enabled"].as<bool>();
  const char*expression=o["expression"]|"";
  if(strlen(expression)>TuyaExpression::MAX_LENGTH)return false;
  if(expression[0]){
   float empty[SENSOR_COUNT]={};
   if(!TuyaExpression::evaluate(expression,empty).valid)return false;
   strlcpy(r.expression,expression,sizeof(r.expression));
  }
  if(o.containsKey("action")){
   const char*action=o["action"]|"";size_t length=strlen(action);
   if(length<7||length>256||strncmp(action,"SET(",4)||action[length-1]!=')')return false;
   String args=String("[")+String(action).substring(4,length-1)+"]";
   DynamicJsonDocument statement(512);
   if(deserializeJson(statement,args.c_str())||!statement.is<JsonArray>()||statement.size()!=3||
     !statement[0].is<const char*>()||!statement[1].is<const char*>())return false;
   const char*device=statement[0],*code=statement[1];
   if(!safeIdentifier(device,64)||!safeIdentifier(code,40))return false;
   JsonVariantConst value=statement[2];
   if(value.isNull()||value.is<JsonObjectConst>()||value.is<JsonArrayConst>()||
     (value.is<double>()&&!isfinite(value.as<double>()))||measureJson(value)>=sizeof(r.value))return false;
   strlcpy(r.device,device,sizeof(r.device));strlcpy(r.code,code,sizeof(r.code));serializeJson(value,r.value,sizeof(r.value));
  }else{
   const char*device=o["device"]|"",*code=o["code"]|"";
   if(!safeIdentifier(device,64)||!safeIdentifier(code,40))return false;
   strlcpy(r.device,device,sizeof(r.device));strlcpy(r.code,code,sizeof(r.code));
   JsonVariantConst value=o["value"];
   if(value.isNull()||value.is<JsonObjectConst>()||value.is<JsonArrayConst>()||
     (value.is<double>()&&!isfinite(value.as<double>()))||measureJson(value)>=sizeof(r.value))return false;
   serializeJson(value,r.value,sizeof(r.value));
  }
  // Legacy flat conditions remain supported when no expression is supplied.
  r.any=o["any"]|false;
  uint32_t n;
  if(!integer(o["weekdays"],1,127,n))return false;r.weekdays=n;
  if(!integer(o["startMinute"],0,1439,n))return false;r.startMinute=n;
  if(!integer(o["endMinute"],0,1439,n))return false;r.endMinute=n;
  if(!integer(o["holdSeconds"],0,604800,n))return false;r.holdSeconds=n;
  if(!integer(o["delaySeconds"],0,604800,n))return false;r.delaySeconds=n;
  if(r.holdSeconds+r.delaySeconds>604800)return false;
  if(!integer(o["cooldownSeconds"],60,604800,n))return false;r.cooldownSeconds=n;
  if(!integer(o["repeatSeconds"],0,604800,n)||(n&&n<60))return false;r.repeatSeconds=n;
  if(r.expression[0]){out[count++]=r;continue;}
  if(!o["any"].is<bool>()||!o["conditions"].is<JsonArrayConst>())return false;
  JsonArrayConst conditions=o["conditions"].as<JsonArrayConst>();
  if(conditions.size()>MAX_CONDITIONS)return false;
  for(JsonObjectConst c:conditions){
   Condition&cond=r.conditions[r.conditionCount++];
   if(!integer(c["sensor"],0,SENSOR_COUNT-1,n))return false;cond.sensor=n;
   if(!integer(c["op"],0,3,n))return false;cond.op=n;
   if(!c["threshold"].is<float>()||!c["hysteresis"].is<float>())return false;
   cond.threshold=c["threshold"].as<float>();cond.hysteresis=c["hysteresis"].as<float>();
   if(!isfinite(cond.threshold)||!isfinite(cond.hysteresis)||cond.hysteresis<0||
      fabs(cond.threshold)>1000000||cond.hysteresis>1000000)return false;
  }
  out[count++]=r;
 }
 return true;
}
void rulesJson(JsonArray array,const Rule*source=rules,size_t count=ruleCount){
 for(size_t i=0;i<count;++i){
  const Rule&r=source[i];JsonObject o=array.createNestedObject();
  o["name"]=r.name;o["enabled"]=r.enabled;o["device"]=r.device;o["code"]=r.code;
  o["expression"]=r.expression;
  DynamicJsonDocument value(256);deserializeJson(value,r.value);o["value"]=value.as<JsonVariant>();
  String action=String("SET(\"")+r.device+"\",\""+r.code+"\","+r.value+")";
  o["action"]=action;
  o["any"]=r.any;o["weekdays"]=r.weekdays;o["startMinute"]=r.startMinute;o["endMinute"]=r.endMinute;
  o["holdSeconds"]=r.holdSeconds;o["delaySeconds"]=r.delaySeconds;
  o["cooldownSeconds"]=r.cooldownSeconds;o["repeatSeconds"]=r.repeatSeconds;
  JsonArray conditions=o.createNestedArray("conditions");
  for(size_t j=0;j<r.conditionCount;++j){
   const Condition&c=r.conditions[j];JsonObject v=conditions.createNestedObject();
   v["sensor"]=c.sensor;v["op"]=c.op;v["threshold"]=c.threshold;v["hysteresis"]=c.hysteresis;
  }
 }
}
void loadRules(){
 if(!sdReady||rulesLoaded)return;
 // Recover last valid settings if power failed during a rename.
 for(const char*path:{"/TUYA.JSON","/TUYA.JSON.BAK"}){
  File f=SD.open(path,FILE_READ);if(!f)continue;
  if(f.size()>10000){f.close();continue;}
  DynamicJsonDocument doc(8192);auto error=deserializeJson(doc,f);f.close();
  Rule candidate[MAX_RULES];size_t count=0;
  if(!error&&(doc["version"]==1||doc["version"]==2)&&parseRules(doc["rules"],candidate,count)){
   ruleCount=count;for(size_t i=0;i<count;++i){rules[i]=candidate[i];runtime[i]=Runtime();failures[i]=0;suspended[i]=false;}
   rulesLoaded=true;return;
  }
 }
 rulesLoaded=true;
}
String digest(const String&text){
 br_sha256_context ctx;uint8_t result[32];char hex[65];
 br_sha256_init(&ctx);br_sha256_update(&ctx,text.c_str(),text.length());br_sha256_out(&ctx,result);
 for(size_t i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",result[i]);return String(hex);
}
String sign(const String&text){
 br_hmac_key_context key;br_hmac_context ctx;uint8_t result[32];char hex[65];
 br_hmac_key_init(&key,&br_sha256_vtable,credentials.secret,strlen(credentials.secret));
 br_hmac_init(&ctx,&key,0);br_hmac_update(&ctx,text.c_str(),text.length());br_hmac_out(&ctx,result);
 for(size_t i=0;i<32;++i)snprintf(hex+i*2,3,"%02X",result[i]);return String(hex);
}
class LimitedCloudStream:public Stream {
 Stream&source;size_t consumed=0,maximum;
 public:
 LimitedCloudStream(Stream&s,size_t limit):source(s),maximum(limit){setTimeout(3500);}
 int available()override{return consumed<maximum?source.available():0;}
 int read()override{if(consumed>=maximum)return -1;int value=source.read();if(value>=0)++consumed;return value;}
 int peek()override{return consumed<maximum?source.peek():-1;}
 void flush()override{}
 size_t write(uint8_t)override{return 0;}
 bool exhausted()const{return consumed>=maximum;}
};
// Every call validates the server chain and hostname. No insecure TLS fallback.
bool request(const char*method,const String&path,const String&body,bool tokenRequest,JsonDocument&reply,JsonVariantConst responseFilter=JsonVariantConst(),size_t maxResponse=4096){
 if(WiFi.status()!=WL_CONNECTED||!validClock()){state="offline";return false;}
 if(!sdReady){state="sd_required";return false;}
 if(ESP.getFreeHeap()<29000||ESP.getMaxFreeBlockSize()<18000){state="low_memory";return false;}
 File cert=SD.open("/TUYA.CA",FILE_READ);
 if(!cert||cert.size()>4096){if(cert)cert.close();state="ca_required";return false;}
 String pem=cert.readString();cert.close();
 BearSSL::X509List trust(pem.c_str());pem="";
 if(!trust.getCount()){state="ca_invalid";return false;}
 BearSSL::WiFiClientSecure client;client.setTrustAnchors(&trust);
 client.setBufferSizes(16384,512);client.setTimeout(3500);
 HTTPClient http;http.setTimeout(3500);http.setReuse(false);http.useHTTP10(true);
 if(!http.begin(client,String("https://")+HOSTS[credentials.region]+path)){state="http_error";return false;}
 struct timeval now;gettimeofday(&now,nullptr);
 char timestamp[24];snprintf(timestamp,sizeof(timestamp),"%llu",static_cast<unsigned long long>(now.tv_sec)*1000ULL+now.tv_usec/1000);
 String toSign=String(method)+"\n"+digest(body)+"\n\n"+path;
 String signature=sign(String(credentials.clientId)+(tokenRequest?String(""):accessToken)+timestamp+toSign);
 http.addHeader("client_id",credentials.clientId);http.addHeader("t",timestamp);
 http.addHeader("sign_method","HMAC-SHA256");http.addHeader("sign",signature);
 if(!tokenRequest)http.addHeader("access_token",accessToken);
 if(body.length())http.addHeader("Content-Type","application/json");
 int status=http.sendRequest(method,body);
 if(status!=200){state=status<0?"tls_or_network_error":"http_error";http.end();return false;}
 int length=http.getSize();if(length>static_cast<int>(maxResponse)){state="reply_too_large";http.end();return false;}
 LimitedCloudStream stream(http.getStream(),maxResponse);
 DeserializationError error;
 if(responseFilter.isNull())error=deserializeJson(reply,stream);
 else error=deserializeJson(reply,stream,DeserializationOption::Filter(responseFilter));
 bool exhausted=stream.exhausted();http.end();
 if(error){state=exhausted?"reply_too_large":error==DeserializationError::NoMemory?"low_memory":"invalid_reply";return false;}
 if(reply["success"]!=true){
  // Never display tokens, secrets, response bodies or untrusted cloud messages.
  state="api_error";
  int code=reply["code"]|0;
  if(code==1010||code==1011||code==1012){accessToken="";tokenExpires=0;}
  return false;
 }
 return true;
}
bool ensureToken(){
 time_t now=time(nullptr);
 if(accessToken.length()&&now+60<tokenExpires)return true;
 DynamicJsonDocument reply(2048);
 if(!request("GET","/v1.0/token?grant_type=1","",true,reply))return false;
 const char*token=reply["result"]["access_token"]|"";
 uint32_t seconds=reply["result"]["expire_time"]|0UL;
 if(!token[0]||strlen(token)>256||seconds<60||seconds>86400){state="invalid_reply";return false;}
 accessToken=token;tokenExpires=time(nullptr)+seconds;state="connected";return true;
}
void expireDeviceJob(){
 if(deviceJob.queued&&uint32_t(millis()-deviceJob.started)>120000){
  deviceJob.queued=false;deviceJob.status="error";deviceJob.error="device_timeout";
 }
}
bool loadDeviceProfile(JsonDocument&profile){
 if(!sdReady)return false;
 for(const char*path:{"/TUYA.ACC","/TUYA.ACC.BAK"}){
  File file=SD.open(path,FILE_READ);if(!file)continue;
  if(file.size()>512){file.close();continue;}
  auto error=deserializeJson(profile,file);file.close();
  if(!error&&String(profile["clientId"]|"")==credentials.clientId&&
     profile["region"].as<unsigned>()==credentials.region&&
     TuyaDevices::identifier(profile["uid"]|"",64))return true;
  profile.clear();
 }
 return false;
}
void deviceStep(){
 if(!deviceJob.queued)return;
 lastAttempt=millis();attempted=true;
 if(accessToken.isEmpty()||time(nullptr)+60>=tokenExpires){
  if(!ensureToken()&&++deviceJob.attempts>=3){deviceJob.queued=false;deviceJob.status="error";deviceJob.error=state;}
  return;
 }
 deviceJob.status="loading";
 DynamicJsonDocument responseFilter(512),reply(1536);TuyaDevices::filter(responseFilter);
 bool ok=request("GET",TuyaDevices::path(deviceJob.uid,deviceJob.cursor),"",false,reply,responseFilter.as<JsonVariantConst>(),24576);
 if(!ok){
  deviceJob.error=state;
  // Retry only when a token was invalidated. Other errors are actionable and require a new click.
  if(accessToken.isEmpty()&&++deviceJob.attempts<3){deviceJob.status="queued";return;}
  deviceJob.queued=false;deviceJob.status="error";return;
 }
 DynamicJsonDocument page(2048);
 if(!TuyaDevices::sanitize(reply.as<JsonVariantConst>(),page,deviceJob.uid,deviceJob.cursor,
    credentials.clientId,credentials.region,static_cast<uint32_t>(time(nullptr)))){
  deviceJob.queued=false;deviceJob.status="error";deviceJob.error="invalid_reply";return;
 }
 String text;serializeJson(page,text);
 if(!writeFile("/TUYA.DEV",text)){
  deviceJob.queued=false;deviceJob.status="error";deviceJob.error="save_failed";return;
 }
 deviceJob.queued=false;deviceJob.status="done";deviceJob.error="";
}
bool sendRule(size_t index){
 const Rule&r=rules[index];
 String body="{\"commands\":[{\"code\":\""+String(r.code)+"\",\"value\":"+r.value+"}]}";
 DynamicJsonDocument reply(1024);
 if(!request("POST",String("/v1.0/iot-03/devices/")+r.device+"/commands",body,false,reply))return false;
 if(reply["result"]!=true){state="command_rejected";return false;}
 state="confirming";verifyRule=index;verifyAttempts=0;verifyAfter=millis();return true;
}
bool confirmRule(size_t index){
 const Rule&r=rules[index];DynamicJsonDocument reply(4096);
 if(!request("GET",String("/v1.0/iot-03/devices/")+r.device+"/status","",false,reply))return false;
 DynamicJsonDocument expected(256);deserializeJson(expected,r.value);
 for(JsonObjectConst value:reply["result"].as<JsonArrayConst>()){
  if(String(value["code"]|"")==r.code){
   String actual,wanted;serializeJson(value["value"],actual);serializeJson(expected,wanted);
   if(actual==wanted){acknowledge(runtime[index],millis());state="confirmed";return true;}
  }
 }
 state="not_confirmed";return false;
}
bool freshData(){
 return tuyaHasMeasurement&&uint32_t(millis()-tuyaMeasurementMs)<=
   std::min(uint32_t(300),std::max(uint32_t(30),appSettings.registerIntervalSeconds*3))*1000UL;
}
void tick(){
 const uint32_t now=millis();if(uint32_t(now-lastTick)<1000)return;lastTick=now;
 expireDeviceJob();
 loadRules();
 if(!credentials.enabled&&!testRequested&&!deviceJob.queued){
  for(size_t i=0;i<ruleCount;++i)resetPending(runtime[i]);
  verifyRule=-1;state="disabled";return;
 }
 if(!credentials.clientId[0]||!credentials.secret[0]){state="credentials_required";return;}
 if(!validClock()||WiFi.status()!=WL_CONNECTED){
  for(size_t i=0;i<ruleCount;++i)resetPending(runtime[i]);
  verifyRule=-1;state="offline";return;
 }
 if(uint32_t(now-bootMs)<30000){state="warming_up";return;}
 if(!sdReady){
  for(size_t i=0;i<ruleCount;++i)resetPending(runtime[i]);
  verifyRule=-1;state="sd_required";return;
 }
 struct tm local;time_t clock=time(nullptr);localtime_r(&clock,&local);
 float values[SENSOR_COUNT]={currentPower.pv,currentPower.load,currentPower.grid,
  float(currentBatteryPercent),currentBatteryVoltage,currentGridVoltage,currentPvVoltage,
  currentLoadVoltage,currentPower.batteryCharge,currentPower.batteryDischarge};
 bool fresh=freshData();bool due[MAX_RULES]={};
 evaluate(rules,runtime,ruleCount,values,fresh&&credentials.enabled,true,(local.tm_wday+6)%7,
  local.tm_hour*60+local.tm_min,now,due);
 for(size_t i=0;i<ruleCount;++i){
  if(!runtime[i].active){failures[i]=0;suspended[i]=false;}
  if(suspended[i])due[i]=false;
 }
 if(verifyRule>=0&&!runtime[verifyRule].active){verifyRule=-1;state="cancelled";}
 if(attempted&&uint32_t(now-lastAttempt)<15000)return;
 if(testRequested){
  testRequested=false;lastAttempt=now;attempted=true;accessToken="";tokenExpires=0;
  if(ensureToken())state="connected";return;
 }
 if(!fresh){
  if(deviceJob.queued&&verifyRule<0)deviceStep();else state="stale_data";
  return;
 }
 if(state=="stale_data"||state=="offline"||state=="sd_required"||state=="warming_up")state="ready";
 if(verifyRule>=0){
  if(uint32_t(now-verifyAfter)<15000)return;
  lastAttempt=now;attempted=true;
  if(accessToken.isEmpty()||time(nullptr)+60>=tokenExpires){
   if(!ensureToken()&&++verifyAttempts>=3){suspended[verifyRule]=true;verifyRule=-1;state="confirmation_failed";}
   return;
  }
  if(confirmRule(verifyRule)){failures[verifyRule]=0;verifyRule=-1;return;}
  if(++verifyAttempts>=3){
   suspended[verifyRule]=true;verifyRule=-1;state="confirmation_failed";
  }
  return;
 }
 for(size_t i=0;i<ruleCount;++i)if(due[i]){
  lastAttempt=now;attempted=true;
  if(accessToken.isEmpty()||time(nullptr)+60>=tokenExpires){
   if(!ensureToken()&&++failures[i]>=3){suspended[i]=true;state="retry_limit";}
   return;
  }
  if(!sendRule(i)&&++failures[i]>=3){suspended[i]=true;state="retry_limit";}
  return;
 }
 if(deviceJob.queued)deviceStep();
}
void jsonReply(int status,const char*code){
 DynamicJsonDocument doc(256);doc["status"]=code;String body;serializeJson(doc,body);
 anenjiWebServer.send(status,"application/json; charset=utf-8",body);
}
bool readBody(JsonDocument&doc){
 // application/json is not a cross-origin simple form request; no CORS is enabled.
 if(!anenjiWebServer.header("Content-Type").startsWith("application/json")){
  jsonReply(415,"json_required");return false;
 }
 const String&body=anenjiWebServer.arg("plain");
 if(body.length()>10000||deserializeJson(doc,body)){jsonReply(400,"invalid_json");return false;}
 return true;
}
void getDevices(){
 expireDeviceJob();
 DynamicJsonDocument doc(3072),profile(512);
 doc["status"]=deviceJob.status;doc["jobId"]=deviceJob.id;doc["error"]=deviceJob.error;
 doc["uid"]="";
 if(loadDeviceProfile(profile)){
  const char*uid=profile["uid"];doc["uid"]=uid;
  File file=SD.open("/TUYA.DEV",FILE_READ);
  if(file){
   DynamicJsonDocument page(2048);bool valid=file.size()<=4096&&!deserializeJson(page,file);file.close();
   if(valid&&page["version"]==1&&String(page["uid"]|"")==uid&&
      String(page["clientId"]|"")==credentials.clientId&&page["region"].as<unsigned>()==credentials.region){
    // Construct the response explicitly rather than echoing arbitrary on-card JSON.
    JsonObject output=doc.createNestedObject("page");
    output["uid"]=uid;output["updatedAt"]=page["updatedAt"];output["requestCursor"]=page["requestCursor"];
    output["nextCursor"]=page["nextCursor"];output["hasMore"]=page["hasMore"];output["total"]=page["total"];
    JsonArray devices=output.createNestedArray("devices");
    size_t count=0;for(JsonObjectConst item:page["devices"].as<JsonArrayConst>()){
     if(++count>TuyaDevices::PAGE_SIZE||!TuyaDevices::identifier(item["id"]|"",64))break;
     JsonObject out=devices.createNestedObject();out["id"]=item["id"];out["name"]=item["name"];
     out["category"]=item["category"];out["online"]=item["online"];
    }
   }
  }
 }
 if(doc.overflowed()){jsonReply(500,"low_memory");return;}
 String body;serializeJson(doc,body);anenjiWebServer.sendHeader("Cache-Control","no-store");
 anenjiWebServer.send(200,"application/json; charset=utf-8",body);
}
void queueDevices(){
 expireDeviceJob();DynamicJsonDocument doc(1024);if(!readBody(doc))return;
 const char*uid=doc["uid"]|"",*cursor=doc["cursor"]|"";
 if(!TuyaDevices::identifier(uid,64)||!TuyaDevices::cursorValid(cursor)){jsonReply(400,"invalid_uid_or_cursor");return;}
 if(deviceJob.queued){jsonReply(409,"device_busy");return;}
 if(!credentials.clientId[0]||!credentials.secret[0]){jsonReply(400,"credentials_required");return;}
 if(!sdReady){jsonReply(503,"sd_required");return;}
 // Later pages must belong to the current account/cache and match its continuation token.
 if(cursor[0]){
  File file=SD.open("/TUYA.DEV",FILE_READ);DynamicJsonDocument page(2048);
  bool valid=file&&file.size()<=4096&&!deserializeJson(page,file);if(file)file.close();
  if(!valid||String(page["uid"]|"")!=uid||String(page["clientId"]|"")!=credentials.clientId||
    page["region"].as<unsigned>()!=credentials.region||page["hasMore"]!=true||String(page["nextCursor"]|"")!=cursor){jsonReply(409,"device_refresh_required");return;}
 }
 DynamicJsonDocument profile(512);profile["uid"]=uid;profile["clientId"]=credentials.clientId;profile["region"]=credentials.region;
 String text;serializeJson(profile,text);
 DynamicJsonDocument previous(512);
 bool unchanged=loadDeviceProfile(previous)&&String(previous["uid"]|"")==uid;
 if(!unchanged&&!writeFile("/TUYA.ACC",text)){jsonReply(500,"save_failed");return;}
 strlcpy(deviceJob.uid,uid,sizeof(deviceJob.uid));strlcpy(deviceJob.cursor,cursor,sizeof(deviceJob.cursor));
 deviceJob.started=millis();++deviceJob.id;deviceJob.attempts=0;deviceJob.queued=true;deviceJob.status="queued";deviceJob.error="";
 DynamicJsonDocument response(256);response["status"]="queued";response["jobId"]=deviceJob.id;
 String body;serializeJson(response,body);anenjiWebServer.send(202,"application/json; charset=utf-8",body);
}

void getConfig(){
 DynamicJsonDocument doc(8192);
 doc["language"]=appSettings.language;doc["enabled"]=bool(credentials.enabled);
 doc["region"]=credentials.region;doc["clientId"]=credentials.clientId;
 doc["hasSecret"]=credentials.secret[0]!=0;doc["status"]=state;
 doc["sdReady"]=sdReady;doc["hasCA"]=sdReady&&SD.exists("/TUYA.CA");
 doc["fresh"]=freshData();doc["heap"]=ESP.getFreeHeap();
 rulesJson(doc.createNestedArray("rules"));
 String body;serializeJson(doc,body);anenjiWebServer.sendHeader("Cache-Control","no-store");
 anenjiWebServer.send(200,"application/json; charset=utf-8",body);
}
void saveConfig(){
 DynamicJsonDocument doc(1024);if(!readBody(doc))return;
 if(!doc["enabled"].is<bool>()||!doc["region"].is<unsigned>()||doc["region"].as<unsigned>()>6){jsonReply(400,"invalid_config");return;}
 const char*id=doc["clientId"]|"",*secret=doc["secret"]|"";
 if(!safeIdentifier(id,32)||strlen(secret)>64){jsonReply(400,"invalid_config");return;}
 Credentials next=credentials;
 next.magic=MAGIC;next.version=1;next.region=doc["region"];next.enabled=doc["enabled"].as<bool>();next.reserved=0;
 memset(next.clientId,0,sizeof(next.clientId));strlcpy(next.clientId,id,sizeof(next.clientId));
 if(secret[0]){memset(next.secret,0,sizeof(next.secret));strlcpy(next.secret,secret,sizeof(next.secret));}
 if(!next.secret[0]){jsonReply(400,"credentials_required");return;}
 next.crc=credentialCrc(next);
 if(!saveCredentials(next)){jsonReply(500,"save_failed");return;}
 jsonReply(200,credentials.enabled?"ready":"disabled");
}
void saveRules(){
 if(!sdReady){jsonReply(503,"sd_required");return;}
 DynamicJsonDocument doc(8192);if(!readBody(doc))return;
 Rule candidate[MAX_RULES];size_t count=0;
 if(!parseRules(doc["rules"],candidate,count)){jsonReply(400,"invalid_rules");return;}
 doc.clear();doc["version"]=2;rulesJson(doc.createNestedArray("rules"),candidate,count);
 if(doc.overflowed()){jsonReply(500,"low_memory");return;}
 String body;serializeJson(doc,body);
 if(body.length()>10000){jsonReply(400,"invalid_rules");return;}
 if(!writeFile("/TUYA.JSON",body)){jsonReply(500,"save_failed");return;}
 ruleCount=count;for(size_t i=0;i<MAX_RULES;++i){rules[i]=candidate[i];runtime[i]=Runtime();failures[i]=0;suspended[i]=false;}
 rulesLoaded=true;verifyRule=-1;jsonReply(200,"saved");
}
void saveCA(){
 DynamicJsonDocument doc(6144);if(!readBody(doc))return;
 const char*pem=doc["pem"]|"";
 if(strlen(pem)>4096){jsonReply(400,"ca_invalid");return;}
 BearSSL::X509List trust(pem);
 if(!trust.getCount()){jsonReply(400,"ca_invalid");return;}
 if(!writeFile("/TUYA.CA",String(pem))){jsonReply(500,"save_failed");return;}
 accessToken="";tokenExpires=0;verifyRule=-1;
 for(size_t i=0;i<MAX_RULES;++i){runtime[i]=Runtime();failures[i]=0;suspended[i]=false;}
 jsonReply(200,"saved");
}
}
#include "tuya_page.h"
void tuyaBegin(){TuyaCloud::bootMs=millis();TuyaCloud::loadCredentials();TuyaCloud::loadRules();}
void tuyaLoop(){TuyaCloud::tick();}
void tuyaServiceDevices(){
 // No rule evaluation/commands while waiting for an inverter TCP connection.
 using namespace TuyaCloud;expireDeviceJob();
 const uint32_t now=millis();
 if(!deviceJob.queued||verifyRule>=0||!credentials.clientId[0]||!credentials.secret[0]||
   !sdReady||!validClock()||WiFi.status()!=WL_CONNECTED||uint32_t(now-bootMs)<30000||
   (attempted&&uint32_t(now-lastAttempt)<15000))return;
 deviceStep();
}
void tuyaRegisterWeb(){
 anenjiWebServer.collectHeaders("Content-Type");
 anenjiWebServer.on("/tuya",HTTP_GET,[](){anenjiWebServer.send_P(200,PSTR("text/html; charset=utf-8"),TUYA_PAGE);});
 anenjiWebServer.on("/api/tuya",HTTP_GET,TuyaCloud::getConfig);
 anenjiWebServer.on("/api/tuya/config",HTTP_POST,TuyaCloud::saveConfig);
 anenjiWebServer.on("/api/tuya/devices",HTTP_GET,TuyaCloud::getDevices);
 anenjiWebServer.on("/api/tuya/devices",HTTP_POST,TuyaCloud::queueDevices);
 anenjiWebServer.on("/api/tuya/rules",HTTP_POST,TuyaCloud::saveRules);
 anenjiWebServer.on("/api/tuya/ca",HTTP_POST,TuyaCloud::saveCA);
 anenjiWebServer.on("/api/tuya/test",HTTP_POST,[](){
  DynamicJsonDocument doc(128);if(!TuyaCloud::readBody(doc))return;
  TuyaCloud::testRequested=true;TuyaCloud::jsonReply(202,"queued");
 });
}
#endif
