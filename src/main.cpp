#include <Arduino.h>
#include "si5351.h"
#include "Wire.h"

Si5351 si5351;
int32_t cal_factor = 148870;

#define ACTIVE_LOW 0
#define ACTIVE_HIGH 1

// pin definition for ESP8266 12E
#pragma region Pin_definitions
#define DIT_PIN D3
#define DAH_PIN D4
#define PIN_CW_OUT D5
#define PIN_BUZZER_OUT D6
#pragma endregion Pin_definitions

enum DeviceModes
{
  STANDALONE,
  WEBSERVER,
  WSJTX
};

enum OperatingModes
{
  CW,
  PIXIE_CW,
  WSPR,
  FT8,
  FSQ_2,
  FSQ_3,
  FSQ_4_5,
  FSQ_6
};

// common global states
#pragma region Common_Global_States

int wpm = 15;                          // Words per minute for cw mode
DeviceModes deviceMode = STANDALONE;   // default device mode is standalone
OperatingModes operatingMode = CW;     // default op mode is CW
uint64_t frequency = 7023000 * 100ULL; // 7.023 MHz
int32_t cal_factor = 148870;           // si5351 calibration factor
boolean txEnabled = false;             // flag to denote if transmit is enabled or disabled
char txMessage[100] = "";              // tx message

#pragma endregion Common_Global_States

// CW Keyer functionality
#pragma region CW_Keyer
const int paddleInputLevel = ACTIVE_LOW; // paddle input active-low

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
  if (paddleInputLevel == ACTIVE_LOW)
    ditState = !digitalRead(DIT_PIN);
  else
    ditState = !!digitalRead(DIT_PIN);
}

// read dah state
IRAM_ATTR void handleDahInterrupt()
{
  // Get the pin reading.
  if (paddleInputLevel == ACTIVE_LOW)
    dahState = !digitalRead(DAH_PIN);
  else
    dahState = !!digitalRead(DAH_PIN);
}

// Turn output off
void keyUp()
{
  digitalWrite(PIN_CW_OUT, LOW);
  noTone(PIN_BUZZER_OUT);
  si5351.output_enable(SI5351_CLK0, 0);
}
// Turn output on
void keyDown()
{
  digitalWrite(PIN_CW_OUT, HIGH);
  tone(PIN_BUZZER_OUT, 1000);
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
  pinMode(PIN_CW_OUT, OUTPUT);

  // attach interrupts for cw keyer paddles
  attachInterrupt(digitalPinToInterrupt(DIT_PIN), handleDitInterrupt, CHANGE);
  attachInterrupt(digitalPinToInterrupt(DAH_PIN), handleDahInterrupt, CHANGE);

  // Start serial and initialize the Si5351
  Serial.begin(57600);
  si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);

  si5351.set_correction(cal_factor, SI5351_PLL_INPUT_XO);

  // Set CLK0 to output 7 MHz
  si5351.set_freq(702300000ULL, SI5351_CLK0);

  si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_2MA);
  // output on/off
  si5351.output_enable(SI5351_CLK0, 0);
}

// main loop

unsigned long now = 0;
void loop()
{
  now = millis();

  // si5351
  si5351.update_status();
  Serial.print("SYS_INIT: ");
  Serial.print(si5351.dev_status.SYS_INIT);
  Serial.print("  LOL_A: ");
  Serial.print(si5351.dev_status.LOL_A);
  Serial.print("  LOL_B: ");
  Serial.print(si5351.dev_status.LOL_B);
  Serial.print("  LOS: ");
  Serial.print(si5351.dev_status.LOS);
  Serial.print("  REVID: ");
  Serial.println(si5351.dev_status.REVID);

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

  // end of loop
}