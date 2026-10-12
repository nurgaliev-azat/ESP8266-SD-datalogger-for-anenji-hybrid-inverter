#ifndef ANENJI_REGISTERS_H
#define ANENJI_REGISTERS_H
#include <Arduino.h>

enum RegisterType{REG_SHORT,REG_USHORT,REG_FLAGS32,REG_ASCII};

struct RegisterDefinition{
  uint16_t address;
  const char* name;
  const char* title;
  RegisterType type;
  float scale;
  const char* unit;
};

const RegisterDefinition ANENJI_REGISTERS[]={
  {100,"Fault Code","Код аварии",REG_FLAGS32,1.0f,""},
  {108,"Warning Code","Код предупреждения",REG_FLAGS32,1.0f,""},
  {201,"Working Mode","Режим работы",REG_USHORT,1.0f,""},
  {202,"Grid Voltage","Напряжение сети",REG_SHORT,0.1f,"В"},
  {203,"Grid Frequency","Частота сети",REG_SHORT,0.01f,"Гц"},
  {204,"Grid Power","Мощность сети",REG_SHORT,1.0f,"Вт"},
  {205,"Inv Voltage","Напряжение инвертора",REG_SHORT,0.1f,"В"},
  {206,"Inv Current","Ток инвертора",REG_SHORT,0.1f,"А"},
  {207,"Inv Frequency","Частота инвертора",REG_SHORT,0.01f,"Гц"},
  {208,"Inv Power","Мощность инвертора",REG_SHORT,1.0f,"Вт"},
  {209,"Charge Power","Мощность заряда",REG_SHORT,1.0f,"Вт"},
  {210,"Load Voltage","Напряжение нагрузки",REG_SHORT,0.1f,"В"},
  {211,"Load Current","Ток нагрузки",REG_SHORT,0.1f,"А"},
  {212,"Load Frequency","Частота нагрузки",REG_SHORT,0.01f,"Гц"},
  {213,"Load Power","Мощность нагрузки",REG_SHORT,1.0f,"Вт"},
  {214,"Apparent Power","Полная мощность",REG_SHORT,1.0f,"ВА"},
  {215,"Battery Voltage","Напряжение батареи",REG_SHORT,0.1f,"В"},
  {216,"Battery Current","Ток батареи",REG_SHORT,0.1f,"А"},
  {217,"Battery Power","Мощность батареи",REG_SHORT,1.0f,"Вт"},
  {219,"PV Voltage","Напряжение PV",REG_SHORT,0.1f,"В"},
  {220,"PV Current","Ток PV",REG_SHORT,0.1f,"А"},
  {223,"PV Power","Мощность PV",REG_SHORT,1.0f,"Вт"},
  {224,"PV Charge Power","Мощность заряда PV",REG_SHORT,1.0f,"Вт"},
  {225,"Load Percent","Загрузка",REG_SHORT,0.01f,"%"},
  {226,"DC-DC Temperature","Температура DCDC",REG_SHORT,1.0f,"°C"},
  {227,"Inv Temperature","Температура инвертора",REG_SHORT,1.0f,"°C"},
  {229,"Battery Percent","Заряд батареи",REG_USHORT,1.0f,"%"},
  {232,"Battery Current 2","Ток батареи 2",REG_SHORT,0.1f,"А"},
  {233,"Inv Charge Current","Ток заряда инвертора",REG_SHORT,0.1f,"А"},
  {234,"PV Charge Current","Ток заряда PV",REG_SHORT,0.1f,"А"}
};

const size_t ANENJI_REGISTER_COUNT=
  sizeof(ANENJI_REGISTERS)/sizeof(ANENJI_REGISTERS[0]);

