# CW-Skimmer Qt5 GUI - Quick Start Guide

## Building

### Prerequisites
- GCC/G++ with C99 and C++17 support
- Qt5 (libqt5-dev, qt5-qmake)
- POSIX-compliant system (Linux)

### Build Steps

```bash
cd /home/stevew/cw-skimmer

# Build CLI and shared library (always works)
make

# Build Qt5 GUI (requires Qt5)
make gui

# Or build everything at once
make all gui
```

### Output Binaries
```
bin/
├── cw-skimmer       - Original CLI tool
├── cw-skimmer-gui   - New Qt5 GUI (recommended)
└── libcwskimmer_c.so - Shared library
```

## Running the GUI

### Simple Start
```bash
./bin/cw-skimmer-gui
```

### With Config File
```bash
# The GUI will look for cw-skimmer.conf in current directory
# Create one if needed with radio connection details:

cat > cw-skimmer.conf << 'EOF'
radio_host=127.0.0.1
radio_port=4532
center_frequency=14074000
sample_rate=48000
spot_server_host=127.0.0.1
spot_server_port=7373
spot_server_callsign=CWSKIMMER
detection_threshold=60
log_level=1
log_file=cw-skimmer.log
EOF

./bin/cw-skimmer-gui
```

## GUI Overview

### Main Tabs

1. **Spectrum Tab**
   - Real-time frequency spectrum with signal markers
   - Green dots show detected signals
   - Older signals fade out (10-second window)
   - Grid overlay for frequency reference

2. **Signals Tab**
   - Table of detected CW signals (most recent on top)
   - Columns: Frequency, SNR, Confidence, Tone Purity, Bandwidth
   - Keeps up to 1000 signals in history
   - Useful for analyzing signal characteristics

3. **Spots Tab**
   - Reported spot contacts (callsigns sent to spotting network)
   - Columns: Callsign, Frequency, SNR, Confidence, Timestamp
   - Keeps up to 500 spots in history
   - View of what the detector has reported

4. **Logs Tab**
   - Real-time application logs
   - Color-coded by severity:
     - Gray: DEBUG messages
     - Black: INFO messages
     - Orange: WARNING messages
     - Red: ERROR messages
   - Keeps last 1000 lines

### Toolbar Buttons

- **Start**: Begin CW detection (connects to radio)
- **Stop**: Stop detection (disconnects from radio)
- **Settings**: Configure radio connection and detection parameters
- **Clear**: Clear all tables and logs

### Status Bar

Shows real-time information:
- **Status**: RUNNING or STOPPED
- **Connected**: YES or NO (radio connection)
- **Buffer**: Current I/Q buffer fill level
- **CPU**: Estimated CPU usage percentage
- **Queue**: Spot report retry queue size

## Configuration

### Settings Dialog

Open via **Settings** button or **Tools → Settings** menu.

#### Radio Connection
- **Radio Host**: IP address of TCI-compatible radio (default: 127.0.0.1)
- **Radio Port**: TCI port number (default: 4532)

#### Detection Parameters
- **Detection Threshold**: CW confidence threshold 0-100% (default: 60%)

#### Spot Reporting
- **Reporter Callsign**: Your callsign for spot reports (default: CWSKIMMER)

## Common Workflows

### Monitor 20m Band (14MHz)

1. Set radio to 14074 kHz (CW segment)
2. Open Settings dialog
3. Verify radio_host and radio_port
4. Click OK
5. Click **Start** button
6. Watch Spectrum tab for signals
7. Check Signals tab for details
8. Spots tab shows reported contacts

### Analyze Signal Characteristics

1. Run detector in normal mode
2. Switch to **Signals tab**
3. For each signal, observe:
   - **SNR (dB)**: Signal strength relative to noise
   - **Confidence (%)**: Detector certainty it's CW
   - **Tone Purity**: How "pure" the sine wave is (0-1)
   - **Bandwidth (Hz)**: Signal width

### Debug Connection Issues

1. Open **Logs tab**
2. Click **Start** button
3. Check for error messages in red
4. Common issues:
   - "Failed to connect to radio" → Check radio_host and radio_port
   - "Failed to subscribe to I/Q stream" → Radio may not support TCI
   - Connection state may take a few seconds

### Monitor Long-Term Detection

1. Start detector
2. Leave GUI running
3. Check Signals tab periodically for activity
4. Use Spots tab to see successfully reported contacts
5. Logs tab shows any warnings or errors

## Troubleshooting

### GUI Won't Start
```bash
# Check if Qt5 is installed
ldd ./bin/cw-skimmer-gui | grep Qt5

# If missing, install:
sudo apt-get install qt5-default libqt5widgets5
```

### Can't Connect to Radio
- Verify radio is running and TCI enabled
- Check IP address: `ping <radio_host>`
- Verify port: `telnet <radio_host> <radio_port>`
- Check firewall settings

### High CPU Usage
- Reduce detection threshold (Settings)
- Check for hardware overload
- Close other applications

### No Signals Detected
- Verify center frequency matches band
- Check signal strength on radio
- Ensure detection threshold is reasonable
- Try lowering threshold in Settings

### Memory Usage Growing
- This is normal (caches signals/spots)
- Use Clear button to reset tables
- Large buffer fills are typical during activity

## Performance Tips

1. **For minimal CPU**: Run CLI version instead
   ```bash
   ./bin/cw-skimmer cw-skimmer.conf
   ```

2. **For better responsiveness**: 
   - Close other tabs when not needed
   - Use Clear button periodically
   - Monitor buffer fill in status bar

3. **For long-running operation**:
   - Monitor logs for errors
   - Keep radio connection stable
   - Check available disk space for logs

## Advanced Usage

### Running Both CLI and GUI
You can run both simultaneously on different ports:

```bash
# Terminal 1: CLI tool
./bin/cw-skimmer cw-skimmer.conf

# Terminal 2: GUI (on different port)
cat > cw-skimmer-gui.conf << 'EOF'
radio_host=127.0.0.1
radio_port=4533    # Different port
...
EOF

# Edit config file path in code or use symlink trick
ln -s cw-skimmer-gui.conf cw-skimmer.conf
./bin/cw-skimmer-gui
```

### Redirecting Spots Server
Change `spot_server_host` and `spot_server_port` in Settings or config file to report to different RBN server.

### Logging
Enable debug logging in config:
```ini
log_level=0    # 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR
log_file=cw-skimmer-debug.log
```

## Support

For issues:
1. Check **Logs tab** for error messages
2. Verify configuration in **Settings**
3. Review `cw-skimmer.conf` file
4. Run original CLI tool to isolate issues: `./bin/cw-skimmer cw-skimmer.conf`

## Files and Directories

```
cw-skimmer/
├── bin/
│   ├── cw-skimmer          # CLI executable
│   ├── cw-skimmer-gui      # GUI executable
│   └── libcwskimmer_c.so   # Shared library
├── src/                    # C detector source
├── gui/                    # Qt5 GUI source
├── cw-skimmer.conf        # Configuration file
└── cw-skimmer.log         # Log output file
```

## Keyboard Shortcuts

Currently supported:
- Alt+F4 or Ctrl+Q: Quit application

(Additional shortcuts can be added in Settings menu)

---

**Last Updated**: 2026-06-04
**Version**: 1.0 (Qt5 GUI Release)
