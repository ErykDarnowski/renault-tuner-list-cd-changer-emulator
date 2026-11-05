/*
  RadioSim - simulates a Renault Tuner List head unit so the ESP32
  TLCDCEmu changer emulator can be tested on the bench, without the car.

  Wiring (to the changer-connector side of your emulator board):
    Uno D2  <- connector pin 14  (emulator -> radio, Q1 drain)
    Uno D3  -> connector pin 13  (radio -> emulator, Q2 gate)
    Uno GND -- connector pin 15  / ESP32 GND
  Power the ESP32 from USB. Do NOT feed pins 16/17 from the Uno.

  Line format: 9600 baud, 8 data bits, even parity, 1 stop bit.
  The bits are bit-banged so the USB serial port stays free for the log
  (open the Serial Monitor at 115200 baud).

  LINE_INVERTED 1 = line idles LOW (what the emulator's MOSFET stage
  produces, and what the firmware assumes the radio uses).
  If every byte shows a parity/framing error, try 0.
*/

#include <Arduino.h>

#define RX_PIN        2      // must be D2 (INT0)
#define TX_PIN        3
#define LINE_INVERTED 1
#define BIT_US        104    // 9600 baud

// Placeholder "head unit version" frame. The emulator only checks that the
// length is 7 and the first byte is 0x31; the other bytes are a guess.
const uint8_t HU_VERSION[7]  = {0x31, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t CMD_ENTER[2]   = {0x2C, 0xFF};
const uint8_t CMD_EXIT[2]    = {0x2C, 0x00};
const uint8_t CMD_PLAY[1]    = {0x13};
const uint8_t CMD_PAUSE[1]   = {0x1C};
const uint8_t CMD_STANDBY[1] = {0x19};
const uint8_t CMD_CD[1]      = {0x24};
const uint8_t CMD_NEXT[2]    = {0x17, 0x01};
const uint8_t CMD_PREV[3]    = {0x22, 0x01, 0x02};

// ---------------------------------------------------------------- line I/O

#define RXBUF 64
volatile uint8_t rxData[RXBUF];
volatile uint8_t rxFlag[RXBUF];
volatile uint8_t rxHead = 0, rxTail = 0;

static inline uint8_t lineLevel() {          // logical level, 1 = idle/mark
  uint8_t raw = (PIND & _BV(PD2)) ? 1 : 0;
#if LINE_INVERTED
  return raw ^ 1;
#else
  return raw;
#endif
}

static inline void setLine(uint8_t logical) {
#if LINE_INVERTED
  logical ^= 1;
#endif
  if (logical) PORTD |= _BV(PD3); else PORTD &= ~_BV(PD3);
}

// Fires on the leading edge of a start bit and samples the whole byte.
void rxIsr() {
  delayMicroseconds(BIT_US / 2 - 8);
  if (lineLevel() != 0) { EIFR = _BV(INTF0); return; }   // glitch
  uint8_t b = 0, ones = 0;
  for (uint8_t i = 0; i < 8; i++) {
    delayMicroseconds(BIT_US - 2);
    uint8_t bit = lineLevel();
    b |= bit << i;
    ones += bit;
  }
  delayMicroseconds(BIT_US - 2);
  uint8_t parity = lineLevel();
  delayMicroseconds(BIT_US - 2);
  uint8_t stop = lineLevel();

  uint8_t flags = 0;
  if ((ones + parity) & 1) flags |= 1;   // parity error
  if (!stop)               flags |= 2;   // framing error

  uint8_t next = (rxHead + 1) % RXBUF;
  if (next != rxTail) { rxData[rxHead] = b; rxFlag[rxHead] = flags; rxHead = next; }
  EIFR = _BV(INTF0);                     // drop edges seen inside this byte
}

void txByte(uint8_t b) {
  uint8_t ones = 0;
  noInterrupts();
  setLine(0); delayMicroseconds(BIT_US);                 // start
  for (uint8_t i = 0; i < 8; i++) {
    uint8_t bit = (b >> i) & 1;
    ones += bit;
    setLine(bit); delayMicroseconds(BIT_US);
  }
  setLine(ones & 1); delayMicroseconds(BIT_US);          // even parity
  setLine(1);        delayMicroseconds(BIT_US);          // stop
  interrupts();
}

// ---------------------------------------------------------------- logging

void printHex(uint8_t b) {
  if (b < 0x10) Serial.print('0');
  Serial.print(b, HEX);
}

void printBytes(const uint8_t *d, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) { printHex(d[i]); Serial.print(' '); }
}

