/**
 * MQTT协议实现文件
 * 
 * 该文件实现了基于MQTT和UDP的音频通信协议，主要功能包括：
 * 1. MQTT连接的建立和维护
 *    - TLS加密连接
 *    - 自动重连机制
 *    - 心跳保活
 * 
 * 2. 消息的发送和接收
 *    - JSON格式消息处理
 *    - 消息类型分发
 *    - QoS等级控制
 * 
 * 3. 音频通道的创建和管理
 *    - UDP通道建立
 *    - 会话管理
 *    - 音频参数协商
 * 
 * 4. 数据加密传输
 *    - AES-CTR加密
 *    - 序列号防重放
 *    - 丢包检测
 * 
 * 使用说明：
 * 1. 需要在配置文件中设置MQTT连接参数
 * 2. 音频数据使用opus编码
 * 3. 支持双向音频流
 * 
 * 依赖项：
 * - FreeRTOS：事件组和互斥锁
 * - mbedtls：AES加密
 * - cJSON：消息解析
 * - ESP-IDF：日志系统
 */

#include "mqtt_protocol.h"  // MQTT协议类定义
#include "board.h"         // 硬件抽象层
#include "application.h"   // 应用程序框架
#include "settings.h"      // 配置管理

#include <esp_log.h>      // ESP32日志系统
#include <ml307_mqtt.h>   // MQTT客户端实现
#include <ml307_udp.h>    // UDP客户端实现
#include <cstring>        // 字符串操作
#include <arpa/inet.h>    // 网络字节序转换

#define TAG "MQTT"        // 日志标签，用于标识日志来源

// 构造函数：初始化MQTT协议对象
// 功能：
// 1. 创建FreeRTOS事件组用于同步操作
// 2. 启动MQTT客户端并建立连接
// 注意：
// - 构造函数可能会抛出异常（如内存分配失败）
// - MQTT连接失败不会抛出异常，但会记录错误日志
MqttProtocol::MqttProtocol() {
    // 创建事件组，用于同步操作
    // 事件组用于在异步操作中等待服务器响应
    event_group_handle_ = xEventGroupCreate();
    if (event_group_handle_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create event group");
        throw std::runtime_error("Failed to create event group");
    }

    // 启动MQTT客户端并尝试连接
    StartMqttClient();
}

// 析构函数：清理资源
// 功能：
// 1. 关闭并清理UDP连接
// 2. 断开并清理MQTT连接
// 3. 释放FreeRTOS事件组
// 注意：
// - 析构函数会自动关闭所有网络连接
// - 会记录清理过程的日志
// - 不会抛出异常
MqttProtocol::~MqttProtocol() {
    ESP_LOGI(TAG, "MqttProtocol deinit");
    // 清理UDP客户端，关闭音频通道
    if (udp_ != nullptr) {
        delete udp_;  // 会自动关闭UDP连接
        udp_ = nullptr;
    }
    // 清理MQTT客户端，断开MQTT连接
    if (mqtt_ != nullptr) {
        delete mqtt_;  // 会自动断开MQTT连接
        mqtt_ = nullptr;
    }
    // 删除FreeRTOS事件组，释放系统资源
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
        event_group_handle_ = nullptr;
    }
}

