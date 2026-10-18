"""Exercise the firmware's actual paged sample function and cache lifetime."""
from pathlib import Path
import subprocess
import tempfile
source = (Path(__file__).parent / '../ANENJI_Dataloger/ANENJI_Dataloger.ino').read_text()
start = source.index('size_t getTodaySamples(WebSample* output,size_t maxSamples,size_t offset){')
end = source.index('\nvoid copyWebText', start)
function = source[start:end]
program = r'''
#include <cassert>
#include <cstdint>
#include <ctime>
#include <memory>
#include <new>
int allocated=0;
struct StoredRecord { uint32_t slot; uint16_t wh[5]; };
struct DayCache {
 StoredRecord records[288]; bool valid[288];
 DayCache(){++allocated;} ~DayCache(){--allocated;}
};
struct WebSample {uint32_t timestamp; int minuteOfDay; float gridPower,pvPower,loadPower,batteryChargePower,batteryDischargePower;};
constexpr uint32_t FIVE_MINUTES=300;
bool bucketHasMeasurement=false;uint32_t activeSlot=0;int points=0;
bool validClock(){return true;}
void readDay(DayCache& cache,int,int,int){
 for(int i=0;i<288;++i){cache.valid[i]=i<points;cache.records[i]={uint32_t(i+1),{1,2,3,4,5}};}
}
''' + function + r'''
int main(){
 WebSample output[12];
 points=13;assert(getTodaySamples(output,12,0)==12);assert(allocated==1);assert(output[0].pvPower==24);
 assert(getTodaySamples(output,12,12)==1);assert(output[0].minuteOfDay==60);assert(allocated==0);
 points=24;assert(getTodaySamples(output,12,0)==12);assert(getTodaySamples(output,12,12)==12);assert(allocated==1);
 assert(getTodaySamples(output,12,24)==0);assert(allocated==0);
 points=0;assert(getTodaySamples(output,12,0)==0);assert(allocated==0);
 points=2;bucketHasMeasurement=true;activeSlot=1;
 assert(getTodaySamples(output,12,0)==1);assert(output[0].timestamp==600);assert(allocated==0);
 bucketHasMeasurement=false;points=20;getTodaySamples(output,12,0);assert(allocated==1);
 points=1;assert(getTodaySamples(output,12,0)==1);assert(allocated==0);
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'samples.cpp'
    binary = Path(directory) / 'samples'
    cpp.write_text(program)
    subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: sample paging, empty history, active bucket exclusion and cache release/replacement')
