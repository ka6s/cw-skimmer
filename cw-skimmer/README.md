# CW Skimmer

CW Skimmer copies Morse from a TCI cable radio (HPSDR-RS, Thetis, DeskHPSDR and maybe the expert SDR software)) on Linux and presents in a  Qt5 window on a  48 kHz waterfall. Copy for each signal sits to the right of the tracings in the waterfall.  If you use the mouse to select the trace it moves the decoded content to the main Morse Decoder window below the waterfall.  

The radio default is `127.0.0.1` port `50001`, protocol `websocket`. **Start** opens that live session. The toolbar lamp is green **Connected** only while detection is running and the TCI session is up. Otherwise it is dark red **Not connected**. **Stream** selects IQ (complex baseband, the normal ±24 kHz waterfall) or demodulated receiver audio.

## Waterfall

Wide mode is the working spectrum. It is one 1024-point FFT per 1024 samples, about 21.3 ms per column at 48 kHz. Each bin is 46.875 Hz, and the band is exactly 48 kHz. On IQ the picture is mirrored, so the higher frequency is toward the bottom. The window shows 10 kHz at a time and opens centered on the VFO. The frequency is listed on the left side of waterfall.  The left scroll bar moves the green trace line, while the right scroll bar moves the waterfall across the 48KHz spectrum.

To move a decoded line down to the Main Morse decoder and line up your transmit signal click on a CW trace in the waterfall.  

**Tools → Spectrum: Narrow 3 kHz** is a finer experiment around the VFO. **Wide 48 kHz** restores the normal waterfall. Columns are stamped from the sample clock (one hop per column), not from the wall clock. A batch of columns that shared one timestamp would collapse dits.

## Copy beside the waterfall

The band is 48 slices of 1 kHz, from −24 kHz to +24 kHz. Each slice has its own spectrum decoder and follows the strongest signal inside it. A signal is strong enough to take a slice when it is at least 16 dB above that column's noise, the same rise that paints a white trace. A drift of about 150 Hz stays on the same decoder. A louder signal farther away in the slice takes over after it has been the strongest for two columns, and it has to beat the current signal by more than 1.5 dB. A single-column spike does not move the slice.

When the signal stops and nothing else is in the slice, the line stays on the last frequency and the text stays. A new signal that then holds for two columns moves the line, and the text already decoded stays with it. New characters are added after that text.

The line is locked to the trace. The full decoder string is kept. Characters enter on the right and move left. The horizontal bar under the copy, or Shift+wheel, scrolls back through earlier text. A shorter republish does not blank a line.

**Clear** at the top right of that copy wipes the side text and starts those slice decoders over. The waterfall and the bottom Morse panes stay as they are. The toolbar **Clear** still clears all of them.

**Settings** can run fewer than 48 slices (1–48). A lower number keeps the slices around the center of the band.

## Morse decoder

The bottom pane opens on **DeepCW**. The **Decoder** button cycles Threshold, then Mask, then Spectrum, then Cwsk, then DeepCW. That button changes only the bottom pane. The copy beside the waterfall always uses its own spectrum decoders.

