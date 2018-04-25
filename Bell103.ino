/**
  Bell103 - An Arduino-based Bell 103-compatible modem (300 baud)

  Copyright (C) 2018 Costin STROIE <costinstroie@eridu.eu.org>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

  Bell 103:
    300 baud
    8 data bits
    1 bit start
    1 bit stop
    0 parity bits
*/

#include "fifo.h"

// Use the PWM DAC (8 bits, one output PIN, uses Timer2) or
// the resistor ladder (4 bits, 4 PINS)
#define PWM_DAC

// The PWM pin may be 3 or 11 (Timer2)
#define PWM_PIN 3

// CPU frequency correction for sampling timer
#define F_COR (-120000L)

// Sampling frequency
#define F_SAMPLE  9600
#define BAUD      300
#define DATABITS  8
#define STARTBITS 1
#define STOPBITS  1
// Mark and space
enum BIT {SPACE, MARK, NONE};

// Mark and space, TX and RX frequencies
const uint16_t txSpce = 1070;  // 934uS  14953CPU
const uint16_t txMark = 1270;  // 787uS  12598CPU
const uint16_t rxSpce = 2025;  // 493uS   7901CPU
const uint16_t rxMark = 2225;  // 449uS   7191CPU

// Here are only the first quarter wave samples
const uint8_t wvSmpl[] = {
  0x80, 0x83, 0x86, 0x89, 0x8c, 0x8f, 0x92, 0x95,
  0x98, 0x9b, 0x9e, 0xa2, 0xa5, 0xa7, 0xaa, 0xad,
  0xb0, 0xb3, 0xb6, 0xb9, 0xbc, 0xbe, 0xc1, 0xc4,
  0xc6, 0xc9, 0xcb, 0xce, 0xd0, 0xd3, 0xd5, 0xd7,
  0xda, 0xdc, 0xde, 0xe0, 0xe2, 0xe4, 0xe6, 0xe8,
  0xea, 0xeb, 0xed, 0xee, 0xf0, 0xf1, 0xf3, 0xf4,
  0xf5, 0xf6, 0xf8, 0xf9, 0xfa, 0xfa, 0xfb, 0xfc,
  0xfd, 0xfd, 0xfe, 0xfe, 0xfe, 0xff, 0xff, 0xff
};

// Count samples for quarter, half and full wave
const uint8_t   wvPtsQart = sizeof(wvSmpl) / sizeof(*wvSmpl);
const uint8_t   wvPtsHalf = wvPtsQart + wvPtsQart;
const uint16_t  wvPtsFull = wvPtsHalf + wvPtsHalf;

// Wave index steps for SPACE, MARK and NONE bits
const uint8_t wvStep[] = {
  (wvPtsFull * (uint32_t)txSpce + F_SAMPLE / 2) / F_SAMPLE,
  (wvPtsFull * (uint32_t)txMark + F_SAMPLE / 2) / F_SAMPLE,
  0
};

// Number of samples for each serial bit
const uint8_t txSamplesMax  = F_SAMPLE / BAUD;
// Samples counter for each serial bit
uint8_t       txSamples     = 0;

// Number of maximum carrier bits in preamble and trail
const uint8_t txCarrierMax  = 240;  // 800ms
// Carrier bits counter in preamble and trail
uint8_t       txCarrierBits = 0;


// States in TX state machine
enum TX_STATE {PREAMBLE, START_BIT, DATA_BIT, STOP_BIT, TRAIL, OFFLINE};

// Transmission related data
struct TX_t {
  uint8_t onair = 0;        // TX enabled or disabled
  uint8_t state = OFFLINE;  // TX state (TX_STATE enum)
  uint8_t data  = 0;        // currently transmitting byte
  uint8_t bit   = NONE;     // currently transmitting bit
  uint8_t bits  = 0;        // already transmitted bits
  uint8_t idx   = 0;        // wave samples index (start with first sample)
  uint8_t step  = 0;        // wave samples increment step
} tx;

// FIFO
FIFO txFIFO(6);


