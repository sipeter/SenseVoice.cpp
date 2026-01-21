/*
 * Modified stream.cc for Windows Offline Voice Input (IPC Mode)
 * 这是一个完整的替换文件，专为 "C# 极客输入法" 设计。
 */

#include "common-sdl.h"
#include "common.h"
#include "sense-voice.h"
#include "silero-vad.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <cstring>
#include <atomic>
#include <iostream>

// ==========================================
// 全局控制信号 (用于线程间通信)
// ==========================================
std::atomic<bool> g_is_recording(false);
std::atomic<bool> g_should_exit(false);
std::atomic<bool> g_needs_flush(false);

// ==========================================
// 辅助结构与函数 (保留自原版)
// ==========================================

// 简单的 WAV 头定义，虽然我们不存文件，但保留结构体以防万一后续扩展
struct WAVHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t file_size;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1;
    uint16_t num_channels = 1;
    uint32_t sample_rate = SENSE_VOICE_SAMPLE_RATE;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size;
    WAVHeader() {
        byte_rate = sample_rate * num_channels * bits_per_sample / 8;
        block_align = num_channels * bits_per_sample / 8;
        file_size = 0; data_size = 0;
    }
};

void float_to_pcm16(const std::vector<float>& float_audio, std::vector<int16_t>& pcm16_audio) {
    pcm16_audio.resize(float_audio.size());
    for (size_t i = 0; i < float_audio.size(); ++i) {
        float sample = std::max(-1.0f, std::min(1.0f, float_audio[i]));
        pcm16_audio[i] = static_cast<int16_t>(sample * 32767.0f);
    }
}

// 参数配置结构体
struct sense_voice_stream_params {
    int32_t n_threads = std::min(4, (int32_t) std::thread::hardware_concurrency());
    int32_t n_processors = 1;
    int32_t capture_id = -1;
    int32_t chunk_size = 50; // ms
    bool use_gpu = true;
    bool flash_attn = false;
    bool debug_mode = false;
    bool use_vad = false; // 输入法模式下，我们手动控制，默认关闭VAD
    bool use_itn = false; // 逆文本正则化 (数字转汉字等)
    bool use_prefix = false;
    std::string language = "auto";
    std::string model = "models/ggml-base.en.bin";
    float speech_prob_threshold = 0.1f;
};

// 参数解析函数 (完整版)
static bool get_stream_params(int argc, char **argv, sense_voice_stream_params &params) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-t" || arg == "--threads") {
            params.n_threads = std::stoi(argv[++i]);
        } else if (arg == "-c" || arg == "--capture") {
            params.capture_id = std::stoi(argv[++i]);
        } else if (arg == "-l" || arg == "--language") {
            params.language = argv[++i];
        } else if (arg == "-m" || arg == "--model") {
            params.model = argv[++i];
        } else if (arg == "-ng" || arg == "--no-gpu") {
            params.use_gpu = false;
        } else if (arg == "-fa" || arg == "--flash-attn") {
            params.flash_attn = true;
        } else if (arg == "--use-itn") {
            params.use_itn = true;
        } else if (arg == "--use-vad") {
            params.use_vad = true;
        } else if (arg == "-h" || arg == "--help") {
            fprintf(stderr, "Usage: %s -m models/sense-voice-small.bin -l zh --use-itn\n", argv[0]);
            exit(0);
        }
    }
    return true;
}

// 资源释放函数
void sense_voice_free(struct sense_voice_context *ctx) {
    if (ctx) {
        ggml_free(ctx->model.ctx);
        ggml_backend_buffer_free(ctx->model.buffer);
        // 如果使用了 VAD，释放相关资源
        if (ctx->state) {
            if (ctx->state->vad_ctx) ggml_free(ctx->state->vad_ctx);
            if (ctx->state->vad_lstm_hidden_state_buffer) ggml_backend_buffer_free(ctx->state->vad_lstm_hidden_state_buffer);
            if (ctx->state->vad_lstm_context_buffer) ggml_backend_buffer_free(ctx->state->vad_lstm_context_buffer);
            sense_voice_free_state(ctx->state);
        }
        delete ctx->model.model->encoder;
        delete ctx->model.model;
        delete ctx;
    }
}