void printName(const uint8_t *p, uint8_t n) {
  switch (p[0]) {
    case 0x11: Serial.print(F("boot: changer announces itself")); break;
    case 0x15: Serial.print(F("boot 1")); break;
    case 0x20:
      if (n > 1 && p[1] == 0x05)      Serial.print(F("status: preparing play"));
      else if (n > 1 && p[1] == 0x03) Serial.print(F("status: paused"));
      else                            Serial.print(F("boot 2 / status: standby"));
      break;
    case 0x25: Serial.print(F("boot 3")); break;
    case 0x26: Serial.print(F("boot 4")); break;
    case 0x62: Serial.print(F("confirms HU version")); break;
    case 0x21: Serial.print(F("confirms command")); break;
    case 0x27: Serial.print(F("track info")); break;
    case 0x47: Serial.print(F("playing: disc/track/time")); break;
    default:   Serial.print(F("?")); break;
  }
}

// ---------------------------------------------------------------- protocol

uint8_t frame[40];
uint8_t fLen = 0;
unsigned long lastByteMs = 0;
bool gotAck = false;
bool pendingVersion = false;
bool synced = false;   // like a freshly started radio: ignore frames until a boot frame
uint8_t hostSeq = 0;
unsigned long bytesIn = 0, badBytes = 0;

void processFrame() {
  uint8_t n = frame[2];
  uint8_t x = 0;
  for (uint8_t i = 0; i < n + 3; i++) x ^= frame[i];

  Serial.print(F("ESP> "));
  printBytes(frame, n + 4);
  Serial.print(F(" ["));
  printName(frame + 3, n);
  Serial.print(']');
  if (x != frame[n + 3]) {
    Serial.print(F("  BAD CHECKSUM, expected "));
    printHex(x);
    Serial.println();
    return;
  }
  Serial.println();

  if (frame[3] == 0x11) synced = true;
  if (!synced) {
    Serial.println(F("     ignored: radio just started, waiting for a boot frame"));
    return;
  }

  delay(2);
  txByte(0xC5);
  Serial.println(F("UNO> C5 (ack)"));

  if (frame[3] == 0x26) pendingVersion = true;
  if (frame[3] == 0x62)
    Serial.println(F("*** Handshake complete: a real radio would now offer the changer ***"));
}

void handleByte(uint8_t b, uint8_t flags) {
  bytesIn++;
  lastByteMs = millis();
  if (flags) {
    badBytes++;
    Serial.print(F("     [byte "));
    printHex(b);
    Serial.print(flags & 1 ? F(" parity err") : F(""));
    Serial.print(flags & 2 ? F(" framing err") : F(""));
    Serial.println(']');
  }
  if (fLen == 0) {
    if (b == 0xC5) { gotAck = true; Serial.println(F("ESP> C5 (ack)")); return; }
    if (b != 0x3D) { Serial.print(F("ESP> stray byte ")); printHex(b); Serial.println(); return; }
  }
  frame[fLen++] = b;
  if (fLen >= 3 && fLen == frame[2] + 4) { processFrame(); fLen = 0; }
  else if (fLen >= sizeof(frame)) fLen = 0;
}

