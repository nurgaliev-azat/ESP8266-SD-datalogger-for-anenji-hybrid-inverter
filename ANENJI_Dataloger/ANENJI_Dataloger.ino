#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <stddef.h>
#include "Registers.h"
#include "web_server.h"

constexpr uint16_t UDP_PORT=58899,TCP_PORT=8899;
constexpr uint8_t MODBUS_UNIT_ID=1,MODBUS_FUNCTION=3,SD_CS_PIN=D4;
constexpr uint32_t TCP_CONNECT_TIMEOUT_MS=30000;
constexpr uint32_t MODBUS_RESPONSE_TIMEOUT_MS=5000;
constexpr size_t MAX_RESPONSE_SIZE=80;
constexpr size_t EEPROM_SIZE=4096,SETTINGS_BYTES=256;
constexpr uint32_t SETTINGS_MAGIC=0x414E5345;
constexpr uint16_t SETTINGS_VERSION=1;
constexpr uint32_t FIVE_MINUTES=300;

struct ReadRange{uint16_t start,count;};
const ReadRange READ_RANGES[]={{100,2},{108,2},{201,34}};
constexpr size_t READ_RANGE_COUNT=sizeof(READ_RANGES)/sizeof(READ_RANGES[0]);

struct SettingsRecord{
  uint32_t magic;
  uint16_t version;
  char ssid[33];
  char password[65];
  char inverterIp[16];
  uint16_t pageUpdateSeconds;
  uint16_t registerIntervalSeconds;
  int8_t timezone;
  uint8_t language;
  uint16_t crc;
};

struct StoredRecord{
  uint32_t slot;
  uint16_t wh[5];
  uint16_t crc;
};

static_assert(sizeof(SettingsRecord)<=SETTINGS_BYTES,
              "Настройки не помещаются в EEPROM");
static_assert(sizeof(StoredRecord)==16,
              "Запись истории должна занимать 16 байт");

constexpr size_t EEPROM_RECORD_COUNT=
  (EEPROM_SIZE-SETTINGS_BYTES)/sizeof(StoredRecord);

static_assert(EEPROM_RECORD_COUNT==240,
              "Неожиданная ёмкость EEPROM");

struct InstantPower{
  float grid,pv,load,batteryCharge,batteryDischarge;
};

struct EnergyAccumulator{
  float gridWs,pvWs,loadWs,batteryChargeWs,batteryDischargeWs;
};

WiFiUDP udp;
WiFiServer tcpServer(TCP_PORT);
WiFiClient inverterClient;
WebSettings appSettings;

InstantPower previousPower={},currentPower={};
EnergyAccumulator currentBucket={};
bool previousPowerValid=false,bucketHasMeasurement=false;
bool setupAccessPoint=false,sdReady=false;
uint32_t activeSlot=0,lastSdProbeMillis=0,currentDayNumber=0;
time_t previousMeasurementTime=0;
float currentGridVoltage=0,currentPvVoltage=0;
float currentLoadVoltage=0,currentBatteryVoltage=0;
uint8_t currentBatteryPercent=0;

String currentStatus="Waiting for data";
String currentUpdateTime="-";
String currentMode="Ожидание данных";
String currentFaults="Неполадок нет";
String currentWarnings="Предупреждений нет";
bool faultRead=false,warningRead=false,modeRead=false;
uint32_t currentFaultFlags=0,currentWarningFlags=0;
uint16_t currentModeCode=0;

StoredRecord dayRecords[288];
bool dayValid[288];

void fillWebLiveData(WebLiveData& data);
size_t getTodaySamples(WebSample* output,size_t maxSamples,size_t offset);
size_t getHistoryForWeb(WebHistoryHour* output,size_t maxRecords,
                        const String& scale,uint16_t year,
                        uint8_t month,uint8_t day);
bool saveBucket();
bool saveSettings(const WebSettings& settings);
bool saveLanguage(const String& language);
void tuyaBegin();
void tuyaLoop();
void tuyaRegisterWeb();
void tuyaServiceDevices();
uint32_t tuyaMeasurementMs=0;
bool tuyaHasMeasurement=false;

uint16_t crc16Modbus(const uint8_t* data,size_t length){
  uint16_t crc=0xFFFF;
  for(size_t i=0;i<length;++i){
    crc^=data[i];
    for(uint8_t b=0;b<8;++b)
      crc=(crc&1)?(crc>>1)^0xA001:crc>>1;
  }
  return crc;
}

int32_t signedRegister(uint16_t raw){
  return raw>=32768?static_cast<int32_t>(raw)-65536:raw;
}

bool validClock(){return time(nullptr)>=1577836800;}

uint32_t measurementSlot(time_t receivedAt){
  return static_cast<uint32_t>((receivedAt-1)/FIVE_MINUTES);
}

uint32_t currentDayId(){
  time_t now=time(nullptr);
  if(!validClock())return 0;
  struct tm t;
  localtime_r(&now,&t);
  return static_cast<uint32_t>(t.tm_year)*400UL+t.tm_yday+1;
}

String currentDateTimeString(){
  time_t now=time(nullptr);
  if(!validClock())return "-";
  struct tm t;
  localtime_r(&now,&t);
  char text[32];
  snprintf(text,sizeof(text),"%04d-%02d-%02d %02d:%02d:%02d",
    t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec);
  return String(text);
}

uint16_t settingsCrc(const SettingsRecord& record){
  return crc16Modbus(reinterpret_cast<const uint8_t*>(&record),
                     offsetof(SettingsRecord,crc));
}

