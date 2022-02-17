#include <Arduino.h>
#include <si5351.h>
#include <JTEncode.h>
#include <rs_common.h>
#include <int.h>
#include <string.h>
#include "Wire.h"

#define ACTIVE_LOW 0
#define ACTIVE_HIGH 1

// Mode defines
#define JT9_TONE_SPACING 174  // ~1.74 Hz
#define JT65_TONE_SPACING 269 // ~2.69 Hz
#define JT4_TONE_SPACING 437  // ~4.37 Hz
#define WSPR_TONE_SPACING 146 // ~1.46 Hz
#define FSQ_TONE_SPACING 879  // ~8.79 Hz
#define FT8_TONE_SPACING 625  // ~6.25 Hz

#define JT9_DELAY 576     // Delay value for JT9-1
#define JT65_DELAY 371    // Delay in ms for JT65A
#define JT4_DELAY 229     // Delay value for JT4A
#define WSPR_DELAY 683    // Delay value for WSPR
#define FSQ_2_DELAY 500   // Delay value for 2 baud FSQ
#define FSQ_3_DELAY 333   // Delay value for 3 baud FSQ
#define FSQ_4_5_DELAY 222 // Delay value for 4.5 baud FSQ
#define FSQ_6_DELAY 167   // Delay value for 6 baud FSQ
#define FT8_DELAY 159     // Delay value for FT8

#define JT9_DEFAULT_FREQ 14078700UL
#define JT65_DEFAULT_FREQ 14078300UL
#define JT4_DEFAULT_FREQ 14078500UL
#define WSPR_DEFAULT_FREQ 14097200UL
#define FSQ_DEFAULT_FREQ 7105350UL // Base freq is 1350 Hz higher than dial freq in USB
#define FT8_DEFAULT_FREQ 14075000UL

// Enumerations
#pragma region Enums
enum DeviceModes
{
  STANDALONE,
  WEBSERVER,
  WSJTX
};

enum OperatingModes
{
  MODE_CW,
  MODE_PIXIE_CW,
  MODE_WSPR,
  MODE_FT8,
  MODE_FSQ_2,
  MODE_FSQ_3,
  MODE_FSQ_4_5,
  MODE_FSQ_6,
  MODE_JT9,
  MODE_JT65,
  MODE_JT4,
};
#pragma endregion Enums

// Class instantiation
Si5351 si5351;
JTEncode jtencode;

// pin definition for ESP8266 12E
#pragma region Pin_definitions

#define DIT_PIN D3    // DIT paddle
#define DAH_PIN D4    // DAH paddle
#define PTT_PIN D5    // Will be active while transmitting
#define BUZZER_PIN D6 // Buzzer output

const int cwPaddlePinActiveLevel = ACTIVE_LOW;
const int pttPinActiveLevel = ACTIVE_LOW;

#pragma endregion Pin_definitions

// common global states
#pragma region Common_Global_States

int wpm = 15;                             // Words per minute for cw mode
DeviceModes deviceMode = STANDALONE;      // default device mode is standalone
OperatingModes operatingMode = MODE_CW;   // default op mode is CW
uint64_t frequency = 7023000 * 100ULL;    // 7.023 MHz
int32_t si5351CalibrationFactor = 149300; // si5351 calibration factor
boolean txEnabled = false;                // flag to denote if transmit is enabled or disabled
char txMessage[100] = "";                 // tx message
char myCallsign[10] = "VU2EHJ";
char dxCallsign[10] = "VU3HZX";
char myGridLocator[10] = "NL66WE";
uint8_t dBm = 27;
uint8_t txBuffer[255];
uint8_t symbolCount;
uint16_t toneDelay, toneSpacing;

#pragma endregion Common_Global_States

