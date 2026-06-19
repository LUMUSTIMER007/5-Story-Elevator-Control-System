#include <LiquidCrystal.h>
#include <avr/io.h>
//LCD Display
// RS=30, E=31, D4=32, D5=33, D6=34, D7=35
LiquidCrystal lcd(30, 31, 32, 33, 34, 35);

//Motor pins
int ENA = 2;
int IN1 = 3;
int IN2 = 4;

const int LOAD_PIN = A0;

//Floor Buttons
int upBtns[4]   = {36, 37, 39, 41};  // floors 1,2,3,4 UP
int downBtns[4] = {38, 40, 42, 43};  // floors 2,3,4,5 DOWN

//Keypad(4X3)
int rowPins[4] = {44, 45, 46, 47};
int colPins[3] = {48, 49, 50};
char keyMap[4][3] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};

//Elevator States
#define IDLE         0
#define ACCELERATING 1
#define CRUISING     2
#define DECELERATING 3
#define DOOR_OPEN    4

//Elevator Variables
int currentFloor  = 1;
int targetFloor   = 1;
int direction     = 0;      // 1=up, -1=down, 0=idle
int elevState     = IDLE;

bool requestQueue[6] = {false};  // index 1-5

//Timing variables
unsigned long motionStart     = 0;
unsigned long lastBtnTime     = 0;
unsigned long lastLCDTime     = 0;
unsigned long lastKeyTime     = 0;
// calculated for the given conditions
#define ACCEL_TIME   1000 
#define CRUISE_TIME  2000
#define DECEL_TIME   1000
#define DOOR_TIME    2000
#define BTN_INTERVAL  50
#define LCD_INTERVAL  300
#define KEY_DEBOUNCE  300

//## LED Control Using Embedded C ##//

void initLEDs() {
  DDRA  |=  0x1F;   // set PA0-PA4 as OUTPUT (00011111)
  PORTA &= ~0x1F;   // turn all LEDs off
}

void setFloorLED(int floor) {
  PORTA &= ~0x1F;                      // clear all first
  if (floor >= 1 && floor <= 5)
    PORTA |= (1 << (floor - 1));       // set only current floor bit
}

//## Motor Functions ##//

int getMotorSpeed() {
  int pot = analogRead(LOAD_PIN);
  float voltage = pot * (5.0 / 1023.0);

  int load = 0;
  if      (voltage >= 1.75) load = 300;
  else if (voltage >= 1.25) load = 200;
  else if (voltage >= 0.75) load = 100;

  int spd = 160 + (load / 100) * 25;
  return constrain(spd, 160, 255);
}

void motorUp(int spd) {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  analogWrite(ENA, spd);
}

void motorDown(int spd) {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  analogWrite(ENA, spd);
}

void motorStop() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  analogWrite(ENA, 0);
}

//## LCD Update ##//

void updateLCD() {
  lcd.setCursor(0, 0);
  lcd.print("Flr:");
  lcd.print(currentFloor);
  lcd.print(" Tgt:");
  lcd.print(targetFloor);
  lcd.print("     ");

  lcd.setCursor(0, 1);
  if      (elevState == DOOR_OPEN)  lcd.print("Door: OPEN      ");
  else if (direction ==  1)         lcd.print("Going: UP       ");
  else if (direction == -1)         lcd.print("Going: DOWN     ");
  else                              lcd.print("Status: IDLE    ");
}

//## Keypad SCAN ##//
// how it works is pull one row low at a time and then check all columns

char scanKeypad() {
  for (int r = 0; r < 4; r++) {
    digitalWrite(rowPins[r], LOW);
    for (int c = 0; c < 3; c++) {
      if (digitalRead(colPins[c]) == LOW) {
        digitalWrite(rowPins[r], HIGH);
        return keyMap[r][c];
      }
    }
    digitalWrite(rowPins[r], HIGH);
  }
  return 0;
}

//## Read All Buttons ##//

void readAllButtons() {
  unsigned long now = millis();

  // outside UP buttons - floors 1 to 4
  for (int i = 0; i < 4; i++) {
    if (digitalRead(upBtns[i]) == HIGH)
      requestQueue[i + 1] = true;
  }

  // outside DOWN buttons - floors 2 to 5
  for (int i = 0; i < 4; i++) {
    if (digitalRead(downBtns[i]) == HIGH)
      requestQueue[i + 2] = true;
  }

  // keypad with debounce
  if (now - lastKeyTime < KEY_DEBOUNCE) return;

  char key = scanKeypad();
  if (key == 0) return;

  lastKeyTime = now;

  if (key >= '1' && key <= '5') {
    int floor = key - '0';
    requestQueue[floor] = true;
    lcd.setCursor(0, 1);
    lcd.print("Req: Floor ");
    lcd.print(floor);
    lcd.print("    ");
  }

  if (key == '*') {
    for (int i = 1; i <= 5; i++) requestQueue[i] = false;
    lcd.setCursor(0, 1);
    lcd.print("Queue Cleared   ");
  }
}

