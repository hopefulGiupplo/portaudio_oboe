Portaudio implementation for android using Oboe.

In order to use this implementation correctly, be sure to include the "portaudio.h" and "pa_oboe.h"
headers in your project.

Building
----
To build portaudio with Oboe an android NDK is needed to crosscompile it.

TODO:
----
  - Testing. This implementation was non-extensively tested for VoIP calls and blocking streams - for
    everything else, it should have a decent structure.

  - Implementing onErrorAfterClose in a way that works, and checking the callback methods.

Misc
----
Latency and Sharing Mode:
Using LowLatency and SharingMode Exclusive is possible, but has yet to be implemented a function in
pa_oboe.h that sets said flags, so you'll have to manually set those properties in the
OboeEngine::OpenStream function.


Audio Format:
If you need to select a specific audio format, you'll have to manually set it in PaOboe_OpenStream
by modifying the format selection marked with a FIXME.
I'm positive that automatic format selection is possible, but simply using
PaUtil_SelectClosestAvailableFormat will not get you anywhere.


Buffer sizes:
Portaudio often tries to get approximately low buffer sizes, and if you need specific sizes for your
buffer you should manually modify it (or make a simple function that can set it). For your convenience,
there is a FIXME as a bookmark.


Device selection and/or switching mid-stream:
Device selection can be handled by a java/kotlin method that uses getDevices() in order to identify
which device to select. Switching mid-stream gives an oboe::ErrorDisconnected result, and you'll have
to stop, close and reopen the involved streams with an unspecified device (or a specific device if
you know its ID).