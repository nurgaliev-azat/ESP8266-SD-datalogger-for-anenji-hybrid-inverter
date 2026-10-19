"""Verify the actual firmware polling loop dispatches rules on a new snapshot."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
ino = (root / 'ANENJI_Dataloger/ANENJI_Dataloger.ino').read_text()
cloud = (root / 'ANENJI_Dataloger/tuya_cloud.h').read_text()
def function(source, name):
    start = source.index(name)
    clean = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*', lambda match: ' ' * len(match[0]), source)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        if clean[end] == '{': depth += 1
        elif clean[end] == '}': depth -= 1
        end += 1
    return source[start:end]
program = r'''
#include <cassert>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
bool setupAccessPoint=false,tuyaMeasurementPending=false,failMeasurement=false;
uint32_t clockMs=30000;int forced=0;
std::vector<std::string> events;
constexpr int WL_CONNECTED=3;
struct {int status(){return WL_CONNECTED;}} WiFi;
struct {void stop(){}} inverterClient;
struct {void begin(){}} tcpServer;
struct {uint32_t registerIntervalSeconds=30;} appSettings;
struct ReadRange{uint16_t start,count;};
constexpr ReadRange READ_RANGES[]={{100,2},{108,2},{201,34}};
constexpr size_t READ_RANGE_COUNT=3;
uint32_t millis(){return clockMs;}
void delay(int ms){clockMs+=ms;}
void handleWebServer(){}
void probeSd(){}
bool connectToWiFi(){return true;}
void startSetupAccessPoint(){}
void synchronizeTime(){}
bool validClock(){return true;}
void closeElapsedBucket(time_t){events.push_back("history");}
bool readModbusRange(uint16_t start,uint16_t){
 events.push_back("read"+std::to_string(start));
 if(start==201){if(failMeasurement)return false;tuyaMeasurementPending=true;}
 return true;
}
namespace TuyaCloud {
void tick(bool registersUpdated=false){
 events.push_back(registersUpdated?"registerRules":"timerRules");if(registersUpdated)++forced;
}
}
''' + function(cloud, 'void tuyaRegistersUpdated(){') + '\n' + function(cloud, 'void tuyaLoop(){') + '\n' + function(ino, 'void loop(){') + r'''
int main(){
 loop();assert(forced==1&&!tuyaMeasurementPending);
 auto found=[&](const std::string& item){for(size_t i=0;i<events.size();++i)if(events[i]==item)return i;return events.size();};
 assert(found("read201")<found("registerRules")&&found("registerRules")<found("history"));
 loop();assert(forced==1); // no register poll due: background timer only
 tuyaRegistersUpdated();assert(forced==1); // consumed snapshot cannot be dispatched twice
 clockMs=60000;failMeasurement=true;loop();assert(forced==1);
 clockMs=90000;failMeasurement=false;loop();assert(forced==2); // 30s polls, not 5min history
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'registers.cpp'
    binary = Path(directory) / 'registers'
    cpp.write_text(program)
    subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: rules follow new register snapshots before history, failed reads do not dispatch, background timers remain active')