- **Spectrum** reads the waterfall at the tuned frequency. White is key-down (16 dB above the noise). Once down, the key stays down until the bin falls below noise + 12.5 dB. Dit, dah, letter, and word come from the peaks of the run-length histogram. A character is copied after six dahs of newer spectrum, so the mark is already drawn before the letter appears.
- **Threshold** is mark and space against an auto threshold, with auto WPM.
- **Mask** draws dit and dah boxes on the signal-trace scope.
- **Cwsk** Vertibi based channel decoder. It reads the radio samples at the frequency you clicked, through its own filter and element model. It does not use the waterfall picture. Click a signal with the trace window open, then choose Cwsk. The side copy is unchanged.
- **DeepCW** is the neural Morse model from the [DeepCW engine](https://github.com/e04/deepcw-engine). It reads real audio, not IQ. The clicked tone is shifted to 800 Hz, low-passed, and resampled to 3200 Hz, which is the middle of the model's 400–1200 Hz band. That band is the same job as the audio waterfall on [DeepCW](https://github.com/e04/web-deep-cw-decoder). Click a signal, then choose DeepCW. That click does not open the signal-trace window. The side copy is unchanged. The model file is `models/model.onnx` (AGPL-3.0).

Defaults to DeepCW since this seems to be the best decode algorithm by a fair amount. 

A click on the waterfall selects that row. The green line is drawn on the row under the pointer, not on a nearby peak. The green text on the line is the dial frequency sent to the radio, in MHz to 1 Hz. It is set when the line moves and stays put when the radio reports a new center, so it can be compared with the dial. The click also copies the text already decoded in that 1 kHz slice into the bottom Morse pane, and keeps decoding the same signal in both places. The signal-trace window opens for every decoder except DeepCW. The side line stays. Closing the trace leaves the side copy where it is.

On a live radio that click also sets the dial, so the operator hears the signal. The command is TCI `dds`, then `vfo`, both at the same frequency. The axis, the status **RF**, the slider, and the green text are the dial frequency. On IQ that is the center minus the picture offset. When the radio reports the new center, that station is the center bin of the IQ. The green line and the decoder move there, and the 10 kHz view follows. That bin is the sidetone in the headphones and the transmit frequency. The dial stays on the commanded frequency. The click does not tune a second time. A click already within about 30 Hz of the middle does not retune. Training playback does not retune. The trace fine-tune and AFC do not move the radio. AFC peak-lock starts off.

deskHPSDR places the CW carrier on the IQ center: the oscillator is one sidetone off the dial, and the IQ stream is shifted by that sidetone. This program does not add a pitch of its own, and it does not blank that center bin. When the dial arrives, the selected signal, the green line, and the decoder are that center. Put the radio in CW before the click. The tune does not change mode.

In receiver-audio mode the dial is set to the labeled frequency, center plus the picture offset. On USB or CWU, raising the VFO lowers the audio pitch. LSB can step the other way.

## CW keyboard

A keyboard sits under the Morse decoder. Characters typed there are sent by the radio. The default is TCI `cw_macros`. Set `cw_send=ky` and `cat_port` to use Kenwood TS-2000 `KY` on that CAT socket instead. That socket is Thetis CAT, or deskHPSDR through rigctld. The built-in deskHPSDR TS-2000 port does not implement KY. `cat_port` 0 stays on TCI. Enter adds a word space and sends now. Backspace drops a character that has not been sent. Esc or **Stop** aborts. The spinner sets `cw_keyer_speed` and `cw_macros_speed`, from 5 to 60 WPM. The radio should already be in CW. Nothing is sent while detection is stopped or while a training file is playing.

F1 through F5 sit between the decoder Clear button and the keyboard. Each key has a text box. Pressing that function key, or its button, sends the text in the box through the same path. The five messages are saved for the next launch.

In every decoder pane, CQ is green (`#00ff66`) wherever those two letters sit together. A call sign turns red (`#ff3333`) once that same call appears at least twice in that buffer. Both copies go red, including inside a run with no word spaces. A slash or other punctuation splits a run. Ordinary text is light. The pane color is painted first, then the green and red marks, so one red call does not paint the rest of the line red.

## Build

Qt5, GCC, and libwebsockets.

```bash
make; make gui
```

That builds `bin/cw-skimmer-gui`. Quit a running window and start that binary again after a build. `make` builds the command-line detector and `bin/libcwskimmer_c.so`. `make test`, `make replay`, and `make synth` build the older detector checks.

## Windows 10

The Makefile looks at the compiler (`gcc -dumpmachine`) and at `OS` / `uname`. A MinGW target builds a native Win32 window and does not call Qt. On that host, `make` and `make gui` both write `bin/cw-skimmer-gui.exe`. Linux still uses the commands above.

Build from the MSYS2 **MinGW 64-bit** shell, the one whose prompt says `MINGW64`. The plain MSYS gcc cannot build this window.

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make mingw-w64-x86_64-pkg-config mingw-w64-x86_64-libwebsockets
make
```

Put `cw-skimmer.conf` in the same folder as the exe. The window uses the controls that ship with Windows 10. It has the 48 kHz waterfall, the 10 kHz scroll, the 1 kHz labels, the copy beside the waterfall, click tuning, F1–F5, and the CW keyboard. F1–F5 are saved in `cw-skimmer-fn.ini` next to the exe. Start the radio before Send or a function key. DeepCW and the Linux audio-output picker stay on the Qt build. The line under the waterfall shows the decode for the kilohertz you clicked.

`cw-skimmer.conf` holds the radio, the center frequency (14074000 Hz), the sample rate (48000), `spectrum_span_hz=0` for the wide waterfall, and `multi_decode_channels=48`. `spot_enabled=0`. An empty `training_file` means the live radio. `cw_send=tci` and `cw_wpm=20` are the keyboard defaults. `cat_port=0` leaves Kenwood KY off.

The command-line program still contains the earlier detector, dit/dah decoder, validator, and optional telnet spotting. The copy in the window comes from the spectrum, threshold, mask, Cwsk, and DeepCW decoders above. `make ort` fetches ONNX Runtime into `third_party/onnxruntime` if it is not already there. `make deepcw-test` decodes a synthetic tone with the engine.
