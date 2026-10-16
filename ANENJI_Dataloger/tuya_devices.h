#ifndef ANENJI_TUYA_DEVICES_H
#define ANENJI_TUYA_DEVICES_H
#include <Arduino.h>
#include <ArduinoJson.h>
namespace TuyaDevices {
constexpr size_t PAGE_SIZE=3,MAX_CURSOR=256;
inline bool identifier(const char*value,size_t maximum){
 if(!value||!value[0]||strlen(value)>maximum)return false;
 for(const char*p=value;*p;++p){
  if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'))return false;
 }
 return true;
}
inline bool cursorValid(const char*value){
 if(!value||strlen(value)>MAX_CURSOR)return false;
 for(const char*p=value;*p;++p)if(static_cast<unsigned char>(*p)<32||static_cast<unsigned char>(*p)>126)return false;
 return true;
}
inline String encode(const char*value){
 String result;
 for(const uint8_t*p=reinterpret_cast<const uint8_t*>(value);*p;++p){
  if((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='_'||*p=='.'||*p=='~')result+=char(*p);
  else{char hex[4];snprintf(hex,sizeof(hex),"%%%02X",*p);result+=hex;}
 }
 return result;
}
// Query keys are already sorted for the cloud signature.
inline String path(const char*uid,const char*cursor){
 String out="/v1.2/iot-03/devices?";
 if(cursor[0])out+=String("last_row_key=")+encode(cursor)+"&";
 out+=String("page_size=3&source_id=")+encode(uid)+"&source_type=tuyaUser";
 return out;
}
inline void filter(JsonDocument&doc){
 doc.clear();doc["success"]=true;doc["code"]=true;
 doc["result"]["has_more"]=true;doc["result"]["last_row_key"]=true;doc["result"]["total"]=true;
 JsonObject item=doc["result"]["list"].createNestedObject();
 item["id"]=true;item["name"]=true;item["category"]=true;item["online"]=true;
}
inline void clipped(char*output,size_t capacity,const char*input){
 size_t count=strlen(input);
 if(count>=capacity){count=capacity-1;while(count&&(static_cast<uint8_t>(input[count])&0xc0)==0x80)--count;}
 memcpy(output,input,count);output[count]=0;
}
// Explicit allowlist: local_key, IP, location and unrelated fields cannot enter the cache/UI.
inline bool sanitize(JsonVariantConst input,JsonDocument&out,const char*uid,const char*cursor,
 const char*client,uint8_t region,uint32_t updatedAt){
 JsonObjectConst result=input["result"];
 if(!result["list"].is<JsonArrayConst>()||!result["has_more"].is<bool>())return false;
 JsonArrayConst list=result["list"].as<JsonArrayConst>();if(list.size()>PAGE_SIZE)return false;
 const char*next=result["last_row_key"]|"";
 bool more=result["has_more"].as<bool>();
 if(!cursorValid(next)||(more&&(!next[0]||!strcmp(next,cursor)||list.size()==0)))return false;
 out.clear();out["version"]=1;out["uid"]=uid;out["clientId"]=client;out["region"]=region;
 out["requestCursor"]=cursor;out["nextCursor"]=next;out["hasMore"]=more;out["updatedAt"]=updatedAt;
 if(result["total"].is<uint32_t>())out["total"]=result["total"].as<uint32_t>();
 JsonArray devices=out.createNestedArray("devices");
 for(JsonObjectConst item:list){
  const char*id=item["id"]|"";
  if(!identifier(id,64))return false;
  if(!item["name"].isNull()&&!item["name"].is<const char*>())return false;
  const char*name=item["name"]|id,*category=item["category"]|"";
  char shortName[129],shortCategory[65];clipped(shortName,sizeof(shortName),name);clipped(shortCategory,sizeof(shortCategory),category);
  JsonObject device=devices.createNestedObject();device["id"]=id;device["name"]=shortName;device["category"]=shortCategory;
  if(item["online"].is<bool>())device["online"]=item["online"].as<bool>();else device["online"]=nullptr;
 }
 return !out.overflowed();
}
}
#endif
