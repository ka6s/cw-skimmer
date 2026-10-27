# CW Skimmer

CW Skimmer copies Morse from a TCI radio on Linux. The program is a Qt5 window on a C detector. A 48 kHz waterfall fills the middle of the window. Copy for each signal sits on that signal, and one Morse decoder sits along the bottom.

The radio default is `127.0.0.1` port `50001`, protocol `websocket`. **Start** opens that live session and leaves `training_file` empty. The toolbar lamp is green **Connected** only while detection is running and the TCI session is up. Otherwise it is dark red **Not connected**. **Stream** selects IQ (complex baseband, the normal ±24 kHz waterfall) or demodulated receiver audio.

## Waterfall

Wide mode is the working spectrum. It is one 1024-point FFT per 1024 samples, about 21.3 ms per column at 48 kHz. Each bin is 46.875 Hz, and the band is exactly 48 kHz. On IQ the picture is mirrored, so the higher frequency is toward the bottom. The window shows 10 kHz at a time and opens centered on the VFO. One vertical scrollbar, and the wheel over either the waterfall or the copy, moves both together. Wheel up moves toward the top of the picture. When the band is 10 kHz or narrower, the scrollbar is disabled and the whole span is shown.

A tune slider sits on the left of the waterfall. Its top is the positive picture offset, and each step is 50 Hz. On IQ that end is the lower frequency. Dragging moves the green trace marker. Releasing the slider, or a key press on it, retunes the signal trace the same way a click on the waterfall does. The readout above the slider is the dial frequency for that offset. The handle follows a waterfall click, the trace fine-tune, and AFC when AFC is on. It stays put while you are dragging it. The status line shows that same frequency as **RF**. Until a trace is selected, the status line does not show a frequency. On an IQ stream the radio dial is set to the clicked frequency. The waterfall is mirrored, so a station above the dial is drawn below the center. The dial, the axis, and the green text are the center minus the picture offset. The axis is megahertz to five decimal places, in the same bright face as the copy beside the waterfall. Adding the offset walks the dial down through the bottom of the band. The green line stays on the click until the dial arrives. On IQ that station is then the center bin, the carrier heard at the sidetone and the frequency the radio transmits. The line and the decoder move to that center. The 10 kHz view follows it. AFC peak-lock starts off.

The FFT twiddle is `cos + I·sin`. The picture on screen is the mirror of true frequency, and the decoders, the click frequency, and the training labels are all built on that picture. The twiddle stays as it is.

**Tools → Spectrum: Narrow 3 kHz** is a finer experiment around the VFO. **Wide 48 kHz** restores the normal waterfall. Columns are stamped from the sample clock (one hop per column), not from the wall clock. A batch of columns that shared one timestamp would collapse dits.

## Copy beside the waterfall

The band is 48 slices of 1 kHz, from −24 kHz to +24 kHz. Each slice has its own spectrum decoder and follows the strongest signal inside it. A signal is strong enough to take a slice when it is at least 16 dB above that column's noise, the same rise that paints a white trace. A drift of about 150 Hz stays on the same decoder. A louder signal farther away in the slice takes over after it has been the strongest for two columns, and it has to beat the current signal by more than 1.5 dB. A single-column spike does not move the slice.

When the signal stops and nothing else is in the slice, the line stays on the last frequency and the text stays. A new signal that then holds for two columns moves the line, and the text already decoded stays with it. New characters are added after that text.

The side text is Courier New 11 pt, with no frequency label. The line is locked to the trace. The full decoder string is kept. Characters enter on the right and move left. The horizontal bar under the copy, or Shift+wheel, scrolls back through earlier text. A shorter republish does not blank a line.

**Clear** at the top right of that copy wipes the side text and starts those slice decoders over. The waterfall and the bottom Morse panes stay as they are. The toolbar **Clear** still clears all of them.

**Settings** can run fewer than 48 slices (1–48). A lower number keeps the slices around the center of the band.

## Morse decoder

The bottom pane opens on **DeepCW**. The **Decoder** button cycles Threshold, then Mask, then Spectrum, then Cwsk, then DeepCW. That button changes only the bottom pane. The copy beside the waterfall always uses its own spectrum decoders.

