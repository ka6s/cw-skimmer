# CW Skimmer for Linux

A high-performance CW (Morse code) signal detector and decoder for Linux, using Bayesian machine learning and TCI protocol to connect with software-defined radios.

## Features

- **TCI Protocol Integration**: Connect to any TCI-compatible radio (FlexRadio, ICOM, Kenwood, Yaesu, etc.) via TCP/IP
- **48kHz Spectrum Analysis**: Real-time processing of full 48kHz I/Q spectrum
- **Polyphase Filterbank**: Efficient channelization into ~100Hz bins (480 channels)
- **Bayesian CW Detection**: ML-based signal classification using Bayesian decision trees
- **Morse Decoder**: Automatic CW to callsign decoding
- **Telnet Spotting**: Report detected CW contacts to RBN-compatible spotting servers
- **High Performance**: C-based implementation for minimal CPU overhead
- **GUI similar to CW-Skimmer windows application

## Building

### Requirements
- GCC or Clang
- Standard C library (libc)
- libm (math library)
- POSIX-compliant system (Linux, BSD, macOS)

### Compilation
```bash
cd cw-skimmer
make clean
make
```

Build outputs:
- `bin/cw-skimmer` - Main executable
- `bin/test_detector` - Unit tests

## Configuration

Create or edit `cw-skimmer.conf`:

```ini
# Radio TCI Connection
radio_host=127.0.0.1
radio_port=4532
center_frequency=14074000
sample_rate=48000

# Spot Server Connection (telnet)
spot_server_host=127.0.0.1
spot_server_port=7373
spot_server_callsign=CWSKIMMER

# Detection Parameters
detection_threshold=60

# Logging
log_level=1
log_file=cw-skimmer.log
```

## Running

```bash
./bin/cw-skimmer cw-skimmer.conf
```

Or with defaults:
```bash
./bin/cw-skimmer
```

## Architecture

```
Radio (TCI/TCP)
    ↓
I/Q Circular Buffer
    ↓
Polyphase Filterbank (48kHz → 480 × 100Hz)
    ↓
Bayesian CW Detector (per-channel analysis)
    ↓
Morse Decoder (timing analysis → callsigns)
    ↓
Telnet Reporter (spot submission)
```

## Modules

### Core Components

- **tci_client.c/h** - TCI protocol client for radio communication
- **audio_processor.c/h** - Polyphase filterbank for spectrum channelization
- **bayesian_tree.c/h** - Bayesian classifier for CW detection
- **cw_detector.c/h** - Per-channel signal detection and analysis
- **cw_decoder.c/h** - Morse code timing analysis and decoding
- **spot_reporter.c/h** - Telnet client for spot reporting
- **config.c/h** - Configuration file parsing
- **logger.c/h** - Logging system

## Signal Detection Pipeline

1. **Acquisition**: Connect to radio via TCI, receive 48kHz I/Q stream
2. **Channelization**: Polyphase filterbank divides spectrum into 480 parallel channels (~100Hz each)
3. **Power Estimation**: Compute power spectrum density per channel
4. **Feature Extraction**:
   - Tone purity (sine wave quality)
   - Signal-to-noise ratio (SNR)
   - Bandwidth (typical CW: 50-200Hz)
   - Keying envelope characteristics
   - Adjacent channel rejection
5. **Bayesian Classification**: Evaluate probability of CW signal using feature vector
6. **Morse Decoding**: Analyze keying timing to extract Morse symbols
7. **Reporting**: Submit valid detections via telnet to spotting server

## Bayesian CW Classification

The detector uses a Bayesian decision tree with 6 features:

| Feature | Weight | Purpose |
|---------|--------|---------|
| Tone Purity | 0.25 | CW should be pure sine (high purity) |
| Keying Regularity | 0.20 | CW has consistent on/off timing |
| SNR | 0.15 | Differentiates signal from noise |
| Bandwidth | 0.15 | CW is narrow (<200Hz) |
| Envelope Shape | 0.15 | CW has defined keying envelope |
| Adjacent Channel Rejection | 0.10 | Filters adjacent interference |

## Testing

Run unit tests:
```bash
make test
```

This runs Bayesian classifier and detector tests to verify core functionality.

## Performance

Expected performance on modern hardware:
- CPU usage: <10% for real-time processing
- Latency: <500ms from signal onset to spot report
- Detection accuracy: >90% on strong signals (SNR > 3dB)

## Known Limitations (Phase Implementation)

- Phase 1-2: Foundation & basic I/Q processing
- Phase 3: Bayesian detector operational
- Phase 4: Morse decoder (symbol timing analysis)
- Phase 5: Full telnet reporting with retry queue
- Phase 6: Production optimization & testing

## Future Enhancements

- Multi-VFO support for simultaneous monitoring
- Machine learning model training from live data
- Integration with RBN database for validation
- Support for additional TCI radios
- Real-time spectrum visualization
- PostgreSQL database backend for long-term archival
- Web dashboard for monitoring

## License

MIT License

## Contributing

Contributions welcome. Please ensure:
- Code compiles with `-Wall -Wextra`
- Unit tests pass (`make test`)
- No new warnings introduced
- Changes include relevant documentation

## Support

For issues or questions:
1. Check the logs in `cw-skimmer.log`
2. Enable DEBUG logging: `log_level=0` in config
3. Verify TCI connection to radio is working
4. Confirm telnet server is accessible (if reporting enabled)