// ==========================================
// 核心逻辑：音频工作线程
// ==========================================
void AudioWorker(sense_voice_stream_params params) {
    // 1. 初始化 SDL 音频采集
    audio_async audio(params.chunk_size << 2);
    if (!audio.init(params.capture_id, SENSE_VOICE_SAMPLE_RATE)) {
        fprintf(stderr, "Error: Audio init failed!\n");
        return;
    }
    audio.resume(); // 保持设备开启

    // 2. 加载 SenseVoice 模型
    struct sense_voice_context_params cparams = sense_voice_context_default_params();
    cparams.use_gpu = params.use_gpu;
    cparams.flash_attn = params.flash_attn;
    cparams.use_itn = params.use_itn;

    struct sense_voice_context *ctx = sense_voice_small_init_from_file_with_params(params.model.c_str(), cparams);
    if (!ctx) {
        fprintf(stderr, "Error: Failed to load model '%s'\n", params.model.c_str());
        return;
    }

    ctx->language_id = sense_voice_lang_id(params.language.c_str());
    if (ctx->language_id == -1) ctx->language_id = sense_voice_lang_id("auto");

    // 3. 准备数据缓冲区
    std::vector<float> pcmf32_audio; // 临时接收音频
    std::vector<double> pcmf32;      // 累积音频用于推理
    pcmf32.reserve(32000);

    sense_voice_full_params wparams = sense_voice_full_default_params(SENSE_VOICE_SAMPLING_GREEDY);
    wparams.language = params.language.c_str();
    wparams.n_threads = params.n_threads;

    int idenitified_floats = 0;

    // 向 C# 发送就绪信号
    std::cout << "[[ENGINE_READY]]" << std::endl;

    // 4. 进入死循环，等待 g_is_recording 信号
    while (!g_should_exit) {
        // A. 录音状态：持续获取音频
        if (g_is_recording) {
            audio.get(params.chunk_size, pcmf32_audio);
            if (!pcmf32_audio.empty()) {
                pcmf32.insert(pcmf32.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                pcmf32_audio.clear();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        // B. 触发推理：流式触发 vs 最终冲刷触发
        const int STEP_SAMPLES = (800 * SENSE_VOICE_SAMPLE_RATE) / 1000;
        bool should_inference = (g_is_recording && (pcmf32.size() > idenitified_floats + STEP_SAMPLES));
        bool is_final_flush = (!g_is_recording && g_needs_flush);

        if (should_inference || is_final_flush) {
            
            // 【核心修复】：如果是最终冲刷，模拟“人肉长按”，多抓 200ms 的声音
            if (is_final_flush) {
                // 等待声音从声卡硬件流向程序驱动
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                // 强制把驱动缓冲区里的最后一点音频“抠出来”
                audio.get(params.chunk_size, pcmf32_audio);
                if (!pcmf32_audio.empty()) {
                    pcmf32.insert(pcmf32.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                    pcmf32_audio.clear();
                }
            }

            // 执行推理：处理 pcmf32 里的所有数据
            int process_len = (int)pcmf32.size(); 
            if (process_len > 0) {
                if (sense_voice_full_parallel(ctx, wparams, pcmf32, process_len, params.n_processors) == 0) {
                    // 确保 C# 能够即时收到
                    std::cout << "RES: "; 
                    sense_voice_print_output(ctx, params.use_prefix, params.use_itn, true);
                    std::cout << std::endl; 
                    idenitified_floats = process_len;
                }
            }

            // 冲刷结束后的收尾工作
            if (is_final_flush) {
                g_needs_flush = false; 
                pcmf32.clear();
                idenitified_floats = 0;
                audio.clear(); // 清空 SDL 内部队列
                std::cout << "[[STOPPED]]" << std::endl; // 通知 C# 流程彻底结束
            }
        }

        // C. 休眠逻辑：既不录音也不冲刷时深睡
        if (!g_is_recording && !g_needs_flush) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    sense_voice_free(ctx);
}

// ==========================================
// 主程序入口：命令行参数解析 & IPC 监听
// ==========================================
int main(int argc, char **argv) {
    // 禁用 stdout 缓存，确保 C# 能即时收到数据
    std::setvbuf(stdout, NULL, _IONBF, 0);

    sense_voice_stream_params params;
    if (!get_stream_params(argc, argv, params)) return 1;

    // 启动音频线程
    std::thread worker(AudioWorker, params);

    // 主线程：监听标准输入 (IPC)
    std::string line;
    while (std::getline(std::cin, line)) {
        // 清理行尾换行符 (Windows/Linux 兼容)
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }

        if (line == "START") {
            g_is_recording = true;
            g_needs_flush = false; // 开始新录音，重置冲刷标志
        } 
        else if (line == "STOP") {
            g_is_recording = false; // 必须设为 false，否则 AudioWorker 不会进入 Flush 逻辑
            g_needs_flush = true;   // 标记需要进行最后一次冲刷
            // 可以在这里打印一个标记，告诉 C# 本次录音结束
            // std::cout << "[[STOPPED]]" << std::endl;
        }
        else if (line == "EXIT") {
            g_should_exit = true;
            break;
        }
    }

    if (worker.joinable()) worker.join();
    return 0;
}