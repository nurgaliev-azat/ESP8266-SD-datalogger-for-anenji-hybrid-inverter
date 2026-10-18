"""Host-test actual firmware catalogue jobs with an in-memory SD and mocked cloud.
Usage: python3 tests/catalogue_test.py /path/to/ArduinoJson/src
Requires g++ and ArduinoJson 6.21.5 (the same library as the firmware).
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'ANENJI_Dataloger/tuya_cloud.h').read_text()
def extract(name):
    start = source.index(name)
    pos = source.index('{', start)
    depth = 1
    end = pos + 1
    clean = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*', lambda match: ' ' * len(match[0]), source)
    while depth:
        if clean[end] == '{': depth += 1
        elif clean[end] == '}': depth -= 1
        end += 1
    return source[start:end]
arduino = r'''
#pragma once
#include <string>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <algorithm>
class String:public std::string {public:
 using std::string::string;
 String(const std::string& s):std::string(s){}
 bool isEmpty()const{return empty();}
 void remove(size_t index){erase(index);}
 size_t write(uint8_t c){push_back(c);return 1;}
 size_t write(const uint8_t* p,size_t n){append(reinterpret_cast<const char*>(p),n);return n;}
};
inline void yield(){}
inline size_t strlcpy(char*d,const char*s,size_t n){size_t l=strlen(s);if(n){memcpy(d,s,std::min(l,n-1));d[std::min(l,n-1)]=0;}return l;}
'''
pre = r'''
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cassert>
#include <map>
#include <memory>
#include <ctime>
#include "tuya_devices.h"
#include "tuya_rules.h"
using namespace TuyaRules;
Rule rules[MAX_RULES];Runtime runtime[MAX_RULES];bool suspended[MAX_RULES]={};size_t ruleCount=1;
int verifyRule=-1;uint8_t verifyAttempts=0;uint32_t verifyAfter=0;
std::map<std::string,std::string> files;
struct File {
 std::string data;size_t pos=0;bool opened=false;
 operator bool()const{return opened;}
 size_t size()const{return data.size();}
 int read(){return pos<data.size()?static_cast<unsigned char>(data[pos++]):-1;}
 size_t readBytes(char* out,size_t n){size_t len=std::min(n,data.size()-pos);memcpy(out,data.data()+pos,len);pos+=len;return len;}
 void close(){}
};
constexpr int FILE_READ=0,WL_CONNECTED=3;
struct {File open(const char* path,int){auto i=files.find(path);return i==files.end()?File{}:File{i->second,0,true};}bool exists(const char* path){return files.count(path)>0;}} SD;
struct {int status(){return WL_CONNECTED;}} WiFi;
struct {char clientId[33]="client",secret[65]="secret";uint8_t region=3;} credentials;
bool sdReady=true,attempted=false,autoCatalogueChecked=false;uint32_t bootMs=0,lastAttempt=0,nowMs=60000;
uint32_t millis(){return nowMs;}
bool validClock(){return true;}
String accessToken="token",state="ready";time_t tokenExpires=time(nullptr)+3600;
bool ensureToken(){accessToken="token";return true;}
constexpr int CONTENT_LENGTH_UNKNOWN=-1;
struct Server {
 std::string body;int code=0;bool progress=false,stopped=false;
 bool hasArg(const char* key){return progress&&std::string(key)=="progress";}
 void sendHeader(const char*,const char*){}
 void setContentLength(int){}
 void send(int status,const char*,const String& text){code=status;body=text;}
 void sendContent(const String& text){body+=text;}
 Server& client(){return *this;}void stop(){stopped=true;}
} anenjiWebServer;
void jsonReply(int status,const char* error){anenjiWebServer.code=status;anenjiWebServer.body=error;}
bool failWrite=false,failCloud=false,rejectCommand=false,wrongStatus=false;int cloudRequests=0;
bool writeFile(const char* path,const String& text){if(failWrite)return false;files[path]=text;return true;}
bool request(const char*,const String& path,const String&,bool,DynamicJsonDocument& reply,JsonVariantConst filter=JsonVariantConst(),size_t=4096){
 ++cloudRequests;if(failCloud){state="api_error";return false;}
 if(path.find("/commands")!=std::string::npos)return !deserializeJson(reply,rejectCommand?"{\"success\":true,\"result\":false}":"{\"success\":true,\"result\":true}");
 if(path.find("/status")!=std::string::npos)return !deserializeJson(reply,wrongStatus?"{\"success\":true,\"result\":[{\"code\":\"switch_1\",\"value\":false}]}":"{\"success\":true,\"result\":[{\"code\":\"switch_1\",\"value\":true}]}");
 const char* first=R"({"success":true,"result":{"has_more":true,"last_row_key":"next","list":[{"id":"d1","name":"One","local_key":"SECRET"},{"id":"d2","name":"Two"},{"id":"d3","name":"Three"}]}})";
 const char* last=R"({"success":true,"result":{"has_more":false,"last_row_key":"","list":[{"id":"d4","name":"Four"}]}})";
 return !deserializeJson(reply,path.find("last_row_key=next")!=std::string::npos?last:first,DeserializationOption::Filter(filter));
}
'''
job_start = source.index('struct DeviceJob {')
job_end = source.index('} deviceJob;',job_start)+len('} deviceJob;')
error_start = source.index('struct RuleError {')
error_end = source.index('int verifyRule=', error_start)
functions = source[error_start:error_end] + '\n' + '\n'.join(extract(name) for name in [
 'void expireDeviceJob()', 'bool loadDeviceProfile(', 'bool loadCatalogue(',
 'String cataloguePagePath(', 'bool readCataloguePage(', 'bool beginCatalogue(',
 'void maybeInitialCatalogue()', 'void deviceStep()', 'void getDevices()', 'bool sendRule(', 'bool confirmRule(', 'void getRuleAlerts()'])
post = r'''
int main(){
 // First connection automatically schedules exactly one complete catalogue job.
 files["/TUYA.CA"]="test CA";
 files["/TUYA.ACC"]=R"({"uid":"uid","clientId":"client","region":3})";
 maybeInitialCatalogue();assert(deviceJob.queued);assert(cloudRequests==0);
 deviceStep();assert(deviceJob.queued&&deviceJob.pages==1&&deviceJob.count==3);assert(!files.count("/TUYA.CAT"));
 nowMs+=15000;deviceStep();assert(!deviceJob.queued&&deviceJob.count==4);assert(std::string(deviceJob.status)=="done");
 DynamicJsonDocument manifest(768),page(2048);assert(loadCatalogue(manifest,"uid"));assert(manifest["pages"]==2&&manifest["count"]==4);
 assert(readCataloguePage(page,manifest.as<JsonVariantConst>(),0));assert(page["devices"][0]["local_key"].isNull());
 assert(readCataloguePage(page,manifest.as<JsonVariantConst>(),1));assert(page["devices"][0]["id"]=="d4");
 getDevices();assert(anenjiWebServer.code==200);DynamicJsonDocument response(4096);
 assert(!deserializeJson(response,anenjiWebServer.body));assert(response["page"]["devices"].size()==4);assert(response["page"]["hasMore"]==false);assert(anenjiWebServer.body.find("SECRET")==std::string::npos);
 // Reboot/reconnection never starts another cloud load for a saved catalogue.
 deviceJob={};autoCatalogueChecked=false;maybeInitialCatalogue();assert(!deviceJob.queued&&cloudRequests==2);
 auto oldManifest=files["/TUYA.CAT"];unsigned oldBank=manifest["bank"];
 // Refresh leaves the published snapshot untouched while pages are downloading.
 assert(beginCatalogue("uid"));assert(deviceJob.bank!=oldBank);deviceStep();assert(files["/TUYA.CAT"]==oldManifest);
 anenjiWebServer.progress=true;getDevices();assert(!deserializeJson(response,anenjiWebServer.body));assert(response["loading"]==true&&response["page"].isNull());anenjiWebServer.progress=false;
 getDevices();assert(!deserializeJson(response,anenjiWebServer.body));assert(response["page"]["devices"].size()==4);
 failCloud=true;deviceStep();assert(!deviceJob.queued&&std::string(deviceJob.status)=="error");assert(files["/TUYA.CAT"]==oldManifest);failCloud=false;
 autoCatalogueChecked=false;maybeInitialCatalogue();assert(!deviceJob.queued);
 // Explicit retry publishes a complete replacement, not an appended/partial list.
 assert(beginCatalogue("uid"));deviceStep();deviceStep();assert(loadCatalogue(manifest,"uid"));assert(manifest["count"]==4&&manifest["bank"].as<unsigned>()!=oldBank);
 assert(!loadCatalogue(manifest,"other_uid"));
 // An initial failed attempt survives restart and waits for explicit refresh.
 files.erase("/TUYA.CAT");files.erase("/TUYA.CAT.BAK");deviceJob={};autoCatalogueChecked=false;
 maybeInitialCatalogue();assert(!deviceJob.queued);
 // SD write failure must not launch a job.
 failWrite=true;assert(!beginCatalogue("uid"));assert(!deviceJob.queued);failWrite=false;
 // Scenario failures retain rule/device context independently of global connection status.
 strlcpy(rules[0].name,"Pump",sizeof(rules[0].name));strlcpy(rules[0].device,"d4",sizeof(rules[0].device));
 strlcpy(rules[0].code,"switch_1",sizeof(rules[0].code));strlcpy(rules[0].value,"true",sizeof(rules[0].value));
 failCloud=true;assert(!sendRule(0));assert(ruleErrors[0].active&&std::string(ruleErrors[0].code)=="api_error");failCloud=false;
 state="ready";getRuleAlerts();assert(!deserializeJson(response,anenjiWebServer.body));assert(response[0]["name"]=="Pump"&&response[0]["device"]=="d4"&&response[0]["code"]=="api_error");
 assert(sendRule(0));assert(ruleErrors[0].active);wrongStatus=true;assert(!confirmRule(0));assert(std::string(ruleErrors[0].code)=="not_confirmed");
 wrongStatus=false;assert(confirmRule(0));assert(!ruleErrors[0].active);getRuleAlerts();assert(anenjiWebServer.body=="[]");
 rejectCommand=true;assert(!sendRule(0));assert(std::string(ruleErrors[0].code)=="command_rejected");suspended[0]=true;getRuleAlerts();assert(!deserializeJson(response,anenjiWebServer.body));assert(response[0]["paused"]==true);
 // Timeout exposes an error; it never publishes a partial catalogue.
 assert(beginCatalogue("uid"));nowMs+=120001;expireDeviceJob();assert(!deviceJob.queued&&deviceJob.error=="device_timeout");
}
'''
with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    (tmp/'Arduino.h').write_text(arduino)
    cpp=tmp/'catalogue.cpp';binary=tmp/'catalogue'
    cpp.write_text(pre+source[job_start:job_end]+'\n'+functions+post)
    subprocess.run(['g++','-std=c++17','-I'+str(tmp),'-I'+sys.argv[1],'-I'+str(root/'ANENJI_Dataloger'),str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: first auto-load, all pages, secret filtering, SD restore, no automatic reload, atomic refresh, cloud/SD failures, timeout and scenario failure/recovery alerts')
