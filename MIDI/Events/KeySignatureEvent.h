#pragma once

#include "MIDIEvent.h"

// Meta 0x59. The score view uses this to draw the key signature and to decide whether a
// black key needs an accidental (and whether to spell it as a sharp or a flat).
struct KeySignatureEvent : public MIDIEvent
{
	// Negative values are flats, positive values are sharps (-7..7).
	int8_t sharpsFlats;
	bool minor;

	KeySignatureEvent(long tick, int8_t sf, bool mi) : MIDIEvent(tick), sharpsFlats(sf), minor(mi)
	{

	}
};