void loadSettings(){
  SettingsRecord record={};
  EEPROM.get(0,record);
  bool ok=record.magic==SETTINGS_MAGIC&&
    record.version==SETTINGS_VERSION&&
    record.crc==settingsCrc(record)&&
    memchr(record.ssid,0,sizeof(record.ssid))&&
    memchr(record.password,0,sizeof(record.password))&&
    memchr(record.inverterIp,0,sizeof(record.inverterIp));

  appSettings.ssid=ok?record.ssid:"";
  appSettings.password=ok?record.password:"";
  appSettings.inverterIp=ok?record.inverterIp:"192.168.0.148";
  appSettings.pageUpdateSeconds=ok?record.pageUpdateSeconds:5;
  appSettings.registerIntervalSeconds=ok?
    record.registerIntervalSeconds:30;
  appSettings.timezone=ok?record.timezone:5;
  appSettings.language=ok&&record.language==1?"ru":"en";

  if(appSettings.pageUpdateSeconds<1||
     appSettings.pageUpdateSeconds>3600)appSettings.pageUpdateSeconds=5;
  if(appSettings.registerIntervalSeconds<5||
     appSettings.registerIntervalSeconds>3600)
    appSettings.registerIntervalSeconds=30;
  if(appSettings.timezone< -12||
     appSettings.timezone>14)appSettings.timezone=5;
}

bool persistSettings(const WebSettings& settings){
  SettingsRecord oldRecord={};
  EEPROM.get(0,oldRecord);
  SettingsRecord record={};
  record.magic=SETTINGS_MAGIC;
  record.version=SETTINGS_VERSION;

  strlcpy(record.ssid,settings.ssid.c_str(),
          sizeof(record.ssid));
  strlcpy(record.password,settings.password.c_str(),
          sizeof(record.password));
  strlcpy(record.inverterIp,settings.inverterIp.c_str(),
          sizeof(record.inverterIp));

  record.pageUpdateSeconds=settings.pageUpdateSeconds;
  record.registerIntervalSeconds=
    settings.registerIntervalSeconds;
  record.timezone=settings.timezone;
  record.language=settings.language=="ru"?1:0;
  record.crc=settingsCrc(record);

  EEPROM.put(0,record);

  if(!EEPROM.commit()){
    EEPROM.put(0,oldRecord); // Restore RAM mirror after a failed flash write.
    Serial.println("[SAVE] Ошибка записи настроек");
    return false;
  }

  appSettings=settings;
  Serial.println("[SAVE] Настройки сохранены");
  return true;
}

bool saveSettings(const WebSettings& settings){
  if(bucketHasMeasurement&&activeSlot&&!saveBucket()){
    Serial.println("[SAVE] Не удалось сохранить текущую пятиминутку");
    return false;
  }

  return persistSettings(settings);
}

bool saveLanguage(const String& language){
  if(language!="en"&&language!="ru")return false;
  if(language==appSettings.language)return true;
  WebSettings settings=appSettings;
  settings.language=language;
  return persistSettings(settings);
}

bool connectToWiFi(){
  if(appSettings.ssid.isEmpty())return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(appSettings.ssid.c_str(),appSettings.password.c_str());
  uint32_t started=millis();
  Serial.print("Подключение к Wi-Fi");
  while(WiFi.status()!=WL_CONNECTED&&millis()-started<20000){
    Serial.print('.');
    delay(500);
  }
  Serial.println();
  if(WiFi.status()!=WL_CONNECTED)return false;
  Serial.print("IP ESP8266: ");
  Serial.println(WiFi.localIP());
  return true;
}

void synchronizeTime(){
  configTime(static_cast<long>(appSettings.timezone)*3600L,0,
             "pool.ntp.org","time.nist.gov");
  uint32_t started=millis();
  while(!validClock()&&millis()-started<15000)delay(500);
  Serial.println(validClock()?
    "Время синхронизировано":"Время не синхронизировано");
}

void startSetupAccessPoint(){
  if(setupAccessPoint)return;
  setupAccessPoint=true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Anenji-setup","12345678");
  beginWebServer(fillWebLiveData,getTodaySamples,getHistoryForWeb,
                 saveSettings,saveLanguage,appSettings,true);
  tuyaRegisterWeb();
  Serial.println(WiFi.softAPIP());
}

void printHexByte(uint8_t value){
  if(value<16)Serial.print('0');
  Serial.print(value,HEX);
}

void printHexBuffer(const uint8_t* data,size_t length){
  for(size_t i=0;i<length;++i){
    printHexByte(data[i]);
    if(i+1<length)Serial.print(' ');
  }
  Serial.println();
}

const RegisterDefinition* findRegisterDefinition(uint16_t address){
  for(size_t i=0;i<ANENJI_REGISTER_COUNT;++i)
    if(ANENJI_REGISTERS[i].address==address)
      return &ANENJI_REGISTERS[i];
  return nullptr;
}

bool getRegisterValue(uint16_t start,uint16_t count,const uint8_t* bytes,
                      uint16_t address,uint16_t& value){
  if(address<start||address>=start+count)return false;
  uint16_t i=address-start;
  value=(static_cast<uint16_t>(bytes[i*2])<<8)|bytes[i*2+1];
  return true;
}

bool notifyDatalogger(){
  IPAddress ip;
  if(!ip.fromString(appSettings.inverterIp))return false;
  String command="set>server="+WiFi.localIP().toString()+
                 ":"+String(TCP_PORT)+";";
  if(!udp.begin(0))return false;
  udp.beginPacket(ip,UDP_PORT);
  udp.print(command);
  udp.endPacket();
  Serial.print("UDP: ");
  Serial.println(command);

  uint32_t started=millis();
  while(millis()-started<2000){
    if(udp.parsePacket()>0){
      Serial.print("UDP-ответ: ");
      while(udp.available())
        Serial.write(static_cast<uint8_t>(udp.read()));
      Serial.println();
      break;
    }
    delay(10);
  }
  udp.stop();
  return true;
}

