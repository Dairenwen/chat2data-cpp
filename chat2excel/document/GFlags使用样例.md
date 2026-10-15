/

# GFlags使用样例

gflags是 Google 开发的一个开源库，用于 C++ 应用程序中命令行参数的声明、定义和解析。gflags 库提供了一种简单的方式来添加、解析和文档化命令行标志（flags），使得程序可以根据不同的运行时配置进行调整。

它具有如下几个特点：

- **易于使用**：gflags 提供了一套简单直观的 API 来定义和解析命令行标志，使得开发者可以轻松地为应用程序添加新的参数。
- **自动帮助和文档**：gflags 可以自动生成每个标志的帮助信息和文档，这有助于用户理解如何使用程序及其参数。
- **类型安全**：gflags 支持多种数据类型的标志，包括布尔值、整数、字符串等，并且提供了类型检查和转换。
- **多平台支持**：gflags 可以在多种操作系统上使用，包括 Windows、Linux 和 macOS。
- **可扩展性**：gflags 允许开发者自定义标志的注册和解析逻辑，提供了强大的扩展性。

[官方文档](https://gflags.github.io/gflags/)；[代码仓库](https://github.com/gflags/gflags.git)；

## 1. gflags安装

直接命令安装：

```bash
dev@dev-host:~/workspace/gflags$ sudo apt-get install libgflags-dev
```

原码安装：

```bash
# 下载源码
git clone https://github.com/gflags/gflags.git
# 切换目录
cd gflags/
mkdir build
cd build/
# 生成Makefile
cmake ..
# 编译代码
make
# 安装
make install
```

## 2. gflags使用

### 2.1 包含头文件

使用gflags库来定义/解析命令行参数必须包含如下头文件

```cpp
#include <gflags/gflags.h>
```

程序编译时库的链接

```bash
g++ main.cc -o main -lgflags
```

### 2.2 定义参数

利用 gflags 提供的宏定义来定义参数。该宏的 3 个参数分别为命令行参数名，参数默认值，参数的帮助信息。

```cpp
DEFINE_bool(reuse_addr, true, "是否开始网络地址重用选项");
DEFINE_int32(log_level, 1, "日志等级：1-DEBUG, 2-WARN, 3-ERROR");
DEFINE_string(log_file, "stdout", "日志输出位置设置，默认为标准输出");
```

gflags支持定义多种类型的宏函数：

```cpp
DEFINE_bool
DEFINE_int32
DEFINE_int64
DEFINE_uint64
DEFINE_double
DEFINE_string
```

### 2.3 访问参数

可以在程序中通过 `FLAGS_reuse_addr` 像正常变量一样访问标志参数。在上面的例子中，可以通过 `FLAGS_log_level` 和 `FLAGS_log_file` 变量来访问命令行参数。

### 2.4 不同文件访问参数

如果想在另外一个文件访问当前文件的参数，以参数 `FLAGS_reuse_addr` 为例，可以使用宏 `DECLARE_bool(reuse_addr)` 来声明引入这个参数。相当于做了 `extern FLAGS_reuse_addr`，定义外部链接属性。

### 2.5 初始化所有参数

当我们定义好参数后，需要告诉可执行程序去处理解析命令行传入的参数，使得 `FLAGS_*` 变量能得到正确赋值。我们需要在main函数中，调用下面的函数来解决命令行传入的所有参数。

```cpp
int main(int argc, char *argv[])
{
    google::ParseCommandLineFlags(&argc, &argv, true);
    return 0;
}
```

- `argc` 和 `argv` 就是 main 的入口参数
- 第三个参数被称为 `remove_flags`。如果它为 `true`，表示 `ParseCommandLineFlags` 会从 `argv` 中移除标识和它们的参数，相应减少 `argc` 的值。如果它为 `false`，`ParseCommandLineFlags` 会保留 `argc` 不变，但将会重新调整它们的顺序，使得标识在前面。

### 2.6 运行参数设置

gflags提供了多种命令行设置参数的方式。

**string和int设置参数**

```bash
exec --log_file="./main.log"
exec -log_file="./main.log"
exec --log_file "./main.log"
exec -log_file "./main.log"
```

**bool设置参数**

```bash
exec --reuse_addr
exec --noreuse_addr
exec --reuse_addr=true
exec --reuse_addr=false
```

`--` 将会终止标识的处理。比如在 `exec -f1 1 -- -f2 2` 中，`f1` 被认为是一个标识，但 `f2` 不会

### 2.7 配置文件的使用

配置文件的使用，其实就是为了让程序的运行参数配置更加标准化，不需要每次运行的时候都手动收入每个参数的数值，而是通过配置文件，一次编写，永久使用。需要注意的是，配置文件中选项名称必须与代码中定义的选项名称一致，尤其需要注意的是**配置文件中的字符串数据不需要使用双引号。**

样例：

`main.conf`

```ini
-reuse_addr=true
-log_level=3
-log_file=./log/main.log
```

```bash
exec --flagfile=./main.conf
```

### 2.8 特殊参数标识

gflags也默认提供了几个特殊的标识。

```bash
--help    # 显示文件中所有标识的帮助信息
--helpfull # 和-help 一样，帮助信息更全面一些
--helpshort # 只显示当前执行文件里的标志
--helpxml   # 以 xml 方式打印，方便处理
--version # 打印版本信息，由 google::SetVersionString()设定
--flagfile
-flagfile=f #从文件 f 中读取命令行参数
```

## 3. 案例

### 3.1 目录结构

```text
.
|-- main.cc
|-- main.conf
`-- makefile
```

### 5.2 样例

**配置文件：**

`main.conf`

```ini
-reuse_addr=true
-log_level=3
-log_file=./log/main.log
```

**样例代码：**

`main.cc`

```cpp
#include <gflags/gflags.h>
#include <iostream>

DEFINE_bool(reuse_addr, true, "是否开始网络地址重用选项");
DEFINE_int32(log_level, 1, "日志等级：1-DEBUG, 2-WARN, 3-ERROR");
DEFINE_string(log_file, "stdout", "日志输出位置设置，默认为标准输出");

int main(int argc, char* argv[])
{
    // 解析命令行参数
    google::ParseCommandLineFlags(&argc, &argv, true);

    // 输出参数值
    std::cout << "reuse: " << FLAGS_reuse_addr << std::endl;
    std::cout << "level: " << FLAGS_log_level << std::endl;
    std::cout << "file: " << FLAGS_log_file << std::endl;

    return 0;
}
```

### 5.3 项目构建

```makefile
main : main.cc
    g++ -std=c++17 $^ -o $@ -lgflags
```

### 5.4 样例运行

运行代码:

```Shell
dev@fc56006988fe:~/workspace/temp/gflags$ make
g++ -std=c++17 main.cc -o main -lgflags
dev@fc56006988fe:~/workspace/temp/gflags$ ./main -helpshort
main: Warning: SetUsageMessage() never called

  Flags from main.cc:
    -log_file (日志输出位置设置，默认为标准输出) type: string
      default: "stdout"
    -log_level (日志等级：1-DEBUG, 2-WARN, 3-ERROR) type: int32 default: 1
    -reuse_addr (是否开始网络地址重用选项) type: bool default: true
dev@fc56006988fe:~/workspace/temp/gflags$ ./main
reuse: 1
level: 1
file: stdout
dev@fc56006988fe:~/workspace/temp/gflags$ ./main -flagfile=./main.conf
reuse: 1
level: 3
file: ./log/main.log
dev@fc56006988fe:~/workspace/temp/gflags$ ./main --noreuse_addr --log_level=2
--log_file=./log
reuse: 0
level: 2
file: ./log
```
