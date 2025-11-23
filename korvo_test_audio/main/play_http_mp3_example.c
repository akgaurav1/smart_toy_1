/* Play an MP3 file from HTTP

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"

#include "esp_peripherals.h"
#include "periph_wifi.h"
#include "periph_adc_button.h"
#include "board.h"
#include "board_pins_config.h"
#include "audio_hal.h"
#include "esp_http_client.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

static const char *TAG = "HTTP_MP3_EXAMPLE";
static int current_volume = 100;  // Current volume level (0-100)

// Recording configuration
#define RECORD_SAMPLE_RATE  (16000)
#define RECORD_BITS         (16)
#define RECORD_CHANNELS     (1)
#define RECORD_SERVER_URI   "http://192.168.1.18:8000/api/audio"  // Change this to your server API URL

// Recording pipeline
static audio_pipeline_handle_t record_pipeline = NULL;
static audio_element_handle_t i2s_stream_reader = NULL;
static audio_element_handle_t http_stream_writer = NULL;
static bool is_recording = false;

// Helper function to safely stop and reset recording pipeline
static void safe_reset_recording_pipeline(void)
{
    if (record_pipeline == NULL) {
        return;
    }
    
    // Check element states before attempting to stop
    audio_element_state_t i2s_state = audio_element_get_state(i2s_stream_reader);
    audio_element_state_t http_state = audio_element_get_state(http_stream_writer);
    
    // Only stop if elements are actually running
    if (i2s_state == AEL_STATE_RUNNING || i2s_state == AEL_STATE_PAUSED ||
        http_state == AEL_STATE_RUNNING || http_state == AEL_STATE_PAUSED ||
        i2s_state == AEL_STATE_ERROR || http_state == AEL_STATE_ERROR) {
        
        // Mark ringbuffer as done first
        if (i2s_state == AEL_STATE_RUNNING || i2s_state == AEL_STATE_PAUSED) {
            audio_element_set_ringbuf_done(i2s_stream_reader);
        }
        
        // Stop pipeline
        audio_pipeline_stop(record_pipeline);
        audio_pipeline_wait_for_stop(record_pipeline);
        
        // Reset only if pipeline was actually running
        if (i2s_state != AEL_STATE_STOPPED && i2s_state != AEL_STATE_FINISHED &&
            http_state != AEL_STATE_STOPPED && http_state != AEL_STATE_FINISHED) {
            audio_pipeline_reset_ringbuffer(record_pipeline);
            audio_pipeline_reset_elements(record_pipeline);
        }
        
        audio_pipeline_terminate(record_pipeline);
    }
    
    is_recording = false;
}

// HTTP stream event handler for recording
esp_err_t _http_stream_event_handle(http_stream_event_msg_t *msg)
{
    esp_http_client_handle_t http = (esp_http_client_handle_t)msg->http_client;
    char len_buf[16];
    static int total_write = 0;

    if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
        ESP_LOGI(TAG, "[REC] HTTP client PRE_REQUEST, connecting to server...");
        esp_http_client_set_method(http, HTTP_METHOD_POST);
        esp_http_client_set_header(http, "Content-Type", "audio/pcm");
        esp_http_client_set_header(http, "Transfer-Encoding", "chunked");
        
        // Set timeout for connection (10 seconds)
        esp_http_client_set_timeout_ms(http, 10000);
        
        // Send audio parameters as headers
        char dat[10] = {0};
        snprintf(dat, sizeof(dat), "%d", RECORD_SAMPLE_RATE);
        esp_http_client_set_header(http, "x-audio-sample-rates", dat);
        memset(dat, 0, sizeof(dat));
        snprintf(dat, sizeof(dat), "%d", RECORD_BITS);
        esp_http_client_set_header(http, "x-audio-bits", dat);
        memset(dat, 0, sizeof(dat));
        snprintf(dat, sizeof(dat), "%d", RECORD_CHANNELS);
        esp_http_client_set_header(http, "x-audio-channel", dat);
        
        total_write = 0;
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_ON_REQUEST) {
        // Write data in chunked transfer encoding format
        // Format: <chunk_length_hex>\r\n<data>\r\n
        int wlen = sprintf(len_buf, "%x\r\n", msg->buffer_len);
        if (esp_http_client_write(http, len_buf, wlen) <= 0) {
            ESP_LOGE(TAG, "[REC] Failed to write chunk length");
            return ESP_FAIL;
        }
        
        // Write the actual audio data
        if (esp_http_client_write(http, msg->buffer, msg->buffer_len) <= 0) {
            ESP_LOGE(TAG, "[REC] Failed to write audio data");
            return ESP_FAIL;
        }
        
        // Write chunk terminator
        if (esp_http_client_write(http, "\r\n", 2) <= 0) {
            ESP_LOGE(TAG, "[REC] Failed to write chunk terminator");
            return ESP_FAIL;
        }
        
        total_write += msg->buffer_len;
        ESP_LOGD(TAG, "[REC] Total bytes written: %d", total_write);
        return msg->buffer_len;
    }

    if (msg->event_id == HTTP_STREAM_POST_REQUEST) {
        ESP_LOGI(TAG, "[REC] HTTP client POST_REQUEST, writing end chunk marker");
        // Write final chunk marker: 0\r\n\r\n
        if (esp_http_client_write(http, "0\r\n\r\n", 5) <= 0) {
            ESP_LOGE(TAG, "[REC] Failed to write end chunk marker");
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_FINISH_REQUEST) {
        ESP_LOGI(TAG, "[REC] HTTP client FINISH_REQUEST, total bytes: %d", total_write);
        char *buf = calloc(1, 128);
        if (buf) {
            int read_len = esp_http_client_read(http, buf, 127);
            if (read_len > 0) {
                buf[read_len] = 0;
                ESP_LOGI(TAG, "[REC] Server response: %s", buf);
            }
            free(buf);
        }
        return ESP_OK;
    }
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
    ESP_ERROR_CHECK(esp_netif_init());
#else
    tcpip_adapter_init();
#endif

    audio_pipeline_handle_t pipeline;
    audio_element_handle_t http_stream_reader, i2s_stream_writer, mp3_decoder;

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);
    
    ESP_LOGI(TAG, "[ 1.1 ] Set volume to maximum");
    current_volume = 100;
    audio_hal_set_volume(board_handle->audio_hal, current_volume);
    
    ESP_LOGI(TAG, "[ 1.2 ] Initialize ADC codec for microphone");
    audio_hal_ctrl_codec(board_handle->adc_hal, AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[2.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGI(TAG, "[2.1] Create http stream to read data");
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_stream_reader = http_stream_init(&http_cfg);

    ESP_LOGI(TAG, "[2.2] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[2.3] Create mp3 decoder to decode mp3 file");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_decoder = mp3_decoder_init(&mp3_cfg);

    ESP_LOGI(TAG, "[2.4] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, http_stream_reader, "http");
    audio_pipeline_register(pipeline, mp3_decoder,        "mp3");
    audio_pipeline_register(pipeline, i2s_stream_writer,  "i2s");

    ESP_LOGI(TAG, "[2.5] Link it together http_stream-->mp3_decoder-->i2s_stream-->[codec_chip]");
    const char *link_tag[3] = {"http", "mp3", "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 3);

    ESP_LOGI(TAG, "[2.6] Set up  uri (http as http_stream, mp3 as mp3 decoder, and default output is i2s)");
    audio_element_set_uri(http_stream_reader, "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.mp3");

    ESP_LOGI(TAG, "[ 3 ] Start and wait for Wi-Fi network");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    periph_wifi_cfg_t wifi_cfg = {
        .wifi_config.sta.ssid = CONFIG_WIFI_SSID,
        .wifi_config.sta.password = CONFIG_WIFI_PASSWORD,
    };
    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
    esp_periph_start(set, wifi_handle);
    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);
    
    ESP_LOGI(TAG, "[ 3.1 ] Initialize ADC buttons for volume control and recording");
    audio_board_key_init(set);
    
    ESP_LOGI(TAG, "[ 3.2 ] Create recording pipeline for microphone input");
    // Create recording pipeline: i2s_stream_reader -> http_stream_writer
    audio_pipeline_cfg_t record_pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    record_pipeline = audio_pipeline_init(&record_pipeline_cfg);
    mem_assert(record_pipeline);
    
    // Create HTTP stream writer for uploading audio
    http_stream_cfg_t http_record_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_record_cfg.type = AUDIO_STREAM_WRITER;
    http_record_cfg.event_handle = _http_stream_event_handle;
    http_stream_writer = http_stream_init(&http_record_cfg);
    
    // Create I2S stream reader for microphone
    i2s_stream_cfg_t i2s_record_cfg = I2S_STREAM_CFG_DEFAULT_WITH_TYLE_AND_CH(
        CODEC_ADC_I2S_PORT, RECORD_SAMPLE_RATE, RECORD_BITS, AUDIO_STREAM_READER, RECORD_CHANNELS);
    i2s_record_cfg.type = AUDIO_STREAM_READER;
    i2s_record_cfg.out_rb_size = 16 * 1024;  // Buffer size for recording
    i2s_record_cfg.std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;  // Set mono mode for single channel
    i2s_stream_reader = i2s_stream_init(&i2s_record_cfg);
    
    // Register elements to recording pipeline
    audio_pipeline_register(record_pipeline, i2s_stream_reader, "i2s_rec");
    audio_pipeline_register(record_pipeline, http_stream_writer, "http_rec");
    
    // Link recording pipeline: i2s_rec -> http_rec
    const char *record_link_tag[2] = {"i2s_rec", "http_rec"};
    audio_pipeline_link(record_pipeline, &record_link_tag[0], 2);
    
    // Set I2S clock for recording
    i2s_stream_set_clk(i2s_stream_reader, RECORD_SAMPLE_RATE, RECORD_BITS, RECORD_CHANNELS);
    
    ESP_LOGI(TAG, "[ 3.3 ] Recording pipeline ready. Press [REC] button to start/stop recording");
    
    // Example of using an audio event -- START
    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Set up event listener for recording pipeline");
    audio_pipeline_set_listener(record_pipeline, evt);

    ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 5 ] Ready. Playback pipeline not started (recording only mode)");
    // Playback pipeline is created but not started - only recording is active
    // Uncomment the line below if you want to enable playback:
    // audio_pipeline_run(pipeline);

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        // Handle button events
        if (msg.source_type == PERIPH_ID_ADC_BTN) {
            if (msg.cmd == PERIPH_ADC_BUTTON_PRESSED) {
                if ((int)msg.data == get_input_volup_id()) {
                    current_volume += 10;
                    if (current_volume > 100) {
                        current_volume = 100;
                    }
                    audio_hal_set_volume(board_handle->audio_hal, current_volume);
                    ESP_LOGI(TAG, "[ * ] Volume Up: %d%%", current_volume);
                    continue;
                } else if ((int)msg.data == get_input_voldown_id()) {
                    current_volume -= 10;
                    if (current_volume < 0) {
                        current_volume = 0;
                    }
                    audio_hal_set_volume(board_handle->audio_hal, current_volume);
                    ESP_LOGI(TAG, "[ * ] Volume Down: %d%%", current_volume);
                    continue;
                } else if ((int)msg.data == get_input_rec_id()) {
                    // Start recording
                    if (!is_recording) {
                        ESP_LOGI(TAG, "[ * ] [REC] Button pressed - Starting recording...");
                        
                        // Check if pipeline elements are already running, stop and reset first
                        audio_element_state_t i2s_state = audio_element_get_state(i2s_stream_reader);
                        audio_element_state_t http_state = audio_element_get_state(http_stream_writer);
                        if (i2s_state == AEL_STATE_RUNNING || i2s_state == AEL_STATE_PAUSED ||
                            http_state == AEL_STATE_RUNNING || http_state == AEL_STATE_PAUSED ||
                            i2s_state == AEL_STATE_ERROR || http_state == AEL_STATE_ERROR) {
                            ESP_LOGW(TAG, "[ * ] Pipeline already running or in error state, resetting...");
                            safe_reset_recording_pipeline();
                            vTaskDelay(pdMS_TO_TICKS(100)); // Small delay to ensure cleanup
                        }
                        
                        // Set URI and start recording
                        audio_element_set_uri(http_stream_writer, RECORD_SERVER_URI);
                        audio_pipeline_run(record_pipeline);
                        is_recording = true;
                        ESP_LOGI(TAG, "[ * ] Recording started. Audio will be sent to: %s", RECORD_SERVER_URI);
                    }
                    continue;
                }
            } else if (msg.cmd == PERIPH_ADC_BUTTON_RELEASE || msg.cmd == PERIPH_ADC_BUTTON_LONG_RELEASE) {
                if ((int)msg.data == get_input_rec_id()) {
                    // Stop recording
                    if (is_recording) {
                        ESP_LOGI(TAG, "[ * ] [REC] Button released - Stopping recording...");
                        safe_reset_recording_pipeline();
                        ESP_LOGI(TAG, "[ * ] Recording stopped and pipeline reset");
                    }
                    continue;
                }
            }
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
            && msg.source == (void *) mp3_decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(mp3_decoder, &music_info);

            ESP_LOGI(TAG, "[ * ] Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);

            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
            continue;
        }

        // Handle recording pipeline errors
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT 
            && msg.source == (void *) http_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
            int status = (int)msg.data;
            // Check for any error status
            if (status == AEL_STATUS_ERROR_OPEN || 
                status == AEL_STATUS_ERROR_INPUT ||
                status == AEL_STATUS_ERROR_PROCESS ||
                status == AEL_STATUS_ERROR_OUTPUT ||
                status == AEL_STATUS_ERROR_CLOSE ||
                status == AEL_STATUS_ERROR_TIMEOUT ||
                status == AEL_STATUS_ERROR_UNKNOWN) {
                ESP_LOGE(TAG, "[ * ] Recording pipeline error detected: %d", status);
                if (status == AEL_STATUS_ERROR_OPEN) {
                    ESP_LOGE(TAG, "[ * ] Connection failed. Check if server is running at: %s", RECORD_SERVER_URI);
                }
                // Use safe reset function to avoid duplicate operations
                safe_reset_recording_pipeline();
                continue;
            }
        }
    }
    // Example of using an audio event -- END

    ESP_LOGI(TAG, "[ 6 ] Stop audio_pipeline");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);

    /* Terminate the pipeline before removing the listener */
    audio_pipeline_unregister(pipeline, http_stream_reader);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    audio_pipeline_unregister(pipeline, mp3_decoder);

    audio_pipeline_remove_listener(pipeline);

    /* Stop all peripherals before removing the listener */
    esp_periph_set_stop_all(set);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    // Stop recording pipeline if active
    if (is_recording) {
        ESP_LOGI(TAG, "[ 7 ] Stop recording pipeline");
        audio_pipeline_stop(record_pipeline);
        audio_pipeline_wait_for_stop(record_pipeline);
        audio_pipeline_terminate(record_pipeline);
        audio_pipeline_unregister(record_pipeline, i2s_stream_reader);
        audio_pipeline_unregister(record_pipeline, http_stream_writer);
        audio_pipeline_deinit(record_pipeline);
        audio_element_deinit(i2s_stream_reader);
        audio_element_deinit(http_stream_writer);
    }
    
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(http_stream_reader);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(mp3_decoder);
    esp_periph_set_destroy(set);
}