bool ensureInverterConnection(){
  if(inverterClient&&inverterClient.connected())return true;
  inverterClient.stop();
  if(!notifyDatalogger())return false;
  uint32_t started=millis();
  while(millis()-started<TCP_CONNECT_TIMEOUT_MS){
    WiFiClient client=tcpServer.available();
    if(client){
      inverterClient=client;
      Serial.println(inverterClient.remoteIP());
      return true;
    }
    handleWebServer();
    tuyaServiceDevices(); // Manual catalog work can progress while the inverter is disconnected.
    delay(10);
  }
  return false;
}

void buildReadRequest(uint16_t start,uint16_t count,uint8_t request[8]){
  request[0]=MODBUS_UNIT_ID;
  request[1]=MODBUS_FUNCTION;
  request[2]=highByte(start);
  request[3]=lowByte(start);
  request[4]=highByte(count);
  request[5]=lowByte(count);
  uint16_t crc=crc16Modbus(request,6);
  request[6]=lowByte(crc);
  request[7]=highByte(crc);
}

bool readExactly(WiFiClient& client,uint8_t* dest,size_t count,
                 uint32_t timeoutMs){
  size_t received=0;
  uint32_t started=millis();
  while(received<count){
    while(client.available()&&received<count){
      int v=client.read();
      if(v>=0)dest[received++]=static_cast<uint8_t>(v);
    }
    if(received==count)return true;
    if(millis()-started>=timeoutMs)return false;
    if(!client.connected()&&!client.available())return false;
    handleWebServer();
    delay(1);
  }
  return true;
}

uint16_t historyCrc(const StoredRecord& record){
  return crc16Modbus(reinterpret_cast<const uint8_t*>(&record),
                     offsetof(StoredRecord,crc));
}

bool validHistoryRecord(const StoredRecord& record){
  return record.slot>=1577836800UL/FIVE_MINUTES&&
    record.slot!=UINT32_MAX&&record.crc==historyCrc(record);
}

size_t historyAddress(uint32_t slot){
  return SETTINGS_BYTES+
    (slot%EEPROM_RECORD_COUNT)*sizeof(StoredRecord);
}

bool readEepromRecord(size_t index,StoredRecord& record){
  EEPROM.get(SETTINGS_BYTES+index*sizeof(StoredRecord),record);
  return validHistoryRecord(record)&&
    record.slot%EEPROM_RECORD_COUNT==index;
}

bool saveToEeprom(const StoredRecord& record){
  EEPROM.put(historyAddress(record.slot),record);
  return EEPROM.commit();
}

bool recordDate(uint32_t slot,int& year,int& month,int& day,int& hour,
                int& minute){
  time_t started=static_cast<time_t>(slot*FIVE_MINUTES+1);
  struct tm t;
  if(!localtime_r(&started,&t))return false;
  year=t.tm_year+1900;
  month=t.tm_mon+1;
  day=t.tm_mday;
  hour=t.tm_hour;
  minute=t.tm_min;
  return minute%5==0;
}

bool recordPath(uint32_t slot,char* path,size_t capacity){
  int y,m,d,h,min;
  if(!recordDate(slot,y,m,d,h,min))return false;
  int n=snprintf(path,capacity,"/%04d%02d%02d.CSV",y,m,d);
  return n>0&&static_cast<size_t>(n)<capacity;
}

enum LineResult{LINE_END,LINE_OK,LINE_BAD};

int readLine(File& file,char* line,size_t capacity){
  size_t length=0;
  while(file.available()){
    int ch=file.read();
    if(ch=='\n'){
      line[length]='\0';
      return LINE_OK;
    }
    if(length+1>=capacity){
      while(file.available()){
        if(file.read()=='\n')break;
        yield();
      }
      return LINE_BAD;
    }
    line[length++]=static_cast<char>(ch);
  }
  return length?LINE_BAD:LINE_END;
}

bool parseRecord(char* line,StoredRecord& record){
  uint32_t value[7];
  char* p=line;
  for(size_t i=0;i<7;++i){
    char* end=nullptr;
    unsigned long parsed=strtoul(p,&end,10);
    if(end==p)return false;
    if(i<6){
      if(*end!=',')return false;
      p=end+1;
    }else if(*end!='\0'&&*end!='\r')return false;
    value[i]=static_cast<uint32_t>(parsed);
  }
  if(value[6]>UINT16_MAX)return false;
  for(size_t i=1;i<=5;++i)
    if(value[i]>UINT16_MAX)return false;
  record={};
  record.slot=value[0];
  for(size_t i=0;i<5;++i)record.wh[i]=value[i+1];
  record.crc=value[6];
  return validHistoryRecord(record);
}

bool sameRecord(const StoredRecord& a,const StoredRecord& b){
  if(a.slot!=b.slot)return false;
  for(size_t i=0;i<5;++i)
    if(a.wh[i]!=b.wh[i])return false;
  return true;
}

bool latestSdRecord(uint32_t slot,StoredRecord& found){
  if(!sdReady)return false;
  char path[20];
  if(!recordPath(slot,path,sizeof(path)))return false;
  File file=SD.open(path,FILE_READ);
  if(!file)return false;
  bool exists=false;
  char line[112];
  while(true){
    int result=readLine(file,line,sizeof(line));
    if(result==LINE_END)break;
    StoredRecord candidate;
    if(result==LINE_OK&&parseRecord(line,candidate)&&
       candidate.slot==slot){
      found=candidate;
      exists=true;
    }
    yield();
  }
  file.close();
  return exists;
}