void pump() {
  while (rxTail != rxHead) {
    uint8_t b = rxData[rxTail], fl = rxFlag[rxTail];
    rxTail = (rxTail + 1) % RXBUF;
    handleByte(b, fl);
  }
  if (fLen && millis() - lastByteMs > 30) {
    Serial.print(F("ESP> incomplete frame: "));
    printBytes(frame, fLen);
    Serial.println();
    fLen = 0;
  }
}

bool sendFrame(const uint8_t *payload, uint8_t n, const __FlashStringHelper *label) {
  uint8_t f[32];
  f[0] = 0x3D; f[1] = hostSeq; f[2] = n;
  memcpy(f + 3, payload, n);
  uint8_t x = 0;
  for (uint8_t i = 0; i < n + 3; i++) x ^= f[i];
  f[n + 3] = x;

  for (uint8_t attempt = 1; attempt <= 3; attempt++) {
    Serial.print(F("UNO> "));
    printBytes(f, n + 4);
    Serial.print(F(" ["));
    Serial.print(label);
    Serial.println(']');
    gotAck = false;
    for (uint8_t i = 0; i < n + 4; i++) txByte(f[i]);
    unsigned long t = millis();
    while (millis() - t < 250) {
      pump();
      if (gotAck) { hostSeq++; return true; }
    }
    Serial.println(F("     no C5 from emulator, retrying"));
  }
  Serial.println(F("     gave up: emulator never acknowledged"));
  return false;
}

void help() {
  Serial.println(F("RadioSim ready. The handshake runs by itself; commands:"));
  Serial.println(F("  v version   e enter CDC   q exit CDC"));
  Serial.println(F("  p play      z pause       s standby"));
  Serial.println(F("  n next      b prev        c CD change   h help"));
  Serial.println(F("  r simulate a radio restart (stop answering until a boot frame)"));
}

// ---------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);
  pinMode(RX_PIN, INPUT_PULLUP);       // stands in for the radio's pull-up
  pinMode(TX_PIN, OUTPUT);
  setLine(1);                          // idle
  attachInterrupt(digitalPinToInterrupt(RX_PIN), rxIsr, LINE_INVERTED ? RISING : FALLING);
  help();
}

void loop() {
  pump();

  if (pendingVersion) {
    pendingVersion = false;
    delay(20);
    sendFrame(HU_VERSION, sizeof(HU_VERSION), F("HU version"));
  }

  if (Serial.available()) {
    switch (Serial.read()) {
      case 'v': sendFrame(HU_VERSION, sizeof(HU_VERSION), F("HU version")); break;
      case 'e': sendFrame(CMD_ENTER, sizeof(CMD_ENTER), F("enter CDC mode")); break;
      case 'q': sendFrame(CMD_EXIT, sizeof(CMD_EXIT), F("exit CDC mode")); break;
      case 'p': sendFrame(CMD_PLAY, sizeof(CMD_PLAY), F("play")); break;
      case 'z': sendFrame(CMD_PAUSE, sizeof(CMD_PAUSE), F("pause")); break;
      case 's': sendFrame(CMD_STANDBY, sizeof(CMD_STANDBY), F("standby")); break;
      case 'c': sendFrame(CMD_CD, sizeof(CMD_CD), F("CD change")); break;
      case 'n': sendFrame(CMD_NEXT, sizeof(CMD_NEXT), F("next")); break;
      case 'b': sendFrame(CMD_PREV, sizeof(CMD_PREV), F("prev")); break;
      case 'h': help(); break;
      case 'r':
        synced = false;
        pendingVersion = false;
        hostSeq = 0;
        Serial.println(F("*** simulated radio restart ***"));
        break;
    }
  }

  // Warn if the line sits in the "start bit" state: usually means wrong
  // polarity, a missing ground, or pin 14 not connected.
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 3000) {
    lastCheck = millis();
    if (lineLevel() == 0 && millis() - lastByteMs > 3000)
      Serial.println(F("!! D2 is stuck in the active state: check ground, pin 14 wiring, or flip LINE_INVERTED"));
    if (bytesIn == 0)
      Serial.println(F(".. nothing received from the emulator yet"));
  }
}
