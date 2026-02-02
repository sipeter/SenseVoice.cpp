#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include "zian_services.h"

int main(int argc, char* argv[]) {
    // 设置 DLL 搜索目录为当前 exe 同级目录下的 "lib" 文件夹
    // 这样可以让根目录保持干净
    SetDllDirectoryA("lib");

    if (argc < 2) {
        std::cerr << "ZianCore Engine" << std::endl;
        std::cerr << "Usage: ZianCore.exe --mode [input|file] [options...]" << std::endl;
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "--list-devices") {
        print_audio_devices();
        return 0;
    }

    // 参数重组 (Argument Shifting)
    // 我们需要移除 argv[1] (即 "--mode") 和 argv[2] (即 "input/file")
    // 将剩余参数构造一个新的 argv 传给子模块，模拟它们直接被调用的场景
    
    if (mode == "--mode" && argc >= 3) {
        std::string actual_mode = argv[2];
        
        std::vector<char*> sub_argv;
        sub_argv.push_back(argv[0]); // 保留程序名
        
        // 从索引 3 开始复制剩余参数 (例如 -m models/base.bin ...)
        for (int i = 3; i < argc; ++i) {
            sub_argv.push_back(argv[i]);
        }
        
        // 兼容 C 风格数组
        int sub_argc = static_cast<int>(sub_argv.size());

        // 路由分发
        if (actual_mode == "input") {
            return run_input_mode(sub_argc, sub_argv.data());
        }
        else if (actual_mode == "file") {
            return run_transcribe_mode(sub_argc, sub_argv.data());
        }
    }

    std::cerr << "Error: Invalid arguments." << std::endl;
    std::cerr << "Example: ZianCore.exe --mode input -t 4 ..." << std::endl;
    return 1;
}
