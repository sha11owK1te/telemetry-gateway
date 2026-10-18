# Pulse · Telemetry Gateway

**设备数据采集与监控网关｜C++17 · C99 · SQLite · Python**

无需开发板即可演示：模拟多台环境传感器，接收任意切分的二进制字节块，
验证 CRC 和组帧期限，通过有界队列异步存储，并在浏览器查看设备状态、曲线和异常。

本项目是一个可复现的本地主机应用。输入适配器使用 HTTP 携带原始字节，
尚未接入真实 UART、DMA 或 RTOS；复用的 `serial-frame` C99 核心可供后续板端移植。

## 一键演示

Windows 便携交付包已经带有 `build/gateway.exe`，安装 Python 3.9+ 后，在项目根目录运行：

```powershell
python scripts/run_demo.py
```

启动本地网关、4 台模拟设备，并打开 `http://127.0.0.1:8080`。
Ctrl+C 停止演示。默认数据库位于 `data/telemetry.db`，历史记录在下次启动时保留。
如端口占用：`python scripts/run_demo.py --port 8090`。

源码包不包含可执行文件，需要先构建。

## 从源码构建

所有运行所需的 C/C++ 依赖均已固定版本并随源码提供，构建无需联网下载依赖。
需要 C++17 编译器及 Python；使用 CMake 时还需要 CMake 3.16+。

### GCC / Clang（无需 CMake）

```bash
python scripts/build.py
python scripts/run_demo.py
```

通过 `CC` / `CXX` 选择编译器。Windows 可使用完整 MinGW-w64 工具链，
也可使用 Zig 0.13.0：

```powershell
python scripts/build.py --zig C:/tools/zig/zig.exe
python scripts/run_demo.py
```

编译器本身不包含在源码包和便携包中；便携包运行已编译程序时不需要 C++ 编译器。

### CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python scripts/run_demo.py
```

### 分开启动

```bash
# 终端 1；Windows 使用 build/gateway.exe
./build/gateway --db telemetry.db --web web --port 8080

# 终端 2
python scripts/simulator.py --devices 4 --interval 0.8
```

## 可以演示什么

- 在线设备列表：5 秒内有已存储有效样本视为在线；选择设备查看最新 120 条温湿度。
- 数据统计：CRC 错误、半帧超时、队列丢弃、队列峰值和历史总记录数。
- 故障实验：页面按钮发送真实 CRC 错帧或截断帧，观察后台解析器的处理结果。
- CSV 导出：按设备导出最近至多 5,000 条已存储记录。
- 停止模拟器后，设备自动变为离线；重新启动网关后仍可查询已提交历史。
- 页面不使用外部字体、CDN 或前端构建工具，所有数据来自实际 C++ API。

## 结构

```text
Python 模拟设备
    │ HTTP POST 原始字节块；同一来源顺序发送
    ▼
cpp-httplib / 4 个 HTTP 工作线程
    ▼
按来源隔离的 serial-frame C99 解析器
    │ CRC16 → 固定长度 schema 与字段校验
    ▼
有界队列（默认 1,024 条；满时丢弃最新样本）
    ▼
单写入线程 → SQLite WAL / FULL
    ▲
后台每 25 ms 检查半帧期限

浏览器 → 设备 / 历史 / 统计 / CSV 查询 API
```

网关自有逻辑位于 `src/`；HTTP 协议处理来自 cpp-httplib，JSON 来自 nlohmann/json，
数据库来自 SQLite。没有自行实现 HTTP 服务器或数据库内核。
完整并发、持久化和故障边界见 [设计说明](docs/design.md)。

## 测试与压力实验

构建后运行黑盒集成测试（Windows 替换可执行文件扩展名）：

```bash
python tests/test_integration.py --binary build/gateway
```

测试通过真实 HTTP 调用和真实临时 SQLite 数据库检查：拆包/连续帧、
CRC 错误后的恢复、无新输入时的后台超时、多来源隔离、payload 校验、
参数和请求大小限制、历史查询/导出、并发生产者、重启持久化及确定性队列拥塞。

在一个单独启动、未运行模拟器的网关上执行本地压力实验：

```bash
python scripts/benchmark.py --producers 4 --requests 50 --batch 10
```

输出实际接受/丢弃条数、HTTP 延迟 P50/P95、接受速率以及持久化完成时间。
这些是本机 HTTP 路径的测量值，不是 MCU、UART 或生产环境性能。

自动注入 CRC 错误和半帧：

```bash
python scripts/simulator.py --fault-every 10
```

## 项目文件

```text
src/                  C++ 网关、队列、存储与 HTTP 路由
vendor/               固定的第三方依赖及许可证
web/                  无构建依赖的监控页面
scripts/protocol.py   独立 Python 编码器
scripts/simulator.py  模拟设备与分块发送
scripts/benchmark.py  主机压力实验
tests/                黑盒集成测试
docs/                 协议、设计、验证记录和项目经历建议
```

## 当前边界

- 服务只绑定 `127.0.0.1`，用于本地学习和演示，没有账号、TLS 或远程部署功能。
- 同一来源的分块请求必须由调用方按序发送；最多 32 个来源，设备编号 1–64。
- 请求最多 4,096 字节；遥测值温度 −50–150 °C、湿度 0–100%。
- HTTP 202 表示数据已解析并入队，**不表示已经持久化**。进程突然终止可能丢失队列数据。
- 队列满时返回 HTTP 429，并报告本次 `accepted` 和 `dropped`；整块重发可能产生重复记录。
- 当前序列号仅用于观察，不实现去重、ACK、重传、设备身份绑定或端到端可靠交付。
- CRC 错误丢弃完整候选帧；损坏长度可能吞掉后续帧。超时不能恢复已经丢弃的数据。
- 历史数据未实现自动保留策略；输入/错误计数和最近事件为进程内统计，重启后清零。

## 来源与许可证

自有代码使用 MIT 许可证。第三方组件保留各自许可证与版本，见
[THIRD_PARTY.md](THIRD_PARTY.md) 和 `vendor/dependencies.json`。
项目经历应区分使用开源组件和自己完成的功能，见 [简历与面试准备](docs/resume.md)。