/**
  Linear interpolation

  @param v0 previous value (8 bits)
  @param v1 next value     (8 bits)
  @param t                 (4 bits)
  @result interpolated value
*/
uint8_t lerp(uint8_t v0, uint8_t v1, uint8_t t) {
  return ((int16_t)v0 * (0x10 - t) + (int16_t)v1 * t) >> 4;
}

/**
  Get an instant wave sample

  @param idx the wave index (0..wvPtsFull-1)
*/
inline uint8_t wvSample(uint8_t idx) {
  // Work on half wave
  uint8_t hIdx = idx & 0x7F;
  // Check if in second quarter wave
  if (hIdx >= wvPtsQart)
    // Descending slope
    hIdx = wvPtsHalf - hIdx - 1;
  // Get the value
  uint8_t result = wvSmpl[hIdx];
  // Check if in second half wave
  if (idx >= wvPtsHalf)
    // Under X axis
    result = 0xFF - result;
  return result;
}

/**
  Send the sample to the DAC

  @param sample the sample to output to DAC
*/
inline void txDAC(uint8_t sample) {
#ifdef PWM_DAC
  // Use the 8-bit PWM DAC
#if PWM_PIN == 11
  OCR2A = sample;
#else
  OCR2B = sample;
#endif //PWM_PIN
#else
  // Use the 4-bit resistor ladder DAC
  PORTD = (sample & 0xF0) | _BV(3);
#endif //PWM_DAC
}

/**
  TX workhorse.  This function is called by ISR for each output sample.
  Immediately after starting, it gets the sample value and sends it to DAC.
*/
void txHandle() {
  // First thing first: get the sample
  uint8_t sample = wvSample(tx.idx);

  // Check if we are transmitting
  if (tx.onair > 0) {
    // Output the sample
    txDAC(sample);
    // Step up the index for the next sample
    tx.idx += wvStep[tx.bit];

    // Check if we have sent all samples for a bit.
    if (txSamples++ >= txSamplesMax) {
      // Reset the samples counter
      txSamples = 0;

      // One bit finished, check the state and choose the next bit and state
      switch (tx.state) {
        // We have been offline, prepare the transmission
        case OFFLINE:
          tx.state  = PREAMBLE;
          tx.bit    = MARK;
          tx.idx    = 0;
          txCarrierBits = 0;
          // Get data from FIFO, if any
          if (not txFIFO.empty())
            tx.data = txFIFO.out();
          break;

        // We are sending the preamble carrier
        case PREAMBLE:
          // Check if we have sent the carrier long enough
          if (++txCarrierBits >= txCarrierMax) {
            // Carrier sent, go to the start bit
            tx.state  = START_BIT;
            tx.bit    = SPACE;
          }
          break;

        // We have sent the start bit, go on with data bits, LSB first
        case START_BIT:
          tx.state  = DATA_BIT;
          tx.bit    = tx.data & 0x01;
          tx.data   = tx.data >> 1;
          tx.bits   = 0;
          break;

        // We are sending the data bits, keep sending until MSB
        case DATA_BIT:
          // Check if we have sent all the bits
          if (++tx.bits < DATABITS) {
            // Keep sending the data bits, LSB to MSB
            tx.bit  = tx.data & 0x01;
            tx.data = tx.data >> 1;
          }
          else {
            // We have sent all the data bits, go on with the stop bit
            tx.state  = STOP_BIT;
            tx.bit    = MARK;
          }
          break;

        // We have sent the stop bit, try to get the next byte, if any
        case STOP_BIT:
          // Check the TX FIFO
          if (txFIFO.empty()) {
            // No more data to send, go on with the trail carrier
            tx.state  = TRAIL;
            tx.bit    = MARK;
            txCarrierBits = 0;
          }
          else {
            // There is still data, get one byte and return to the start bit
            tx.state  = START_BIT;
            tx.bit    = SPACE;
            tx.data   = txFIFO.out();
          }
          break;

        // We are sending the trail carrier
        case TRAIL:
          // Check if we have sent the trail carrier long enough
          if (++txCarrierBits > txCarrierMax) {
            // Disable TX and go offline
            tx.onair  = 0;
            tx.state  = OFFLINE;
          }
          else if (txCarrierBits == txCarrierMax) {
            // After the last trail carrier bit, send isoelectric line
            tx.bit    = NONE;
            // Prepare for the future TX
            tx.idx    = 0;
            txSamples = 0;
          }
          // Still sending the trail carrier, check the TX FIFO again
          else if (not txFIFO.empty()) {
            // There is new data in FIFO, start over
            tx.state  = START_BIT;
            tx.bit    = SPACE;
            tx.data   = txFIFO.out();
          }
          break;
      }
    }
  }
}

