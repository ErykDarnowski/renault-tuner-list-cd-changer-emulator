#include <TLCDCEmu.h>

TLCDCEmu emulator;
char btName[] = "Renault Megane II";

void setup(){
  Serial.begin(115200);
  delay(10);
  Serial.println("[APP] TLCDCEmu starting");
  emulator.init(btName);
  Serial.println("[APP] init done, waiting for radio and phone");
}

void loop(){
  emulator.talk();
}