bool appendToSd(const StoredRecord& record){
  if(!sdReady)return false;
  StoredRecord existing;
  if(latestSdRecord(record.slot,existing)&&
     sameRecord(existing,record))return true;

  char path[20];
  if(!recordPath(record.slot,path,sizeof(path)))return false;
  File file=SD.open(path,FILE_WRITE);
  if(!file){
    sdReady=false;
    return false;
  }

  char line[112];
  int length=snprintf(line,sizeof(line),
    "\n%lu,%u,%u,%u,%u,%u,%u\n",
    static_cast<unsigned long>(record.slot),
    record.wh[0],record.wh[1],record.wh[2],
    record.wh[3],record.wh[4],record.crc);

  bool written=length>0&&
    static_cast<size_t>(length)<sizeof(line)&&
    file.write(reinterpret_cast<const uint8_t*>(line),length)==
      static_cast<size_t>(length);

  file.close();

  StoredRecord verified;
  if(!written||!latestSdRecord(record.slot,verified)||
     !sameRecord(record,verified)){
    sdReady=false;
    return false;
  }
  return true;
}

bool migrateEeprom(){
  if(!sdReady)return false;
  bool changed=false;
  size_t transferred=0;
  bool complete=true;
  StoredRecord record;
  for(size_t i=0;i<EEPROM_RECORD_COUNT;++i){
    if(!readEepromRecord(i,record))continue;
    if(!appendToSd(record)){
      complete=false;
      break;
    }
    for(size_t b=0;b<sizeof(StoredRecord);++b)
      EEPROM.write(SETTINGS_BYTES+i*sizeof(StoredRecord)+b,0xFF);
    changed=true;
    ++transferred;
    Serial.printf("[SYNC] slot=%lu подтверждён на SD\n",
              static_cast<unsigned long>(record.slot));
    yield();
  }
  if(changed){
  bool committed=EEPROM.commit();
  Serial.printf("[SYNC] Подтверждено на SD: %u; очистка EEPROM: %s\n",
                static_cast<unsigned>(transferred),
                committed ? "OK" : "ОШИБКА");
  if(!committed)return false;
}
return complete;
}

void probeSd(){
  uint32_t now=millis();
  if(now-lastSdProbeMillis<15000)return;
  lastSdProbeMillis=now;

  if(sdReady){
    File root=SD.open("/");
    if(root){
      root.close();
      return;
    }
    Serial.println("[SD] Карта перестала отвечать");
    sdReady=false;
  }

  if(SD.begin(SD_CS_PIN)){
    sdReady=true;
    Serial.println("[SD] Карта обнаружена, начинаю перенос EEPROM");
    if(!migrateEeprom())
      Serial.println("[SD] Перенос EEPROM завершён не полностью");
  }else{
    Serial.println("[SD] Карта недоступна; история будет записываться в EEPROM");
  }
}

void inspectStorage(){
  size_t good=0,bad=0;
  StoredRecord record;
  for(size_t i=0;i<EEPROM_RECORD_COUNT;++i){
    if(readEepromRecord(i,record)){
      ++good;
      continue;
    }
    bool empty=true;
    size_t address=SETTINGS_BYTES+i*sizeof(StoredRecord);
    for(size_t b=0;b<sizeof(StoredRecord);++b)
      if(EEPROM.read(address+b)!=0xFF)empty=false;
    if(!empty)++bad;
  }
  Serial.printf("EEPROM: корректных=%u, некорректных=%u\n",
                static_cast<unsigned>(good),
                static_cast<unsigned>(bad));

  if(!sdReady)return;
  File root=SD.open("/");
  if(!root){
    sdReady=false;
    return;
  }

  size_t validLines=0,invalidLines=0;
  while(true){
    File entry=root.openNextFile();
    if(!entry)break;
    const char* name=entry.name();
    const char* base=strrchr(name,'/');
    base=base?base+1:name;
    size_t len=strlen(base);
    bool csv=len==12&&strcmp(base+8,".CSV")==0;
    if(csv){
      char line[112];
      while(true){
        int state=readLine(entry,line,sizeof(line));
        if(state==LINE_END)break;
        StoredRecord r;
        char expected[20];
        if(state==LINE_OK&&parseRecord(line,r)&&
           recordPath(r.slot,expected,sizeof(expected))&&
           strcmp(expected+1,base)==0)++validLines;
        else if(state==LINE_BAD||line[0]!='\0')++invalidLines;
        yield();
      }
    }
    entry.close();
    yield();
  }
  root.close();
  Serial.printf("SD: корректных строк=%u, повреждённых=%u\n",
                static_cast<unsigned>(validLines),
                static_cast<unsigned>(invalidLines));
}

void readDay(int year,int month,int day){
  memset(dayValid,0,sizeof(dayValid));
  char path[20];
  snprintf(path,sizeof(path),"/%04d%02d%02d.CSV",
           year,month,day);

  if(sdReady){
    File file=SD.open(path,FILE_READ);
    if(file){
      char line[112];
      while(true){
        int state=readLine(file,line,sizeof(line));
        if(state==LINE_END)break;
        StoredRecord record;
        int y,m,d,h,min;
        if(state==LINE_OK&&parseRecord(line,record)&&
           recordDate(record.slot,y,m,d,h,min)&&
           y==year&&m==month&&d==day){
          size_t index=h*12+min/5;
          if(index<288){
            dayRecords[index]=record;
            dayValid[index]=true;
          }
        }
        yield();
      }
      file.close();
    }
  }

  StoredRecord record;
  for(size_t i=0;i<EEPROM_RECORD_COUNT;++i){
    if(!readEepromRecord(i,record))continue;
    int y,m,d,h,min;
    if(recordDate(record.slot,y,m,d,h,min)&&
       y==year&&m==month&&d==day){
      size_t index=h*12+min/5;
      if(index<288){
        dayRecords[index]=record;
        dayValid[index]=true;
      }
    }
  }
}

