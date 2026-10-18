# 首版本地验证记录

日期：2026-10-18。

## 环境和构建

- Windows x64，Python 3.13.9。
- Zig 0.13.0 的 C/C++ 编译驱动，C99 / C++17，SQLite 编译使用 O2，应用使用 O1。
- 通过 `python scripts/build.py --zig <zig.exe>` 成功构建真实 Windows 可执行文件。
- 首次构建时 Zig 自带 libunwind 报出 dllexport 重声明警告；链接成功，随后完成下述运行测试。
- 9 个 vendored 依赖文件 SHA-256 验证通过；JSON 头文件另与上游 v3.12.0 发布校验值核对。
- SQLite 源码压缩包的 SHA3-256 与官网下载页核对。

## 黑盒集成测试

```text
python tests/test_integration.py --binary build/gateway.exe
Ran 14 tests in 11.624s
OK
```

测试使用真实 C++ HTTP 服务、真实临时 SQLite 数据库和 Python 编码器，包含：
CRC 标准向量、逐字节拆分和连续多帧、负温度与二进制序列号、噪声和 CRC 故障恢复、
无新 feed 时的后台超时、来源隔离、字段校验、参数和请求大小限制、来源容量限制、
历史排序/过滤/CSV、4 个并发生产者、重启后已提交数据保留、确定性队列拥塞，
以及数据库被外部写事务锁住后的失败计数与后续恢复。

## 浏览器验收

使用真实本地服务与 Python 模拟器，确认：

- 4 台模拟设备出现，在线状态、温湿度和序列号持续更新。
- 切换设备后曲线与设备标签同步更新。
- 点击 CRC 故障按钮后，错误计数增加，并产生事件。
- 点击半帧超时按钮后，无需再发送完整帧，超时计数自动增加。
- 图表绘制正常，浏览器未记录 JavaScript error/warn。
- 针对验收窗口中指标卡换行拥挤的问题调整了窄屏布局，重新加载后核对。

历史导出内容由 HTTP 集成测试验证；浏览器导出按钮未单独完成下载验收。

## 本机负载实验

在独立、无其他生产者的临时网关上运行：

```text
python scripts/benchmark.py --producers 4 --requests 25 --batch 10
```

结果：1,000 条样本接受并最终存储，队列丢弃 0，存储错误 0。
原始结果见 [benchmark-local.json](benchmark-local.json)。
这是一次较短的本机功能/负载冒烟实验，包含 Python 客户端、HTTP 和 OS 调度开销，
不代表稳定峰值性能，也不能外推为 UART、MCU 或生产部署的吞吐能力。

## 尚未执行的验证

- CMake 构建路径及 GitHub Actions 的 Linux/GCC、Windows/MSVC 矩阵尚未实际运行。
- 未接入开发板、真实 UART、DMA、RTOS 或远端设备。
- 未验证长期运行、磁盘耗尽、系统掉电或跨主机部署；强制终止不保证队列样本落盘。
