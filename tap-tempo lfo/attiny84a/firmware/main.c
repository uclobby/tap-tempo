//
// Tap-tempo LFO for 8-bit AVR.
// 
// Copyright (C) 2013-2016 Harald Sabro
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// Contact info
// ------------
// Website: sabrotone.com
// Email: harald (AT) website 
//

//
// 
// Note: Debounce code based on article at:
//       http://www.ganssle.com/debouncing-pt2.htm
//
//       PWM DDS implementation inspired by:
//       http://interface.khm.de/index.php/lab/experiments/arduino-dds-sinewave-generator/
//
//       Rotary encoder decoding based on artivle at:
//       https://www.circuitsathome.com/mcu/rotary-encoder-interrupt-service-routine-for-avr-micros
//
// Note 2: The "divide clock by 8" fuse bit needs to be cleared for this code
//         to run at the proper speed.
//

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/atomic.h>

#include "switching.h"
#include "signaling.h"
#include "main.h"

//
// Defines and structs.
//

#define TIMER1_PRESCALER                64.0f

//
// Timer1 trigger rate in Hertz.
//

#define TIMER1_FREQUENCY                0.001f

//
// Timer1 trigger rate in milliseconds.
//

#define TIMER1_SAMPLE_RATE              1

//
// In milliseconds (i.e. 1 sec. / 1Hz)
//

#define DEFAULT_TEMPO                   1000

//
// Global variables.
//

volatile uint8_state_flags g_state;

volatile uint16_t g_tempo_ms_count;
volatile uint16_t g_mode_reset_ms_count;

extern volatile uint8_t g_base_table_index;
extern volatile uint32_t g_base_duty_cycle;
extern volatile uint32_t g_base_phase_accumulator;

extern volatile uint16_t g_speed_adjustment_ms_count;

/*====== Public functions ===================================================== 
=============================================================================*/

