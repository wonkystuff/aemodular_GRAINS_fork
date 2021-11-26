/*
	"West Coast Osc" / "Scheveningen"; an oscillator for the Ginko Synthese "Grains" module,
	by Kassen Oud. This is based on the "West Coast" style of synthesis, where
	we build up sounds starting with a simple waveform and adding harmonics, as
	opposed to starting with a harmonically rich sound and filtering that down.
	All synthesis bits written by Kassen Oud, using infrastructure borrowed from
	Peter Knight's work, which was adapted by Jan Willem before I got my hands on
	it.

LICENSE:
	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.

DESCRIPTION:
	We use two sine waves, the first we'll call the "master", the second the
	"slave". The master just tracks the pitch CV. The slave is some set
	interval above the master. In addition the slave is hard-synced to the
	master. These two are ring-modulating each other. This is followed by a
	wave-folder; a classical "West Coast" style effect which is a bit like a
	clip, except once the wave hits the maximum excursion it "folds" back
	towards the centre. Of these two effects, the ring-modulator and the
	wave-wrapper, the first is more mellow and smooth, while the second tends
	to be more up-front. By combining those, and modulating them, we can get a
	rich pallet of timbres from a relatively simple module like the Grains.

	I tried to comment all of this so people who'd like to can learn from it or
	borrow ideas. However, because we're also trying to do fairly advanced
	stuff on a modest CPU, there is some trickery going on that may be hard to
	understand for people who aren't yet familiar with the intricacies of
	digital integer math and bitwise operations. If that's you then perhaps the
	"PWM-SAW" code would be a better place to start your journey.

MANUAL:
	Knob c / cv-c:
		Offset of the slave oscillator's pitch, relative to the master's.
	Knob b / cv-b:
		Amount of wave-folding.
	Knob d / cv-d: Base Tuning.
		Sets the base pitch.
	Knob a; Pitch Modulation
		This gets added to the value set by Knob d.

CHANGELOG:
	Oct 2 2020; initial release
	Nov 26 2021: Updated for core1.æ
*/

#include "wonkystuffCommon.h"

//variables
bool     master_polarity = 0;
bool     slave_polarity = 0;
uint8_t  output = 0;
uint16_t master_phase = 0;
uint16_t phase_inc = 1;
uint16_t slave_offset_current = 1;
uint16_t slave_offset_measured = 1;
uint16_t wrap_factor = 64;
uint16_t neg_wrap_factor = 64;
uint16_t slave_phase = 0;
uint16_t master_value = 0;
uint16_t slave_value = 0;
uint32_t accumulator = 0; //I want a hardware accumulator :-(

// Map inputs
#define SLAVE_KNOB			(wsKnob3)
#define WRAP_KNOB			(wsKnob2)
#define PITCH_KNOB			(wsKnob1)
#define PITCH_CV			(wsKnob4)

void
setup(void)
{
  wsInit();                 // general initialisation
  wsInitPWM();              // We're using PWM here
  wsInitAudioLoop();        // initialise the timer to give us an interrupt at the sample rate
                            // make sure to define the wsAudioLoop function!
}

void
loop(void)
{
	//calculate the pitch
	uint16_t pwmv = min( 1023, analogRead(PITCH_CV) + max(0,analogRead(PITCH_KNOB)-600));

	//look up the phase increase per sample
	phase_inc = wsFetchOctaveLookup(pwmv);

	//Measure the slave osc's frequency offset
	//we later smooth this to avoid unintended clicks in the sound
	slave_offset_measured = analogRead( SLAVE_KNOB );

	//Measure the amoun of wave-folding needed. This actually results in 2
	//values as we use asymmetrical wave-folding to preserve more of the fundamental
	wrap_factor =  min( 64 + ( analogRead(WRAP_KNOB) >> 1 ), 511);
	neg_wrap_factor = max( wrap_factor >> 1, 64 );
}


void
wsAudioLoop(void)
{
	// write the output first to avoid jitter
	wsWriteToPWM(output);

	//increase the master phase
	master_phase += phase_inc;

	//slave osc pitch offset smoothing. Without this we'd get undesired clicks in the sound
	if (slave_offset_current < slave_offset_measured) slave_offset_current++;
	else if (slave_offset_current > slave_offset_measured) slave_offset_current--;

	//turn master into a sine, or rather; a close approximation. The proper sine function
	//is slow and practical tables are lo-fi and/or slow as progmem is so slow
	master_value = master_phase;
	master_value <<= 1;
	master_value >>= 8;
	master_value *= (~master_value) & 255;
	master_value <<= 2;
	master_value >>= 8;
	master_polarity = master_phase & 0b1000000000000000;

	//calculate the slave phase in 32bit resolution
	//multiply the phase by a 10 bit number, then go down 8 bits again
	//the result will be a 2 octave increase max.
	//On top of that we say that we want the slave to at least run at
	//the master's phase's rate.
	//Finally we truncate back into 16 bit, as though the slave phase flowed over
	accumulator = master_phase;
	accumulator *= slave_offset_current;
	accumulator >>= 8;
	accumulator += master_phase;
	accumulator &= 65535; //max 16 bit int

	slave_phase = accumulator;

	//turn slave into a sine too, like we did with the master.
	slave_value = slave_phase;
	slave_value <<= 1;
	slave_value >>= 8;
	slave_value *= (~slave_value) & 255;
	slave_value <<= 2;
	slave_value >>= 8;
	slave_polarity = slave_phase & 0b1000000000000000;

	//multiply master and slave for the ring-mod,
	//taking care to invert the phase where needed
	master_value *= slave_value;
	master_value >>= 8;
	master_polarity ^= slave_polarity;

	master_value = master_value >> 1;

	//wave-wrapping can result in frequency doubling, and we want to preserve bass
	//so we use asymmetrical wave-wrapping
	master_value *= master_polarity?wrap_factor:neg_wrap_factor;

	//bit-shifting by 8 bits is way faster than by other amounts
	//so this is faster than >>= 6, as we know the topmost bits are 0
	master_value <<= 2;
	master_value >>= 8;

	//detect whether folding will be more involved than inverting the range
	//from 128 to 254
	if (master_value & 0b1111111100000000)
	{
		//values between 255 and 511 fold back past the "0" line
		if (master_value & 0b0000000100000000) master_polarity = !master_polarity;
		//mask out bits beyond which the process just repeats
		master_value &= 0b0000000011111111;
	}
	//actual folding
	if (master_value > (uint16_t)127)
	{
		master_value = 127 - (master_value & 0b0000000001111111);
	}

	//only now do we make a bipolar signal
	if (master_polarity) output = 127 - master_value;
	else output = 128 + master_value;
}

