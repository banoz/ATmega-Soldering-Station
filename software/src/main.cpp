// SolderingStation2
//
// ATmega328-controlled Soldering Station for WMRT Tips.
//
// This version of the code implements:
// - Temperature measurement of the tip
// - Direct or PID control of the heater
// - Temperature control via rotary encoder
// - Boost mode by short pressing rotary encoder switch
// - Setup menu by long pressing rotary encoder switch
// - Handle movement detection (by checking ball switch)
// - Iron unconnected detection (by idenfying invalid temperature readings)
// - Time driven sleep/power off mode if iron is unused (movement detection)
// - Measurement of input voltage, Vcc and ATmega's internal temperature
// - Information display on OLED
// - Buzzer
// - Calibrating and managing different soldering tips
// - Storing user settings into the EEPROM
// - Tip change detection
// - Can be used with either N or P channel mosfets
// - Screen flip support
// - Rotary encoder reverse support
//
// Power supply should be in the range of 16V/2A to 24V/3A and well
// stabilized.
//
// For calibration you need a soldering iron tips thermometer. For best results
// wait at least three minutes after switching on the soldering station before
// you start the calibration process.
//
// Controller:  ATmega328p
// Core:        Barebones ATmega (https://github.com/carlosefr/atmega)
// Clockspeed:  16 MHz external
//
// It is recommended not to use a bootloader!
//
// Thank you for the code contributions:
// - John Glavinos, https://youtu.be/4YDcWfOQmz4
// - createskyblue, https://github.com/createskyblue
// - TaaraLabs, https://github.com/TaaraLabs
// - muink, https://github.com/muink
//
// 2023 by Vas
// Project Files (Github):  https://github.com/banoz/ATmega-Soldering-Station
//
// 2019 - 2022 by Stefan Wagner
// Project Files (EasyEDA): https://easyeda.com/wagiminator
// Project Files (Github):  https://github.com/wagiminator
// License: http://creativecommons.org/licenses/by-sa/3.0/

#include <Arduino.h>

// Libraries
#include <U8g2lib.h>   // https://github.com/olikraus/u8glib
#include <PID_v1.h>    // https://github.com/wagiminator/ATmega-Soldering-Station/blob/master/software/libraries/Arduino-PID-Library.zip
                       // (old cpp version of https://github.com/mblythe86/C-PID-Library/tree/master/PID_v1)
#include <EEPROM.h>    // for storing user settings into EEPROM
#include <avr/sleep.h> // for sleeping during ADC sampling

// Firmware version
#define VERSION "v2.0"

// Type of MOSFET
#define N_MOSFET // P_MOSFET or N_MOSFET

// Type of OLED Controller
#define SSD1306 // SSD1306 or SH1106

// Type of rotary encoder
#define ROTARY_TYPE 1 // 0: 2 increments/step; 1: 4 increments/step (default)

// Pins
#define SENSOR_PIN A0  // tip temperature sense
#define VIN_PIN A1     // input voltage sense
#define COLDJ_PIN A6   // cold junction temperature sense
#define BUZZER_PIN 5   // buzzer
#define BUTTON_PIN 6   // rotary encoder switch
#define ROTARY_1_PIN 7 // rotary encoder 1
#define ROTARY_2_PIN 8 // rotary encoder 2
#define CONTROL_PIN 9  // heater MOSFET PWM control
#define REED_PIN A3    // handle reed switch
#define REED_PU_PIN 4  // handle reed switch pull-up

// Default temperature control values (recommended soldering temperature: 300-380°C)
#define TEMP_MIN 150     // min selectable temperature
#define TEMP_MAX 400     // max selectable temperature
#define TEMP_DEFAULT 320 // default start setpoint
#define TEMP_SLEEP 150   // temperature in sleep mode
#define TEMP_BOOST 50    // temperature increase in boost mode
#define TEMP_STEP 10     // rotary encoder temp change steps

// Default tip temperature calibration values
#define TEMP200 216     // temperature at ADC = 200
#define TEMP280 308     // temperature at ADC = 280
#define TEMP360 390     // temperature at ADC = 360
#define TEMPCHP 30      // chip temperature while calibration
#define TIPMAX 8        // max number of tips
#define TIPNAMELENGTH 6 // max length of tip names (including termination)
#define TIPNAME "WMRT"  // default tip name

// Default timer values (0 = disabled)
#define TIME2SLEEP 5   // time to enter sleep mode in minutes
#define TIME2OFF 15    // time to shut off heater in minutes
#define TIMEOFBOOST 40 // time to stay in boost mode in seconds

// Control values
#define TIME2SETTLE 950  // time in microseconds to allow OpAmp output to settle
#define SMOOTHIE 0.05    // OpAmp output smooth factor (1=no smoothing; 0.05 default)
#define PID_ENABLE false // enable PID control
#define BEEP_ENABLE true // enable/disable buzzer
#define BODYFLIP false   // enable/disable screen flip
#define ECREVERSE false  // enable/disable rotary encoder reverse
#define MAINSCREEN 1     // type of main screen (0: big numbers; 1: more infos)

// EEPROM identifier
#define EEPROM_IDENT 0xE76C // to identify if EEPROM was written by this program

// MOSFET control definitions
#if defined(P_MOSFET) // P-Channel MOSFET
#define HEATER_ON 255
#define HEATER_OFF 0
#define HEATER_PWM Output
#elif defined(N_MOSFET) // N-Channel MOSFET
#define HEATER_ON 0
#define HEATER_OFF 255
#define HEATER_PWM 255 - Output
#else
#error Wrong MOSFET type!
#endif

