/*
 * Modified stream.cc for Windows Offline Voice Input (Geek Edition)
 * 包含：无限长语音支持 (Smart Segmentation) + 耳语增强 (AGC)
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
#include <cmath> // used for AGC

// ==========================================
// 全局控制信号
// ==========================================
std::atomic<bool> g_is_recording(false);
std::atomic<bool> g_should_exit(false);
std::atomic<bool> g_needs_flush(false);

// ==========================================
// 【Task 2】耳语增强 (AGC) 实现
// ==========================================
// 简单的自动增益控制：将过小的声音放大，但保留底噪
void apply_whisper_enhancement(std::vector<float>& audio_chunk) {
    if (audio_chunk.empty()) return;

    float max_amp = 0.0f;
    for (float sample : audio_chunk) {
        float abs_val = std::abs(sample);
        if (abs_val > max_amp) max_amp = abs_val;
    }

    // 参数配置
    const float NOISE_GATE = 0.01f; // 噪音门限：低于此值被视为背景噪音，不盲目放大
    const float TARGET_AMP = 0.5f;  // 目标音量：希望放大到的峰值 (0.0 - 1.0)
    const float MAX_GAIN = 5.0f;    // 最大增益倍数：防止爆音

    // 逻辑：如果声音大于噪音门限，但小于目标音量，则进行放大
    if (max_amp > NOISE_GATE && max_amp < TARGET_AMP) {
        float gain = TARGET_AMP / max_amp;
        if (gain > MAX_GAIN) gain = MAX_GAIN;

        // 应用增益
        for (size_t i = 0; i < audio_chunk.size(); ++i) {
            audio_chunk[i] *= gain;
            // 硬限幅防止溢出
            if (audio_chunk[i] > 1.0f) audio_chunk[i] = 1.0f;
            if (audio_chunk[i] < -1.0f) audio_chunk[i] = -1.0f;
        }
    }
}

// ==========================================
// 辅助结构与函数 (保留)
// ==========================================
struct sense_voice_stream_params {
    int32_t n_threads = std::min(4, (int32_t) std::thread::hardware_concurrency());
    int32_t n_processors = 1;
    int32_t capture_id = -1;
    int32_t chunk_size = 50; // ms
    bool use_gpu = true;
    bool flash_attn = false;
    bool debug_mode = false;
    bool use_vad = false;
    bool use_itn = false;
    bool use_prefix = false;
    std::string language = "auto";
    std::string model = "models/ggml-base.en.bin";
    float speech_prob_threshold = 0.1f;
};

static bool get_stream_params(int argc, char **argv, sense_voice_stream_params &params) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-t" || arg == "--threads") params.n_threads = std::stoi(argv[++i]);
        else if (arg == "-c" || arg == "--capture") params.capture_id = std::stoi(argv[++i]);
        else if (arg == "-l" || arg == "--language") params.language = argv[++i];
        else if (arg == "-m" || arg == "--model") params.model = argv[++i];
        else if (arg == "-ng" || arg == "--no-gpu") params.use_gpu = false;
        else if (arg == "-fa" || arg == "--flash-attn") params.flash_attn = true;
        else if (arg == "--use-itn") params.use_itn = true;
        else if (arg == "--use-vad") params.use_vad = true;
    }
    return true;
}

void sense_voice_free(struct sense_voice_context *ctx) {
    if (ctx) {
        ggml_free(ctx->model.ctx);
        ggml_backend_buffer_free(ctx->model.buffer);
        delete ctx->model.model->encoder;
        delete ctx->model.model;
        delete ctx;
    }
}

// ==========================================
// 核心逻辑：音频工作线程
// ==========================================
void AudioWorker(sense_voice_stream_params params) {
    audio_async audio(params.chunk_size << 2);
    if (!audio.init(params.capture_id, SENSE_VOICE_SAMPLE_RATE)) {
        fprintf(stderr, "Error: Audio init failed!\n");
        return;
    }
    audio.resume();

    struct sense_voice_context_params cparams = sense_voice_context_default_params();
    cparams.use_gpu = params.use_gpu;
    cparams.flash_attn = params.flash_attn;
    cparams.use_itn = params.use_itn;

    struct sense_voice_context *ctx = sense_voice_small_init_from_file_with_params(params.model.c_str(), cparams);
    if (!ctx) return;

    ctx->language_id = sense_voice_lang_id(params.language.c_str());
    if (ctx->language_id == -1) ctx->language_id = sense_voice_lang_id("auto");

    std::vector<float> pcmf32_audio; // 临时接收音频
    std::vector<double> pcmf32;      // 累积音频用于推理
    pcmf32.reserve(32000 * 30);      // 预留空间

    sense_voice_full_params wparams = sense_voice_full_default_params(SENSE_VOICE_SAMPLING_GREEDY);
    wparams.language = params.language.c_str();
    wparams.n_threads = params.n_threads;

    int idenitified_floats = 0;
    
    // 分段逻辑使用的变量
    const int SAMPLE_RATE = SENSE_VOICE_SAMPLE_RATE;
    float current_chunk_max_amp = 0.0f; // 当前小块的音量峰值

    std::cout << "[[ENGINE_READY]]" << std::endl;

    while (!g_should_exit) {
        // A. 录音状态
        if (g_is_recording) {
            audio.get(params.chunk_size, pcmf32_audio);
            if (!pcmf32_audio.empty()) {
                
                // 【Task 2 调用点】应用耳语增强 (AGC)
                apply_whisper_enhancement(pcmf32_audio);

                // 计算当前 chunk 的静音状态 (用于分段判断)
                current_chunk_max_amp = 0.0f;
                for(float f : pcmf32_audio) current_chunk_max_amp = std::max(current_chunk_max_amp, std::abs(f));

                // 存入主缓冲区
                pcmf32.insert(pcmf32.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                pcmf32_audio.clear();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        // B. 触发推理 (常规流式 + 最终冲刷 + 智能分段)
        const int STEP_SAMPLES = (800 * SAMPLE_RATE) / 1000;
        bool should_inference = (g_is_recording && (pcmf32.size() > idenitified_floats + STEP_SAMPLES));
        bool is_final_flush = (!g_is_recording && g_needs_flush);

        // 【Task 1】智能分段检测逻辑
        bool trigger_segmentation = false;
        if (g_is_recording && pcmf32.size() > 0) {
            double duration_sec = (double)pcmf32.size() / SAMPLE_RATE;
            bool is_quiet = (current_chunk_max_amp < 0.05f); // 静音门限

            // 规则1：超过15秒且遇到静音 -> 切分
            if (duration_sec > 15.0 && is_quiet) trigger_segmentation = true;
            // 规则2：超过28秒 (模型极限) -> 强制切分
            else if (duration_sec > 28.0) trigger_segmentation = true;
        }

        if (should_inference || is_final_flush || trigger_segmentation) {
            
            // 最终冲刷时的尾部处理
            if (is_final_flush) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                audio.get(params.chunk_size, pcmf32_audio);
                if (!pcmf32_audio.empty()) {
                    apply_whisper_enhancement(pcmf32_audio); // 尾部也要增强
                    pcmf32.insert(pcmf32.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                    pcmf32_audio.clear();
                }
            }

            // 执行推理
            int process_len = (int)pcmf32.size(); 
            if (process_len > 0) {
                if (sense_voice_full_parallel(ctx, wparams, pcmf32, process_len, params.n_processors) == 0) {
                    
                    // 【Task 1】输出逻辑：如果是分段，输出 SEG；如果是流式或结束，输出 RES
                    if (trigger_segmentation) {
                         std::cout << "SEG: "; // Segment Commmitted
                    } else {
                         std::cout << "RES: "; // Interim Result
                    }
                    
                    sense_voice_print_output(ctx, params.use_prefix, params.use_itn, true);
                    std::cout << std::endl; 
                    
                    idenitified_floats = process_len;
                }
            }

            // 【Task 1】分段后的清理工作
            if (trigger_segmentation) {
                // 清空推理缓冲区，像新开始一样，但不需要重启 SDL 设备
                pcmf32.clear();
                idenitified_floats = 0;
                // 注意：不设置 g_needs_flush，因为用户还在按着键
            }

            // 最终冲刷后的清理工作
            if (is_final_flush) {
                g_needs_flush = false; 
                pcmf32.clear();
                idenitified_floats = 0;
                audio.clear(); 
                std::cout << "[[STOPPED]]" << std::endl; 
            }
        }

        if (!g_is_recording && !g_needs_flush) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    sense_voice_free(ctx);
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, NULL, _IONBF, 0);
    sense_voice_stream_params params;
    if (!get_stream_params(argc, argv, params)) return 1;

    std::thread worker(AudioWorker, params);

    std::string line;
    while (std::getline(std::cin, line)) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }

        if (line == "START") {
            g_is_recording = true;
            g_needs_flush = false;
        } 
        else if (line == "STOP") {
            g_is_recording = false;
            g_needs_flush = true;
        }
        else if (line == "EXIT") {
            g_should_exit = true;
            break;
        }
    }

    if (worker.joinable()) worker.join();
    return 0;
}