// 启动MQTT客户端
// 功能：
// 1. 从配置文件加载MQTT连接参数
// 2. 创建并配置MQTT客户端
// 3. 设置消息处理回调
// 4. 建立安全连接(TLS)
// 返回值：
//   true: 连接成功且订阅主题完成
//   false: 连接失败或配置错误
bool MqttProtocol::StartMqttClient() {
    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "Mqtt client already started");
        delete mqtt_;
    }

    // 从配置文件加载MQTT连接参数
    // 配置文件格式：
    // {
    //   "endpoint": "mqtt服务器地址",
    //   "client_id": "客户端唯一标识",
    //   "username": "认证用户名",
    //   "password": "认证密码",
    //   "subscribe_topic": "订阅主题",
    //   "publish_topic": "发布主题"
    // }
    Settings settings("mqtt", false);
    endpoint_ = settings.GetString("endpoint");         // MQTT服务器地址（支持TLS的端点）
    client_id_ = settings.GetString("client_id");       // 客户端唯一标识
    username_ = settings.GetString("username");         // 认证用户名
    password_ = settings.GetString("password");         // 认证密码
    subscribe_topic_ = settings.GetString("subscribe_topic"); // 服务器消息订阅主题
    publish_topic_ = settings.GetString("publish_topic");     // 客户端消息发布主题

    if (endpoint_.empty()) {
        ESP_LOGE(TAG, "MQTT endpoint is not specified");
        return false;
    }

    // 创建MQTT客户端实例
    mqtt_ = Board::GetInstance().CreateMqtt();
    mqtt_->SetKeepAlive(90);  // 设置90秒的保活时间，确保连接不会因为空闲而断开

    // 设置断开连接的回调函数
    // 当连接意外断开时，会触发此回调
    mqtt_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Disconnected from endpoint");
    });

    // 设置消息接收回调
    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        // 解析接收到的JSON消息
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (type == nullptr) {
            ESP_LOGE(TAG, "Message type is not specified");
            cJSON_Delete(root);
            return;
        }

        // 处理不同类型的消息
        if (strcmp(type->valuestring, "hello") == 0) {
            // 处理服务器的hello响应
            ParseServerHello(root);
        } else if (strcmp(type->valuestring, "goodbye") == 0) {
            // 处理服务器的goodbye消息
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            if (session_id == nullptr || session_id_ == session_id->valuestring) {
                // 安排在主线程中关闭音频通道
                Application::GetInstance().Schedule([this]() {
                    CloseAudioChannel();
                });
            }
        } else if (on_incoming_json_ != nullptr) {
            // 处理其他类型的JSON消息
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
    });

    // 尝试连接到MQTT服务器
    ESP_LOGI(TAG, "Connecting to endpoint %s", endpoint_.c_str());
    if (!mqtt_->Connect(endpoint_, 8883, client_id_, username_, password_)) {  // 使用TLS端口8883
        ESP_LOGE(TAG, "Failed to connect to endpoint");
        if (on_network_error_ != nullptr) {
            on_network_error_("无法连接服务");  // 触发网络错误回调
        }
        return false;
    }

    // 连接成功后订阅主题
    ESP_LOGI(TAG, "Connected to endpoint");
    if (!subscribe_topic_.empty()) {
        mqtt_->Subscribe(subscribe_topic_, 2);  // QoS=2，确保消息只收到一次
    }
    return true;  // 连接和订阅都成功完成
}

// 发送文本消息到MQTT服务器
// 功能：将文本消息发布到配置的MQTT主题
// 参数：
//   text: 要发送的文本消息（通常是JSON格式）
// 注意：
// - 如果发布主题未配置，消息将被静默丢弃
// - 不会重试发送失败的消息
// - 消息发送是异步的，不会等待服务器确认
void MqttProtocol::SendText(const std::string& text) {
    // 检查发布主题是否为空
    if (publish_topic_.empty()) {
        ESP_LOGW(TAG, "Publish topic is not configured");
        return;
    }
    // 发布消息到指定主题
    // QoS默认为0，最多发送一次，不保证到达
    mqtt_->Publish(publish_topic_, text);
}

// 发送加密的音频数据
// 功能：
// 1. 使用AES-CTR模式加密音频数据
// 2. 添加序列号防止重放攻击
// 3. 通过UDP发送加密后的数据
// 数据包格式：
//   [nonce(16字节)][加密数据]
//   nonce结构：
//   - 字节0-1：固定标识符
//   - 字节2-3：数据大小(网络字节序)
//   - 字节4-11：保留
//   - 字节12-15：序列号(网络字节序)
void MqttProtocol::SendAudio(const std::string& data) {
    std::lock_guard<std::mutex> lock(channel_mutex_);  // 保护UDP发送，确保线程安全
    if (udp_ == nullptr) {
        return;  // UDP通道未建立，不能发送数据
    }

    // 构建加密用的nonce（16字节）
    std::string nonce(aes_nonce_);
    *(uint16_t*)&nonce[2] = htons(data.size());        // 写入数据大小（网络字节序）
    *(uint32_t*)&nonce[12] = htonl(++local_sequence_); // 写入递增的序列号（网络字节序）

    // 准备加密后的数据缓冲区
    std::string encrypted;
    encrypted.resize(aes_nonce_.size() + data.size());  // 预分配空间：nonce + 加密数据
    memcpy(encrypted.data(), nonce.data(), nonce.size()); // 复制nonce到数据包头部

    // 使用AES-CTR模式加密数据
    size_t nc_off = 0;  // CTR模式的偏移量
    uint8_t stream_block[16] = {0};  // CTR模式的流密码块
    if (mbedtls_aes_crypt_ctr(&aes_ctx_, data.size(), &nc_off, (uint8_t*)nonce.c_str(), stream_block,
        (uint8_t*)data.data(), (uint8_t*)&encrypted[nonce.size()]) != 0) {
        ESP_LOGE(TAG, "Failed to encrypt audio data");
        return;
    }
    // 通过UDP发送加密后的数据包
    udp_->Send(encrypted);
}