void checkSerial() {
  // Check any data on serial port
  uint8_t c = Serial.peek();
  if (c != 0xFF) {
    // There is data on serial port
    if (not txFIFO.full()) {
      // FIFO not full, we can send the data
      c = Serial.read();
      txFIFO.in(c);
      // Keep transmitting
      tx.onair = 1;
    }
  }
}

// ADC Interrupt vector
ISR(ADC_vect) {
  TIFR1 = _BV(ICF1);
  txHandle();
}

/**
  Main Arduino setup function
*/
void setup() {
  Serial.begin(9600);

  // TC1 Control Register B: No prescaling, WGM mode 12
  TCCR1A = 0;
  TCCR1B = _BV(CS10) | _BV(WGM13) | _BV(WGM12);
  // Top set for 9600 baud
  ICR1 = ((F_CPU + F_COR) / F_SAMPLE) - 1;

  // Vcc with external capacitor at AREF pin, ADC Left Adjust Result
  ADMUX = _BV(REFS0) | _BV(ADLAR);

  // Analog input A0
  // Port C Data Direction Register
  DDRC  &= ~_BV(0);
  // Port C Data Register
  PORTC &= ~_BV(0);
  // Digital Input Disable Register 0
  DIDR0 |= _BV(0);

  // Timer/Counter1 Capture Event
  ADCSRB = _BV(ADTS2) | _BV(ADTS1) | _BV(ADTS0);
  // ADC Enable, ADC Start Conversion, ADC Auto Trigger Enable,
  // ADC Interrupt Enable, ADC Prescaler 16
  ADCSRA = _BV(ADEN) | _BV(ADSC) | _BV(ADATE) | _BV(ADIE) | _BV(ADPS2);


#ifdef PWM_DAC
  // Set up Timer 2 to do pulse width modulation on the PWM PIN
  // Use internal clock (datasheet p.160)
  ASSR &= ~(_BV(EXCLK) | _BV(AS2));

  // Set fast PWM mode  (p.157)
  TCCR2A |= _BV(WGM21) | _BV(WGM20);
  TCCR2B &= ~_BV(WGM22);

#if PWM_PIN == 11
  // Configure the PWM PIN 11 (PB3)
  PORTB &= ~(_BV(PORTB3));
  DDRD  |= _BV(PORTD3);
  // Do non-inverting PWM on pin OC2A (p.155)
  // On the Arduino this is pin 11.
  TCCR2A = (TCCR2A | _BV(COM2A1)) & ~_BV(COM2A0);
  TCCR2A &= ~(_BV(COM2B1) | _BV(COM2B0));
  // No prescaler (p.158)
  TCCR2B = (TCCR2B & ~(_BV(CS12) | _BV(CS11))) | _BV(CS10);
  // Set initial pulse width to the first sample.
  OCR2A  = wvSmpl[0];
#else
  // Configure the PWM PIN 3 (PD3)
  PORTD &= ~(_BV(PORTD3));
  DDRD  |= _BV(PORTD3);
  // Do non-inverting PWM on pin OC2B (p.155)
  // On the Arduino this is pin 3.
  TCCR2A = (TCCR2A | _BV(COM2B1)) & ~_BV(COM2B0);
  TCCR2A &= ~(_BV(COM2A1) | _BV(COM2A0));
  // No prescaler (p.158)
  TCCR2B = (TCCR2B & ~(_BV(CS12) | _BV(CS11))) | _BV(CS10);
  // Set initial pulse width to the first sample.
  OCR2B  = wvSmpl[0];
#endif // PWM_PIN
#else
  // Configure resistor ladder DAC
  DDRD |= 0xF8;
#endif
}

/**
  Main Arduino loop
*/
void loop() {
  checkSerial();
}