inline const char* workingModeName(uint16_t mode,bool ru=true){
  switch(mode){
    case 0:return ru?"Включено":"Power on";
    case 1:return ru?"Режим ожидания":"Standby";
    case 2:return ru?"Основной режим":"Main mode";
    case 3:return ru?"Автономная работа":"Off-grid operation";
    case 4:return ru?"Байпас от сети к нагрузке":"Grid bypass to load";
    case 5:return ru?"Зарядка":"Charging";
    case 6:return ru?"Авария":"Fault";
    default:return ru?"Неизвестно":"Unknown";
  }
}

inline const char* faultName(uint8_t bit,bool ru=true){
  switch(bit){
    case 1:return ru?"Перегрев DCDC":"DC-DC overtemperature";
    case 2:return ru?"Перенапряжение батареи":"Battery overvoltage";
    case 4:return ru?"Короткое замыкание выхода":"Output short circuit";
    case 5:return ru?"Перенапряжение инвертора":"Inverter overvoltage";
    case 6:return ru?"Перегрузка выхода":"Output overload";
    case 7:return ru?"Перенапряжение шины":"Bus overvoltage";
    case 9:return ru?"Переток PV":"PV overcurrent";
    case 10:return ru?"Перенапряжение PV":"PV overvoltage";
    case 11:return ru?"Переток батареи":"Battery overcurrent";
    case 12:return ru?"Переток инвертора":"Inverter overcurrent";
    case 13:return ru?"Низкое напряжение шины":"Bus undervoltage";
    case 15:return ru?"Высокая составляющая DC":"High DC component";
    case 17:return ru?"Ошибка нулевого смещения выхода":"Output zero-offset error";
    case 18:return ru?"Ошибка нулевого смещения инвертора":"Inverter zero-offset error";
    case 19:return ru?"Ошибка нулевого смещения батареи":"Battery zero-offset error";
    case 20:return ru?"Ошибка нулевого смещения PV":"PV zero-offset error";
    case 21:return ru?"Низкое напряжение инвертора":"Inverter undervoltage";
    case 22:return ru?"Защита от отрицательной мощности":"Negative power protection";
    case 23:return ru?"Потеря ведущего устройства":"Master device lost";
    case 24:return ru?"Ошибка синхронизации":"Synchronization error";
    case 26:return ru?"Несовместимые версии":"Incompatible versions";
    default:return ru?"Неизвестная авария":"Unknown fault";
  }
}

inline const char* warningName(uint8_t bit,bool ru=true){
  switch(bit){
    case 0:return ru?"Потеря перехода через ноль":"Zero crossing lost";
    case 1:return ru?"Ненормальная форма сети":"Abnormal grid waveform";
    case 2:return ru?"Перенапряжение сети":"Grid overvoltage";
    case 3:return ru?"Низкое напряжение сети":"Grid undervoltage";
    case 4:return ru?"Высокая частота сети":"Grid frequency too high";
    case 5:return ru?"Низкая частота сети":"Grid frequency too low";
    case 6:return ru?"Низкое напряжение PV":"PV undervoltage";
    case 7:return ru?"Перегрев":"Overtemperature";
    case 8:return ru?"Низкое напряжение батареи":"Battery undervoltage";
    case 9:return ru?"Батарея не подключена":"Battery disconnected";
    case 10:return ru?"Перегрузка":"Overload";
    case 11:return ru?"Уравнительный заряд":"Equalization charge";
    case 12:return ru?"Глубокий разряд батареи":"Deep battery discharge";
    case 13:return ru?"Ограничение мощности":"Power limiting";
    case 14:return ru?"Заблокирован вентилятор":"Fan blocked";
    case 15:return ru?"Недостаточно энергии PV":"Insufficient PV energy";
    case 16:return ru?"Прерывание параллельной связи":"Parallel communication interrupted";
    case 17:return ru?"Несовместимый режим":"Incompatible mode";
    case 18:return ru?"Разница напряжений батарей":"Battery voltage difference";
    default:return ru?"Неизвестное предупреждение":"Unknown warning";
  }
}
#endif