// 关闭音频通道
// 功能：
// 1. 安全关闭UDP连接
// 2. 通知服务器结束会话
// 3. 触发关闭回调
// 注意：
// - 该函数是线程安全的
// - 可以重复调用（幂等操作）
void MqttProtocol::CloseAudioChannel() {
    {
        // 使用互斥锁保护UDP实例的删除
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (udp_ != nullptr) {
            delete udp_;  // 关闭UDP连接并释放资源
            udp_ = nullptr;
        }
    }

    // 发送goodbye消息通知服务器
    std::string message = "{";
    message += "\"session_id\":\"" + session_id_ + "\",";  // 包含会话ID
    message += "\"type\":\"goodbye\"";                     // 消息类型为goodbye
    message += "}";
    SendText(message);  // 通过MQTT发送

    // 触发通道关闭回调
    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

// 打开音频通道
// 功能：
// 1. 确保MQTT连接正常
// 2. 向服务器发送hello消息请求建立UDP通道
// 3. 等待服务器响应并建立UDP连接
// 4. 设置音频数据接收回调
// 返回值：
//   true: 通道建立成功
//   false: 建立失败（可能是连接超时或服务器拒绝）
bool MqttProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGI(TAG, "MQTT is not connected, try to connect now");
        if (!StartMqttClient()) {
            return false;
        }
    }

    session_id_ = "";

    // 构建并发送hello消息，包含：
    // - 协议版本号
    // - 传输方式(UDP)
    // - 音频参数(opus格式、采样率、通道数、帧长度)
    std::string message = "{";
    message += "\"type\":\"hello\",";
    message += "\"version\": 3,";
    message += "\"transport\":\"udp\",";
    message += "\"audio_params\":{";
#if CONFIG_USE_OPUS_CODEC
    message += "\"format\":\"opus\", \"sample_rate\":16000, \"channels\":1, \"frame_duration\":" + std::to_string(OPUS_FRAME_DURATION_MS);
#else
    message += "\"format\":\"pcm\", \"sample_rate\":16000, \"channels\":1";
#endif
    message += "}}";
    SendText(message);

    // 等待服务器hello响应，超时时间10秒
    // 使用FreeRTOS事件组等待服务器的hello响应
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        if (on_network_error_ != nullptr) {
            on_network_error_("等待响应超时");
        }
        return false;
    }

    // 创建新的UDP客户端
    std::lock_guard<std::mutex> lock(channel_mutex_);  // 使用互斥锁保护UDP实例的创建
    if (udp_ != nullptr) {
        delete udp_;
    }
    udp_ = Board::GetInstance().CreateUdp();  // 创建UDP实例用于音频数据传输
    // 设置UDP消息接收回调
    udp_->OnMessage([this](const std::string& data) {
        // 验证数据包的基本格式
        if (data.size() < sizeof(aes_nonce_)) {
            ESP_LOGE(TAG, "Invalid audio packet size: %zu", data.size());
            return;
        }
        // 检查数据包类型（0x01表示音频数据）
        if (data[0] != 0x01) {
            ESP_LOGE(TAG, "Invalid audio packet type: %x", data[0]);
            return;
        }
        // 处理数据包序列号，确保按顺序处理
        uint32_t sequence = ntohl(*(uint32_t*)&data[12]);
        // 丢弃过期的数据包
        if (sequence < remote_sequence_) {
            ESP_LOGW(TAG, "Received audio packet with old sequence: %lu, expected: %lu", sequence, remote_sequence_);
            return;
        }
        // 检测是否有丢包
        if (sequence != remote_sequence_ + 1) {
            ESP_LOGW(TAG, "Received audio packet with wrong sequence: %lu, expected: %lu", sequence, remote_sequence_ + 1);
        }

        // 解密音频数据
        std::string decrypted;
        size_t decrypted_size = data.size() - aes_nonce_.size();
        size_t nc_off = 0;
        uint8_t stream_block[16] = {0};
        decrypted.resize(decrypted_size);
        auto nonce = (uint8_t*)data.data();                    // nonce在数据包头部
        auto encrypted = (uint8_t*)data.data() + aes_nonce_.size(); // 加密数据紧跟在nonce后
        int ret = mbedtls_aes_crypt_ctr(&aes_ctx_, decrypted_size, &nc_off, nonce, stream_block, encrypted, (uint8_t*)decrypted.data());
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to decrypt audio data, ret: %d", ret);
            return;
        }
        if (on_incoming_audio_ != nullptr) {
            on_incoming_audio_(decrypted);
        }
        remote_sequence_ = sequence;
    });

    udp_->Connect(udp_server_, udp_port_);

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