// JTEncode logic
#pragma region JTEncode
// Loop through the string, transmitting one character at a time.
void jtTransmitMessage()
{
  uint8_t i;

  // Reset the tone to the base frequency and turn on the output
  si5351.output_enable(SI5351_CLK0, 1);
  if (pttPinActiveLevel == ACTIVE_LOW)
    digitalWrite(PTT_PIN, LOW);
  else
    digitalWrite(PTT_PIN, HIGH);

  // Now transmit the channel symbols
  if (operatingMode == MODE_FSQ_2 || operatingMode == MODE_FSQ_3 || operatingMode == MODE_FSQ_4_5 || operatingMode == MODE_FSQ_6)
  {
    uint8_t j = 0;

    while (txBuffer[j++] != 0xff)
      ;

    symbolCount = j - 1;
  }

  for (i = 0; i < symbolCount; i++)
  {
    si5351.set_freq(frequency + (txBuffer[i] * toneSpacing), SI5351_CLK0);
    delay(toneDelay);
  }

  // Turn off the output
  si5351.output_enable(SI5351_CLK0, 0);
  if (pttPinActiveLevel == ACTIVE_LOW)
    digitalWrite(PTT_PIN, HIGH);
  else
    digitalWrite(PTT_PIN, LOW);
}

void setTxBuffer()
{
  // Clear out the transmit buffer
  memset(txBuffer, 0, 255);

  // Set the proper frequency and timer CTC depending on mode
  switch (operatingMode)
  {
  case MODE_JT9:
    jtencode.jt9_encode(txMessage, txBuffer);
    break;
  case MODE_JT65:
    jtencode.jt65_encode(txMessage, txBuffer);
    break;
  case MODE_JT4:
    jtencode.jt4_encode(txMessage, txBuffer);
    break;
  case MODE_WSPR:
    jtencode.wspr_encode(myCallsign, myGridLocator, dBm, txBuffer);
    break;
  case MODE_FT8:
    jtencode.ft8_encode(txMessage, txBuffer);
    break;
  case MODE_FSQ_2:
  case MODE_FSQ_3:
  case MODE_FSQ_4_5:
  case MODE_FSQ_6:
    jtencode.fsq_dir_encode(myCallsign, dxCallsign, ' ', txMessage, txBuffer);
    break;
  }
}
#pragma endregion JTEncode

// CW Keyer functionality
#pragma region CW_Keyer
// calculate timings in miliseconds based on the formula: T = 1200 / WPM
int ditLength = 1200 / wpm;
int dahLength = 1200 * 3 / wpm;

// holds the current state of the paddles
volatile boolean ditState;
volatile boolean dahState;

// Morse keyer states
enum MorseStates
{
  START,
  DITSTATE,
  DAHSTATE,
  ENDCHAR,
  ENDWORD,
};
MorseStates keyerState = START;

// read dit state
IRAM_ATTR void handleDitInterrupt()
{
  // Get the pin reading.
  if (cwPaddlePinActiveLevel == ACTIVE_LOW)
    ditState = !digitalRead(DIT_PIN);
  else
    ditState = !!digitalRead(DIT_PIN);
}

// read dah state
IRAM_ATTR void handleDahInterrupt()
{
  // Get the pin reading.
  if (cwPaddlePinActiveLevel == ACTIVE_LOW)
    dahState = !digitalRead(DAH_PIN);
  else
    dahState = !!digitalRead(DAH_PIN);
}

// Turn output off
void keyUp()
{
  if (pttPinActiveLevel == ACTIVE_LOW)
    digitalWrite(PTT_PIN, HIGH);
  else
    digitalWrite(PTT_PIN, LOW);

  noTone(BUZZER_PIN);
  si5351.output_enable(SI5351_CLK0, 0);
}
// Turn output on
void keyDown()
{
  if (pttPinActiveLevel == ACTIVE_LOW)
    digitalWrite(PTT_PIN, LOW);
  else
    digitalWrite(PTT_PIN, HIGH);

  tone(BUZZER_PIN, 1000);
  si5351.output_enable(SI5351_CLK0, 1);
}

// state variables for CW
boolean sendingDit = false;
boolean completedDit = false;
unsigned long lastDitTriggerd = 0;

boolean sendingDah = false;
boolean completedDah = false;
unsigned long lastDahTriggerd = 0;

boolean keyerIdle = false;
unsigned long lastKeyerIdleTriggered = 0;

boolean nextKeyerStateSet = false;
MorseStates nextKeyerState;
#pragma endregion CW_Keyer

