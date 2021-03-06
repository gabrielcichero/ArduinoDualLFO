///////////////////////////////////////////////////////////////
//
//  Dual LFO
//
//  Two channels of control voltages generated by an Arduino via PWM
//
//  There are several controls and LEDs referenced in this sketch:
//
//  2 LEDS make up an LED Bar displaying the current mode
//
//  1 Momentary switch for changing modes (single down/up increments mode)
//
//  1 Momentary switch and two 10k potentiometers for each LFO:
//
//  The switch increments through different wave tables
//  One potentiometer is for LFO frequency
//  One potentiometer is for LFO depth
//
//  Finally, the LFO outputs are on pins 3 and 11. Since the output is PWM, it's 
//  important to add low-pass filters to these pins to smooth out the resulting waveform
//
//  See accompanying Fritzing documents for circuit information.
//
//  The MIT License (MIT)
//  
//  Copyright (c) 2013 Robert W. Gallup (www.robertgallup.com)
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//  
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.
// 

#define PM_OFF   0
#define PM_FX    1
#define PM_DRONE 2
#define PM_TUNE  3

#include "avr/pgmspace.h"

// Control Framework
#include "CS_Led.h"
#include "CS_LEDBar.h"
#include "CS_Pot.h"
#include "CS_Switch.h"

// Waves
#include "noise256.h"
#include "ramp256.h"
#include "saw256.h"
#include "sine256.h"
#include "tri256.h"
#include "pulse8.h"
#include "pulse16.h"
#include "pulse64.h"
#include "sq256.h"

// Macros for clearing and setting bits
#define cbi(sfr, bit) (_SFR_BYTE(sfr) &= ~_BV(bit))
#define sbi(sfr, bit) (_SFR_BYTE(sfr) |= _BV(bit))

#define NUM_MODES 3

// I/O Devices
CS_LEDBar    modeDisplay(7, 2);         // Mode display is two pins starting on pin 7

CS_Switch    modeSwitch( 9);

CS_Pot       LFO1_DepthKnob (4);
CS_Pot       LFO1_FreqKnob (0);
CS_Switch    LFO1_WaveSwitch(10);

CS_Pot       LFO2_DepthKnob (5);
CS_Pot       LFO2_FreqKnob (1);
CS_Switch    LFO2_WaveSwitch (12);

// Interrupt frequency (16,000,000 / 510)
// 510 is divisor rather than 512 since with phase correct PWM
// an interrupt occurs after one up/down count
const float clock = 31372.5;

// LFO Wave Table Numbers
byte LFO1_WaveTableNum = 0;
byte LFO2_WaveTableNum = 0;

// Wave table pointers
byte *waveTables[] = {sine256, ramp256, saw256, tri256, pulse8, pulse16, pulse64, sq256, noise256};
#define NUM_WAVES (sizeof(waveTables) / sizeof(byte *))

// Interrupt vars are volatile
volatile byte tickCounter;               // Counts interrupt "ticks". Reset every 125  
volatile byte fourMilliCounter;          // Counter incremented every 4ms

volatile unsigned long LFO1_Base=255;     // Base frequency LFO1
volatile unsigned long accumulatorA;     // Accumulator LFO1
volatile unsigned long LFO1_TuningWord;  // Frequency DDS tuning
volatile unsigned  int LFO1_Depth;       // Frequency voltage depth
volatile byte offsetA;                   // Wave table offset

volatile unsigned long LFO2_Base=50;     // Base frequency LFO2
volatile unsigned long accumulatorB;     // Accumulator LFO2
volatile unsigned long LFO2_TuningWord;  // Volume DDS tuning
volatile unsigned  int LFO2_Depth;       // Volume voltage depth
volatile byte offsetB;                   // Wave table offset

volatile byte *LFO1_WaveTable;
volatile byte *LFO2_WaveTable;

volatile byte mode = 1;

