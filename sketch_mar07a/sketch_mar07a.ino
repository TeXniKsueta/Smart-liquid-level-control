#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <GyverDS18.h>
#include <ModbusMaster.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------- ПИНЫ ----------------
#define BTN_PIN      A0
#define TURB_PIN     A1
#define POT_PIN      A2
#define DS_PIN       8

#define MAX485_DE_RE 2

#define LED_WORK     9
#define LED_EMERG    10

#define PUMP_FILL_PIN   5
#define PUMP_DRAIN_PIN  6

// ---------------- КНОПКИ ----------------
#define KEY_NONE    0
#define KEY_SELECT  1
#define KEY_CONFIRM 2
#define KEY_BTN3    3   // ручной залив
#define KEY_BTN4    4   // ручной слив
#define KEY_EMERG   5

// ---------------- СОСТОЯНИЯ ----------------
bool isAuto = true;
bool modeConfirmed = false;
bool settingUstavka = false;
bool isEmergency = false;

// ---------------- ПАРАМЕТРЫ ----------------
int ustavka = 0;
int lastShownUstavka = -1;
float hysteresis = 0.5;

// ---------------- РУЧНОЕ УПРАВЛЕНИЕ ----------------
bool manualFillOn = false;
bool manualDrainOn = false;

// ---------------- ДАТЧИКИ ----------------
GyverDS18Single ds(DS_PIN);
ModbusMaster node;

float temperature = 0.0;
int turbidityRaw = 0;
int turbidityPercent = 0;
float waterLevel = 0.0;

// ---------------- ТАЙМЕРЫ ----------------
unsigned long tempRequestTime = 0;
unsigned long lastTempCycle = 0;
bool tempWaiting = false;

unsigned long lastTurbidityRead = 0;
unsigned long lastLevelRead = 0;

// ---------------- LCD ----------------
bool screenDirty = true;

// ----------------------------------------------------
// RS485
// ----------------------------------------------------
void preTransmission() {
  digitalWrite(MAX485_DE_RE, HIGH);
}

void postTransmission() {
  digitalWrite(MAX485_DE_RE, LOW);
}

// ----------------------------------------------------
// СВЕТОДИОДЫ
// ----------------------------------------------------
void updateLeds() {
  if (isEmergency) {
    digitalWrite(LED_WORK, LOW);
    digitalWrite(LED_EMERG, HIGH);
  } else {
    digitalWrite(LED_WORK, HIGH);
    digitalWrite(LED_EMERG, LOW);
  }
}

// ----------------------------------------------------
// НАСОСЫ / РЕЛЕ
// LOW = включено
// ----------------------------------------------------
void pumpsOff() {
  digitalWrite(PUMP_FILL_PIN, HIGH);
  digitalWrite(PUMP_DRAIN_PIN, HIGH);
}

void pumpFillOn() {
  digitalWrite(PUMP_FILL_PIN, LOW);
  digitalWrite(PUMP_DRAIN_PIN, HIGH);
}

void pumpDrainOn() {
  digitalWrite(PUMP_FILL_PIN, HIGH);
  digitalWrite(PUMP_DRAIN_PIN, LOW);
}

void resetManualPumps() {
  manualFillOn = false;
  manualDrainOn = false;
  pumpsOff();
}

void applyManualPumps() {
  if (manualFillOn) {
    pumpFillOn();
  } else if (manualDrainOn) {
    pumpDrainOn();
  } else {
    pumpsOff();
  }
}

void controlPumpsAuto() {
  float upperLimit = ustavka + hysteresis;
  float lowerLimit = ustavka - hysteresis;

  if (waterLevel < lowerLimit) {
    pumpFillOn();
  } else if (waterLevel > upperLimit) {
    pumpDrainOn();
  } else {
    pumpsOff();
  }
}

void controlPumps() {
  if (isEmergency || !modeConfirmed || settingUstavka) {
    pumpsOff();
    return;
  }

  if (isAuto) {
    controlPumpsAuto();
  } else {
    applyManualPumps();
  }
}