// 解析服务器返回的hello消息
// 功能：
// 1. 验证传输方式是否为UDP
// 2. 获取会话ID和音频参数
// 3. 设置UDP连接参数
// 4. 初始化AES加密
// 参数：
//   root: 服务器返回的JSON消息根节点
void MqttProtocol::ParseServerHello(const cJSON* root) {
    // 验证传输方式
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    // 获取会话ID，用于标识当前音频会话
    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (session_id != nullptr) {
        session_id_ = session_id->valuestring;
    }

    // 获取服务器支持的音频参数
    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (audio_params != NULL) {
        // 获取采样率
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (sample_rate != NULL) {
            server_sample_rate_ = sample_rate->valueint;
        }
    }

    // 获取UDP连接参数
    auto udp = cJSON_GetObjectItem(root, "udp");
    if (udp == nullptr) {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    // 设置UDP服务器地址和端口
    udp_server_ = cJSON_GetObjectItem(udp, "server")->valuestring;
    udp_port_ = cJSON_GetObjectItem(udp, "port")->valueint;
    
    // 获取加密参数
    auto key = cJSON_GetObjectItem(udp, "key")->valuestring;     // AES密钥
    auto nonce = cJSON_GetObjectItem(udp, "nonce")->valuestring; // 初始化向量

    // 初始化AES加密
    aes_nonce_ = DecodeHexString(nonce);                         // 解码nonce
    mbedtls_aes_init(&aes_ctx_);                                // 初始化AES上下文
    mbedtls_aes_setkey_enc(&aes_ctx_,                          // 设置128位AES密钥
        (const unsigned char*)DecodeHexString(key).c_str(), 128);
    
    // 重置序列号计数器
    local_sequence_ = 0;  // 本地发送序列号
    remote_sequence_ = 0; // 远程接收序列号
    
    // 通知等待线程服务器已响应
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

// 十六进制字符集（用于十六进制字符串转换）
static const char hex_chars[] = "0123456789ABCDEF";

// 辅助函数：将单个十六进制字符转换为对应的数值
// 参数：
//   c: 十六进制字符（'0'-'9', 'A'-'F', 'a'-'f'）
// 返回值：
//   对应的数值(0-15)
//   对于无效输入返回0
// 注意：
// - 函数不检查输入字符的有效性
// - 使用内联优化性能
static inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';        // 处理数字字符
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;   // 处理大写字母
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;   // 处理小写字母
    return 0;  // 对于无效输入，返回0
}

// 将十六进制字符串解码为二进制数据
// 功能：
// 1. 将十六进制字符串转换为原始二进制数据
// 2. 每两个十六进制字符转换为一个字节
// 3. 支持大小写混合的十六进制字符
// 参数：
//   hex_string: 十六进制字符串（长度必须是偶数）
// 返回值：
//   解码后的二进制数据
// 注意：
// - 输入字符串长度必须是偶数
// - 无效字符会被当作0处理
std::string MqttProtocol::DecodeHexString(const std::string& hex_string) {
    std::string decoded;
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        char byte = (CharToHex(hex_string[i]) << 4) | CharToHex(hex_string[i + 1]);
        decoded.push_back(byte);
    }
    return decoded;
}

// 检查音频通道是否已打开
// 功能：检查UDP通道是否已建立并可用
// 返回值：
//   true: UDP通道已建立且可用
//   false: UDP通道未建立或已关闭
// 注意：
// - 该函数是线程安全的
// - 仅检查UDP实例是否存在，不验证连接状态
bool MqttProtocol::IsAudioChannelOpened() const {
    return udp_ != nullptr;  // 通过检查UDP实例判断通道状态
}