int main()
{
    //
    // Entry point and main loop.
    //
    
    //
    // Specify which pins are to be output pins (and consequently which ones
    // are to be input pins).
    //
    
    DDRA = (1 << WAVE_MODE_OUT) | (1 << MULTI_MODE_OUT) | (1 << LED_OUT) | (1 << TEMPO_OUT);
    DDRB = (1 << SPEED_MODE_OUT) | (1 << LFO_OUT);
    
    //
    // Enable pull-up resistors on input pins and drive output pins high.
    //
    
    PORTA = 0xff;
    PORTB = 0xff;
    
    //
    // Initialize switching.
    //
    
    InitializeSwitching();
    
    //
    // Set up the random number generator for the random waveform.
    //
    
    SeedRandomNumberGenerator(0);
    UpdateRandomNumber();
    
    //
    // Initialize signaling, including setting the default base tempo.
    //
    
    SetBaseTempo(DEFAULT_TEMPO);
    
    //
    // Disable USI and ADC to conserve power.
    //
    
    PRR = (1 << PRUSI) | (1 << PRADC);
    
    //
    // Set up Timer0 in fast PWM mode with no prescaler and a non-inverted
    // compare.
    // This timer is used to generate the LFO PWM output signal.
    //
    
    TCCR0A = (1 << COM0A1) | (1<<COM0B1);                 // Clear OC0A on compare match. Set OC0A at BOTTOM.
    TCCR0A |= (1 << WGM01) | (1 << WGM00);  // Fast-PWM (TOP == 0xff).
    TCCR0B = (1 << CS00);                   // No prescaler.
    TIMSK0 = (1 << TOIE0);                  // Timer0 overflow interrupt.
    
    //
    // Set up Timer1 to trigger an interrupt every 1ms.
    // This timer is used to trigger the tap switch sampling routine.
    //
    // Note: In compare mode the compares are done against the OCR1C register.
    //
    
    //
    // The trigger value should come out as
    // ((8,000,000 / 64.0) * 0.001) - 1 = 124
    // which is within 8-bit range (though we could have used a 16-bit compare
    // value for this particular timer).
    // 
    
    OCR1A = ((CLOCK_FREQUENCY / TIMER1_PRESCALER) * TIMER1_FREQUENCY) - 1;
    TCCR1A = 0x00;
    TCCR1B = (1 << WGM12);                  // CTC mode (TOP == OCR1A).
    TCCR1B |= (1 << CS11) | (1 << CS10);    // Prescalar of 64.
    TCNT1 = 0x0000;                         // Starting at 0.
    TIMSK1 = (1 << OCIE1A);                 // Enable timer 1A compare.
    
    //
    // Set up PA4, PA5 and PB1 (PCINT4, PCINT5 and PCINT9 respectively) for pin
    // change interrupts.
    //
    
    PCMSK0 = (1 << PCINT4) | (1 << PCINT5); // PCINT7:0 mask.
    PCMSK1 = (1 << PCINT9);                 // PCINT11:8 mask.
    GIMSK = (1 << PCIE0) | (1 << PCIE1);    // Enable pin change interrupts on
                                            // both PCINT7:0 and PCINT11:8.
    
    //
    // Enable global interrupts. No interrupts will happen without this,
    // regardless of individual flags set elsewhere.
    //
    
    sei();
    
    //
    // Main loop.
    //
    
    while (1)
    {
        //
        // Continuously poll the tap input switch.
        //
        // Note: This routine has been moved to the main loop so as not to
        //       delay the interrupt processing unnecessarily. There is enough
        //       calculation going on with the debouncing and checking that,
        //       when run inside it's ISR, the ISR that is responsible for
        //       generating the PWM/LFO signal is prevented from running at the
        //       required speed and thus starts skipping.
        //       Since we're running in main() the ISRs will take priority over
        //       the switch calculations.
        // 
        // Note 2: The code that actually updates tempo counting state etc. is
        //         wrapped in an atomic block to prevent the ISRs to modify
        //         state midways; i.e. basic multi-thread synchronization.
        //
        
        CalculateSwitchStates();
        
        if (SwitchWasClosed(1 << TAP_IN))
        {
            //
            // We've got a state change on the tap input switch pin going from
            // open to closed state, i.e. someone just stepped on the tap
            // switch.
            //  
            
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
            {
                //
                // Always reset the output signal on a manual tap.
                //
                
                if (g_state.is_counting_tempo == 0)
                {
                    ResetSignals();
                    StartTempoCount();
                }
                else
                {
                    ResetSignals();
                    StopTempoCount();
                    
                    g_state.has_received_tap_input = 1;
                }
            }
            
            //
            // Just once, use the newly entered tap tempo value to seed
            // the random number generator, so it will be different
            // each time.
            //
            
            if (g_state.has_random_seed == 0)
            {
                if (g_state.has_received_tap_input == 0)
                {
                    g_state.has_random_seed = 1;
                    
                    SeedRandomNumberGenerator(g_tempo_ms_count);
                    UpdateRandomNumber();
                }
            }
        }
        
        if (SwitchWasClosed(1 << MODE_IN))
        {
            g_state.is_counting_mode_reset_time = 1;
        }
        
        if (SwitchWasOpened(1 << MODE_IN))
        {
            //
            // We've got a state change on the mode input switch pin going from
            // open to closed state, i.e. someone just toggled the mode switch.
            //
            
            //
            // If the mode reset counter is running, and it has exceeded the
            // specified number of seconds, interpret the switch release as
            // reset of the current mode rather than a regular mode change.
            //
            
            if (g_state.is_resetting_mode == 1)
            {
                g_state.is_resetting_mode = 0;
            }
            else
            {
                g_state.is_counting_mode_reset_time = 0;
                g_mode_reset_ms_count = 0;
                
                ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
                {
                    SetNextSelectionMode();
                }
            }
        }
    }
}

//
// Timer0 overflow interrupt handler. This is where the LFO signal is
// generated.
// Frequency: 31.25kHz
//

