# Audio Recording Server

Python HTTP server for receiving PCM audio data from ESP device.

## Features

- Accepts POST requests at `/api/audio`
- Handles `Content-Type: audio/pcm`
- Reads audio parameters from custom headers:
  - `x-audio-sample-rates`: Sample rate (e.g., 16000)
  - `x-audio-bits`: Bits per sample (e.g., 16)
  - `x-audio-channel`: Number of channels (e.g., 1)
- Saves received audio as raw PCM files
- Logs all requests and responses
- Provides health check and recordings list endpoints

## Installation

1. Install Python dependencies:
```bash
pip install -r requirements.txt
```

## Usage

1. Start the server:
```bash
python server.py
```

The server will start on `http://0.0.0.0:8000` (accessible from any network interface).

2. Update your ESP device configuration:
   - Set `RECORD_SERVER_URI` to `http://192.168.1.100:8000/api/audio` (replace with your server's IP)

3. Record audio:
   - Press REC button on ESP device to start recording
   - Release REC button to stop recording
   - Audio will be streamed to the server in real-time

## API Endpoints

### POST `/api/audio`
Receives PCM audio data from ESP device.

**Headers:**
- `Content-Type: audio/pcm`
- `x-audio-sample-rates: 16000`
- `x-audio-bits: 16`
- `x-audio-channel: 1`

**Response:**
```json
{
  "status": "success",
  "message": "Audio received and saved",
  "filename": "recording_20240101_120000.pcm",
  "size_bytes": 32000,
  "duration_seconds": 1.0,
  "sample_rate": "16000",
  "bits_per_sample": "16",
  "channels": "1"
}
```

### GET `/api/health`
Health check endpoint.

**Response:**
```json
{
  "status": "healthy",
  "service": "audio-recording-server",
  "recordings_dir": "recordings"
}
```

### GET `/api/recordings`
List all recorded audio files.

**Response:**
```json
{
  "status": "success",
  "count": 2,
  "recordings": [
    {
      "filename": "recording_20240101_120000.pcm",
      "size_bytes": 32000,
      "created": "2024-01-01T12:00:00"
    }
  ]
}
```

## Recordings

All received audio files are saved in the `recordings/` directory as raw PCM files. Files are named with timestamps: `recording_YYYYMMDD_HHMMSS.pcm`

## Playing PCM Files

To play the recorded PCM files, you can use tools like:

**ffplay (from FFmpeg):**
```bash
ffplay -f s16le -ar 16000 -ac 1 recording_20240101_120000.pcm
```

**SoX:**
```bash
play -t raw -r 16000 -e signed-integer -b 16 -c 1 recording_20240101_120000.pcm
```

**Convert to WAV:**
```bash
ffmpeg -f s16le -ar 16000 -ac 1 -i recording_20240101_120000.pcm recording_20240101_120000.wav
```

## Configuration

- **Port**: Default is 8000. Change in `server.py` if needed.
- **Host**: Default is `0.0.0.0` (all interfaces). Change in `server.py` if needed.
- **Recordings directory**: Default is `recordings/`. Automatically created if it doesn't exist.

## Logging

The server logs all requests, including:
- Audio parameters (sample rate, bits, channels)
- Content length
- File save operations
- Errors

Logs are printed to stdout with timestamps.