// Define the aggressive and conservative PID tuning parameters
double aggKp = 11, aggKi = 0.5, aggKd = 1;
double consKp = 11, consKi = 3, consKd = 5;

// Default values that can be changed by the user and stored in the EEPROM
uint16_t DefaultTemp = TEMP_DEFAULT;
uint16_t SleepTemp = TEMP_SLEEP;
uint8_t BoostTemp = TEMP_BOOST;
uint8_t time2sleep = TIME2SLEEP;
uint8_t time2off = TIME2OFF;
uint8_t timeOfBoost = TIMEOFBOOST;
uint8_t MainScrType = MAINSCREEN;
bool PIDenable = PID_ENABLE;
bool beepEnable = BEEP_ENABLE;
bool BodyFlip = BODYFLIP;
bool ECReverse = ECREVERSE;

// Default values for tips
uint16_t CalTemp[TIPMAX][4] = {TEMP200, TEMP280, TEMP360, TEMPCHP};
char TipName[TIPMAX][TIPNAMELENGTH] = {TIPNAME};
uint8_t CurrentTip = 0;
uint8_t NumberOfTips = 1;

// Menu items
const char *SetupItems[] = {"Setup Menu", "Tip Settings", "Temp Settings",
                            "Timer Settings", "Control Type", "Main Screen",
                            "Buzzer", "Screen Flip", "EC Reverse", "Information", "Return"};
const char *TipItems[] = {"Tip:", "Change Tip", "Calibrate Tip",
                          "Rename Tip", "Delete Tip", "Add new Tip", "Return"};
const char *TempItems[] = {"Temp Settings", "Default Temp", "Sleep Temp",
                           "Boost Temp", "Return"};
const char *TimerItems[] = {"Timer Settings", "Sleep Timer", "Off Timer",
                            "Boost Timer", "Return"};
const char *ControlTypeItems[] = {"Control Type", "Direct", "PID"};
const char *MainScreenItems[] = {"Main Screen", "Big Numbers", "More Infos"};
const char *StoreItems[] = {"Store Settings ?", "No", "Yes"};
const char *SureItems[] = {"Are you sure ?", "No", "Yes"};
const char *BuzzerItems[] = {"Buzzer", "Disable", "Enable"};
const char *FlipItems[] = {"Screen Flip", "Disable", "Enable"};
const char *ECReverseItems[] = {"EC Reverse", "Disable", "Enable"};
const char *DefaultTempItems[] = {"Default Temp", "\xB0"
                                                  "C"};
const char *SleepTempItems[] = {"Sleep Temp", "\xB0"
                                              "C"};
const char *BoostTempItems[] = {"Boost Temp", "\xB0"
                                              "C"};
const char *SleepTimerItems[] = {"Sleep Timer", "Minutes"};
const char *OffTimerItems[] = {"Off Timer", "Minutes"};
const char *BoostTimerItems[] = {"Boost Timer", "Seconds"};
const char *DeleteMessage[] = {"Warning", "You cannot", "delete your", "last tip!"};
const char *MaxTipMessage[] = {"Warning", "You reached", "maximum number", "of tips!"};

// Variables for pin change interrupt
volatile uint8_t a0, b0, c0, d0;
volatile bool ab0;
volatile int count, countMin, countMax, countStep;

// Variables for temperature control
uint16_t SetTemp, ShowTemp, Step;
double Input, Output, Setpoint, RawTemp, CurrentTemp, ChipTemp, CJTemp;

// Variables for voltage readings
uint16_t Vcc, Vin;

// State variables
volatile bool inSleepMode = false;
volatile bool inOffMode = false;
volatile bool inBoostMode = false;
volatile bool inCalibMode = false;
volatile bool isWorky = true;
volatile bool beepIfWorky = true;
volatile bool TipIsPresent = true;

// Timing variables
uint32_t sleepmillis;
uint32_t boostmillis;
uint32_t buttonmillis;
uint8_t goneMinutes;
uint8_t goneSeconds;
uint8_t SensorCounter = 255;

// Specify variable pointers and initial PID tuning parameters
PID ctrl(&Input, &Output, &Setpoint, aggKp, aggKi, aggKd, P_ON_E, DIRECT);

// Setup u8g object depending on OLED controller
#if defined(SSD1306)
U8G2_SSD1306_128X64_NONAME_1_HW_I2C u8g(U8G2_R0, 255, 5, 4); // U8G_I2C_OPT_DEV_0|U8G_I2C_OPT_NO_ACK|U8G_I2C_OPT_FAST);
#elif defined(SH1106)
U8GLIB_SH1106_128X64 u8g(U8G_I2C_OPT_FAST | U8G_I2C_OPT_NO_ACK);
#else
#error Wrong OLED controller type!
#endif

void AddTipScreen();
void beep();
void calculateTemp();
void CalibrationScreen();
void ChangeTipScreen();
void DeleteTipScreen();
uint16_t denoiseAnalog(byte);
double getChipTemp();
uint16_t getColdJ();
void getEEPROM();
int getRotary();
uint16_t getVCC();
uint16_t getVIN();
void InfoScreen();
void InputNameScreen();
uint16_t InputScreen(const char **);
void MainScreen();
uint8_t MenuScreen(const char **, uint8_t, uint8_t);
void MessageScreen(const char **, uint8_t);
void ROTARYCheck();
void SENSORCheck();
void SetFlip();
void setRotary(int, int, int, int);
void SetupScreen();
void SLEEPCheck();
void TempScreen();
void Thermostat();
void TimerScreen();
void TipScreen();
void updateEEPROM();