uint16_t toStoredWh(float ws){
  if(ws<=0)return 0;
  float value=ws/3600.0f;
  if(value>=65535)return UINT16_MAX;
  return static_cast<uint16_t>(value+0.5f);
}

bool saveBucket(){
  if(!bucketHasMeasurement)return true;

  StoredRecord record={};
  record.slot=activeSlot;
  record.wh[0]=toStoredWh(currentBucket.gridWs);
  record.wh[1]=toStoredWh(currentBucket.pvWs);
  record.wh[2]=toStoredWh(currentBucket.loadWs);
  record.wh[3]=toStoredWh(currentBucket.batteryChargeWs);
  record.wh[4]=toStoredWh(currentBucket.batteryDischargeWs);
  record.crc=historyCrc(record);

  if(sdReady&&appendToSd(record)){
    Serial.printf("[HISTORY] SD: slot=%lu, Wh=%u,%u,%u,%u,%u\n",
      static_cast<unsigned long>(record.slot),
      record.wh[0],record.wh[1],record.wh[2],
      record.wh[3],record.wh[4]);
    return true;
  }

  if(saveToEeprom(record)){
    Serial.printf("[HISTORY] EEPROM: slot=%lu, Wh=%u,%u,%u,%u,%u\n",
      static_cast<unsigned long>(record.slot),
      record.wh[0],record.wh[1],record.wh[2],
      record.wh[3],record.wh[4]);
    return true;
  }

  Serial.printf("[HISTORY] ОШИБКА записи slot=%lu\n",
                static_cast<unsigned long>(record.slot));
  return false;
}

float positivePower(float x){return x>0?x:0;}

InstantPower makeInstantPower(float grid,float pv,float load,
                              float battery){
  InstantPower p={};
  p.grid=positivePower(grid);
  p.pv=positivePower(pv);
  p.load=positivePower(load);
  if(battery>=0)p.batteryCharge=battery;
  else p.batteryDischarge=-battery;
  return p;
}

void integrateBucket(const InstantPower& a,const InstantPower& b,
                     uint32_t seconds){
  if(!seconds||seconds>3600)return;
  currentBucket.gridWs+=(a.grid+b.grid)*0.5f*seconds;
  currentBucket.pvWs+=(a.pv+b.pv)*0.5f*seconds;
  currentBucket.loadWs+=(a.load+b.load)*0.5f*seconds;
  currentBucket.batteryChargeWs+=
    (a.batteryCharge+b.batteryCharge)*0.5f*seconds;
  currentBucket.batteryDischargeWs+=
    (a.batteryDischarge+b.batteryDischarge)*0.5f*seconds;
}

void closeElapsedBucket(time_t now){
  if(!activeSlot)return;
  time_t boundary=static_cast<time_t>((activeSlot+1)*FIVE_MINUTES);
  if(now<=boundary)return;

  uint32_t grace=appSettings.registerIntervalSeconds+5;
  bool recent=previousPowerValid&&
    previousMeasurementTime<=boundary&&
    boundary-previousMeasurementTime<=grace;

  if(bucketHasMeasurement&&recent&&
     boundary>previousMeasurementTime){
    integrateBucket(previousPower,previousPower,
                    boundary-previousMeasurementTime);
  }

  if(!saveBucket())return;

  currentBucket={};
  bucketHasMeasurement=false;
  activeSlot=measurementSlot(now);

  if(recent&&now-boundary<=grace){
    previousMeasurementTime=boundary;
  }else{
    previousPowerValid=false;
  }
}

void accountMeasurement(const InstantPower& power,time_t now){
  if(!validClock())return;
  uint32_t slot=measurementSlot(now);

  if(!activeSlot)activeSlot=slot;
  if(slot<activeSlot){
    previousPowerValid=false;
    currentBucket={};
    bucketHasMeasurement=false;
    activeSlot=slot;
  }

  if(slot!=activeSlot){
    time_t boundary=static_cast<time_t>((activeSlot+1)*FIVE_MINUTES);
    uint32_t grace=appSettings.registerIntervalSeconds+5;
    bool recent=previousPowerValid&&
      previousMeasurementTime<=boundary&&
      boundary-previousMeasurementTime<=grace;

    if(bucketHasMeasurement&&recent&&
       boundary>previousMeasurementTime)
      integrateBucket(previousPower,previousPower,
                      boundary-previousMeasurementTime);

    if(!saveBucket())return;

    currentBucket={};
    bucketHasMeasurement=false;
    activeSlot=slot;
    if(recent&&now-boundary<=grace)
      previousMeasurementTime=boundary;
    else previousPowerValid=false;
  }

  if(previousPowerValid&&now>=previousMeasurementTime&&
     now-previousMeasurementTime<=3600)
    integrateBucket(previousPower,power,
                    now-previousMeasurementTime);

  previousPower=power;
  previousMeasurementTime=now;
  previousPowerValid=true;
  bucketHasMeasurement=true;

  if(now%FIVE_MINUTES==0){
    if(saveBucket()){
      currentBucket={};
      bucketHasMeasurement=false;
      activeSlot=slot+1;
    }
  }
}

