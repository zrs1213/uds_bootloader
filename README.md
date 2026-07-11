# UDS Bootloader 协议栈 + 轻量 OTA 调度层（车载诊断自研项目）

> 从零手写的一套**车载 UDS 诊断 + Bootloader 刷写协议栈**，覆盖 ISO 15765-2（CAN-TP）
> 传输层与 ISO 14229-1（UDS）应用层，并在其上叠加一层**轻量车端 OTA 刷写调度层**
> （云端升级包 → CBC-MAC 验签 → 复用 $34/$36/$37 引擎落盘）。项目定位为**秋招车载嵌入式 /
> 底盘域 UDS 诊断**方向的高含金量自研项目，与底盘域 IBCU 诊断栈实习经历形成互补
> （实习偏"用 AUTOSAR 工具配 UDS"，本项目偏"手写 UDS 协议栈 + 双通道刷写架构 + OTA 调度"）。

---

## 1. 架构

```
┌─────────────────────────────────────────────────────────────┐
│   OTA 刷写调度层 (ota_agent.c)        【轻量车端代理】      │  收云端包→CBC-MAC 验签→调引擎
├─────────────────────────────────────────────────────────────┤
│                     诊断仪 / 上位机 (Python)                  │
│  uds_client.py  ·  flash_tool.py  ·  aes.py (Seed&Key 派生)   │
└───────────────┬──────────────────────────────┬──────────────┘
                │  CAN 帧 (ISO-TP 分段)          │
        ┌───────▼────────────┐          ┌───────▼────────────┐
        │   CAN-TP (发送端)  │          │   CAN-TP (接收端)  │
        │  SF / FF / CF / FC  │          │  多帧重组 / 流控    │
        └───────┬────────────┘          └───────┬────────────┘
                │                                │
        ┌───────▼────────────────────────────────▼───────────┐
        │                 UDS 应用层 (uds_service.c)            │
        │  $10/$11/$22/$27/$2E/$28/$31/$34/$36/$37/$3E/$85 │
        │  会话状态机 + S3超时 + 安全等级 + NRC 框架            │
        └───────┬────────────────────────────────┬───────────┘
                │                                │
        ┌───────▼──────────┐          ┌─────────▼────────────┐
        │  Security (AES)   │          │  IO 抽象 (uds_io.c)   │
        │  $27 Seed&Key      │          │  Flash 擦/写/读 + DID │
        │  + CBC-MAC 验签    │          │                      │
        └───────────────────┘          └──────────────────────┘
```

三层解耦：**协议层（CAN-TP / UDS）+ OTA 调度层均不直接操作任何硬件寄存器**。
所有收发经回调 `can_tx_callback_t` 注入；存储经 `uds_io.c` 抽象；升级包经"虚拟通道"投递字节数组。
因此同一套协议代码可：
- 在 **Host**（CANoe 仿真 / 单元测试）下用 RAM 缓冲跑通；
- 在 **STM32（Cortex-M4）** 下把回调换成 HAL_CAN、IO 换成 HAL_FLASH，零改动协议逻辑；
- OTA 层在真实车端只需把"虚拟通道收包"替换为 T-Box 的 4G/以太网收包。

---

## 2. 已实现能力

**CAN-TP（ISO 15765-2）**
- SF / FF / CF / FC 四种帧构建与 PCI 解析
- 接收端多帧重组状态机（BS 流控、SN 序列号校验、溢出保护）
- 发送端分段状态机（STmin 帧间隔、BS 块流控、FC WAIT 等待）

**UDS 应用层（ISO 14229-1）**
| SID | 服务 | 说明 |
|-----|------|------|
| $10 | DiagnosticSessionControl | 默认/编程/扩展会话切换，切换后安全复位 |
| $11 | ECUReset | 硬/软复位，复位回落默认会话 |
| $22 | ReadDataByIdentifier | 支持单条/多条 DID |
| $27 | SecurityAccess | **AES-128-ECB Seed&Key** 安全解锁 |
| $2E | WriteDataByIdentifier | DID 写入（只读 DID 拦截） |
| $28 | CommunicationControl | 开关非诊断通信（量产刷写前置） |
| $31 | RoutineControl | 例程控制（Flash CRC / 擦除 / 预编程检查） |
| $34 | RequestDownload | 下载请求 + 地址/长度校验 + 擦除 |
| $36 | TransferData | 块传输 + BSC 连续性校验 + CRC32 |
| $37 | RequestTransferExit | 传输退出 + 完整性校验 |
| $3E | TesterPresent | 保活 + S3 计时刷新，支持抑制位 |
| $85 | ControlDTCSetting | 开关 DTC 记录（量产刷写前置） |

