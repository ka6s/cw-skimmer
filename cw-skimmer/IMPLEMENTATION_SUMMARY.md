# How the skimmer decodes

This is the path that puts text on the screen. Operating instructions are in [README.md](./README.md) and [GUI_QUICKSTART.md](./GUI_QUICKSTART.md).

## Radio frames

A TCI session is semicolon-terminated text, then binary frames. Each frame is a 64-byte header (`src/tci_stream.h`) followed by interleaved float32 samples. For IQ, `length` is the number of float32 values, so 512 complex pairs have length 1024. The GUI worker in `gui/detectorworker.cpp` drives `src/cwskimmer_api.c`, which owns the TCI client.

**Start** clears `training_file` and connects to the radio. **Start playback** is the only control that opens a `.tcistream`.

## Waterfall

Wide mode, `spectrum_span_hz=0`, is one 1024-point FFT per 1024 samples (`src/audio_processor.c`). At 48 kHz that is about 21.3 ms per column and 46.875 Hz per bin. The band is exactly 48 kHz. High frequency is drawn at the top.

The FFT twiddle is `cos + I·sin`. The picture is the mirror of true frequency. The click frequency, the slice decoders, and the training labels are all in that same picture. The twiddle is left as it is.

Columns are stamped from the sample clock, one hop per column. Sharing one wall-clock stamp across a batch of columns collapses dits.

The window shows 10 kHz. `gui/spectrumwidget.cpp` and `gui/decodewidget.cpp` share one vertical scrollbar. Value 0 is the highest 10 kHz. The view opens centered on 0 Hz IF. Wheel up moves toward higher frequency. A band of 10 kHz or less shows the whole span and disables the scrollbar.

A vertical tune slider on the left of the waterfall uses the same IF offset as a waterfall click. Dragging previews the green marker. Mouse release, or a key change while the slider is not held, calls the click path. The range follows the spectrum span, starting at ±24 kHz, in 50 Hz steps. **Clear** on `DecodeWidget` emits `clearRequested`. The main window clears `MultiChannelDecoder` and then the widget, so the next column does not repaint the old text.

**Tools → Spectrum: Narrow 3 kHz** is the finer experiment. **Wide 48 kHz** puts the normal waterfall back.

## One decoder per kilohertz

`gui/multichanneldecoder.cpp` divides the span into slices of `kSlotHz` (1000 Hz), up to `kMaxChannels` (48). On a 48 kHz spectrum that is 48 slices from −24 kHz to +24 kHz. Slice 24 contains 0 Hz up to, but not including, 1000 Hz. A peak on an exact boundary belongs to the higher slice.

Each active slice owns a headless `SpectrumMorseWindow`. It follows the strongest peak in the slice that is at least 16 dB above the column noise, the same rise as a white trace. A drift inside about 150 Hz stays on that decoder. A louder peak farther away must lead for two columns and beat the current level by more than 1.5 dB before the slice moves. Moving keeps the text already decoded and appends what the new signal decodes. A one-column spike does not take the slice.

When the followed signal goes quiet and nothing else is in the slice, the lock and the text stay. A new peak that then holds for two columns moves the slice and keeps that text. The side line is matched by slice index and painted with `offsetToY` on the followed frequency. A shorter republish does not blank the row.

`multi_decode_channels` defaults to 48. A smaller setting keeps the center slices.

## Spectrum Morse

`gui/spectrummorsewindow.cpp` reads one waterfall column at the tuned frequency. White (key-down) is 16 dB above the noise. Once down, the key stays down until the bin falls below noise + 12.5 dB. Dit, dah, letter, and word come from the peaks of the run-length histogram. A character is published after six dahs of newer spectrum.