String diagnosticText(uint32_t flags,bool faults,bool ru=true){
  if(!flags)return faults?(ru?"Неполадок нет":"No faults"):
    (ru?"Предупреждений нет":"No warnings");
  String text=faults?(ru?"Неполадки: ":"Faults: "):
    (ru?"Предупреждения: ":"Warnings: ");
  bool first=true;
  for(uint8_t bit=0;bit<32;++bit){
    if(!(flags&(UINT32_C(1)<<bit)))continue;
    if(!first)text+=F("; ");
    first=false;
    text+=faults?faultName(bit,ru):warningName(bit,ru);
    text+=F(" [");
    text+=String(bit);
    text+=']';
    if(text.length()>210){
      text+=F("; …");
      break;
    }
  }
  return text;
}

void processDiagnosticRegisters(uint16_t start,uint16_t count,
                                const uint8_t* bytes){
  if(count<2||(start!=100&&start!=108))return;
  uint32_t flags=(static_cast<uint32_t>(bytes[0])<<24)|
    (static_cast<uint32_t>(bytes[1])<<16)|
    (static_cast<uint32_t>(bytes[2])<<8)|bytes[3];

  if(start==100){
    currentFaultFlags=flags;
    currentFaults=diagnosticText(flags,true);
    faultRead=true;
  }else{
    currentWarningFlags=flags;
    currentWarnings=diagnosticText(flags,false);
    warningRead=true;
  }
}

void processPowerRegisters(uint16_t start,uint16_t count,
                           const uint8_t* bytes){
  if(start!=201)return;
  uint16_t grid=0,pv=0,load=0,battery=0;
  uint16_t gv=0,pvV=0,lv=0,bv=0,mode=0,percent=0;

  if(!getRegisterValue(start,count,bytes,204,grid)||
     !getRegisterValue(start,count,bytes,223,pv)||
     !getRegisterValue(start,count,bytes,213,load)||
     !getRegisterValue(start,count,bytes,217,battery))return;

  getRegisterValue(start,count,bytes,202,gv);
  getRegisterValue(start,count,bytes,219,pvV);
  getRegisterValue(start,count,bytes,210,lv);
  getRegisterValue(start,count,bytes,215,bv);
  getRegisterValue(start,count,bytes,201,mode);
  getRegisterValue(start,count,bytes,229,percent);

  InstantPower measured=makeInstantPower(
    signedRegister(grid),signedRegister(pv),
    signedRegister(load),signedRegister(battery));

  time_t receivedAt=time(nullptr);
  if(validClock())accountMeasurement(measured,receivedAt);

  currentPower=measured;
  currentGridVoltage=signedRegister(gv)*0.1f;
  currentPvVoltage=signedRegister(pvV)*0.1f;
  currentLoadVoltage=signedRegister(lv)*0.1f;
  currentBatteryVoltage=signedRegister(bv)*0.1f;
  currentBatteryPercent=static_cast<uint8_t>(
    min(static_cast<uint16_t>(100),percent));
  currentModeCode=mode;
  currentMode=workingModeName(mode);
  modeRead=true;
  tuyaMeasurementMs=millis();
  tuyaHasMeasurement=true;
  currentStatus="Data updated";
  currentUpdateTime=currentDateTimeString();
}

void printRegisterValue(uint16_t address,uint16_t raw){
  const RegisterDefinition* def=findRegisterDefinition(address);
  Serial.print("Register ");
  Serial.print(address);
  Serial.print(" | HEX 0x");
  printHexByte(highByte(raw));
  printHexByte(lowByte(raw));
  Serial.print(" | ");
  if(!def){
    Serial.println(raw);
    return;
  }
  Serial.print(def->title);
  Serial.print(" = ");
  float value=def->type==REG_SHORT?
    signedRegister(raw)*def->scale:raw*def->scale;
  Serial.print(value,2);
  if(def->unit&&def->unit[0]){
    Serial.print(' ');
    Serial.print(def->unit);
  }
  if(address==201){
    Serial.print(" (");
    Serial.print(workingModeName(raw));
    Serial.print(')');
  }
  Serial.println();
}

void printRegisterRange(uint16_t start,uint16_t count,
                        const uint8_t* bytes){
  Serial.println("----------------------------------------");
  Serial.printf("Диапазон %u-%u\n",start,start+count-1);
  for(uint16_t i=0;i<count;++i){
    uint16_t raw=(static_cast<uint16_t>(bytes[i*2])<<8)|
                 bytes[i*2+1];
    printRegisterValue(start+i,raw);
  }
  processDiagnosticRegisters(start,count,bytes);
  processPowerRegisters(start,count,bytes);
}

bool readModbusRange(uint16_t start,uint16_t count){
  if(!count||count>34||!ensureInverterConnection())return false;
  uint8_t request[8];
  buildReadRequest(start,count,request);
  Serial.print("Modbus request: ");
  printHexBuffer(request,sizeof(request));

  if(inverterClient.write(request,sizeof(request))!=sizeof(request)){
    inverterClient.stop();
    return false;
  }

  uint8_t response[MAX_RESPONSE_SIZE]={};
  if(!readExactly(inverterClient,response,3,
                  MODBUS_RESPONSE_TIMEOUT_MS)){
    inverterClient.stop();
    return false;
  }

  size_t length=(response[1]&0x80)?5:3+response[2]+2;
  if(length>sizeof(response)||length<5||
     !readExactly(inverterClient,response+3,length-3,
                  MODBUS_RESPONSE_TIMEOUT_MS)){
    inverterClient.stop();
    return false;
  }

  Serial.print("Modbus response: ");
  printHexBuffer(response,length);

  uint16_t crc=crc16Modbus(response,length-2);
  uint16_t received=response[length-2]|
    (static_cast<uint16_t>(response[length-1])<<8);

  if(crc!=received||response[0]!=MODBUS_UNIT_ID){
    inverterClient.stop();
    return false;
  }
  if(response[1]==(MODBUS_FUNCTION|0x80)){
    Serial.printf("Исключение Modbus: %u\n",response[2]);
    return false;
  }
  if(response[1]!=MODBUS_FUNCTION||response[2]!=count*2)
    return false;

  printRegisterRange(start,count,response+3);
  return true;
}

