/*
 * Modified stream.cc for Windows Offline Voice Input (Geek Edition)
 * 包含功能：
 * 1. 无限长语音支持 (Smart Segmentation)
 * 2. 耳语增强 (AGC)
 * 3. [新增] 调试音频保存 (Save debug.wav)
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
#include <ctime> // for timestamp
#include "../zian_services.h"

// ==========================================
// 全局控制信号
// ==========================================
std::atomic<bool> g_is_recording(false);
std::atomic<bool> g_should_exit(false);
std::atomic<bool> g_needs_flush(false);

// ==========================================
// 音频文件保存辅助函数 (用于调试)
// ==========================================

// 复用已有的 wav 转换逻辑
void float_to_pcm16(const std::vector<float>& float_audio, std::vector<int16_t>& pcm16_audio) {
    pcm16_audio.resize(float_audio.size());
    for (size_t i = 0; i < float_audio.size(); ++i) {
        float sample = std::max(-1.0f, std::min(1.0f, float_audio[i]));
        pcm16_audio[i] = static_cast<int16_t>(sample * 32767.0f);
    }
}

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

void write_wav_file(const std::string& filename, const std::vector<float>& audio_data, int sample_rate) {
    if (audio_data.empty()) return;

    std::vector<int16_t> pcm16;
    float_to_pcm16(audio_data, pcm16);

    WAVHeader header;
    header.sample_rate = sample_rate;
    header.byte_rate = header.sample_rate * 2; // 16bit = 2 bytes
    header.data_size = (uint32_t)pcm16.size() * 2;
    header.file_size = header.data_size + 36;

    std::ofstream file(filename, std::ios::binary);
    if (file.is_open()) {
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(pcm16.data()), pcm16.size() * 2);
        file.close();
        // 只有在 debug 模式或为了确认时才打印，以免污染 IPC 通道，
        // 但这里我们加上前缀 DEBUG: 方便 C# 过滤
        std::cout << "[[DEBUG: Saved " << filename << "]]" << std::endl;
    }
}

// ==========================================
// 耳语增强 (AGC) 实现
// ==========================================
void apply_whisper_enhancement(std::vector<float>& audio_chunk) {
    if (audio_chunk.empty()) return;

    float max_amp = 0.0f;
    for (float sample : audio_chunk) {
        float abs_val = std::abs(sample);
        if (abs_val > max_amp) max_amp = abs_val;
    }

    const float NOISE_GATE = 0.01f; // 噪音门限
    const float TARGET_AMP = 0.5f;  // 目标音量
    const float MAX_GAIN = 5.0f;    // 最大增益

    if (max_amp > NOISE_GATE && max_amp < TARGET_AMP) {
        float gain = TARGET_AMP / max_amp;
        if (gain > MAX_GAIN) gain = MAX_GAIN;

        for (size_t i = 0; i < audio_chunk.size(); ++i) {
            audio_chunk[i] *= gain;
            if (audio_chunk[i] > 1.0f) audio_chunk[i] = 1.0f;
            if (audio_chunk[i] < -1.0f) audio_chunk[i] = -1.0f;
        }
    }
}

// ==========================================
// 辅助结构与函数 (参数解析等)
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
    // 【新增】默认关闭，只有传入 --save-audio 才开启，用于调试的时候，保存wav文件
    bool save_audio = false;
    std::string audio_path = "debug.wav";
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
        // 【新增】解析保存音频的参数 (支持可选路径)
        else if (arg == "--save-audio") {
            params.save_audio = true;
            // 检查下一个参数是否是路径 (不以 - 开头)
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                params.audio_path = argv[++i];
            }
        }
    }
    return true;
}

static void sense_voice_free_stream(struct sense_voice_context *ctx) {
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

    std::vector<float> pcmf32_audio; // 临时接收音频 (Chunk)
    std::vector<double> pcmf32;      // 累积音频用于推理 (Inference Buffer)
    pcmf32.reserve(32000 * 30);

    // 【新增】全量录音缓存，用于保存 debug.wav
    // 【修改】只在开启时预留空间，否则不分配
    std::vector<float> full_session_audio; 
    if(params.save_audio){
        full_session_audio.reserve(16000 * 60); // 预留一分钟
    }
    
    sense_voice_full_params wparams = sense_voice_full_default_params(SENSE_VOICE_SAMPLING_GREEDY);
    wparams.language = params.language.c_str();
    wparams.n_threads = params.n_threads;

    int idenitified_floats = 0;
    
    // 分段逻辑变量
    const int SAMPLE_RATE = SENSE_VOICE_SAMPLE_RATE;
    float current_chunk_max_amp = 0.0f; 

    std::cout << "[[ENGINE_READY]]" << std::endl;

    while (!g_should_exit) {
        // A. 录音状态
        if (g_is_recording) {
            audio.get(params.chunk_size, pcmf32_audio);
            if (!pcmf32_audio.empty()) {
                
                // 1. 应用耳语增强
                apply_whisper_enhancement(pcmf32_audio);

                // 2. 计算音量 (用于分段)
                current_chunk_max_amp = 0.0f;
                for(float f : pcmf32_audio) current_chunk_max_amp = std::max(current_chunk_max_amp, std::abs(f));

                // 3. 存入推理 Buffer
                pcmf32.insert(pcmf32.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                
                // 4. 【新增】存入调试保存 Buffer (保存的是增强后的声音)
                // 【修改】只有开启开关时，才往全量 buffer 里塞数据
                if(params.save_audio){
                    full_session_audio.insert(full_session_audio.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                }
                
                pcmf32_audio.clear();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        // B. 触发推理
        const int STEP_SAMPLES = (800 * SAMPLE_RATE) / 1000;
        bool should_inference = (g_is_recording && (pcmf32.size() > idenitified_floats + STEP_SAMPLES));
        bool is_final_flush = (!g_is_recording && g_needs_flush);

        // 智能分段判断
        bool trigger_segmentation = false;
        if (g_is_recording && pcmf32.size() > 0) {
            double duration_sec = (double)pcmf32.size() / SAMPLE_RATE;
            bool is_quiet = (current_chunk_max_amp < 0.05f); 
            if (duration_sec > 15.0 && is_quiet) trigger_segmentation = true;
            else if (duration_sec > 28.0) trigger_segmentation = true;
        }

        if (should_inference || is_final_flush || trigger_segmentation) {
            
            // 最终冲刷时的尾部处理
            if (is_final_flush) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                audio.get(params.chunk_size, pcmf32_audio);
                if (!pcmf32_audio.empty()) {
                    apply_whisper_enhancement(pcmf32_audio); // 记得尾部也要增强

                    pcmf32.insert(pcmf32.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                    
                    // 【新增】调试 Buffer 也要补上这一块
                    // 【修改】同样加上判断
                    if(params.save_audio){
                        full_session_audio.insert(full_session_audio.end(), pcmf32_audio.begin(), pcmf32_audio.end());                        
                    }
                                      
                    pcmf32_audio.clear();
                }
            }

            // 执行推理
            int process_len = (int)pcmf32.size(); 
            if (process_len > 0) {
                if (sense_voice_full_parallel(ctx, wparams, pcmf32, process_len, params.n_processors) == 0) {
                    if (trigger_segmentation) std::cout << "SEG: "; 
                    else std::cout << "RES: "; 
                    
                    sense_voice_print_output(ctx, params.use_prefix, params.use_itn, true);
                    std::cout << std::endl; 
                    
                    idenitified_floats = process_len;
                }
            }

            // 分段清理 (只清空推理 Buffer，不清空录音文件 Buffer)
            if (trigger_segmentation) {
                pcmf32.clear();
                idenitified_floats = 0;
            }

            // 最终结束清理
            if (is_final_flush) {
                // 【新增】保存录音文件到本地
                // 注意：这会保存从按下 START 到 STOP 的完整过程，包括所有分段
                // 【修改】只有开启开关时才写文件
                if(params.save_audio){
                    // Generate timestamped filename
                    std::time_t now = std::time(nullptr);
                    struct tm tstruct;
                    char buf[80];
                    localtime_s(&tstruct, &now);
                    std::strftime(buf, sizeof(buf), "_%Y-%m-%d_%H-%M-%S", &tstruct);

                    std::string final_path = params.audio_path;
                    size_t lastindex = final_path.find_last_of("."); 
                    if (lastindex == std::string::npos) {
                        final_path += buf; 
                        final_path += ".wav";
                    } else {
                        final_path.insert(lastindex, buf); 
                    }
                     write_wav_file(final_path, full_session_audio, SAMPLE_RATE);
                    full_session_audio.clear(); // 清空以备下次使用
                }

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

    sense_voice_free_stream(ctx);
}

int run_input_mode(int argc, char **argv) {
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