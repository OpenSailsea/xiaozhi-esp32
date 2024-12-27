#ifndef WAKE_WORD_DETECT_H
#define WAKE_WORD_DETECT_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_afe_sr_models.h>
#include <esp_nsn_models.h>

#include <list>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>


class WakeWordDetect {
public:
    WakeWordDetect();
    ~WakeWordDetect();

    void Initialize(int channels, bool reference);
    void Feed(std::vector<int16_t>& data);
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback);
    void OnVadStateChange(std::function<void(bool speaking)> callback);
    void StartDetection();
    void StopDetection();
    bool IsDetectionRunning();
    const std::string& GetLastDetectedWakeWord() const { return last_detected_wake_word_; }

private:
    esp_afe_sr_data_t* afe_detection_data_ = nullptr;
    char* wakenet_model_ = NULL;
    std::vector<std::string> wake_words_;
    std::vector<int16_t> input_buffer_;
    EventGroupHandle_t event_group_;
    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
    std::function<void(bool speaking)> vad_state_change_callback_;
    bool is_speaking_ = false;
    int channels_;
    bool reference_;
    std::string last_detected_wake_word_;

    std::list<std::vector<int16_t>> wake_word_pcm_;

    void StoreWakeWordData(uint16_t* data, size_t size);
    void AudioDetectionTask();
};

#endif