void addRecordEnergy(WebHistoryHour& out,
                     const StoredRecord& record){
  out.hasData=true;
  out.gridKwh+=record.wh[0]/1000.0f;
  out.pvKwh+=record.wh[1]/1000.0f;
  out.loadKwh+=record.wh[2]/1000.0f;
  out.batteryChargeKwh+=record.wh[3]/1000.0f;
  out.batteryDischargeKwh+=record.wh[4]/1000.0f;
}

void addCurrentBucket(WebHistoryHour& out){
  out.hasData=true;
  out.gridKwh+=currentBucket.gridWs/3600000.0f;
  out.pvKwh+=currentBucket.pvWs/3600000.0f;
  out.loadKwh+=currentBucket.loadWs/3600000.0f;
  out.batteryChargeKwh+=
    currentBucket.batteryChargeWs/3600000.0f;
  out.batteryDischargeKwh+=
    currentBucket.batteryDischargeWs/3600000.0f;
}

int daysInMonth(int year,int month){
  static const uint8_t days[]=
    {31,28,31,30,31,30,31,31,30,31,30,31};
  if(month<1||month>12)return 0;
  if(month==2&&year%4==0&&
     (year%100!=0||year%400==0))return 29;
  return days[month-1];
}

size_t getHistoryForWeb(WebHistoryHour* output,size_t maxRecords,
                        const String& scale,uint16_t year,
                        uint8_t month,uint8_t day){
  if(!maxRecords)return 0;

  if(scale=="today"){
    if(!validClock())return 0;
    time_t now=time(nullptr);
    struct tm t;
    localtime_r(&now,&t);
    year=t.tm_year+1900;
    month=t.tm_mon+1;
    day=t.tm_mday;
  }

  if(year<2000||month<1||month>12||
     day<1||day>31)return 0;

  size_t count=scale=="hour"?24:
    scale=="day"?daysInMonth(year,month):
    scale=="month"?12:1;

  if(scale=="today")count=1;
  count=min(count,maxRecords);

  for(size_t i=0;i<count;++i){
    output[i]={};
    output[i].year=year;
    output[i].month=month;
    output[i].day=day;
    if(scale=="hour")output[i].hour=i;
    if(scale=="day")output[i].day=i+1;
    if(scale=="month"){
      output[i].month=i+1;
      output[i].day=1;
    }
    if(scale=="year"){
      output[i].month=1;
      output[i].day=1;
    }
  }

  int firstMonth=scale=="month"||scale=="year"?1:month;
  int lastMonth=scale=="month"||scale=="year"?12:month;

  for(int m=firstMonth;m<=lastMonth;++m){
    int firstDay=scale=="hour"||scale=="today"?day:1;
    int lastDay=scale=="hour"||scale=="today"?
      day:daysInMonth(year,m);

    for(int d=firstDay;d<=lastDay;++d){
      readDay(year,m,d);

      for(size_t i=0;i<288;++i){
        if(!dayValid[i])continue;
        if(bucketHasMeasurement&&dayRecords[i].slot==activeSlot)  continue;
        size_t index=scale=="hour"?i/12:
          scale=="day"?d-1:
          scale=="month"?m-1:0;
        if(index<count)
          addRecordEnergy(output[index],dayRecords[i]);
      }

      if(bucketHasMeasurement&&activeSlot){
        int by,bm,bd,bh,bmin;
        if(recordDate(activeSlot,by,bm,bd,bh,bmin)&&
           by==year&&bm==m&&bd==d){
          size_t index=scale=="hour"?bh:
            scale=="day"?d-1:
            scale=="month"?m-1:0;
          if(index<count)addCurrentBucket(output[index]);
        }
      }
      yield();
    }
  }
  return count;
}

size_t getTodaySamples(WebSample* output,size_t maxSamples,size_t offset){
  if(!validClock())return 0;
  time_t now=time(nullptr);
  struct tm t;
  localtime_r(&now,&t);
  if(offset==0)readDay(t.tm_year+1900,t.tm_mon+1,t.tm_mday);

  size_t result=0,seen=0;
  for(size_t i=0;i<288&&result<maxSamples;++i){
    if(!dayValid[i])continue;
    if(bucketHasMeasurement&&dayRecords[i].slot==activeSlot)  continue;
    if(seen++<offset)continue;
    WebSample& p=output[result++];
    p={};
    p.timestamp=dayRecords[i].slot*FIVE_MINUTES;
    p.minuteOfDay=i*5;
    p.gridPower=dayRecords[i].wh[0]*12.0f;
    p.pvPower=dayRecords[i].wh[1]*12.0f;
    p.loadPower=dayRecords[i].wh[2]*12.0f;
    p.batteryChargePower=dayRecords[i].wh[3]*12.0f;
    p.batteryDischargePower=dayRecords[i].wh[4]*12.0f;
  }
  return result;
}

void copyWebText(char* destination,size_t capacity,
                 const String& source){
  if(capacity)strlcpy(destination,source.c_str(),capacity);
}

