#!/usr/bin/env python3
"""
Audio Recording Server
Accepts PCM audio data from ESP device via HTTP POST
"""

from flask import Flask, request, jsonify
import os
import datetime
import logging
from pathlib import Path

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

app = Flask(__name__)

# Configuration
RECORDINGS_DIR = Path("recordings")
RECORDINGS_DIR.mkdir(exist_ok=True)

# Store active recording session info
active_recordings = {}


@app.before_request
def handle_raw_audio():
    """Handle raw audio data by preventing chunked encoding parsing"""
    if request.endpoint == 'receive_audio' and request.method == 'POST':
        # Remove Transfer-Encoding header to prevent Werkzeug from parsing as chunked
        # This allows us to read raw binary data
        if 'HTTP_TRANSFER_ENCODING' in request.environ:
            del request.environ['HTTP_TRANSFER_ENCODING']
        if 'transfer-encoding' in request.headers:
            request.headers.pop('transfer-encoding', None)


@app.route('/api/audio', methods=['POST'])
def receive_audio():
    """
    Receive PCM audio data from ESP device
    
    Expected headers:
    - Content-Type: audio/pcm
    - x-audio-sample-rates: 16000
    - x-audio-bits: 16
    - x-audio-channel: 1
    """
    try:
        # Extract audio parameters from headers
        sample_rate = request.headers.get('x-audio-sample-rates', '16000')
        bits_per_sample = request.headers.get('x-audio-bits', '16')
        channels = request.headers.get('x-audio-channel', '1')
        content_type = request.headers.get('Content-Type', '')
        
        # Validate content type
        if 'audio/pcm' not in content_type.lower():
            logger.warning(f"Unexpected Content-Type: {content_type}")
        
        # Log request info
        logger.info(f"Received audio data:")
        logger.info(f"  Sample rate: {sample_rate} Hz")
        logger.info(f"  Bits per sample: {bits_per_sample}")
        logger.info(f"  Channels: {channels}")
        logger.info(f"  Content-Type: {content_type}")
        logger.info(f"  Content-Length: {request.content_length} bytes")
        
        # Get audio data - read directly from stream to avoid parsing issues
        # Use request.stream which doesn't trigger Flask's body parsing
        audio_data = b''
        try:
            # Read directly from stream without triggering Flask's parsing
            # This avoids chunked encoding parsing issues
            if request.content_length and request.content_length > 0:
                # Read exact number of bytes if Content-Length is set
                audio_data = request.stream.read(request.content_length)
            else:
                # Read all available data (for streaming without Content-Length)
                chunk_size = 16384  # 16KB chunks
                max_size = 10 * 1024 * 1024  # 10MB safety limit
                while len(audio_data) < max_size:
                    chunk = request.stream.read(chunk_size)
                    if not chunk:
                        break
                    audio_data += chunk
                if len(audio_data) >= max_size:
                    logger.warning("Audio data exceeds 10MB limit, truncating")
        except Exception as stream_error:
            logger.error(f"Error reading audio stream: {stream_error}", exc_info=True)
            # Try fallback: read from raw WSGI input
            try:
                logger.info("Trying fallback: reading from raw WSGI input")
                wsgi_input = request.environ.get('wsgi.input')
                if wsgi_input:
                    if request.content_length and request.content_length > 0:
                        audio_data = wsgi_input.read(request.content_length)
                    else:
                        chunk_size = 16384
                        max_size = 10 * 1024 * 1024
                        while len(audio_data) < max_size:
                            chunk = wsgi_input.read(chunk_size)
                            if not chunk:
                                break
                            audio_data += chunk
            except Exception as fallback_error:
                logger.error(f"Fallback also failed: {fallback_error}")
                return jsonify({
                    'status': 'error',
                    'message': f'Failed to read audio stream: {str(stream_error)}'
                }), 400
        
        if not audio_data:
            logger.warning("No audio data received")
            return jsonify({
                'status': 'error',
                'message': 'No audio data received'
            }), 400
        
        # Generate filename with timestamp
        timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        filename = f"recording_{timestamp}.pcm"
        filepath = RECORDINGS_DIR / filename
        
        # Save raw PCM data
        with open(filepath, 'wb') as f:
            f.write(audio_data)
        
        logger.info(f"Saved audio to: {filepath} ({len(audio_data)} bytes)")
        
        # Calculate duration (approximate)
        sample_rate_int = int(sample_rate)
        bits_per_sample_int = int(bits_per_sample)
        channels_int = int(channels)
        bytes_per_sample = (bits_per_sample_int // 8) * channels_int
        duration_seconds = len(audio_data) / (sample_rate_int * bytes_per_sample)
        
        # Return success response
        response = {
            'status': 'success',
            'message': 'Audio received and saved',
            'filename': filename,
            'size_bytes': len(audio_data),
            'duration_seconds': round(duration_seconds, 2),
            'sample_rate': sample_rate,
            'bits_per_sample': bits_per_sample,
            'channels': channels
        }
        
        logger.info(f"Response: {response}")
        return jsonify(response), 200
        
    except Exception as e:
        logger.error(f"Error processing audio: {str(e)}", exc_info=True)
        return jsonify({
            'status': 'error',
            'message': f'Server error: {str(e)}'
        }), 500


@app.route('/api/health', methods=['GET'])
def health_check():
    """Health check endpoint"""
    return jsonify({
        'status': 'healthy',
        'service': 'audio-recording-server',
        'recordings_dir': str(RECORDINGS_DIR)
    }), 200


@app.route('/api/recordings', methods=['GET'])
def list_recordings():
    """List all recorded audio files"""
    recordings = []
    for file in sorted(RECORDINGS_DIR.glob("*.pcm"), key=os.path.getmtime, reverse=True):
        size = file.stat().st_size
        mtime = datetime.datetime.fromtimestamp(file.stat().st_mtime)
        recordings.append({
            'filename': file.name,
            'size_bytes': size,
            'created': mtime.isoformat()
        })
    
    return jsonify({
        'status': 'success',
        'count': len(recordings),
        'recordings': recordings
    }), 200


if __name__ == '__main__':
    # Run server on all interfaces (0.0.0.0) on port 8000
    logger.info("Starting audio recording server...")
    logger.info(f"Recordings will be saved to: {RECORDINGS_DIR.absolute()}")
    logger.info("Server URL: http://0.0.0.0:8000/api/audio")
    app.run(host='192.168.1.18', port=8000, debug=True)