void setup()
{
  // set the pin modes
  pinMode(SENSOR_PIN, INPUT);
  pinMode(VIN_PIN, INPUT);
  pinMode(COLDJ_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(CONTROL_PIN, OUTPUT);
  pinMode(ROTARY_1_PIN, INPUT);
  pinMode(ROTARY_2_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT);
  pinMode(REED_PIN, INPUT);
  pinMode(REED_PU_PIN, INPUT);

  analogWrite(CONTROL_PIN, HEATER_OFF); // this shuts off the heater
  digitalWrite(BUZZER_PIN, LOW);        // must be LOW when buzzer not in use

  // setup ADC
  ADCSRA |= bit(ADPS0) | bit(ADPS1) | bit(ADPS2); // set ADC prescaler to 128
  ADCSRA |= bit(ADIE);                            // enable ADC interrupt
  interrupts();                                   // enable global interrupts

  // setup pin change interrupt for rotary encoder
  PCMSK0 = bit(PCINT0); // Configure pin change interrupt on Pin8
  PCICR = bit(PCIE0);   // Enable pin change interrupt
  PCIFR = bit(PCIF0);   // Clear interrupt flag

  // prepare and start OLED
  u8g.begin();

  // get default values from EEPROM
  getEEPROM();

  // set screen flip
  SetFlip();

  // read supply voltages in mV
  Vcc = getVCC();
  Vin = getVIN();
  CJTemp = getColdJ();

  // read and set current iron temperature
  SetTemp = DefaultTemp;
  RawTemp = denoiseAnalog(SENSOR_PIN);
  ChipTemp = getChipTemp();
  calculateTemp();

  // set PID output range and start the PID
  ctrl.SetOutputLimits(0, 255);
  ctrl.SetMode(AUTOMATIC);

  // set initial rotary encoder values
  a0 = PINB & 1;
  b0 = PIND >> 7 & 1;
  ab0 = (a0 == b0);
  setRotary(TEMP_MIN, TEMP_MAX, TEMP_STEP, DefaultTemp);

  // reset sleep timer
  sleepmillis = millis();

  // long beep for setup completion
  beep();
  beep();
}

void loop()
{
  ROTARYCheck(); // check rotary encoder (temp/boost setting, enter setup menu)
  SENSORCheck(); // reads temperature and vibration switch of the iron
  Thermostat();  // heater control
  MainScreen();  // updates the main page on the OLED
}

// check rotary encoder; set temperature, toggle boost mode, enter setup menu accordingly
void ROTARYCheck()
{
  // set working temperature according to rotary encoder value
  SetTemp = getRotary();

  // check rotary encoder switch
  uint8_t c = digitalRead(BUTTON_PIN);
  if (!c && c0)
  {
    beep();
    buttonmillis = millis();
    while ((!digitalRead(BUTTON_PIN)) && ((millis() - buttonmillis) < 500))
      ;
    if ((millis() - buttonmillis) >= 500)
      SetupScreen();
    else
    {
      inBoostMode = !inBoostMode;
      if (inBoostMode)
        boostmillis = millis();
    }
  }
  c0 = c;

  // check timer when in boost mode
  if (inBoostMode && timeOfBoost)
  {
    goneSeconds = (millis() - boostmillis) / 1000;
    if (goneSeconds >= timeOfBoost)
    {
      inBoostMode = false; // stop boost mode
      beep();              // beep if boost mode is over
      beepIfWorky = true;  // beep again when working temperature is reached
    }
  }
}

// check and activate/deactivate sleep modes
void SLEEPCheck()
{
  pinMode(REED_PU_PIN, OUTPUT);
  digitalWrite(REED_PU_PIN, HIGH);
  uint16_t reedValue = denoiseAnalog(REED_PIN);
  digitalWrite(REED_PU_PIN, LOW);
  pinMode(REED_PU_PIN, INPUT);

  if (reedValue < 32u) // in stand
  {
    TipIsPresent = true;
    inSleepMode = true;
    inOffMode = false;
  }
  else if (reedValue < 192u) // WMRP - 1k
  {
    TipIsPresent = true;
    inSleepMode = false;
    inOffMode = false;
  }
  else // no tip
  {
    TipIsPresent = false;
    inSleepMode = true;
    inOffMode = true;
  }
}

// reads temperature, vibration switch and supply voltages
void SENSORCheck()
{
  analogWrite(CONTROL_PIN, HEATER_OFF); // shut off heater in order to measure temperature
  SLEEPCheck();                         // check and activate/deactivate sleep modes
  delayMicroseconds(TIME2SETTLE);       // wait for voltage to settle

  uint16_t temp = denoiseAnalog(SENSOR_PIN); // read ADC value for temperature

  if (!SensorCounter--)
  {
    CJTemp = getColdJ();
    Vin = getVIN(); // get Vin every now and then
  }

  if (Vin > 11000) //todo
    analogWrite(CONTROL_PIN, HEATER_PWM); // turn on again heater

  RawTemp += (temp - RawTemp); //todo * SMOOTHIE; // stabilize ADC temperature reading

  calculateTemp(); // calculate real temperature value

  // stabilize displayed temperature when around setpoint
  if ((ShowTemp != Setpoint) || (abs(ShowTemp - CurrentTemp) > 5))
    ShowTemp = CurrentTemp;
  if (abs(ShowTemp - Setpoint) <= 1)
    ShowTemp = Setpoint;

  // set state variable if temperature is in working range; beep if working temperature was just reached
  uint16_t gap = abs(SetTemp - CurrentTemp);
  if (gap < 5)
  {
    if (!isWorky && beepIfWorky)
      beep();
    isWorky = true;
    beepIfWorky = false;
  }
  else
    isWorky = false;

  // checks if tip is present or currently inserted
  while (ShowTemp > 500)
  {
    analogWrite(CONTROL_PIN, HEATER_OFF); // shut off heater
    beep();
  }
  /* todo
    TipIsPresent = false; // tip removed ?
  if (!TipIsPresent && (ShowTemp < 500))
  {                                                    // new tip inserted ?
    analogWrite(CONTROL_PIN, HEATER_OFF);              // shut off heater
    beep();                                            // beep for info
    TipIsPresent = true;                               // tip is present now
    ChangeTipScreen();                                 // show tip selection screen
    updateEEPROM();                                    // update setting in EEPROM
    handleMoved = true;                                // reset all timers
    RawTemp = denoiseAnalog(SENSOR_PIN);               // restart temp smooth algorithm
    c0 = LOW;                                          // switch must be released
    setRotary(TEMP_MIN, TEMP_MAX, TEMP_STEP, SetTemp); // reset rotary encoder
  }
  */
}

// calculates real temperature value according to ADC reading and calibration values
void calculateTemp()
{
  CurrentTemp = RawTemp * 0.836 + 8.91 + (CJTemp - 25);
  return; //todo

  if (RawTemp < 200)
    CurrentTemp = map(RawTemp, 0, 200, 21, CalTemp[CurrentTip][0]);
  else if (RawTemp < 280)
    CurrentTemp = map(RawTemp, 200, 280, CalTemp[CurrentTip][0], CalTemp[CurrentTip][1]);
  else
    CurrentTemp = map(RawTemp, 280, 360, CalTemp[CurrentTip][1], CalTemp[CurrentTip][2]);
}

// controls the heater
void Thermostat()
{
  // define Setpoint acoording to current working mode
  if (inOffMode)
    Setpoint = 0;
  else if (inSleepMode)
    Setpoint = SleepTemp;
  else if (inBoostMode)
    Setpoint = SetTemp + BoostTemp;
  else
    Setpoint = SetTemp;

  // control the heater (PID or direct)

  if (PIDenable)
  {
    Input = CurrentTemp;
    uint16_t gap = abs(Setpoint - CurrentTemp);
    if (gap < 30)
      ctrl.SetTunings(consKp, consKi, consKd);
    else
      ctrl.SetTunings(aggKp, aggKi, aggKd);
    ctrl.Compute();
  }
  else
  {
    // turn on heater if current temperature is below setpoint

    if ((CurrentTemp + 50) < Setpoint)
      Output = 255;
    else if ((CurrentTemp + 25) < Setpoint)
      Output = 128;
    else if ((CurrentTemp + 5) < Setpoint)
      Output = 32;
    else if ((CurrentTemp + 0.5) < Setpoint)
      Output = 8;
    else
      Output = 0;
  }

  if (Vin > 11000) //todo
    analogWrite(CONTROL_PIN, HEATER_PWM); // set heater PWM
}

// creates a short beep on the buzzer
void beep()
{
  if (beepEnable)
  {
    for (uint8_t i = 0; i < 255; i++)
    {
      digitalWrite(BUZZER_PIN, HIGH);
      delayMicroseconds(125);
      digitalWrite(BUZZER_PIN, LOW);
      delayMicroseconds(125);
    }
  }
}

// sets start values for rotary encoder
void setRotary(int rmin, int rmax, int rstep, int rvalue)
{
  countMin = rmin << ROTARY_TYPE;
  countMax = rmax << ROTARY_TYPE;
  countStep = ECReverse ? -rstep : rstep;
  count = rvalue << ROTARY_TYPE;
}

// reads current rotary encoder value
int getRotary()
{
  return (count >> ROTARY_TYPE);
}

// reads user settings from EEPROM; if EEPROM values are invalid, write defaults
void getEEPROM()
{
  uint16_t identifier = (EEPROM.read(0) << 8) | EEPROM.read(1);
  if (identifier == EEPROM_IDENT)
  {
    DefaultTemp = (EEPROM.read(2) << 8) | EEPROM.read(3);
    SleepTemp = (EEPROM.read(4) << 8) | EEPROM.read(5);
    BoostTemp = EEPROM.read(6);
    time2sleep = EEPROM.read(7);
    time2off = EEPROM.read(8);
    timeOfBoost = EEPROM.read(9);
    MainScrType = EEPROM.read(10);
    PIDenable = EEPROM.read(11);
    beepEnable = EEPROM.read(12);
    BodyFlip = EEPROM.read(13);
    ECReverse = EEPROM.read(14);
    CurrentTip = EEPROM.read(15);
    NumberOfTips = EEPROM.read(16);

    uint8_t i, j;
    uint16_t counter = 17;
    for (i = 0; i < NumberOfTips; i++)
    {
      for (j = 0; j < TIPNAMELENGTH; j++)
      {
        TipName[i][j] = EEPROM.read(counter++);
      }
      for (j = 0; j < 4; j++)
      {
        CalTemp[i][j] = EEPROM.read(counter++) << 8;
        CalTemp[i][j] |= EEPROM.read(counter++);
      }
    }
  }
  else
  {
    EEPROM.update(0, EEPROM_IDENT >> 8);
    EEPROM.update(1, EEPROM_IDENT & 0xFF);
    updateEEPROM();
  }
}

// writes user settings to EEPROM using updade function to minimize write cycles
void updateEEPROM()
{
  EEPROM.update(2, DefaultTemp >> 8);
  EEPROM.update(3, DefaultTemp & 0xFF);
  EEPROM.update(4, SleepTemp >> 8);
  EEPROM.update(5, SleepTemp & 0xFF);
  EEPROM.update(6, BoostTemp);
  EEPROM.update(7, time2sleep);
  EEPROM.update(8, time2off);
  EEPROM.update(9, timeOfBoost);
  EEPROM.update(10, MainScrType);
  EEPROM.update(11, PIDenable);
  EEPROM.update(12, beepEnable);
  EEPROM.update(13, BodyFlip);
  EEPROM.update(14, ECReverse);
  EEPROM.update(15, CurrentTip);
  EEPROM.update(16, NumberOfTips);

  uint8_t i, j;
  uint16_t counter = 17;
  for (i = 0; i < NumberOfTips; i++)
  {
    for (j = 0; j < TIPNAMELENGTH; j++)
      EEPROM.update(counter++, TipName[i][j]);
    for (j = 0; j < 4; j++)
    {
      EEPROM.update(counter++, CalTemp[i][j] >> 8);
      EEPROM.update(counter++, CalTemp[i][j] & 0xFF);
    }
  }
}

// check state and flip screen
void SetFlip()
{
}

// draws the main screen
void MainScreen()
{
  u8g.firstPage();
  do
  {
    // draw setpoint temperature
    u8g.setFont(u8g_font_9x15);
    u8g.setFontPosTop();
    u8g.drawStr(0, 0, "SET:");
    u8g.setCursor(40, 0);
    u8g.print(Setpoint, 0);

    // draw status of heater
    u8g.setCursor(83, 0);
    if (ShowTemp > 500)
      u8g.print(F("ERROR"));
    else if (inOffMode)
      u8g.print(F("  OFF"));
    else if (inSleepMode)
      u8g.print(F("SLEEP"));
    else if (inBoostMode)
      u8g.print(F("BOOST"));
    else if (isWorky)
      u8g.print(F("WORKY"));
    else if (Output < 180)
      u8g.print(F(" HEAT"));
    else
      u8g.print(F(" HOLD"));

    // rest depending on main screen type
    if (MainScrType)
    {
      // draw current tip and input voltage
      float fVin = (float)Vin / 1000; // convert mv in V
      u8g.setCursor(0, 52);
      u8g.print(TipName[CurrentTip]);
      u8g.setCursor(83, 52);
      u8g.print(fVin, 1);
      u8g.print(F("V"));
      // draw current temperature
      u8g.setFont(u8g2_font_freedoomr25_tn);
      u8g.setFontPosTop();
      u8g.setCursor(37, 22);
      if (false && ShowTemp > 500)
        u8g.print(F("000"));
      else
        u8g.print(ShowTemp);
    }
    else
    {
      // draw current temperature in big figures
      u8g.setFont(u8g2_font_fub42_tn);
      u8g.setFontPosTop();
      u8g.setCursor(15, 20);
      if (ShowTemp > 500)
        u8g.print(F("000"));
      else
        u8g.print(ShowTemp);
    }
  } while (u8g.nextPage());
}

// setup screen
void SetupScreen()
{
  analogWrite(CONTROL_PIN, HEATER_OFF); // shut off heater
  beep();
  uint16_t SaveSetTemp = SetTemp;
  uint8_t selection = 0;
  bool repeat = true;

  while (repeat)
  {
    selection = MenuScreen(SetupItems, sizeof(SetupItems), selection);
    switch (selection)
    {
    case 0:
      TipScreen();
      repeat = false;
      break;
    case 1:
      TempScreen();
      break;
    case 2:
      TimerScreen();
      break;
    case 3:
      PIDenable = MenuScreen(ControlTypeItems, sizeof(ControlTypeItems), PIDenable);
      break;
    case 4:
      MainScrType = MenuScreen(MainScreenItems, sizeof(MainScreenItems), MainScrType);
      break;
    case 5:
      beepEnable = MenuScreen(BuzzerItems, sizeof(BuzzerItems), beepEnable);
      break;
    case 6:
      BodyFlip = MenuScreen(FlipItems, sizeof(FlipItems), BodyFlip);
      SetFlip();
      break;
    case 7:
      ECReverse = MenuScreen(ECReverseItems, sizeof(ECReverseItems), ECReverse);
      break;
    case 8:
      InfoScreen();
      break;
    default:
      repeat = false;
      break;
    }
  }
  updateEEPROM();
  SetTemp = SaveSetTemp;
  setRotary(TEMP_MIN, TEMP_MAX, TEMP_STEP, SetTemp);
}

// tip settings screen
void TipScreen()
{
  uint8_t selection = 0;
  bool repeat = true;
  while (repeat)
  {
    selection = MenuScreen(TipItems, sizeof(TipItems), selection);
    switch (selection)
    {
    case 0:
      ChangeTipScreen();
      break;
    case 1:
      CalibrationScreen();
      break;
    case 2:
      InputNameScreen();
      break;
    case 3:
      DeleteTipScreen();
      break;
    case 4:
      AddTipScreen();
      break;
    default:
      repeat = false;
      break;
    }
  }
}

// temperature settings screen
void TempScreen()
{
  uint8_t selection = 0;
  bool repeat = true;
  while (repeat)
  {
    selection = MenuScreen(TempItems, sizeof(TempItems), selection);
    switch (selection)
    {
    case 0:
      setRotary(TEMP_MIN, TEMP_MAX, TEMP_STEP, DefaultTemp);
      DefaultTemp = InputScreen(DefaultTempItems);
      break;
    case 1:
      setRotary(20, 200, TEMP_STEP, SleepTemp);
      SleepTemp = InputScreen(SleepTempItems);
      break;
    case 2:
      setRotary(10, 100, TEMP_STEP, BoostTemp);
      BoostTemp = InputScreen(BoostTempItems);
      break;
    default:
      repeat = false;
      break;
    }
  }
}

// timer settings screen
void TimerScreen()
{
  uint8_t selection = 0;
  bool repeat = true;
  while (repeat)
  {
    selection = MenuScreen(TimerItems, sizeof(TimerItems), selection);
    switch (selection)
    {
    case 0:
      setRotary(0, 30, 1, time2sleep);
      time2sleep = InputScreen(SleepTimerItems);
      break;
    case 1:
      setRotary(0, 60, 5, time2off);
      time2off = InputScreen(OffTimerItems);
      break;
    case 2:
      setRotary(0, 180, 10, timeOfBoost);
      timeOfBoost = InputScreen(BoostTimerItems);
      break;
    default:
      repeat = false;
      break;
    }
  }
}

// menu screen
uint8_t MenuScreen(const char *Items[], uint8_t numberOfItems, uint8_t selected)
{
  bool isTipScreen = (Items[0] == TipItems[0]);
  uint8_t lastselected = selected;
  int8_t arrow = 0;
  if (selected)
    arrow = 1;
  numberOfItems >>= 1;
  setRotary(0, numberOfItems - 2, 1, selected);
  bool lastbutton = (!digitalRead(BUTTON_PIN));

  do
  {
    selected = getRotary();
    arrow = constrain(arrow + selected - lastselected, 0, 2);
    lastselected = selected;
    u8g.firstPage();
    do
    {
      u8g.setFont(u8g_font_9x15);
      u8g.setFontPosTop();
      u8g.drawStr(0, 0, Items[0]);
      if (isTipScreen)
        u8g.drawStr(54, 0, TipName[CurrentTip]);
      u8g.drawStr(0, 16 * (arrow + 1), ">");
      for (uint8_t i = 0; i < 3; i++)
      {
        uint8_t drawnumber = selected + i + 1 - arrow;
        if (drawnumber < numberOfItems)
          u8g.drawStr(12, 16 * (i + 1), Items[selected + i + 1 - arrow]);
      }
    } while (u8g.nextPage());
    if (lastbutton && digitalRead(BUTTON_PIN))
    {
      delay(10);
      lastbutton = false;
    }
  } while (digitalRead(BUTTON_PIN) || lastbutton);

  beep();
  return selected;
}

void MessageScreen(const char *Items[], uint8_t numberOfItems)
{
  bool lastbutton = (!digitalRead(BUTTON_PIN));
  u8g.firstPage();
  do
  {
    u8g.setFont(u8g_font_9x15);
    u8g.setFontPosTop();
    for (uint8_t i = 0; i < numberOfItems; i++)
      u8g.drawStr(0, i * 16, Items[i]);
  } while (u8g.nextPage());
  do
  {
    if (lastbutton && digitalRead(BUTTON_PIN))
    {
      delay(10);
      lastbutton = false;
    }
  } while (digitalRead(BUTTON_PIN) || lastbutton);
  beep();
}

// input value screen
uint16_t InputScreen(const char *Items[])
{
  uint16_t value;
  bool lastbutton = (!digitalRead(BUTTON_PIN));

  do
  {
    value = getRotary();
    u8g.firstPage();
    do
    {
      u8g.setFont(u8g_font_9x15);
      u8g.setFontPosTop();
      u8g.drawStr(0, 0, Items[0]);
      u8g.setCursor(0, 32);
      u8g.print(">");
      u8g.setCursor(10, 32);
      if (value == 0)
        u8g.print(F("Deactivated"));
      else
      {
        u8g.print(value);
        u8g.print(" ");
        u8g.print(Items[1]);
      }
    } while (u8g.nextPage());
    if (lastbutton && digitalRead(BUTTON_PIN))
    {
      delay(10);
      lastbutton = false;
    }
  } while (digitalRead(BUTTON_PIN) || lastbutton);

  beep();
  return value;
}

// information display screen
void InfoScreen()
{
  bool lastbutton = (!digitalRead(BUTTON_PIN));

  do
  {
    Vcc = getVCC();                 // read input voltage
    float fVcc = (float)Vcc / 1000; // convert mV in V
    Vin = getVIN();                 // read supply voltage
    float fVin = (float)Vin / 1000; // convert mv in V
    float fTmp = getChipTemp();     // read cold junction temperature
    u8g.firstPage();
    do
    {
      u8g.setFont(u8g_font_9x15);
      u8g.setFontPosTop();
      u8g.setCursor(0, 0);
      u8g.print(F("Firmware: "));
      u8g.print(VERSION);
      u8g.setCursor(0, 16);
      u8g.print(F("Tmp: "));
      u8g.print(fTmp, 1);
      u8g.print(F(" C"));
      u8g.setCursor(0, 32);
      u8g.print(F("Vin: "));
      u8g.print(fVin, 1);
      u8g.print(F(" V"));
      u8g.setCursor(0, 48);
      u8g.print(F("Vcc:  "));
      u8g.print(fVcc, 1);
      u8g.print(F(" V"));
    } while (u8g.nextPage());
    if (lastbutton && digitalRead(BUTTON_PIN))
    {
      delay(10);
      lastbutton = false;
    }
  } while (digitalRead(BUTTON_PIN) || lastbutton);

  beep();
}

// change tip screen
void ChangeTipScreen()
{
  uint8_t selected = CurrentTip;
  uint8_t lastselected = selected;
  int8_t arrow = 0;
  if (selected)
    arrow = 1;
  setRotary(0, NumberOfTips - 1, 1, selected);
  bool lastbutton = (!digitalRead(BUTTON_PIN));

  do
  {
    selected = getRotary();
    arrow = constrain(arrow + selected - lastselected, 0, 2);
    lastselected = selected;
    u8g.firstPage();
    do
    {
      u8g.setFont(u8g_font_9x15);
      u8g.setFontPosTop();
      u8g.setCursor(0, 0);
      u8g.print(F("Select Tip"));
      u8g.drawStr(0, 16 * (arrow + 1), ">");
      for (uint8_t i = 0; i < 3; i++)
      {
        uint8_t drawnumber = selected + i - arrow;
        if (drawnumber < NumberOfTips)
          u8g.drawStr(12, 16 * (i + 1), TipName[selected + i - arrow]);
      }
    } while (u8g.nextPage());
    if (lastbutton && digitalRead(BUTTON_PIN))
    {
      delay(10);
      lastbutton = false;
    }
  } while (digitalRead(BUTTON_PIN) || lastbutton);

  beep();
  CurrentTip = selected;
}

// temperature calibration screen
void CalibrationScreen()
{
  uint16_t CalTempNew[4];
  for (uint8_t CalStep = 0; CalStep < 3; CalStep++)
  {
    SetTemp = CalTemp[CurrentTip][CalStep];
    setRotary(100, 500, 1, SetTemp);
    beepIfWorky = true;
    bool lastbutton = (!digitalRead(BUTTON_PIN));

    do
    {
      SENSORCheck(); // reads temperature and vibration switch of the iron
      Thermostat();  // heater control

      u8g.firstPage();
      do
      {
        u8g.setFont(u8g_font_9x15);
        u8g.setFontPosTop();
        u8g.setCursor(0, 0);
        u8g.print(F("Calibration"));
        u8g.setCursor(0, 16);
        u8g.print(F("Step: "));
        u8g.print(CalStep + 1);
        u8g.print(" of 3");
        if (isWorky)
        {
          u8g.setCursor(0, 32);
          u8g.print(F("Set measured"));
          u8g.setCursor(0, 48);
          u8g.print(F("temp: "));
          u8g.print(getRotary());
        }
        else
        {
          u8g.setCursor(0, 32);
          u8g.print(F("ADC:  "));
          u8g.print(uint16_t(RawTemp));
          u8g.setCursor(0, 48);
          u8g.print(F("Please wait..."));
        }
      } while (u8g.nextPage());
      if (lastbutton && digitalRead(BUTTON_PIN))
      {
        delay(10);
        lastbutton = false;
      }
    } while (digitalRead(BUTTON_PIN) || lastbutton);

    CalTempNew[CalStep] = getRotary();
    beep();
    delay(10);
  }

  analogWrite(CONTROL_PIN, HEATER_OFF); // shut off heater
  delayMicroseconds(TIME2SETTLE);       // wait for voltage to settle
  CalTempNew[3] = getChipTemp();        // read chip temperature
  if ((CalTempNew[0] + 10 < CalTempNew[1]) && (CalTempNew[1] + 10 < CalTempNew[2]))
  {
    if (MenuScreen(StoreItems, sizeof(StoreItems), 0))
    {
      for (uint8_t i = 0; i < 4; i++)
        CalTemp[CurrentTip][i] = CalTempNew[i];
    }
  }
}

// input tip name screen
void InputNameScreen()
{
  uint8_t value;

  for (uint8_t digit = 0; digit < (TIPNAMELENGTH - 1); digit++)
  {
    bool lastbutton = (!digitalRead(BUTTON_PIN));
    setRotary(31, 96, 1, 65);
    do
    {
      value = getRotary();
      if (value == 31)
      {
        value = 95;
        setRotary(31, 96, 1, 95);
      }
      if (value == 96)
      {
        value = 32;
        setRotary(31, 96, 1, 32);
      }
      u8g.firstPage();
      do
      {
        u8g.setFont(u8g_font_9x15);
        u8g.setFontPosTop();
        u8g.setCursor(0, 0);
        u8g.print(F("Enter Tip Name"));
        u8g.setCursor(9 * digit, 48);
        u8g.print(char(94));
        u8g.setCursor(0, 32);
        for (uint8_t i = 0; i < digit; i++)
          u8g.print(TipName[CurrentTip][i]);
        u8g.setCursor(9 * digit, 32);
        u8g.print(char(value));
      } while (u8g.nextPage());
      if (lastbutton && digitalRead(BUTTON_PIN))
      {
        delay(10);
        lastbutton = false;
      }
    } while (digitalRead(BUTTON_PIN) || lastbutton);
    TipName[CurrentTip][digit] = value;
    beep();
    delay(10);
  }
  TipName[CurrentTip][TIPNAMELENGTH - 1] = 0;
  return;
}

// delete tip screen
void DeleteTipScreen()
{
  if (NumberOfTips == 1)
  {
    MessageScreen(DeleteMessage, sizeof(DeleteMessage));
  }
  else if (MenuScreen(SureItems, sizeof(SureItems), 0))
  {
    if (CurrentTip == (NumberOfTips - 1))
    {
      CurrentTip--;
    }
    else
    {
      for (uint8_t i = CurrentTip; i < (NumberOfTips - 1); i++)
      {
        for (uint8_t j = 0; j < TIPNAMELENGTH; j++)
          TipName[i][j] = TipName[i + 1][j];
        for (uint8_t j = 0; j < 4; j++)
          CalTemp[i][j] = CalTemp[i + 1][j];
      }
    }
    NumberOfTips--;
  }
}

// add new tip screen
void AddTipScreen()
{
  if (NumberOfTips < TIPMAX)
  {
    CurrentTip = NumberOfTips++;
    InputNameScreen();
    CalTemp[CurrentTip][0] = TEMP200;
    CalTemp[CurrentTip][1] = TEMP280;
    CalTemp[CurrentTip][2] = TEMP360;
    CalTemp[CurrentTip][3] = TEMPCHP;
  }
  else
    MessageScreen(MaxTipMessage, sizeof(MaxTipMessage));
}

// average several ADC readings in sleep mode to denoise
uint16_t denoiseAnalog(byte port)
{
  uint16_t result = 0;
  ADCSRA |= bit(ADEN) | bit(ADIF); // enable ADC, turn off any pending interrupt
  if (port >= A0)
    port -= A0;                       // set port and
  ADMUX = (0x0F & port) | bit(REFS0); // reference to AVcc
  set_sleep_mode(SLEEP_MODE_ADC);     // sleep during sample for noise reduction
  for (uint8_t i = 0; i < 32; i++)
  {               // get 32 readings
    sleep_mode(); // go to sleep while taking ADC sample
    while (bitRead(ADCSRA, ADSC))
      ;            // make sure sampling is completed
    result += ADC; // add them up
  }
  bitClear(ADCSRA, ADEN); // disable ADC
  return (result >> 5);   // devide by 32 and return value
}

// get internal temperature by reading ADC channel 8 against 1.1V reference
double getChipTemp()
{
  uint16_t result = 0;
  ADCSRA |= bit(ADEN) | bit(ADIF);             // enable ADC, turn off any pending interrupt
  ADMUX = bit(REFS1) | bit(REFS0) | bit(MUX3); // set reference and mux
  delay(20);                                   // wait for voltages to settle
  set_sleep_mode(SLEEP_MODE_ADC);              // sleep during sample for noise reduction
  for (uint8_t i = 0; i < 32; i++)
  {               // get 32 readings
    sleep_mode(); // go to sleep while taking ADC sample
    while (bitRead(ADCSRA, ADSC))
      ;            // make sure sampling is completed
    result += ADC; // add them up
  }
  bitClear(ADCSRA, ADEN);          // disable ADC
  result >>= 2;                    // devide by 4
  return ((result - 2594) / 9.76); // calculate internal temperature in degrees C
}

// get input voltage in mV by reading 1.1V reference against AVcc
uint16_t getVCC()
{
  uint16_t result = 0;
  ADCSRA |= bit(ADEN) | bit(ADIF); // enable ADC, turn off any pending interrupt
  // set Vcc measurement against 1.1V reference
  ADMUX = bit(REFS0) | bit(MUX3) | bit(MUX2) | bit(MUX1);
  delay(1);                       // wait for voltages to settle
  set_sleep_mode(SLEEP_MODE_ADC); // sleep during sample for noise reduction
  for (uint8_t i = 0; i < 16; i++)
  {               // get 16 readings
    sleep_mode(); // go to sleep while taking ADC sample
    while (bitRead(ADCSRA, ADSC))
      ;            // make sure sampling is completed
    result += ADC; // add them up
  }
  bitClear(ADCSRA, ADEN);     // disable ADC
  result >>= 4;               // devide by 16
  return (1125300L / result); // 1125300 = 1.1 * 1023 * 1000
}

// get supply voltage in mV
uint16_t getVIN()
{
  long result;
  result = denoiseAnalog(VIN_PIN); // read supply voltage via voltage divider
  return (result * Vcc / 179.474); // 179.474 = 1023 * R13 / (R12 + R13)
}

uint16_t getColdJ()
{
  uint16_t result = denoiseAnalog(COLDJ_PIN);
  return (result * 0.9 - 113.836);
}

// ADC interrupt service routine
EMPTY_INTERRUPT(ADC_vect); // nothing to be done here

// Pin change interrupt service routine for rotary encoder
ISR(PCINT0_vect)
{
  uint8_t a = PINB & 1;
  uint8_t b = PIND >> 7 & 1;

  if (a != a0)
  { // A changed
    a0 = a;
    if (b != b0)
    { // B changed
      b0 = b;
      count = constrain(count + ((a == b) ? countStep : -countStep), countMin, countMax);
      if (ROTARY_TYPE && ((a == b) != ab0))
      {
        count = constrain(count + ((a == b) ? countStep : -countStep), countMin, countMax);
      }
      ab0 = (a == b);
    }
  }
}
