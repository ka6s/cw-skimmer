# Running CW Skimmer

This is the check for a machine that is about to run the window. The design is [README.md](./README.md). Build steps are [GUI_QUICKSTART.md](./GUI_QUICKSTART.md).

## Build

From this directory, with Qt5, GCC, and libwebsockets installed:

```bash
make gui
```

Confirm `bin/cw-skimmer-gui` exists and is newer than the sources you changed. Quit a window that is already open before starting the new binary.

```bash
./bin/cw-skimmer-gui
```

`make` builds the command-line detector `bin/cw-skimmer` and `bin/libcwskimmer_c.so`. Those are optional for the window.

## Config

`cw-skimmer.conf` next to the program, or in the working directory:

- `radio_host=127.0.0.1`
- `radio_port=50001`
- `radio_protocol=websocket`
- `center_frequency=14074000`
- `sample_rate=48000`
- `spectrum_span_hz=0`
- `multi_decode_channels=48`
- `spot_enabled=0`
- `training_file` empty

The TCI radio application has to be listening on that host and port before **Start**. A local deskHPSDR session answers this WebSocket as a TCI radio.

## Live session

1. Press **Start**.
2. The lamp turns green **Connected** while the session is up and detection is running. Dark red **Not connected** means it is not.
3. The status bar shows RUNNING and the center frequency. The spectrum mode label is WIDE.
4. The waterfall moves. Ten kilohertz are visible, centered on the VFO until you scroll.
5. A signal at least 16 dB above the noise can take its 1 kHz slice. Text appears on that trace, newest character on the right.
6. Click the trace. The signal-trace window opens, the bottom Morse pane shows the same text, and both keep decoding.
7. **Stop** ends the session. The lamp goes dark red.

## Training playback

1. Press **Stop** if a session is running.
2. **Training file…** and choose a `training/set_XX.tcistream`.
3. **Start playback**. The log names the file. At the end it says "Training playback finished."
4. **Start** after that goes back to the live radio.

Generate the files with `make training-data` when they are not already on disk. They are not stored in git.

## Command-line service

[install.sh](./install.sh) installs `bin/cw-skimmer` to `/opt/cw-skimmer` and [systemd/cw-skimmer.service](./systemd/cw-skimmer.service). That unit runs the command-line detector with `/etc/cw-skimmer/cw-skimmer.conf`. It does not run the Qt window.

```bash
sudo systemctl enable cw-skimmer
sudo systemctl start cw-skimmer
sudo systemctl status cw-skimmer
sudo journalctl -u cw-skimmer -f
```

Spotting stays off unless that config sets `spot_enabled=1` and names a telnet host.
