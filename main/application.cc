#include "application.h"
#include "system_info.h"
#include "ml307_ssl_transport.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "font_awesome_symbols.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <cstddef>
#include <vector>

#define TAG "Application"

extern const char p3_err_reg_start[] asm("_binary_err_reg_p3_start");
extern const char p3_err_reg_end[] asm("_binary_err_reg_p3_end");
extern const char p3_err_pin_start[] asm("_binary_err_pin_p3_start");
extern const char p3_err_pin_end[] asm("_binary_err_pin_p3_end");
extern const char p3_err_wificonfig_start[] asm("_binary_err_wificonfig_p3_start");
extern const char p3_err_wificonfig_end[] asm("_binary_err_wificonfig_p3_end");

Application::Application() : background_task_(4096 * 8) {
    event_group_ = xEventGroupCreate();
    ota_.SetCheckVersionUrl(CONFIG_OTA_VERSION_URL);
    ota_.SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
}

Application::~Application() {
    if (protocol_ != nullptr) {
        delete protocol_;
    }
    vEventGroupDelete(event_group_);
}

void Application::Start() {
    auto& board = Board::GetInstance();
    board.Initialize();

    auto builtin_led = board.GetBuiltinLed();
    builtin_led->SetBlue();
    builtin_led->StartContinuousBlink(100);

    /* Setup the display */
    auto display = board.GetDisplay();

    /* Setup the audio codec */
    auto codec = board.GetAudioCodec();
    codec->OnInputReady([this, codec]() {
        BaseType_t higher_priority_task_woken = pdFALSE;
        xEventGroupSetBitsFromISR(event_group_, AUDIO_INPUT_READY_EVENT, &higher_priority_task_woken);
        return higher_priority_task_woken == pdTRUE;
    });
    codec->OnOutputReady([this]() {
        BaseType_t higher_priority_task_woken = pdFALSE;
        xEventGroupSetBitsFromISR(event_group_, AUDIO_OUTPUT_READY_EVENT, &higher_priority_task_woken);
        return higher_priority_task_woken == pdTRUE;
    });
    codec->Start();

    /* Start the main loop */
    xTaskCreate([](void* arg) {
        Application* app = (Application*)arg;
        app->MainLoop();
        vTaskDelete(NULL);
    }, "main_loop", 4096 * 2, this, 2, nullptr);

    /* Wait for the network to be ready */
    board.StartNetwork();

    // Check for new firmware version or get the MQTT broker address
    xTaskCreate([](void* arg) {
        Application* app = (Application*)arg;
        app->CheckNewVersion();
        vTaskDelete(NULL);
    }, "check_new_version", 4096 * 2, this, 1, nullptr);

#if CONFIG_IDF_TARGET_ESP32S3
    audio_processor_.Initialize(codec->input_channels(), codec->input_reference());
    audio_processor_.OnOutput([this](std::vector<int16_t>&& data) {
        // 直接发送PCM数据
        protocol_->SendAudio(std::string(reinterpret_cast<const char*>(data.data()), 
                                       data.size() * sizeof(int16_t)));
    });

    wake_word_detect_.Initialize(codec->input_channels(), codec->input_reference());
    wake_word_detect_.OnVadStateChange([this](bool speaking) {
        Schedule([this, speaking]() {
            auto builtin_led = Board::GetInstance().GetBuiltinLed();
            if (chat_state_ == kChatStateListening) {
                if (speaking) {
                    builtin_led->SetRed(HIGH_BRIGHTNESS);
                } else {
                    builtin_led->SetRed(LOW_BRIGHTNESS);
                }
                builtin_led->TurnOn();
            }
        });
    });

    wake_word_detect_.OnWakeWordDetected([this](const std::string& wake_word) {
        Schedule([this, &wake_word]() {
            if (chat_state_ == kChatStateIdle) {
                SetChatState(kChatStateConnecting);

                if (!protocol_->OpenAudioChannel()) {
                    ESP_LOGE(TAG, "Failed to open audio channel");
                    SetChatState(kChatStateIdle);
                    wake_word_detect_.StartDetection();
                    return;
                }
                
                // 发送唤醒词检测事件
                protocol_->SendWakeWordDetected(wake_word);
                ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
                keep_listening_ = true;
                SetChatState(kChatStateListening);
            } else if (chat_state_ == kChatStateSpeaking) {
                AbortSpeaking(kAbortReasonWakeWordDetected);
            }

            // Resume detection
            wake_word_detect_.StartDetection();
        });
    });
    wake_word_detect_.StartDetection();
#endif

    // Initialize the protocol
    display->SetStatus("初始化协议");
#ifdef CONFIG_CONNECTION_TYPE_WEBSOCKET
    protocol_ = new WebsocketProtocol();
#else
    protocol_ = new MqttProtocol();
#endif
    protocol_->OnNetworkError([this](const std::string& message) {
        Alert("Error", std::move(message));
    });
    protocol_->OnIncomingAudio([this](const std::string& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (chat_state_ == kChatStateSpeaking) {
            audio_decode_queue_.emplace_back(std::move(data));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "服务器的音频采样率 %d 与设备输出的采样率 %d 不一致，重采样后可能会失真",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
        SetDecodeSampleRate(protocol_->server_sample_rate());
        board.SetPowerSaveMode(false);
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        Schedule([this]() {
            SetChatState(kChatStateIdle);
        });
        board.SetPowerSaveMode(true);
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (chat_state_ == kChatStateIdle || chat_state_ == kChatStateListening) {
                        SetChatState(kChatStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (chat_state_ == kChatStateSpeaking) {
                        background_task_.WaitForCompletion();
                        if (keep_listening_) {
                            protocol_->SendStartListening(kListeningModeAutoStop);
                            SetChatState(kChatStateListening);
                        } else {
                            SetChatState(kChatStateIdle);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (text != NULL) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    display->SetChatMessage("assistant", text->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (text != NULL) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                display->SetChatMessage("user", text->valuestring);
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (emotion != NULL) {
                display->SetEmotion(emotion->valuestring);
            }
        }
    });

    // Blink the LED to indicate the device is running
    display->SetStatus("待命");
    builtin_led->SetGreen();
    builtin_led->BlinkOnce();

    SetChatState(kChatStateIdle);
}

void Application::PlayLocalFile(const char* data, size_t size) {
    ESP_LOGI(TAG, "PlayLocalFile: %zu bytes", size);
    SetDecodeSampleRate(16000);
    for (const char* p = data; p < data + size; ) {
        auto p3 = (BinaryProtocol3*)p;
        p += sizeof(BinaryProtocol3);

        auto payload_size = ntohs(p3->payload_size);
        std::string audio_data;
        audio_data.resize(payload_size);
        memcpy(audio_data.data(), p3->payload, payload_size);
        p += payload_size;

        std::lock_guard<std::mutex> lock(mutex_);
        audio_decode_queue_.emplace_back(std::move(audio_data));
    }
}

void Application::OutputAudio() {
    auto now = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int max_silence_seconds = 10;

    std::unique_lock<std::mutex> lock(mutex_);
    if (audio_decode_queue_.empty()) {
        // Disable the output if there is no audio data for a long time
        if (chat_state_ == kChatStateIdle) {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_output_time_).count();
            if (duration > max_silence_seconds) {
                codec->EnableOutput(false);
            }
        }
        return;
    }

    if (chat_state_ == kChatStateListening) {
        audio_decode_queue_.clear();
        return;
    }

    last_output_time_ = now;
    auto audio_data = std::move(audio_decode_queue_.front());
    audio_decode_queue_.pop_front();
    lock.unlock();

    background_task_.Schedule([this, codec, audio_data = std::move(audio_data)]() {
        if (aborted_) {
            return;
        }

        // PCM数据是16位有符号整数
        const int16_t* pcm_data = reinterpret_cast<const int16_t*>(audio_data.data());
        size_t samples = audio_data.size() / sizeof(int16_t);
        
        std::vector<int16_t> output_data;
        
        // 如果采样率不同，需要进行重采样
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            resampler_.Configure(protocol_->server_sample_rate(), codec->output_sample_rate());
            resampler_.Process(pcm_data, samples, output_data);
            ESP_LOGI(TAG, "Resampled %zu samples to %zu samples", samples, output_data.size());
        } else {
            // 直接输出PCM数据
            output_data.assign(pcm_data, pcm_data + samples);
        }
        
        codec->OutputData(output_data);
    });
}

void Application::InputAudio() {
    auto codec = Board::GetInstance().GetAudioCodec();
    std::vector<int16_t> data;
    if (!codec->InputData(data)) {
        return;
    }
    
#if CONFIG_IDF_TARGET_ESP32S3
    if (audio_processor_.IsRunning()) {
        audio_processor_.Input(data);
    }
    if (wake_word_detect_.IsDetectionRunning()) {
        wake_word_detect_.Feed(data);
    }
#else
    if (chat_state_ == kChatStateListening) {
        // PCM数据直接发送，不需要编码
        protocol_->SendAudio(std::string(reinterpret_cast<const char*>(data.data()), 
                                       data.size() * sizeof(int16_t)));
    }
#endif
}

void Application::SetDecodeSampleRate(int sample_rate) {
    // 记录服务器的采样率，用于后续的采样率转换
    server_sample_rate_ = sample_rate;
}
