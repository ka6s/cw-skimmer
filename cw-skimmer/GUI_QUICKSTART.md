# CW Skimmer window

Build and run the Qt5 window. The behavior of the waterfall and the decoders is described in [README.md](./README.md).

## Requirements

- Linux
- GCC and G++ (C11 and C++17)
- Qt5 (`qtbase5-dev`, `qtmultimedia5-dev`, `qt5-qmake`)
- libwebsockets

## Build

From this directory:

```bash
make gui
```

The binary is `bin/cw-skimmer-gui`. Quit any window that is already open, then start that binary. A window left running keeps the previous build.

`make` alone builds the command-line detector, `bin/cw-skimmer`, and `bin/libcwskimmer_c.so`. `install.sh` installs that command-line program. It does not install the window.

## Run

```bash
./bin/cw-skimmer-gui
```

The window reads `cw-skimmer.conf` from the working directory, then from the directory of the binary, then from its parent. The shipped file uses:

- `radio_host=127.0.0.1`
- `radio_port=50001`
- `radio_protocol=websocket`
- `center_frequency=14074000`
- `sample_rate=48000`
- `spectrum_span_hz=0` (wide 48 kHz waterfall)
- `multi_decode_channels=48`
- `spot_enabled=0`
- `training_file=` empty, so **Start** is the live radio

## Toolbar

- **Start** opens the live radio and clears any training file from that session.
- **Stop** ends the radio session or a training playback.
- The lamp is green **Connected** only while detection is running and the TCI session is up.
- **Settings** edits the radio, the detection floor, and how many 1 kHz slices are decoded (1–48).
- **Clear** wipes the waterfall, the side copy, and the bottom Morse panes.
- **Training file…** chooses a `.tcistream` and does not start it.
- **Start playback** plays that file. **Start** does not.
- **Play capture…** replays a saved `.wav` or `.cwtrace` through the signal trace and the bottom decoder.
- **Stream** switches the radio between IQ and demodulated audio.
- **Decoder** cycles the bottom pane: Threshold, then Mask, then Spectrum. The program opens on Spectrum. The lines beside the waterfall stay on Spectrum.
- **Audio out** and **Vol** choose the speaker for the 700 Hz sidetone. Sound plays only while detection is running and the signal trace is open on a frequency.

## Using the window

The waterfall shows 10 kHz. The scrollbar to the right of the spectrum and the copy moves both together. The wheel does the same. Wheel up moves toward higher frequency. The view opens centered on the VFO.

Click a trace to open the signal trace and to copy that slice's text into the bottom Morse decoder. Both windows keep decoding. **Record**, above the waterfall, saves the trace envelope.

CQ is green. A call sign turns red after the same call appears twice in that decoder's text.
