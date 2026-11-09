# chat2data

面向自然语言数据查询的 C++ 项目。目前是开发骨架：提供独立构建、命令行入口和测试，尚未实现查询、网络服务或中间件连接。

## 环境要求

- 支持 C++17 的编译器（已在 GCC 11 上验证）
- CMake 3.16 或更新版本
- 不需要第三方库、数据库或联网下载依赖

以下命令在 `chat2data` 目录下执行。开发容器内的目录为 `/home/dev/workspace/chat2data-cpp/chat2data`。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/chat2data --help
./build/chat2data --version
```

不带参数运行会提示项目仍在开发中；未知参数返回非零状态。发布构建可将 `Debug` 改为 `Release`。安装到指定目录：

```bash
cmake --install build --prefix /tmp/chat2data-install
```

可通过 `-DBUILD_TESTING=OFF` 禁用测试，或在支持的 GCC / Clang 环境中通过 `-DCHAT2DATA_ENABLE_SANITIZERS=ON` 启用地址和未定义行为检查。构建目录可直接删除并重新生成。

## 目录结构

```text
chat2data/
├── CMakeLists.txt        # 构建、测试和安装入口
├── include/chat2data/    # 项目内部模块的公共头文件
├── src/                  # 程序入口和核心实现
├── tests/                # 自动化测试
└── docs/                 # 设计和开发约定
```

设计边界和扩展原则见 [架构说明](docs/architecture.md)。格式约定见 `.clang-format` 和 `.editorconfig`；新增功能应附带覆盖实际行为的测试。