ISR(TIM0_OVF_vect)
{
    //
    // Increase the phase accumulator by a given amount based on the required
    // output signal frequency. Then use the high 8 bits (0-255) of the phase
    // accumulator to identify what part of the wave to plot.
    //
    // We need both the base tempo (for the LED and sync output signal), and
    // the actual LFO signal with applied tempo multiplier.
    //
    // Also see PlotWaveform().
    //
    
    //
    //
    // TODO: Might be a shortcut just dividing by the given multiplier rather
    //       than keeping a second set of variables.
    //
    //
    
    g_base_phase_accumulator += g_base_duty_cycle;
    g_base_table_index = (g_base_phase_accumulator & 0xff000000) >> 24;
    
    //
    // Flag whenever there's an overflow in the base table index, i.e. the base
    // tempo, has just completed a full cycle.
    //
    
    //
    // Draw the next point on the waveform.
    //
    
    PlotWaveform();
}

//
// Timer1 compare interrupt handler. Triggers every k_timer1_frequency seconds.
// Frequency: 1kHz
//
// Note: Some code borrowed from http://www.ganssle.com/debouncing-pt2.htm.
//

ISR(TIM1_COMPA_vect)
{
    //
    // Run each switch input through a debounce routine to make sure we get rid
    // of any noise due to the switch contacts bouncing. This routine
    // DebounceSwitches all 8 input pins on PA simultaneously.
    //
    
    DebounceSwitches();
    
    //
    // Count tempo, if applicable.
    //
    
    if (g_state.is_counting_tempo == 1)
    {
        g_tempo_ms_count++; // Add a millisecond.
        
        //
        // Make sure we don't exceed the maximum tempo length / minimum LFO
        // frequency.
        //
        
        if (g_tempo_ms_count > LFO_MIN_TEMPO)
        {
            TempoCountTimeout();
        }
    }
    
    //
    // Count reset mode, if applicable.
    //
    
    if (g_state.is_counting_mode_reset_time == 1)
    {
        g_mode_reset_ms_count++;
        
        //
        // Reset current mode if enough time has passed.
        //
        
        if (g_mode_reset_ms_count >= MODE_RESET_MIN_TIME)
        {
            g_state.is_resetting_mode = 1;
            g_state.is_counting_mode_reset_time = 0;
            g_mode_reset_ms_count = 0;
            
            ResetCurrentSelectionMode();
        }
    }
    
    //
    // Keep the speed adjustment time counter topped up, if possible.
    //
    
    if (g_speed_adjustment_ms_count < 0xffff)
    {
        g_speed_adjustment_ms_count++;
    }
}

//
// Pin change interrupt handler. Handles the rotary encoder.
//

ISR(PCINT0_vect)
{
    static const int8_t encoder_table[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
    static uint8_t encoder_samples = 3;
    static int8_t encoder_value = 0;
    
    //
    // Keep sampling the four latest rotary states, as pairs of bits, in a
    // shift register. Make room for the next sample (and discard the oldest
    // one).
    //
    
    encoder_samples <<= 2;
    
    //
    // Since the current rotary samples need to be added to the shift
    // register at the start, and they do not necessarily align pin-wise, they
    // must be adjusted accordingly.
    //
    
    encoder_samples |= ((PINA & (1 << ROTARY_A_IN)) >> ROTARY_A_IN) | ((PINA & (1 << ROTARY_B_IN)) >> (ROTARY_B_IN - 1));
    
    //
    // Update the encoder value and check the result.
    //
    
    encoder_value += encoder_table[(encoder_samples & 0x0f)];
    if (encoder_value > 3)
    {
        ModifyCurrentSelectionMode(1);
        
        encoder_value = 0;
    }
    else if (encoder_value < -3)
    {
        ModifyCurrentSelectionMode(-1);
        
        encoder_value = 0;
    }
}

//
// Pin change interrupt handler. Reads input clock/sync pulses.
//

ISR(PCINT1_vect)
{
    static uint8_t previous_sync_input = 0;
    uint8_t sync_input = (PINB & (1 << SYNC_IN));

    //
    // Check for a change on the clock/sync input pin.
    //

    if (sync_input != previous_sync_input)
    {
        previous_sync_input = sync_input;

        //
        // Detect whether this is a falling or rising edge, and start or stop
        // the tempo counting accordingly.
        //
        
        if (sync_input)
        {
            //
            // See main loop.
            //

            StopTempoCount();
        }
        else
        {
            //
            // See main loop.
            //

            StartTempoCount();
        }
    }
}