void fillWebLiveData(WebLiveData& d){
  d={};
  d.valid=previousPowerValid;
  const bool ru=appSettings.language=="ru";
  copyWebText(d.status,sizeof(d.status),currentStatus=="Data updated"?
    String(ru?"Данные обновлены":"Data updated"):
    String(ru?"Ожидание данных":"Waiting for data"));
  copyWebText(d.updateTime,sizeof(d.updateTime),currentUpdateTime);
  copyWebText(d.inverterIp,sizeof(d.inverterIp),appSettings.inverterIp);
  copyWebText(d.mode,sizeof(d.mode),
              modeRead?String(workingModeName(currentModeCode,ru)):
              String(ru?"Ожидание данных":"Waiting for data"));
  copyWebText(d.faults,sizeof(d.faults),
              faultRead?diagnosticText(currentFaultFlags,true,ru):
              String(ru?"Диагностика неполадок недоступна":"Fault diagnostics unavailable"));
  copyWebText(d.warnings,sizeof(d.warnings),
              warningRead?diagnosticText(currentWarningFlags,false,ru):
              String(ru?"Диагностика предупреждений недоступна":"Warning diagnostics unavailable"));

  d.gridPower=currentPower.grid;
  d.pvPower=currentPower.pv;
  d.loadPower=currentPower.load;
  d.batteryChargePower=currentPower.batteryCharge;
  d.batteryDischargePower=currentPower.batteryDischarge;
  d.batteryPower=currentPower.batteryCharge-
                 currentPower.batteryDischarge;
  d.gridVoltage=currentGridVoltage;
  d.pvVoltage=currentPvVoltage;
  d.loadVoltage=currentLoadVoltage;
  d.batteryVoltage=currentBatteryVoltage;
  d.batteryPercent=currentBatteryPercent;

  if(validClock()){
    time_t now=time(nullptr);
    struct tm t;
    localtime_r(&now,&t);
    WebHistoryHour hours[24];
    if(getHistoryForWeb(hours,24,"hour",
       t.tm_year+1900,t.tm_mon+1,t.tm_mday)==24){
      const WebHistoryHour& h=hours[t.tm_hour];
      d.gridKwh=h.gridKwh;
      d.pvKwh=h.pvKwh;
      d.loadKwh=h.loadKwh;
      d.batteryChargeKwh=h.batteryChargeKwh;
      d.batteryDischargeKwh=h.batteryDischargeKwh;
    }
  }
}

void restoreCurrentBucket(){
  if(!validClock())return;

  time_t now=time(nullptr);
  uint32_t slot=measurementSlot(now);

  // Ровно на границе пятиминутки предыдущая запись
  // уже может быть завершённой.
  if(now>=static_cast<time_t>((slot+1)*FIVE_MINUTES))
    return;

  StoredRecord record={};
  StoredRecord eepromRecord={};

  bool found=latestSdRecord(slot,record);

  // Неперенесённая EEPROM-запись имеет приоритет над SD.
  size_t index=slot%EEPROM_RECORD_COUNT;
  if(readEepromRecord(index,eepromRecord)&&
     eepromRecord.slot==slot){
    record=eepromRecord;
    found=true;
  }

  if(!found)return;

  activeSlot=slot;
  currentBucket.gridWs=record.wh[0]*3600.0f;
  currentBucket.pvWs=record.wh[1]*3600.0f;
  currentBucket.loadWs=record.wh[2]*3600.0f;
  currentBucket.batteryChargeWs=record.wh[3]*3600.0f;
  currentBucket.batteryDischargeWs=record.wh[4]*3600.0f;
  bucketHasMeasurement=true;

  // Не интегрировать промежуток между выключением и
  // первым успешным чтением после запуска.
  previousPowerValid=false;
  previousMeasurementTime=0;

  Serial.printf(
    "[RESTORE] Текущая пятиминутка slot=%lu, Wh=%u,%u,%u,%u,%u\n",
    static_cast<unsigned long>(slot),
    record.wh[0],record.wh[1],record.wh[2],
    record.wh[3],record.wh[4]);
}

#include "tuya_cloud.h"

void setup(){
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nANENJI Datalogger / Wemos D1 mini");

  EEPROM.begin(EEPROM_SIZE);
  loadSettings();
  inspectStorage();

  SPI.begin();
  sdReady=SD.begin(SD_CS_PIN);
  tuyaBegin();
  Serial.printf("[SD] При запуске: %s, CS=D4\n",
              sdReady ? "карта доступна" : "карта недоступна");

  if(!connectToWiFi()){
    startSetupAccessPoint();
    return;
  }

synchronizeTime();

if(sdReady){
  inspectStorage();
  if(!migrateEeprom())
    Serial.println("Перенос EEPROM завершён не полностью");
}

restoreCurrentBucket();

  tcpServer.begin();
  beginWebServer(fillWebLiveData,getTodaySamples,getHistoryForWeb,
                 saveSettings,saveLanguage,appSettings,false);
  tuyaRegisterWeb();

  Serial.printf("Резерв EEPROM: %u пятиминуток\n",
                static_cast<unsigned>(EEPROM_RECORD_COUNT));
}

void loop(){
  handleWebServer();
  probeSd();

  if(setupAccessPoint){
    delay(5);
    return;
  }

  if(WiFi.status()!=WL_CONNECTED){
    inverterClient.stop();
    if(!connectToWiFi()){
      startSetupAccessPoint();
      return;
    }
    synchronizeTime();
    tcpServer.begin();
  }

  static uint32_t lastReadMillis=0;
  uint32_t nowMillis=millis();

  if(nowMillis-lastReadMillis>=
     appSettings.registerIntervalSeconds*1000UL){
    lastReadMillis=nowMillis;
    for(size_t i=0;i<READ_RANGE_COUNT;++i){
      readModbusRange(READ_RANGES[i].start,
                      READ_RANGES[i].count);
      handleWebServer();
      delay(200);
    }
  }

  if(validClock())closeElapsedBucket(time(nullptr));
  tuyaLoop();
  delay(2);
}