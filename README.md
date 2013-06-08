JACK MIDI LATENCY TEST
======================

A small utility that measures round-trip latency of [JACK](http://jackaudio.org/)
MIDI events.

Usage
-----

Use a cable to close the loop: physically connect MIDI-out of the soundcard
to MIDI-in of the soundcard.

Start `jack_midi_latency` on a terminal and connect its jack-ports to the
corresponding harware ports. 


See Also
--------

[ALSA sequencer latency test](http://github.com/koppi/alsa-midi-latency-test)