void setup()
{

  // DEBUG ONLY
  //  Serial.begin(115200);                // connect to the serial port (for debug only)

  // PWM Pins
  pinMode(11, OUTPUT);     // pin11= PWM:A
  pinMode( 3, OUTPUT);     // pin 3= PWM:B

  // Startup eye-candy
  byte candy = 1;
  while (modeSwitch.stateDebounced() == 1) {
    modeDisplay.displayNum(candy);
    candy = 3 - candy;
    delay (100);
  }
  modeDisplay.displayNum(mode);
  while (modeSwitch.stateDebounced() == 0){
  };

  // Initialize timers
  Setup_timer2();

  // Initialize wave tables
  LFO1_WaveTable = waveTables[0];
  LFO2_WaveTable = waveTables[0];
  
  // Initialize LFO switch states
  LFO1_WaveSwitch.stateDebounced();
  LFO2_WaveSwitch.stateDebounced();

}
void loop()
{
  while(1) {

    if (fourMilliCounter > 25) {                 // Every 1/10 second
      fourMilliCounter=0;

      // Check performance mode
      byte switchState = modeSwitch.stateDebounced();
      if (modeSwitch.changed()) {
        if (switchState == 1) {
          mode = (mode+1) % NUM_MODES;
          switch (mode) {
            case PM_FX:
              LFO2_Base = 50;
              break;
              
            case PM_DRONE:
              LFO2_Base = 10;
              break;
              
            default:
              LFO2_Base = 0;
          }
        }          
        modeDisplay.displayNum(mode);
      }

      // LFO 1 wave table
      switchState = LFO1_WaveSwitch.stateDebounced();
      if (LFO1_WaveSwitch.changed()) {
        if (switchState == 1) {
          LFO1_WaveTableNum = (LFO1_WaveTableNum + 1) % NUM_WAVES;
          LFO1_WaveTable = waveTables[LFO1_WaveTableNum];
        }
      }

      // LFO 2 wave table
      switchState = LFO2_WaveSwitch.stateDebounced();
      if (LFO2_WaveSwitch.changed()) {
        if (switchState == 1) {
          LFO2_WaveTableNum = (LFO2_WaveTableNum + 1) % NUM_WAVES;
          LFO2_WaveTable = waveTables[LFO2_WaveTableNum];
        }
      }

      // LFO 1
      LFO1_TuningWord = pow(1.02, LFO1_FreqKnob.value()) + 8192;
      LFO1_Depth  = 8 - (LFO1_DepthKnob.value() >> 7);

      // LFO 2
      LFO2_TuningWord = pow(1.02, LFO2_FreqKnob.value()) + 8192;
      LFO2_Depth  = 8 - (LFO2_DepthKnob.value() >> 7);
    }

  }
}

//******************************************************************
// timer2 setup
void Setup_timer2() {

  // Prescaler 1
  sbi (TCCR2B, CS20);
  cbi (TCCR2B, CS21);
  cbi (TCCR2B, CS22);

  // Non-inverted PWM
  cbi (TCCR2A, COM2A0);
  sbi (TCCR2A, COM2A1);
  cbi (TCCR2A, COM2B0);
  sbi (TCCR2A, COM2B1);

  // Phase Correct PWM
  sbi (TCCR2A, WGM20);
  cbi (TCCR2A, WGM21);
  cbi (TCCR2B, WGM22);

  // Enable interrupt
  sbi (TIMSK2,TOIE2);
  
}

////////////////////////////////////////////////////////////////
//
// Timer2 Interrupt Service
// Frequency = 16,000,000 / 510 = 31372.5
//
ISR(TIMER2_OVF_vect) {
  unsigned long temp;

  // Count every four milliseconds
  if(tickCounter++ == 125) {
    fourMilliCounter++;
    tickCounter=0;
  }   

  // Turn everything off for performance mode 0
  if (mode == PM_OFF) {
    OCR2A = 0;
    OCR2B = 0;
    return;
  }

  // Sample wave table for LFO1 (note: Depth modulates down from sample -- good for amplitude modulation)
  accumulatorA  += LFO1_TuningWord;
  offsetA        = accumulatorA >> 24; // high order byte
  temp           = pgm_read_byte_near(LFO1_WaveTable + offsetA);
  temp           = LFO1_Base - (temp >> LFO1_Depth);
  OCR2A          = temp;

  // Sample wave table for LFO2 (note: Depth modulates up from sample -- good for frequency modulation)
  accumulatorB  += LFO2_TuningWord;
  offsetB        = accumulatorB >> 24; // high order byte
  temp           = pgm_read_byte_near(LFO2_WaveTable + offsetB);
  temp           = LFO2_Base + (temp >> LFO2_Depth);
  OCR2B          = (temp > 255) ? 255 : temp;
  
}

