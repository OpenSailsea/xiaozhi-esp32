#include "resampler.h"
#include <esp_log.h>
#include <cmath>

static const char* TAG = "Resampler";

Resampler::Resampler() {}

Resampler::~Resampler() {}

void Resampler::Configure(int input_rate, int output_rate) {
    input_rate_ = input_rate;
    output_rate_ = output_rate;
    ratio_ = static_cast<double>(output_rate) / input_rate;
    ESP_LOGI(TAG, "Resampling ratio: %f", ratio_);
}

void Resampler::Process(const int16_t* input, size_t input_samples, std::vector<int16_t>& output) {
    // 线性插值重采样
    size_t output_samples = static_cast<size_t>(std::ceil(input_samples * ratio_));
    output.resize(output_samples);

    for (size_t i = 0; i < output_samples; i++) {
        double src_idx = i / ratio_;
        size_t src_idx_floor = static_cast<size_t>(src_idx);
        double frac = src_idx - src_idx_floor;

        if (src_idx_floor >= input_samples - 1) {
            output[i] = input[input_samples - 1];
            continue;
        }

        // 线性插值
        int32_t sample1 = input[src_idx_floor];
        int32_t sample2 = input[src_idx_floor + 1];
        output[i] = static_cast<int16_t>(sample1 + (sample2 - sample1) * frac);
    }
}