**NRC 负响应覆盖（12 种）**
`$12`子功能不支持 · `$13`长度错误 · `$22`条件不满足 · `$24`序列错误 ·
`$31`越界/未知DID · `$33`安全拒绝 · `$35`密钥错误 · `$36`尝试超限 ·
`$7E`会话中子功能不支持 · `$7F`会话中服务不支持 · `$10`一般拒绝（`$11`服务不支持预留）

**轻量车端 OTA 刷写调度层（`ota_agent.c/.h`）**
- **CBC-MAC 包级验签**（AES-128, IV=0, 末块 0 填充，复用 `security.c` 的 `AES128_ECB_Encrypt`）：云端升级包到车端后先验签，篡改/伪造固件在第 1 步被拦截，**绝不落盘**。
- **复用刷写引擎**：验签通过后再调 `$34/$36/$37/$31 FF00` 把固件写进 Flash，**零重写刷写逻辑**。
- **块级进度 + 断点续传**：`written_blocks`/`total_blocks` 维护进度，传输中断可完整重传续上。
- **失败回滚标记**：`need_rollback` 表达 A/B 双分区回退决策（平台级工作用标志位表达设计意图）。

**Python 诊断工具（tools/）**
- `aes.py`：纯 Python AES-128-ECB，**FIPS-197 向量自测通过**，与 C 端算法一致
- `uds_client.py`：ISO-TP 分段 + DiagClient + 可运行 Loopback 参考 ECU（端到端闭环演示）
- `flash_tool.py`：Intel HEX → $34/$36/$37 请求序列生成器 + 整包 CRC32 + **CANoe CAPL 脚本导出**

---

## 3. 目录结构

```
uds_bootloader/
├── Makefile                 # make test(本机) / make arm(交叉编译)
├── src/
│   ├── types.h              # CAN 报文结构 + 收发/定时器回调抽象
│   ├── minunit.h            # 极简单元测试框架（零依赖）
│   ├── can_tp.h/.c          # ISO 15765-2 传输层
│   ├── security.h/.c        # AES-128-ECB + $27 Seed&Key + CBC-MAC 原语
│   ├── boot_fsm.h/.c        # 会话状态机 + S3 超时
│   ├── uds_io.h/.c          # Flash/DID 抽象（Host=RAM 缓冲）
│   ├── uds_service.h/.c     # UDS 应用层服务分发 + NRC
│   └── ota_agent.h/.c       # 【新增】车端 OTA 刷写调度层（验签+复用引擎+续传）
├── test/
│   ├── test_main.c          # 测试入口
│   ├── test_can_tp.c        # CAN-TP 单元测试
│   ├── test_security.c      # AES KAT + Seed&Key 闭环
│   ├── test_uds.c           # UDS 服务/NRC/S3 单元测试
│   ├── test_stack.c         # CAN-TP+UDS 端到端集成测试
│   └── test_ota.c           # 【新增】OTA 验签/篡改拦截/断点续传（8 项）
└── tools/
    ├── aes.py               # Python AES-128-ECB
    ├── uds_client.py        # 诊断仪侧工具 + 闭环演示
    ├── flash_tool.py        # HEX→刷写请求 + CAPL 导出
    └── samples/             # 示例固件与生成脚本
```

---

## 4. 快速开始

### 4.1 本机运行单元测试（host）
```bash
make test
# 期望输出：RESULT: ALL PASS (49 tests, 220 assertions)
```

### 4.2 交叉编译校验（证明可在 MCU 目标编译）
```bash
make arm
# 期望：所有 src/*.c 在 Cortex-M4 (thumb) 下 0 warning 编译通过
```

### 4.3 Python 工具
```bash
python tools/aes.py                  # AES KAT 自测
python tools/uds_client.py --demo    # 端到端闭环刷写演示
python tools/flash_tool.py tools/samples/sample.hex --capl download.can
```