// ----------------------------------------------------
// ПРЕОБРАЗОВАНИЯ
// ----------------------------------------------------
int mapTurbidity(int raw) {
  raw = constrain(raw, 200, 800);
  return map(raw, 800, 200, 0, 100);
}

int mapUstavka(int val) {
  return map(val, 0, 1023, 0, 150);
}

// ----------------------------------------------------
// КНОПКИ
// ----------------------------------------------------
int DecodeKey(int adc) {
  // покой: 10..20
  // кнопки: 569, 638, 728, 847, 1013
  if (adc < 300) return KEY_NONE;
  if (adc < 603) return KEY_SELECT;
  if (adc < 683) return KEY_CONFIRM;
  if (adc < 787) return KEY_BTN3;
  if (adc < 930) return KEY_BTN4;
  return KEY_EMERG;
}

int GetKeyValue() {
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(BTN_PIN);
  }
  int adc = sum / 10;
  return DecodeKey(adc);
}

int getButtonEvent() {
  static int lastStable = KEY_NONE;
  static int lastReading = KEY_NONE;
  static unsigned long lastDebounceTime = 0;

  int reading = GetKeyValue();

  if (reading != lastReading) {
    lastDebounceTime = millis();
    lastReading = reading;
  }

  if ((millis() - lastDebounceTime) > 40) {
    if (reading != lastStable) {
      lastStable = reading;
      if (lastStable != KEY_NONE) {
        return lastStable;
      }
    }
  }

  return KEY_NONE;
}

// ----------------------------------------------------
// ЭКРАНЫ
// ----------------------------------------------------
void drawModeMenu() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Select mode:");
  lcd.setCursor(0, 1);
  if (isAuto) lcd.print(">Auto  Manual");
  else        lcd.print(" Auto >Manual");
}

void drawUstavka(int val) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Ustavka:");
  lcd.setCursor(0, 1);
  lcd.print("Value: ");
  lcd.print(val);
  lcd.print("   ");
}

void drawStatus() {
  lcd.clear();

  lcd.setCursor(0, 0);
  if (isAuto) lcd.print("A ");
  else        lcd.print("M ");

  lcd.print("L:");
  lcd.print(waterLevel, 1);

  lcd.setCursor(10, 0);
  lcd.print("U:");
  lcd.print(ustavka);

  lcd.setCursor(0, 1);
  lcd.print("T:");
  lcd.print(temperature, 1);

  lcd.setCursor(8, 1);
  lcd.print("Tb:");
  lcd.print(turbidityPercent);
  lcd.print("%");
}

void drawEmergency() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("!!! EMERGENCY");
  lcd.setCursor(0, 1);
  lcd.print("System stopped");
}

// ----------------------------------------------------
// ОБНОВЛЕНИЕ LCD
// ----------------------------------------------------
void updateScreen() {
  if (!screenDirty) return;

  if (isEmergency) {
    drawEmergency();
  } else if (!modeConfirmed) {
    drawModeMenu();
  } else if (settingUstavka) {
    drawUstavka(mapUstavka(analogRead(POT_PIN)));
  } else {
    drawStatus();
  }

  screenDirty = false;
}

// ----------------------------------------------------
// КНОПОЧНАЯ ЛОГИКА
// ----------------------------------------------------
void processButton(int key) {
  if (key == KEY_EMERG) {
    isEmergency = true;
    resetManualPumps();
    screenDirty = true;
    updateLeds();
    return;
  }

  if (isEmergency) return;

  if (key == KEY_SELECT) {
    if (!modeConfirmed) {
      isAuto = !isAuto;
    } else {
      modeConfirmed = false;
      settingUstavka = false;
      resetManualPumps();
    }
    screenDirty = true;
    return;
  }

  if (key == KEY_CONFIRM) {
    if (!modeConfirmed) {
      modeConfirmed = true;

      if (isAuto) {
        settingUstavka = true;
        lastShownUstavka = -1;
      } else {
        settingUstavka = false;
        resetManualPumps();
      }

      screenDirty = true;
      return;
    }

    if (isAuto && settingUstavka) {
      ustavka = mapUstavka(analogRead(POT_PIN));
      settingUstavka = false;
      screenDirty = true;
      return;
    }
  }

  // Ручное управление только в подтвержденном ручном режиме
  if (modeConfirmed && !settingUstavka && !isAuto) {
    if (key == KEY_BTN3) {
      // Кнопка 3 управляет заливом
      manualFillOn = !manualFillOn;
      if (manualFillOn) {
        manualDrainOn = false;   // одновременно нельзя
      }
      controlPumps();
      screenDirty = true;
      return;
    }

    if (key == KEY_BTN4) {
      // Кнопка 4 управляет сливом
      manualDrainOn = !manualDrainOn;
      if (manualDrainOn) {
        manualFillOn = false;    // одновременно нельзя
      }
      controlPumps();
      screenDirty = true;
      return;
    }
  }
}

