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
- **Decoder** cycles the bottom pane: Threshold, then Mask, then Spectrum, then Cwsk, then DeepCW. The program opens on DeepCW. The lines beside the waterfall stay on Spectrum. DeepCW needs the model in `models/` and ONNX Runtime (`make ort` downloads it).
- **Audio out** and **Vol** choose the speaker for the 700 Hz sidetone. Sound plays only while detection is running and the signal trace is open on a frequency.

## Using the window

The waterfall shows 10 kHz. The scrollbar to the right of the spectrum and the copy moves both together. The wheel does the same. Wheel up moves toward the top of the picture. On IQ the higher frequency is toward the bottom. The view opens centered on the VFO.

The tune slider on the left of the waterfall moves the green trace. On IQ the top of the slider is the lower frequency. Dragging moves the marker, and releasing it retunes the signal trace the same way a click does. **Clear** on the copy beside the waterfall wipes that copy only. The toolbar **Clear** still wipes the waterfall, the side copy, and the bottom Morse panes.

Click a trace to copy that slice's text into the bottom Morse decoder. Both keep decoding. The green line sits on the row you clicked. The green text on that line is the dial frequency sent to the radio, in MHz. The signal-trace window opens unless the decoder is DeepCW. On a live IQ radio the dial moves to that frequency. The green line stays on the click until the dial arrives. The station is then the center bin, the sidetone in the headphones and the transmit frequency. The line and the decoder move there, and the 10 kHz view follows. On IQ the frequency is the center minus the picture offset, so a click below the center tunes up. AFC peak-lock starts off. **Record**, above the waterfall, saves the trace envelope.

The **CW Keyboard** under the Morse decoder sends what you type. **Stop** there aborts the send. Put the radio in CW first. Training playback does not transmit.

CQ is green. A call sign turns red after the same call appears twice in that decoder's text.