### 4.4 与 CANoe 的集成（验证方式）
- **CAPL 仿真 ECU**：用 `flash_tool.py --capl` 导出的激励脚本驱动 Diagnostic Console；
  或用 CAN-TP 模块搭一个虚拟 ECU 节点，回放本栈预期响应，逐条比对 Trace。
- **Diagnostic Console 逐服务验证**：把本栈的 NRC/正响应预期作为 oracle，在 CANoe 中
  跑一遍服务矩阵，确认边界条件（12 种 NRC）全部命中。
- **OTA 子流程（CAPL 仿真 Tester 节点）**：Reload `uds_tester.can` → F9 编译 → Start 后，
  键盘 `o` 跑合法包验签+刷写、键盘 `t` 跑被篡改包被 CBC-MAC 验签拦截。
  （本机无 CANoe/License，CAPL OTA 代码待用户在自有 CANoe 中编译验证；C 栈与
  CBC-MAC 逻辑已由 `make test` 的 8 项 OTA 单测全覆盖。）

---

## 5. 当前验证状态

| 项 | 状态 | 说明 |
|----|------|------|
| ARM 交叉编译（Cortex-M4, `-Wall -Wextra`） | ✅ 0 warning | 6 源文件在 MCU 目标干净编译（arm-none-eabi-gcc 14.2，含 ota_agent.c） |
| Python AES-128-ECB vs FIPS-197 向量 | ✅ 一致 | `python tools/aes.py` |
| Python 端到端闭环（会话+解锁+刷写+回读） | ✅ PASS | `python tools/uds_client.py --demo` |
| Python 协议栈等价验证（20 项断言） | ✅ **20/20 PASS** | `python tools/run_tests.py`（覆盖 ISO-TP / 会话权限 / NRC 边界 / 刷写闭环 / Seed&Key） |
| **C 单元测试 `make test` 绿跑** | ✅ **ALL PASS（49 tests / 220 assertions）** | 已在本机 MinGW-w64 gcc 16.1.0 (UCRT) 实测通过；含 8 项 OTA 代理测试（验签/篡改拦截/断点续传）；另修复 `crc32_update` 长度截断缺陷后全绿 |
| CANoe 实测 | ⏳ 验证平台就绪 | CAPL 激励脚本已由 `flash_tool.py --capl` 生成，license 就绪即可跑；OTA 键盘 `o`/`t` 子流程待用户编译验证 |

> **本机工具链**：通过 `winget download` 从微软 CDN 获取 winlibs gcc 16.1.0（UCRT/posix/seh），
> 哈希校验通过、解压至 `C:\mingw64`，并把 `C:\mingw64\bin` 加入用户 PATH。
> 为兼容 MinGW 中 `make`→`cc` 的默认调用，已在 `bin` 下补 `cc.exe` 副本（=gcc），
> 因此 `make test` / `mingw32-make test` 均可直接绿跑。
> 在你自己装有 gcc/MinGW 或 CANoe 的电脑上，`make test`（或 `make arm`）一条命令即可绿跑，
> 无需任何额外改动。

---

## 6. 面试要点索引

完整的**五维可行性评估、分阶段执行方案、简历描述、面试话术、风险兜底 Q&A** 见同目录
上级文档 **`UDS_Bootloader_总体技术文档.md`**。

核心叙事：选择 CANoe / Host 仿真而非搭硬件实物，是因为 CANoe 的 Diagnostic Console 与
Trace 能精确验证每一帧诊断报文的边界条件（覆盖 12 种 NRC），对诊断协议能力的提升优于
焊板子；CANoe 本身是 Tier1 的行业标准验证工具。本次在协议栈之上叠加的 **OTA 调度层**
（CBC-MAC 包级验签 + 复用刷写引擎 + 断点续传）进一步贴合车端量产刷写架构，且全程复用
自研 AES 原语，差异化明显。

---

> **免责声明**：本项目为**学习 / 求职作品（Educational Purpose）**，AES 主密钥、Flash
> 布局、DID 表均为演示用占位，未对接任何真实车型的产线刷写流程，不涉及 ASIL / 量产。
> OTA 的云端编排、差分升级、A/B 双分区回滚属车企平台级工作，本模块用 `need_rollback`
> 标志位表达设计意图，不做整包平台。