// pin init
void setup()
{
  // set pinmodes for cw keyer pins
  pinMode(DIT_PIN, INPUT_PULLUP);
  pinMode(DAH_PIN, INPUT_PULLUP);
  pinMode(PTT_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  // attach interrupts for cw keyer paddles
  attachInterrupt(digitalPinToInterrupt(DIT_PIN), handleDitInterrupt, CHANGE);
  attachInterrupt(digitalPinToInterrupt(DAH_PIN), handleDahInterrupt, CHANGE);

  // Start serial and initialize the Si5351
  Serial.begin(57600);
  si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);

  si5351.set_correction(si5351CalibrationFactor, SI5351_PLL_INPUT_XO);

  // Set CLK0 to output 7 MHz
  si5351.set_freq(frequency, SI5351_CLK0);

  si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_2MA);
  // output on/off
  si5351.output_enable(SI5351_CLK0, 0);
}

// main loop

unsigned long now = 0;
void loop()
{
  now = millis();

  if (deviceMode == STANDALONE)
  {
    // standalone logic
    if (operatingMode == MODE_CW)
    {
      // CW Keyer State machine
#pragma region CW_Keyer_State_Machine
      switch (keyerState)
      {
      case START:

        if (ditState)
          keyerState = DITSTATE;
        else if (dahState)
          keyerState = DAHSTATE;

        break;

      case DITSTATE:
        // Before dit start:
        if (!sendingDit && !completedDit)
        {
          sendingDit = true;
          lastDitTriggerd = now;
          keyDown();
          break;
        }

        // After dit started:
        if (sendingDit && !completedDit)
        {
          if (now - lastDitTriggerd >= ditLength)
          {
            lastDitTriggerd = 0;
            sendingDit = false;
            completedDit = true;
            keyUp();
            keyerIdle = true;
            lastKeyerIdleTriggered = now;
            break;
          }

          // check opposite paddle
          if (dahState)
          {
            nextKeyerStateSet = true;
            nextKeyerState = DAHSTATE;
          }
        }

        // After dit ended:
        if (!sendingDit && completedDit)
        {
          if (now - lastKeyerIdleTriggered > ditLength)
          {
            keyerState = nextKeyerState;
            nextKeyerStateSet = false;

            keyerIdle = false;
            lastKeyerIdleTriggered = 0;
            completedDit = false;
            break;
          }
          else
          {
            if (!nextKeyerStateSet)
            {
              // check paddles
              if (ditState && dahState)
              {
                nextKeyerState = DAHSTATE;
                nextKeyerStateSet = true;
              }
              else if (dahState)
              {
                nextKeyerState = DAHSTATE;
                nextKeyerStateSet = true;
              }
              else if (ditState)
              {
                nextKeyerState = DITSTATE;
                nextKeyerStateSet = true;
              }
              else
              {
                nextKeyerState = ENDCHAR;
              }
            }
          }
        }

        break;

      case DAHSTATE:
        // Before dah start:
        if (!sendingDah && !completedDah)
        {
          sendingDah = true;
          lastDahTriggerd = now;
          keyDown();
          break;
        }

        // After dah started:
        if (sendingDah && !completedDah)
        {
          if (now - lastDahTriggerd >= dahLength)
          {
            lastDahTriggerd = 0;
            sendingDah = false;
            completedDah = true;
            keyUp();
            keyerIdle = true;
            lastKeyerIdleTriggered = now;
            break;
          }

          // check opposite paddle
          if (ditState)
          {
            nextKeyerStateSet = true;
            nextKeyerState = DITSTATE;
          }
        }

        // After dah ended:
        if (!sendingDah && completedDah)
        {
          if (now - lastKeyerIdleTriggered > ditLength)
          {
            keyerState = nextKeyerState;
            nextKeyerStateSet = false;
            keyerIdle = false;
            lastKeyerIdleTriggered = 0;
            completedDah = false;
            break;
          }
          else
          {
            if (!nextKeyerStateSet)
            {
              // check paddles
              if (ditState && dahState)
              {
                nextKeyerState = DITSTATE;
                nextKeyerStateSet = true;
              }
              else if (dahState)
              {
                nextKeyerState = DAHSTATE;
                nextKeyerStateSet = true;
              }
              else if (ditState)
              {
                nextKeyerState = DITSTATE;
                nextKeyerStateSet = true;
              }
              else
              {
                nextKeyerState = ENDCHAR;
              }
            }
          }
        }

        break;

      case ENDCHAR:
        if (!keyerIdle)
        {
          keyerIdle = true;
          lastKeyerIdleTriggered = now;
        }

        if (now - lastKeyerIdleTriggered > ditLength * 2)
        {
          keyerState = nextKeyerState;
          nextKeyerStateSet = false;
          keyerIdle = false;
          lastKeyerIdleTriggered = 0;
          break;
        }
        else
        {
          if (!nextKeyerStateSet)
          {
            // check paddles
            if (dahState)
            {
              nextKeyerState = DAHSTATE;
              nextKeyerStateSet = true;
            }
            else if (ditState)
            {
              nextKeyerState = DITSTATE;
              nextKeyerStateSet = true;
            }
            else
            {
              nextKeyerState = ENDWORD;
            }
          }
        }

        break;

      case ENDWORD:
        if (!keyerIdle)
        {
          keyerIdle = true;
          lastKeyerIdleTriggered = now;
        }

        if (now - lastKeyerIdleTriggered > ditLength * 4)
        {
          keyerState = nextKeyerState;
          nextKeyerStateSet = false;
          keyerIdle = false;
          lastKeyerIdleTriggered = 0;
          break;
        }
        else
        {
          if (!nextKeyerStateSet)
          {
            // check paddles
            if (dahState)
            {
              nextKeyerState = DAHSTATE;
              nextKeyerStateSet = true;
            }
            else if (ditState)
            {
              nextKeyerState = DITSTATE;
              nextKeyerStateSet = true;
            }
            else
            {
              nextKeyerState = START;
            }
          }
        }

        break;
      }
#pragma endregion CW_Keyer_State_Machine
    }
    else if (operatingMode == MODE_WSPR)
    {
      // standalone WSPR logic here
    }
  }
  else if (deviceMode == WEBSERVER)
  {
    // web server logic here
    switch (operatingMode)
    {
    case MODE_CW:
      // cw logic goes here
      break;

    case MODE_PIXIE_CW:
      // pixie cw logic goes here
      break;

    case MODE_JT9:
      symbolCount = JT9_SYMBOL_COUNT; // From the library defines
      toneSpacing = JT9_TONE_SPACING;
      toneDelay = JT9_DELAY;
      break;
    case MODE_JT65:
      symbolCount = JT65_SYMBOL_COUNT; // From the library defines
      toneSpacing = JT65_TONE_SPACING;
      toneDelay = JT65_DELAY;
      break;
    case MODE_JT4:
      symbolCount = JT4_SYMBOL_COUNT; // From the library defines
      toneSpacing = JT4_TONE_SPACING;
      toneDelay = JT4_DELAY;
      break;
    case MODE_WSPR:
      symbolCount = WSPR_SYMBOL_COUNT; // From the library defines
      toneSpacing = WSPR_TONE_SPACING;
      toneDelay = WSPR_DELAY;
      break;
    case MODE_FT8:
      symbolCount = FT8_SYMBOL_COUNT; // From the library defines
      toneSpacing = FT8_TONE_SPACING;
      toneDelay = FT8_DELAY;
      break;
    case MODE_FSQ_2:
      toneSpacing = FSQ_TONE_SPACING;
      toneDelay = FSQ_2_DELAY;
      break;
    case MODE_FSQ_3:
      toneSpacing = FSQ_TONE_SPACING;
      toneDelay = FSQ_3_DELAY;
      break;
    case MODE_FSQ_4_5:
      toneSpacing = FSQ_TONE_SPACING;
      toneDelay = FSQ_4_5_DELAY;
      break;
    case MODE_FSQ_6:
      toneSpacing = FSQ_TONE_SPACING;
      toneDelay = FSQ_6_DELAY;
      break;
    }

    // todo: for tx call : setTxBuffer() and jtTransmitMessage()
  }
  else
  {
    // logic for device mode WSJTX
  }

  // end of loop
}