# CW Skimmer

CW Skimmer copies Morse from a TCI radio on Linux. The program you run is a Qt5 window. A C library under it talks to the radio, builds the waterfall, and still contains an older command-line detector. The copy on screen comes from the spectrum, threshold, mask, Cwsk, and DeepCW decoders in the window.

The full description of how the window behaves is [README.md](./README.md). This note is the map of that design.

## What is on screen

The middle of the window is a 48 kHz waterfall. On IQ the picture is mirrored, so the higher frequency is toward the bottom. 10 kHz is visible at a time. Copy for each signal is drawn on that signal, to the right of the waterfall. One Morse decoder sits along the bottom and opens on DeepCW.

**Start** connects to the radio at `127.0.0.1` port `50001` over WebSocket. The toolbar lamp is green **Connected** only while detection is running and that session is up. **Stream** selects IQ or demodulated receiver audio. **Training file…** and **Start playback** play a `.tcistream` instead. **Start** itself is always the live radio.

A click on a trace selects that row. The green text on the line is the dial frequency sent to the radio. The click copies that slice's text into the bottom Morse pane, and keeps decoding the same signal in both places. The signal-trace window opens unless the decoder is DeepCW. On a live radio the same click sets the dial to that signal. The green line stays on the click until the dial arrives. On IQ the station is then the center bin, which is the sidetone in the headphones and the transmit frequency. The line and the decoder move there, and the 10 kHz view follows. The speaker is a 700 Hz sidetone of the envelope drawn on the trace. Under the Morse decoder, the CW keyboard sends typed characters through the radio.

## Side decoders

The 48 kHz band is 48 slices of 1 kHz, from −24 kHz to +24 kHz. Each slice has its own spectrum decoder and follows the strongest signal inside it. A drift of about 150 Hz stays. A louder signal farther away in the slice takes over after two columns and keeps the text already decoded. A quiet slice with nothing else in it keeps its line and its copy. Settings can enable fewer than 48 slices and then keeps the ones around the center of the band.

The **Decoder** button cycles the bottom pane through Threshold, Mask, Spectrum, Cwsk, and DeepCW. It does not change the side decoders. DeepCW is the neural model: the clicked signal is turned into real audio at 800 Hz before it is decoded.

## What the C library still does

`bin/cw-skimmer` is the command-line detector. It connects with the same `cw-skimmer.conf`, can play a training file when `training_file` is set, and can report spots over telnet when `spot_enabled` is 1. The shipped config leaves spotting off. That program's Morse path is the dit/dah decoder. The Bayesian detector, validator, and spot reporter remain in `src/` for that command-line path. They are not what paints the lines beside the waterfall.

## Build

```bash
make gui
```

That produces `bin/cw-skimmer-gui`. `make` produces the command-line detector and `bin/libcwskimmer_c.so`. Quit a running window and start the new binary after a build.
