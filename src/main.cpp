#include <Arduino.h>

// pin definition for ESP8266 12E
#define DIT_PIN D1
#define DAH_PIN D2
#define PIN_CW_OUT D5

// paddle input active-low or active-high
#define ACTIVE_LOW 0
#define ACTIVE_HIGH 1
const int paddleInputLevel = ACTIVE_LOW;

int wpm = 15;  // Words per minute
int fwpm = 15; // Farnsworth speed

// calculate timings in miliseconds based on the formula: T = 1200 / WPM
int ditLength = 1200 / wpm;
int dahLength = 1200 * 3 / wpm;
int letterSpaceLength = 1200 * 3 / fwpm;

// holds the current state of the paddles
volatile boolean ditState;
volatile boolean dahState;

// Morse states
enum MorseStates
{
  START,
  DITSTATE,
  DAHSTATE,
  ENDCHAR,
  ENDWORD,
};
MorseStates state = START;

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
}
// Turn output on
void keyDown()
{
  digitalWrite(PIN_CW_OUT, HIGH);
}

// pin init
void setup()
{
  pinMode(DIT_PIN, INPUT_PULLUP);
  pinMode(DAH_PIN, INPUT_PULLUP);
  pinMode(PIN_CW_OUT, OUTPUT);

  // attach interrupts
  attachInterrupt(digitalPinToInterrupt(DIT_PIN), handleDitInterrupt, CHANGE);
  attachInterrupt(digitalPinToInterrupt(DAH_PIN), handleDahInterrupt, CHANGE);

  Serial.begin(115200);
}

// main loop
boolean sendingDit = false;
boolean completedDit = false;
unsigned long lastDitTriggerd = 0;

boolean sendingDah = false;
boolean completedDah = false;
unsigned long lastDahTriggerd = 0;

boolean idle = false;
unsigned long lastIdleTriggered = 0;

boolean nextStateSet = false;
MorseStates nextState;

unsigned long now = 0;
void loop()
{
  now = millis();
  Serial.print("ditState: ");
  Serial.print(ditState);
  Serial.print(" dahState: ");
  Serial.print(dahState);
  Serial.print(" sendingDit: ");
  Serial.print(sendingDit);
  Serial.print(" completedDit: ");
  Serial.print(completedDit);
  Serial.print(" idle: ");
  Serial.print(idle);
  Serial.print(" nextStateSet: ");
  Serial.print(nextStateSet);
  Serial.print(" nextState: ");
  Serial.print(nextState);
  Serial.print(" State: ");
  Serial.println(state);

  switch (state)
  {
  case START:

    if (ditState)
      state = DITSTATE;
    else if (dahState)
      state = DAHSTATE;

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
        idle = true;
        lastIdleTriggered = now;
        break;
      }

      // check opposite paddle
      if (dahState)
      {
        nextStateSet = true;
        nextState = DAHSTATE;
      }
    }

    // After dit ended:
    if (!sendingDit && completedDit)
    {
      if (now - lastIdleTriggered > ditLength)
      {
        state = nextState;
        nextStateSet = false;

        idle = false;
        lastIdleTriggered = 0;
        completedDit = false;
        break;
      }
      else
      {
        if (!nextStateSet)
        {
          // check paddles
          if (ditState && dahState)
          {
            nextState = DAHSTATE;
            nextStateSet = true;
          }
          else if (dahState)
          {
            nextState = DAHSTATE;
            nextStateSet = true;
          }
          else if (ditState)
          {
            nextState = DITSTATE;
            nextStateSet = true;
          }
          else
          {
            nextState = ENDCHAR;
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
        idle = true;
        lastIdleTriggered = now;
        break;
      }

      // check opposite paddle
      if (ditState)
      {
        nextStateSet = true;
        nextState = DITSTATE;
      }
    }

    // After dah ended:
    if (!sendingDah && completedDah)
    {
      if (now - lastIdleTriggered > ditLength)
      {
        state = nextState;
        nextStateSet = false;
        idle = false;
        lastIdleTriggered = 0;
        completedDah = false;
        break;
      }
      else
      {
        if (!nextStateSet)
        {
          // check paddles
          if (ditState && dahState)
          {
            nextState = DITSTATE;
            nextStateSet = true;
          }
          else if (dahState)
          {
            nextState = DAHSTATE;
            nextStateSet = true;
          }
          else if (ditState)
          {
            nextState = DITSTATE;
            nextStateSet = true;
          }
          else
          {
            nextState = ENDCHAR;
          }
        }
      }
    }

    break;

  case ENDCHAR:
    break;

  case ENDWORD:
    break;
  }
}