The bottom pane opens on DeepCW. It uses the spectrum decoder when **Decoder** says Spectrum. Threshold (`gui/thresholdmorsewindow.cpp`) is mark and space with auto WPM. Mask (`gui/maskmorsewindow.cpp`) draws dit and dah boxes on the signal trace. Cwsk reads the radio samples at the clicked frequency. DeepCW (`gui/deepcwmorsewindow.cpp`, `src/deepcw_engine.cpp`) does that too, then shifts the tone to 800 Hz and resamples to 3200 Hz for the ONNX model in `models/model.onnx`. The stitcher follows `decode_morse.py` from the DeepCW engine. The button cycles only that bottom pane.

A waterfall click calls `textForOffset` and `adoptText`, so the bottom panes start from the slice's current text. The side decoder is not cleared, and both keep receiving columns.

## Radio tune and CW keyboard

A waterfall click keeps the frequency of that row. It does not search for a nearby peak. `onSignalTraceRequested` queues the tune. On IQ the Hertz value is the waterfall center minus the picture offset. A station above the dial is drawn at a negative offset, and adding that offset walks the dial down through the bottom of the band. That same Hertz value is painted on the axis and in green on the line, in MHz, and the green text is not rewritten when the center report arrives. For receiver audio the Hertz value is the labeled frequency, center plus the offset. `src/radio_control.c` sends TCI `dds` and then `vfo` at that same value. deskHPSDR ignores `dds` and applies `vfo`. Thetis applies both, and the following `vfo` keeps the dial from being shifted a second time.

The next spectrum columns call `followRadioTune`. The line stays on the click until the dial is within about 40 Hz of the command. After a short settle the picture origin is cleared. On IQ the clicked station is now the dial carrier, and deskHPSDR has shifted that carrier onto FFT bin 0. The green line and the decoder move to that bin, and the 10 kHz view follows it. The waterfall does not blank the center bins. It does not lock to a nearby peak, and it does not tune again. `bin/cw-skimmer.log` records `Click tune` and `Follow` with the power in bin 0. A click within about 30 Hz does not tune. Training playback does not tune. Fine-tune and AFC do not tune. AFC peak-lock starts off. DeepCW uses the same click and keeps the signal-trace window closed. The baseband tap stays armed, and the tone is retuned to bin 0.

The program does not add a sidetone of its own. deskHPSDR already places the CW carrier on the IQ center, which is the sidetone in the headphones and the transmit frequency. Demodulated receiver audio keeps the clicked pitch.

`gui/cwkeyboard.cpp` sits under the Morse group. Typed text is queued on the detection thread and sent as TCI `cw_macros`, or as Kenwood `KY` when `cw_send=ky` and `cat_port` is set. Stop sends `cw_macros_stop` and drops the local queue. The radio should already be in CW. No live radio was used to check this path.

## Color

`gui/cwcopyformat.cpp` is shared by the three bottom panes and the side rows. CQ is green (`#00ff66`) wherever those two letters sit together. A call sign is red (`#ff3333`) once that exact string appears at least twice in that buffer. Punctuation and a slash split a run. Red is painted over green. The pane's ordinary color is painted first, then the marks, so the caret color does not stick.

Side text is Courier New 11 pt. The full string is kept. The painted line is a window with the newest character in the rightmost column. The horizontal bar under the copy, or Shift+wheel, scrolls earlier text.

## Monitor audio

`gui/audiomonitor.cpp` plays a 700 Hz tone gated by the signal-trace envelope. It is not an IQ mixer. A 20 ms timer writes the latest amplitude. Playback resumes after an underrun, and it writes only the bytes the device can take unless the buffer is starved.

## Training files

`tools/cw_training_gen.c` writes `training/set_01` through `set_10`. Each file is 40 seconds of complex baseband at 48 kHz, centered at 14.074 MHz. Carriers in one file are at least 2 kHz apart. The text is dictionary English, at most 127 characters in the `cw_train` preamble. Levels run from about 18 dB above the displayed noise to a carrier amplitude of 0.20. Every tone starts at 0.25 seconds and stops on the last whole word before the closing half second. Expected copy is `training/decoder_labels.tsv`.

The large `.tcistream` files are generated locally and are not part of the git tree. `make training-gen` builds the generator. `make training-data` writes the files.