/* V.IMP - SCAN ALGORITHM
How it works 
1. If going up - look for requests above first 
2. If going down - look for requests below first
3. If nothing found in current deirection - reverse 
4. It is not like FCFS , it sweeps in one direction fully before turing around */

int getNextFloor() {

  // check current direction first
  if (direction == 1) {
    for (int f = currentFloor + 1; f <= 5; f++)
      if (requestQueue[f]) return f;
  }

  if (direction == -1) {
    for (int f = currentFloor - 1; f >= 1; f--)
      if (requestQueue[f]) return f;
  }

  // nothing in current direction, try other direction
  for (int f = currentFloor + 1; f <= 5; f++)
    if (requestQueue[f]) return f;

  for (int f = currentFloor - 1; f >= 1; f--)
    if (requestQueue[f]) return f;

  return -1;  // queue empty
  }
/* Elevator State Machine 
States and transitions:
IDLE → ACCELERATING → CRUISING → DECELERATING → DOOR_OPEN → IDLE

Trapezoidal motion profile:
1.Speed ramps UP during ACCELERATING
2.Stays constant during CRUISING
3.Ramps DOWN during DECELERATING
4.This avoids sudden jerks (smooth start and stop) */

void runElevator() {
  unsigned long now = millis();
  unsigned long elapsed = now - motionStart;
  int spd = getMotorSpeed();

  switch (elevState) {

    case IDLE: {
      int next = getNextFloor();
      if (next == -1) {
        direction = 0;
        motorStop();
        return;
      }
      targetFloor = next;
      direction   = (targetFloor > currentFloor) ? 1 : -1;
      elevState   = ACCELERATING;
      motionStart = now;
      break;
    }

    case ACCELERATING: {
      // ramp speed from 100 to full over ACCEL_TIME
      int rampSpd = map(elapsed, 0, ACCEL_TIME, 100, spd);
      rampSpd = constrain(rampSpd, 100, 255);
      (direction == 1) ? motorUp(rampSpd) : motorDown(rampSpd);

      if (elapsed >= ACCEL_TIME) {
        elevState   = CRUISING;
        motionStart = now;
      }
      break;
    }

    case CRUISING: {
      (direction == 1) ? motorUp(spd) : motorDown(spd);

      if (elapsed >= CRUISE_TIME) {
        elevState   = DECELERATING;
        motionStart = now;
      }
      break;
    }

    case DECELERATING: {
      // ramp speed from full down to 100 over DECEL_TIME
      int rampSpd = map(elapsed, 0, DECEL_TIME, spd, 100);
      rampSpd = constrain(rampSpd, 100, 255);
      (direction == 1) ? motorUp(rampSpd) : motorDown(rampSpd);

      if (elapsed >= DECEL_TIME) {
        motorStop();
        currentFloor = targetFloor;
        requestQueue[currentFloor] = false;
        setFloorLED(currentFloor);
        elevState   = DOOR_OPEN;
        motionStart = now;
        lcd.setCursor(0, 1);
        lcd.print("Door: OPEN      ");
      }
      break;
    }

    case DOOR_OPEN: {
      if (elapsed >= DOOR_TIME) {
        lcd.setCursor(0, 1);
        lcd.print("Door: CLOSED    ");
        elevState = IDLE;
      }
      break;
    }
  }
}

void setup(){
  // LEDs - embedded C
  initLEDs();
  setFloorLED(1);

  // LCD
  lcd.begin(16, 2);
  lcd.print(" Elevator  ");
  lcd.setCursor(0, 1);
  lcd.print(" Initializing.. ");
  delay(2000);
  lcd.clear();

  // motor
  pinMode(ENA, OUTPUT);
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  motorStop();

  // outside buttons - external 10k pull down, press = HIGH
  for (int i = 0; i < 4; i++) {
    pinMode(upBtns[i],   INPUT);
    pinMode(downBtns[i], INPUT);
  }

  // keypad - rows output, cols input pullup
  for (int r = 0; r < 4; r++) {
    pinMode(rowPins[r], OUTPUT);
    digitalWrite(rowPins[r], HIGH);
  }
  for (int c = 0; c < 3; c++) {
    pinMode(colPins[c], INPUT_PULLUP);
  }

  Serial.begin(9600);
  Serial.println("Elevator ready");

}
/*
Three tasks running independently in LOOP:
1. Button reading  - every 50ms
2. Elevator logic  - every loop (time driven inside)
3. LCD update      - every 300ms
*/
void loop() {
  unsigned long now = millis();

  if (now - lastBtnTime >= BTN_INTERVAL) {
    lastBtnTime = now;
    readAllButtons();
  }

  runElevator();

  if (now - lastLCDTime >= LCD_INTERVAL) {
    lastLCDTime = now;
    updateLCD();
  }
}