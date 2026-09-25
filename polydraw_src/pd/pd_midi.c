

//--------------------------------------------------------------------------------------------------

static HMIDIOUT hmidoplaynote = 0;
static void playnoteuninit () { if ((((long)hmidoplaynote)+1)&0xfffffffe) { midiOutClose(hmidoplaynote); hmidoplaynote = 0; } }
double myplaynote (double chn, double frq, double vol)
{
	if (hmidoplaynote == (HMIDIOUT)-1) return(-1.0);
	if (hmidoplaynote == 0)
		if (midiOutOpen(&hmidoplaynote,MIDI_MAPPER,0,0,0) != MMSYSERR_NOERROR)
			{ hmidoplaynote = (HMIDIOUT)-1; return(-1.0); }
	midiOutShortMsg(hmidoplaynote,(min(max((int)vol,0),127)<<16) +
											(min(max((int)frq,0),127)<< 8) +
											(min(max((int)chn,0),255)    ));
	return(0.0);
}