- **Spectrum** reads the waterfall at the tuned frequency. White is key-down (16 dB above the noise). Once down, the key stays down until the bin falls below noise + 12.5 dB. Dit, dah, letter, and word come from the peaks of the run-length histogram. A character is copied after six dahs of newer spectrum, so the mark is already drawn before the letter appears.
- **Threshold** is mark and space against an auto threshold, with auto WPM.
- **Mask** draws dit and dah boxes on the signal-trace scope.
- **Cwsk** Vertibi based channel decoder. It reads the radio samples at the frequency you clicked, through its own filter and element model. It does not use the waterfall picture. Click a signal with the trace window open, then choose Cwsk. The side copy is unchanged.
- **DeepCW** is the neural Morse model from the [DeepCW engine](https://github.com/e04/deepcw-engine). It reads real audio, not IQ. The clicked tone is shifted to 800 Hz, low-passed, and resampled to 3200 Hz, which is the middle of the model's 400–1200 Hz band. That band is the same job as the audio waterfall on [DeepCW](https://github.com/e04/web-deep-cw-decoder). Click a signal, then choose DeepCW. That click does not open the signal-trace window. The side copy is unchanged. The model file is `models/model.onnx` (AGPL-3.0).

A click on the waterfall selects that row. The green line is drawn on the row under the pointer, not on a nearby peak. The green text on the line is the dial frequency sent to the radio, in MHz to 1 Hz. It is set when the line moves and stays put when the radio reports a new center, so it can be compared with the dial. The click also copies the text already decoded in that 1 kHz slice into the bottom Morse pane, and keeps decoding the same signal in both places. The signal-trace window opens for every decoder except DeepCW. The side line stays. Closing the trace leaves the side copy where it is.

On a live radio that click also sets the dial, so the operator hears the signal. The command is TCI `dds`, then `vfo`, both at the same frequency. The axis, the status **RF**, the slider, and the green text are the dial frequency. On IQ that is the center minus the picture offset. When the radio reports the new center, that station is the center bin of the IQ. The green line and the decoder move there, and the 10 kHz view follows. That bin is the sidetone in the headphones and the transmit frequency. The dial stays on the commanded frequency. The click does not tune a second time. A click already within about 30 Hz of the middle does not retune. Training playback does not retune. The trace fine-tune and AFC do not move the radio. AFC peak-lock starts off.

deskHPSDR places the CW carrier on the IQ center: the oscillator is one sidetone off the dial, and the IQ stream is shifted by that sidetone. This program does not add a pitch of its own, and it does not blank that center bin. When the dial arrives, the selected signal, the green line, and the decoder are that center. Put the radio in CW before the click. The tune does not change mode.

In receiver-audio mode the dial is set to the labeled frequency, center plus the picture offset. On USB or CWU, raising the VFO lowers the audio pitch. LSB can step the other way.

## CW keyboard

A keyboard sits under the Morse decoder. Characters typed there are sent by the radio. The default is TCI `cw_macros`. Set `cw_send=ky` and `cat_port` to use Kenwood TS-2000 `KY` on that CAT socket instead. That socket is Thetis CAT, or deskHPSDR through rigctld. The built-in deskHPSDR TS-2000 port does not implement KY. `cat_port` 0 stays on TCI. Enter adds a word space and sends now. Backspace drops a character that has not been sent. Esc or **Stop** aborts. The spinner sets `cw_keyer_speed` and `cw_macros_speed`, from 5 to 60 WPM. The radio should already be in CW. Nothing is sent while detection is stopped or while a training file is playing.

F1 through F5 sit between the decoder Clear button and the keyboard. Each key has a text box. Pressing that function key, or its button, sends the text in the box through the same path. The five messages are saved for the next launch.

In every decoder pane, CQ is green (`#00ff66`) wherever those two letters sit together. A call sign turns red (`#ff3333`) once that same call appears at least twice in that buffer. Both copies go red, including inside a run with no word spaces. A slash or other punctuation splits a run. Ordinary text is light. The pane color is painted first, then the green and red marks, so one red call does not paint the rest of the line red.

## Signal trace and speaker

The trace window follows the clicked signal. AFC peak-lock starts off. With it off, the line stays where the dial follow left it. **Record** saves that trace's envelope. The speaker is a 700 Hz sidetone of the envelope drawn on the trace, keyed from the newest power against the trace threshold. It is not a mixer tuned to the carrier. **Audio out** chooses the device, and **Vol** sets the level. Sound plays only while detection is running and the trace window is open on a signal.

## Training

**Training file…** chooses a `.tcistream` and does not start. **Start playback** plays it. **Start** is the live radio. **Stop** ends either one. A file that runs to the end logs "Training playback finished."

`tools/cw_training_gen.c` writes those files. `make training-gen` builds it, and `make training-data` writes `training/set_01` through `set_10`. Each file is 40 seconds at 48 kHz, complex baseband at 14.074 MHz, spanning ±24 kHz. Carriers in one file are at least 2 kHz apart. The text is dictionary English. Each tone starts at 0.25 seconds and is keyed until the last whole word that fits before the closing half second. Level moves from barely detectable (about 18 dB above the displayed noise, just over the white-trace test) to full strength. Full strength is a carrier amplitude of 0.20. The `cw_train` text is at most 127 characters. Ground truth is `training/decoder_labels.tsv`.

## Build

Qt5, GCC, and libwebsockets.

```bash
make gui
```

That builds `bin/cw-skimmer-gui`. Quit a running window and start that binary again after a build. `make` builds the command-line detector and `bin/libcwskimmer_c.so`. `make test`, `make replay`, and `make synth` build the older detector checks.

`cw-skimmer.conf` holds the radio, the center frequency (14074000 Hz), the sample rate (48000), `spectrum_span_hz=0` for the wide waterfall, and `multi_decode_channels=48`. `spot_enabled=0`. An empty `training_file` means the live radio. `cw_send=tci` and `cw_wpm=20` are the keyboard defaults. `cat_port=0` leaves Kenwood KY off.

The command-line program still contains the earlier detector, dit/dah decoder, validator, and optional telnet spotting. The copy in the window comes from the spectrum, threshold, mask, Cwsk, and DeepCW decoders above. `make ort` fetches ONNX Runtime into `third_party/onnxruntime` if it is not already there. `make deepcw-test` decodes a synthetic tone with the engine.