// ----------------------------------------------------
// ПОТЕНЦИОМЕТР
// ----------------------------------------------------
void handlePot() {
  if (!settingUstavka || isEmergency) return;

  int currentUstavka = mapUstavka(analogRead(POT_PIN));

  if (currentUstavka != lastShownUstavka) {
    lastShownUstavka = currentUstavka;
    screenDirty = true;
  }
}

// ----------------------------------------------------
// ТЕМПЕРАТУРА
// ----------------------------------------------------
void handleTemperature() {
  unsigned long now = millis();

  if (!tempWaiting && (now - lastTempCycle > 3000)) {
    ds.requestTemp();
    tempRequestTime = now;
    tempWaiting = true;
    lastTempCycle = now;
  }

  if (tempWaiting && (now - tempRequestTime > 750)) {
    ds.readTemp();
    temperature = ds.getTemp();
    tempWaiting = false;

    if (modeConfirmed && !settingUstavka && !isEmergency) {
      screenDirty = true;
    }
  }
}

// ----------------------------------------------------
// МУТНОСТЬ
// ----------------------------------------------------
void handleTurbidity() {
  if (millis() - lastTurbidityRead > 500) {
    lastTurbidityRead = millis();

    long sum = 0;
    for (int i = 0; i < 10; i++) {
      sum += analogRead(TURB_PIN);
    }

    turbidityRaw = sum / 10;
    turbidityPercent = mapTurbidity(turbidityRaw);

    if (modeConfirmed && !settingUstavka && !isEmergency) {
      screenDirty = true;
    }
  }
}

// ----------------------------------------------------
// УРОВЕНЬ MODBUS
// ----------------------------------------------------
void handleLevel() {
  if (!modeConfirmed || settingUstavka || isEmergency) return;

  if (millis() - lastLevelRead > 1000) {
    lastLevelRead = millis();

    uint8_t result = node.readHoldingRegisters(0x0000, 6);

    if (result == node.ku8MBSuccess) {
      waterLevel = node.getResponseBuffer(4) / 10.0;
      screenDirty = true;
    }
  }
}

// ----------------------------------------------------
// SETUP / LOOP
// ----------------------------------------------------
void setup() {
  lcd.init();
  lcd.backlight();

  pinMode(MAX485_DE_RE, OUTPUT);
  digitalWrite(MAX485_DE_RE, LOW);

  pinMode(LED_WORK, OUTPUT);
  pinMode(LED_EMERG, OUTPUT);

  pinMode(PUMP_FILL_PIN, OUTPUT);
  pinMode(PUMP_DRAIN_PIN, OUTPUT);
  pumpsOff();

  updateLeds();

  Serial.begin(9600);
  node.begin(1, Serial);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  ds.requestTemp();
  tempRequestTime = millis();
  tempWaiting = true;
  lastTempCycle = millis();

  screenDirty = true;
  updateScreen();
}

void loop() {
  int key = getButtonEvent();
  if (key != KEY_NONE) {
    processButton(key);
  }

  handlePot();
  handleTemperature();
  handleTurbidity();
  handleLevel();

  controlPumps();
  updateLeds();
  updateScreen();
}
