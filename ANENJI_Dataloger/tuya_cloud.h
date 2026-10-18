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
uint32_t tlsHeap=0,tlsMaxBlock=0;int tlsError=0;
uint16_t tlsRxBytes=16384;
int8_t fragmentRegion=-1;bool fragmentSupported=false;uint32_t fragmentCheckedAt=0;
uint32_t bootMs=0,lastTick=0,lastAttempt=0;
bool attempted=false,testRequested=false,rulesLoaded=false;
uint8_t failures[MAX_RULES]={};
bool suspended[MAX_RULES]={};
struct DeviceJob {
 bool queued=false,functions=false;uint32_t id=0,started=0;uint8_t attempts=0;
 char uid[65]={},cursor[TuyaDevices::MAX_CURSOR+1]={};
 uint16_t pages=0;uint32_t count=0,generation=0;uint8_t bank=0;
 const char*status="idle";String error;
} deviceJob;
bool autoCatalogueChecked=false;
struct RuleError {char code[32]={};uint32_t at=0;bool active=false;};
RuleError ruleErrors[MAX_RULES];
void noteRuleError(size_t index){
 if(index>=MAX_RULES)return;
 strlcpy(ruleErrors[index].code,state.c_str(),sizeof(ruleErrors[index].code));
 ruleErrors[index].at=static_cast<uint32_t>(time(nullptr));ruleErrors[index].active=true;
}
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
 autoCatalogueChecked=false;for(auto&error:ruleErrors)error=RuleError();
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
   for(auto&error:ruleErrors)error=RuleError();
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
bool request(const char*method,const String&path,const String&body,bool tokenRequest,DynamicJsonDocument&reply,JsonVariantConst responseFilter=JsonVariantConst(),size_t maxResponse=4096){
 if(WiFi.status()!=WL_CONNECTED||!validClock()){state="offline";return false;}
 if(!sdReady){state="sd_required";return false;}
 const size_t replyCapacity=reply.capacity();
 reply=DynamicJsonDocument(0); // Allocate response JSON only after the TLS handshake releases its validator.
 File cert=SD.open("/TUYA.CA",FILE_READ);
 if(!cert||cert.size()>4096){if(cert)cert.close();state="ca_required";return false;}
 // Check the certificate before allocating TLS buffers or probing the network.
 String pem=cert.readString();cert.close();
 BearSSL::X509List trust(pem.c_str());pem="";
 if(!trust.getCount()){state="ca_invalid";return false;}
 // Probe only public TLS capability; no credentials are sent. Cache per region for an hour.
 if(fragmentRegion!=credentials.region||uint32_t(millis()-fragmentCheckedAt)>=3600000UL){
  fragmentSupported=BearSSL::WiFiClientSecure::probeMaxFragmentLength(HOSTS[credentials.region],443,1024);
  fragmentRegion=credentials.region;fragmentCheckedAt=millis();
 }
 tlsRxBytes=fragmentSupported?1024:16384;
 tlsHeap=ESP.getFreeHeap();tlsMaxBlock=ESP.getMaxFreeBlockSize();tlsError=0;
 const uint32_t requiredHeap=fragmentSupported?15000:29000;
 const uint32_t requiredBlock=fragmentSupported?7000:18000;
 if(tlsHeap<requiredHeap||tlsMaxBlock<requiredBlock){state="low_memory";return false;}
 BearSSL::WiFiClientSecure client;client.setTrustAnchors(&trust);
 client.setBufferSizes(tlsRxBytes,512);client.setTimeout(3500);
 // Confirm the negotiated fragment limit before HTTP headers can carry credentials.
 if(!client.connect(HOSTS[credentials.region],443)){
  tlsError=client.getLastSSLError();state="tls_or_network_error";client.stop();return false;
 }
 if(fragmentSupported&&!client.getMFLNStatus()){
  fragmentSupported=false;state="tls_or_network_error";client.stop();return false;
 }
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
 tlsError=client.getLastSSLError();
 if(status!=200){state=status<0?"tls_or_network_error":"http_error";http.end();return false;}
 int length=http.getSize();if(length>static_cast<int>(maxResponse)){state="reply_too_large";http.end();return false;}
 reply=DynamicJsonDocument(replyCapacity);
 if(reply.capacity()<replyCapacity){state="low_memory";http.end();return false;}
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
// A manifest publishes only complete snapshots. Refresh writes the inactive bank.
bool loadCatalogue(JsonDocument& doc,const char*uid){
 for(const char*path:{"/TUYA.CAT","/TUYA.CAT.BAK"}){
  File f=SD.open(path,FILE_READ);bool ok=f&&f.size()<=768&&!deserializeJson(doc,f);if(f)f.close();
  if(ok&&doc["version"]==2&&String(doc["uid"]|"")==uid&&String(doc["clientId"]|"")==credentials.clientId&&
   doc["region"].as<unsigned>()==credentials.region&&doc["bank"].is<unsigned>()&&doc["bank"].as<unsigned>()<2&&
   doc["pages"].is<unsigned>()&&doc["pages"].as<unsigned>()>0&&doc["pages"].as<unsigned>()<=4096&&
   doc["generation"].is<uint32_t>()&&doc["count"].is<uint32_t>()&&doc["count"].as<uint32_t>()<=doc["pages"].as<unsigned>()*TuyaDevices::PAGE_SIZE)return true;
  doc.clear();
 }
 return false;
}
String cataloguePagePath(uint8_t bank,uint16_t page){
 char path[20];snprintf(path,sizeof(path),"/T%u%04u.JSN",unsigned(bank),unsigned(page));return String(path);
}
bool readCataloguePage(JsonDocument& page,JsonVariantConst manifest,uint16_t index){
 page.clear();File f=SD.open(cataloguePagePath(manifest["bank"],index).c_str(),FILE_READ);
 bool valid=f&&f.size()<=4096&&!deserializeJson(page,f);if(f)f.close();
 if(!valid||page["version"]!=1||page["generation"]!=manifest["generation"]||String(page["uid"]|"")!=String(manifest["uid"]|"")||
   String(page["clientId"]|"")!=credentials.clientId||page["region"].as<unsigned>()!=credentials.region||
   !page["devices"].is<JsonArrayConst>()||page["devices"].size()>TuyaDevices::PAGE_SIZE)return false;
 for(JsonObjectConst d:page["devices"].as<JsonArrayConst>()){
  if(!TuyaDevices::identifier(d["id"]|"",64)||!d["name"].is<const char*>()||strlen(d["name"])>128||
     !d["category"].is<const char*>()||strlen(d["category"])>64||(!d["online"].isNull()&&!d["online"].is<bool>()))return false;
 }
 return true;
}
bool beginCatalogue(const char*uid){
 DynamicJsonDocument manifest(768),profile(512);
 bool previous=loadCatalogue(manifest,uid);
 deviceJob.bank=previous?1-manifest["bank"].as<uint8_t>():0;
 deviceJob.generation=previous?manifest["generation"].as<uint32_t>()+1:static_cast<uint32_t>(time(nullptr));
 profile["uid"]=uid;profile["clientId"]=credentials.clientId;profile["region"]=credentials.region;profile["autoAttempted"]=true;
 String text;serializeJson(profile,text);
 if(!writeFile("/TUYA.ACC",text))return false;
 strlcpy(deviceJob.uid,uid,sizeof(deviceJob.uid));deviceJob.cursor[0]=0;deviceJob.functions=false;
 deviceJob.pages=0;deviceJob.count=0;deviceJob.started=millis();++deviceJob.id;
 deviceJob.attempts=0;deviceJob.queued=true;deviceJob.status="queued";deviceJob.error="";autoCatalogueChecked=true;
 return true;
}
void maybeInitialCatalogue(){
 if(autoCatalogueChecked||deviceJob.queued||!sdReady||!validClock()||WiFi.status()!=WL_CONNECTED||
  !credentials.clientId[0]||!credentials.secret[0]||!SD.exists("/TUYA.CA")||uint32_t(millis()-bootMs)<30000)return;
 DynamicJsonDocument profile(512),manifest(768);
 if(!loadDeviceProfile(profile))return;
 const char*uid=profile["uid"];
 if(loadCatalogue(manifest,uid)||profile["autoAttempted"]==true){autoCatalogueChecked=true;return;}
 if(!beginCatalogue(uid)){deviceJob.status="error";deviceJob.error="save_failed";autoCatalogueChecked=true;}
}
void deviceStep(){
 if(!deviceJob.queued)return;
 lastAttempt=millis();attempted=true;
 if(accessToken.isEmpty()||time(nullptr)+60>=tokenExpires){
  if(!ensureToken()&&++deviceJob.attempts>=3){deviceJob.queued=false;deviceJob.status="error";deviceJob.error=state;}
  return;
 }
 deviceJob.status="loading";
 DynamicJsonDocument responseFilter(512),reply(deviceJob.functions?4096:1536);
 String path;
 if(deviceJob.functions){
  responseFilter["success"]=true;responseFilter["code"]=true;
  JsonObject f=responseFilter["result"]["functions"][0].to<JsonObject>();
  f["code"]=true;f["name"]=true;f["type"]=true;f["values"]=true;
  path="/v1.2/iot-03/devices/"+String(deviceJob.cursor)+"/specification";
 }else{TuyaDevices::filter(responseFilter);path=TuyaDevices::path(deviceJob.uid,deviceJob.cursor);}
 bool ok=request("GET",path,"",false,reply,responseFilter.as<JsonVariantConst>(),24576);
 if(!ok){
  deviceJob.error=state;
  // Retry only when a token was invalidated. Other errors are actionable and require a new click.
  if(accessToken.isEmpty()&&++deviceJob.attempts<3){deviceJob.status="queued";return;}
  deviceJob.queued=false;deviceJob.status="error";return;
 }
 if(deviceJob.functions){
  JsonArrayConst functions=reply["result"]["functions"].as<JsonArrayConst>();
  if(functions.isNull()){deviceJob.queued=false;deviceJob.status="error";deviceJob.error="invalid_reply";return;}
  for(JsonObjectConst f:functions){
   if(!TuyaDevices::identifier(f["code"]|"",40)||!f["type"].is<const char*>()||!f["values"].is<const char*>()){
    deviceJob.queued=false;deviceJob.status="error";deviceJob.error="invalid_reply";return;
   }
  }
  reply.remove("code");reply["uid"]=deviceJob.uid;reply["device"]=deviceJob.cursor;
  reply["clientId"]=credentials.clientId;reply["region"]=credentials.region;
  String text;serializeJson(reply,text);
  bool saved=!reply.overflowed()&&writeFile("/TUYA.FUN",text);
  deviceJob.queued=false;deviceJob.status=saved?"done":"error";deviceJob.error=saved?"":"save_failed";return;
 }
 DynamicJsonDocument page(2048);
 if(!TuyaDevices::sanitize(reply.as<JsonVariantConst>(),page,deviceJob.uid,deviceJob.cursor,
    credentials.clientId,credentials.region,static_cast<uint32_t>(time(nullptr)))){
  deviceJob.queued=false;deviceJob.status="error";deviceJob.error="invalid_reply";return;
 }
 if(deviceJob.pages>=4096){deviceJob.queued=false;deviceJob.status="error";deviceJob.error="catalogue_limit";return;}
 page["generation"]=deviceJob.generation;
 String text;serializeJson(page,text);
 if(page.overflowed()||!writeFile(cataloguePagePath(deviceJob.bank,deviceJob.pages).c_str(),text)){
  deviceJob.queued=false;deviceJob.status="error";deviceJob.error="save_failed";return;
 }
 ++deviceJob.pages;deviceJob.count+=page["devices"].size();deviceJob.attempts=0;deviceJob.started=millis();
 if(page["hasMore"]==true){
  strlcpy(deviceJob.cursor,page["nextCursor"]|"",sizeof(deviceJob.cursor));
  deviceJob.status="loading";return;
 }
 DynamicJsonDocument manifest(768);manifest["version"]=2;manifest["uid"]=deviceJob.uid;
 manifest["clientId"]=credentials.clientId;manifest["region"]=credentials.region;manifest["bank"]=deviceJob.bank;
 manifest["pages"]=deviceJob.pages;manifest["count"]=deviceJob.count;manifest["generation"]=deviceJob.generation;
 manifest["updatedAt"]=static_cast<uint32_t>(time(nullptr));text="";serializeJson(manifest,text);
 bool saved=writeFile("/TUYA.CAT",text);
 deviceJob.queued=false;deviceJob.status=saved?"done":"error";deviceJob.error=saved?"":"save_failed";
}

bool sendRule(size_t index){
 const Rule&r=rules[index];
 String body="{\"commands\":[{\"code\":\""+String(r.code)+"\",\"value\":"+r.value+"}]}";
 DynamicJsonDocument reply(1024);
 if(!request("POST",String("/v1.0/iot-03/devices/")+r.device+"/commands",body,false,reply)){noteRuleError(index);return false;}
 if(reply["result"]!=true){state="command_rejected";noteRuleError(index);return false;}
 state="confirming";verifyRule=index;verifyAttempts=0;verifyAfter=millis();return true;
}
bool confirmRule(size_t index){
 const Rule&r=rules[index];DynamicJsonDocument reply(4096);
 if(!request("GET",String("/v1.0/iot-03/devices/")+r.device+"/status","",false,reply)){noteRuleError(index);return false;}
 DynamicJsonDocument expected(256);deserializeJson(expected,r.value);
 for(JsonObjectConst value:reply["result"].as<JsonArrayConst>()){
  if(String(value["code"]|"")==r.code){
   String actual,wanted;serializeJson(value["value"],actual);serializeJson(expected,wanted);
   if(actual==wanted){acknowledge(runtime[index],millis());state="confirmed";ruleErrors[index].active=false;return true;}
  }
 }
 state="not_confirmed";noteRuleError(index);return false;
}
bool freshData(){
 return tuyaHasMeasurement&&uint32_t(millis()-tuyaMeasurementMs)<=
   std::min(uint32_t(300),std::max(uint32_t(30),appSettings.registerIntervalSeconds*3))*1000UL;
}
void tick(){
 const uint32_t now=millis();if(uint32_t(now-lastTick)<1000)return;lastTick=now;
 expireDeviceJob();
 loadRules();maybeInitialCatalogue();
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
   if(!ensureToken()){noteRuleError(verifyRule);if(++verifyAttempts>=3){suspended[verifyRule]=true;verifyRule=-1;state="confirmation_failed";}}
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
   if(!ensureToken()){noteRuleError(i);if(++failures[i]>=3){suspended[i]=true;state="retry_limit";}}
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
 expireDeviceJob();DynamicJsonDocument doc(512),profile(512),manifest(768),page(2048);
 bool hasProfile=loadDeviceProfile(profile);
 const char*uid=hasProfile?(profile["uid"]|""):"";
 bool cached=hasProfile&&loadCatalogue(manifest,uid);
 const bool available=cached;
 if(anenjiWebServer.hasArg("summary")||(anenjiWebServer.hasArg("progress")&&deviceJob.queued&&!deviceJob.functions))cached=false;
 // Validate the complete snapshot before starting a streamed HTTP response.
 if(cached){
  uint32_t count=0;
  for(uint16_t i=0;i<manifest["pages"].as<unsigned>();++i){
   if(!readCataloguePage(page,manifest.as<JsonVariantConst>(),i)){jsonReply(503,"catalogue_invalid");return;}
   count+=page["devices"].size();yield();
  }
  if(count!=manifest["count"].as<uint32_t>()){jsonReply(503,"catalogue_invalid");return;}
 }
 doc["status"]=deviceJob.functions?"idle":deviceJob.status;doc["jobId"]=deviceJob.id;
 doc["error"]=deviceJob.functions?String(""):deviceJob.error;doc["uid"]=uid;
 doc["loaded"]=deviceJob.count;doc["loading"]=deviceJob.queued&&!deviceJob.functions;
 doc["hasCatalogue"]=available;doc["catalogueGeneration"]=manifest["generation"];
 String text;serializeJson(doc,text);text.remove(text.length()-1);
 anenjiWebServer.sendHeader("Cache-Control","no-store");anenjiWebServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
 anenjiWebServer.send(200,"application/json; charset=utf-8","");anenjiWebServer.sendContent(text);
 if(cached){
  doc.clear();doc["uid"]=uid;doc["requestCursor"]="";doc["nextCursor"]="";doc["hasMore"]=false;
  doc["total"]=manifest["count"];doc["updatedAt"]=manifest["updatedAt"];doc["generation"]=manifest["generation"];
  text="";serializeJson(doc,text);text.remove(text.length()-1);
  anenjiWebServer.sendContent(String(",\"page\":")+text+",\"devices\":[");bool first=true;
  for(uint16_t i=0;i<manifest["pages"].as<unsigned>();++i){
   if(!readCataloguePage(page,manifest.as<JsonVariantConst>(),i)){anenjiWebServer.client().stop();return;}
   for(JsonObjectConst item:page["devices"].as<JsonArrayConst>()){
    doc.clear();doc["id"]=item["id"];doc["name"]=item["name"];doc["category"]=item["category"];doc["online"]=item["online"];
    // ArduinoJson 6 appends to String; 7 replaces it. Keep separators outside serialization.
    if(!first)anenjiWebServer.sendContent(",");
    text="";serializeJson(doc,text);anenjiWebServer.sendContent(text);first=false;
   }
   yield();
  }
  anenjiWebServer.sendContent("]}");
 }
 anenjiWebServer.sendContent("}");anenjiWebServer.sendContent("");
}
void queueDevices(){
 expireDeviceJob();DynamicJsonDocument doc(512),profile(512),manifest(768);if(!readBody(doc))return;
 const char*uid=doc["uid"]|"";
 if(!TuyaDevices::identifier(uid,64)){jsonReply(400,"invalid_uid_or_cursor");return;}
 if(deviceJob.queued){jsonReply(409,"device_busy");return;}
 if(!credentials.clientId[0]||!credentials.secret[0]){jsonReply(400,"credentials_required");return;}
 if(!sdReady){jsonReply(503,"sd_required");return;}
 if(doc["initial"]==true&&loadDeviceProfile(profile)&&String(profile["uid"]|"")==uid&&
    (loadCatalogue(manifest,uid)||profile["autoAttempted"]==true)){jsonReply(200,"cached");return;}
 if(!beginCatalogue(uid)){jsonReply(500,"save_failed");return;}
 DynamicJsonDocument response(256);response["status"]="queued";response["jobId"]=deviceJob.id;
 String body;serializeJson(response,body);anenjiWebServer.send(202,"application/json; charset=utf-8",body);
}

void queueFunctions(){
 expireDeviceJob();DynamicJsonDocument doc(512),profile(512);if(!readBody(doc))return;
 const char*uid=doc["uid"]|"",*device=doc["device"]|"";
 if(!TuyaDevices::identifier(device,64)||!loadDeviceProfile(profile)||String(profile["uid"]|"")!=uid){jsonReply(400,"device_refresh_required");return;}
 if(deviceJob.queued){jsonReply(409,"device_busy");return;}
 if(!credentials.clientId[0]||!credentials.secret[0]){jsonReply(400,"credentials_required");return;}
 deviceJob.functions=true;strlcpy(deviceJob.uid,uid,sizeof(deviceJob.uid));strlcpy(deviceJob.cursor,device,sizeof(deviceJob.cursor));
 deviceJob.started=millis();++deviceJob.id;deviceJob.attempts=0;deviceJob.queued=true;deviceJob.status="queued";deviceJob.error="";
 DynamicJsonDocument response(256);response["jobId"]=deviceJob.id;
 String body;serializeJson(response,body);anenjiWebServer.send(202,"application/json",body);
}
void getFunctions(){
 expireDeviceJob();DynamicJsonDocument doc(6144),profile(512);
 if(loadDeviceProfile(profile)){
  File file=SD.open("/TUYA.FUN",FILE_READ);
  bool valid=file&&file.size()<=8192&&!deserializeJson(doc,file);if(file)file.close();
  if(!valid||String(doc["uid"]|"")!=String(profile["uid"]|"")||String(doc["clientId"]|"")!=credentials.clientId||doc["region"].as<unsigned>()!=credentials.region)doc.clear();
 }
 doc.remove("clientId");doc.remove("region");
 doc["status"]=deviceJob.functions?deviceJob.status:"idle";
 doc["jobId"]=deviceJob.id;doc["error"]=deviceJob.functions?deviceJob.error:String("");
 if(doc.overflowed()){jsonReply(500,"low_memory");return;}
 String body;serializeJson(doc,body);anenjiWebServer.sendHeader("Cache-Control","no-store");anenjiWebServer.send(200,"application/json; charset=utf-8",body);
}

void getRuleAlerts(){
 DynamicJsonDocument doc(1536);JsonArray alerts=doc.to<JsonArray>();
 for(size_t i=0;i<ruleCount;++i)if(ruleErrors[i].active){
  JsonObject e=alerts.createNestedObject();e["name"]=rules[i].name;e["device"]=rules[i].device;
  e["code"]=ruleErrors[i].code;e["at"]=ruleErrors[i].at;e["paused"]=suspended[i];
 }
 if(doc.overflowed()){jsonReply(500,"low_memory");return;}
 String body;serializeJson(doc,body);anenjiWebServer.sendHeader("Cache-Control","no-store");anenjiWebServer.send(200,"application/json; charset=utf-8",body);
}
void getConfig(){
 const uint32_t heap=ESP.getFreeHeap(),block=ESP.getMaxFreeBlockSize();
 DynamicJsonDocument doc(8192);
 doc["language"]=appSettings.language;doc["enabled"]=bool(credentials.enabled);
 doc["region"]=credentials.region;doc["clientId"]=credentials.clientId;
 doc["hasSecret"]=credentials.secret[0]!=0;doc["status"]=state;
 doc["sdReady"]=sdReady;doc["hasCA"]=sdReady&&SD.exists("/TUYA.CA");
 JsonArray alerts=doc.createNestedArray("ruleErrors");
 for(size_t i=0;i<ruleCount;++i)if(ruleErrors[i].active){
  JsonObject error=alerts.createNestedObject();error["rule"]=i;error["name"]=rules[i].name;error["device"]=rules[i].device;
  error["code"]=ruleErrors[i].code;error["at"]=ruleErrors[i].at;error["paused"]=suspended[i];
 }
 doc["fresh"]=freshData();doc["heap"]=heap;doc["maxBlock"]=block;
 doc["tlsHeap"]=tlsHeap;doc["tlsMaxBlock"]=tlsMaxBlock;doc["tlsRxBytes"]=tlsRxBytes;doc["tlsError"]=tlsError;
 rulesJson(doc.createNestedArray("rules"));
 if(doc.overflowed()){jsonReply(500,"low_memory");return;}
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
 for(auto&error:ruleErrors)error=RuleError();
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
 state=credentials.enabled?"ready":"disabled";tlsHeap=0;tlsMaxBlock=0;tlsError=0;
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
 using namespace TuyaCloud;expireDeviceJob();maybeInitialCatalogue();
 const uint32_t now=millis();
 if(!deviceJob.queued||verifyRule>=0||!credentials.clientId[0]||!credentials.secret[0]||
   !sdReady||!validClock()||WiFi.status()!=WL_CONNECTED||uint32_t(now-bootMs)<30000||
   (attempted&&uint32_t(now-lastAttempt)<15000))return;
 deviceStep();
}
void tuyaRegisterWeb(){
 anenjiWebServer.collectHeaders("Content-Type");
 anenjiWebServer.on("/tuya",HTTP_GET,[](){anenjiWebServer.send_P(200,PSTR("text/html; charset=utf-8"),TUYA_PAGE);});
 anenjiWebServer.on("/api/tuya/alerts",HTTP_GET,TuyaCloud::getRuleAlerts);
 anenjiWebServer.on("/api/tuya",HTTP_GET,TuyaCloud::getConfig);
 anenjiWebServer.on("/api/tuya/config",HTTP_POST,TuyaCloud::saveConfig);
 anenjiWebServer.on("/api/tuya/functions",HTTP_GET,TuyaCloud::getFunctions);
 anenjiWebServer.on("/api/tuya/functions",HTTP_POST,TuyaCloud::queueFunctions);
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
