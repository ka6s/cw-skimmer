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

**Tools → Spectrum: Narrow 3 kHz** is the finer experiment. **Wide 48 kHz** puts the normal waterfall back.

## One decoder per kilohertz

`gui/multichanneldecoder.cpp` divides the span into slices of `kSlotHz` (1000 Hz), up to `kMaxChannels` (48). On a 48 kHz spectrum that is 48 slices from −24 kHz to +24 kHz. Slice 24 contains 0 Hz up to, but not including, 1000 Hz. A peak on an exact boundary belongs to the higher slice.

Each active slice owns a headless `SpectrumMorseWindow`. It follows the strongest peak in the slice that is at least 16 dB above the column noise, the same rise as a white trace. A drift inside about 150 Hz stays on that decoder. A louder peak farther away must lead for two columns and beat the current level by more than 1.5 dB before the slice moves. Moving keeps the text already decoded and appends what the new signal decodes. A one-column spike does not take the slice.

When the followed signal goes quiet and nothing else is in the slice, the lock and the text stay. A new peak that then holds for two columns moves the slice and keeps that text. The side line is matched by slice index and painted with `offsetToY` on the followed frequency. A shorter republish does not blank the row.

`multi_decode_channels` defaults to 48. A smaller setting keeps the center slices.

## Spectrum Morse

`gui/spectrummorsewindow.cpp` reads one waterfall column at the tuned frequency. White (key-down) is 16 dB above the noise. Once down, the key stays down until the bin falls below noise + 12.5 dB. Dit, dah, letter, and word come from the peaks of the run-length histogram. A character is published after six dahs of newer spectrum.

The bottom pane uses the same decoder when **Decoder** says Spectrum. Threshold (`gui/thresholdmorsewindow.cpp`) is mark and space with auto WPM. Mask (`gui/maskmorsewindow.cpp`) draws dit and dah boxes on the signal trace. The button cycles only that bottom pane.

A waterfall click calls `textForOffset` and `adoptText`, so the bottom panes start from the slice's current text. The side decoder is not cleared, and both keep receiving columns.

## Color

`gui/cwcopyformat.cpp` is shared by the three bottom panes and the side rows. CQ is green (`#00ff66`) wherever those two letters sit together. A call sign is red (`#ff3333`) once that exact string appears at least twice in that buffer. Punctuation and a slash split a run. Red is painted over green. The pane's ordinary color is painted first, then the marks, so the caret color does not stick.

Side text is Courier New 11 pt. The full string is kept. The painted line is a window with the newest character in the rightmost column. The horizontal bar under the copy, or Shift+wheel, scrolls earlier text.

## Monitor audio

`gui/audiomonitor.cpp` plays a 700 Hz tone gated by the signal-trace envelope. It is not an IQ mixer. A 20 ms timer writes the latest amplitude. Playback resumes after an underrun, and it writes only the bytes the device can take unless the buffer is starved.

## Training files

`tools/cw_training_gen.c` writes `training/set_01` through `set_10`. Each file is 40 seconds of complex baseband at 48 kHz, centered at 14.074 MHz. Carriers in one file are at least 2 kHz apart. The text is dictionary English, at most 127 characters in the `cw_train` preamble. Levels run from about 18 dB above the displayed noise to a carrier amplitude of 0.20. Every tone starts at 0.25 seconds and stops on the last whole word before the closing half second. Expected copy is `training/decoder_labels.tsv`.

The large `.tcistream` files are generated locally and are not part of the git tree. `make training-gen` builds the generator. `make training-data` writes the files.
