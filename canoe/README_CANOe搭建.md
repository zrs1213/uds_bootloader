# CANoe 验证工程搭建说明（UDS Bootloader）

本目录包含一组 **CANoe CAPL** 文件，用于在 CANoe 中"开箱即跑"地验证你手写的 UDS 协议栈。
ECU 侧用 CAPL 仿真（与 `src/uds_service.c` / `security.c` 行为一致），诊断仪侧用 CAPL 驱动并校验，
二者通过 ISO-TP（`0x7E0`→`0x7E8`）通信，安全握手用同一套 AES-128 主密钥，因此 **$27 是真正可验证的**。

## 文件清单

| 文件 | 作用 |
|---|---|
| `uds_ecu.can` | ECU 仿真节点：会话/安全状态机 + 10 项 UDS 服务响应 + ISO-TP 重组 + Flash/DID 仿真 |
| `uds_tester.can` | 诊断仪节点：发请求、算 key 真解锁、收响应并断言 PASS/FAIL；暴露面板/键盘可调用函数 |
| `aes_can.inc` | （已弃用）早期版本曾用 `#include` 共享；当前两个 .can 已各自内联 AES，无需此文件 |
| `uds.dbc` | 诊断报文数据库：定义 `DiagRequest`(0x7E0) / `DiagResponse`(0x7E8) 两条 8 字节报文 |
| `DiagPanel.xvp` | 诊断面板：6 个按钮分别触发各操作 / 一键全流程 |
| `download.can` | （可选参考）由 `tools/flash_tool.py --capl` 生成的刷写激励脚本 |

> 与协议栈的对应关系：会话权限表、NRC、DID 表（0xF190/0xF195/0x0100/0x0201）、
> Flash 基址 `0x08010000`/64KB、下载块 32 字节、主密钥 `DE AD BE EF … BA 98`、PRNG 种子 `0xCAFEBABE`
> 全部与 C 源码一致。

## 在 CANoe 中搭建（约 5 分钟）

1. **新建工程**：File → New → CAN 500kbps（或 250kbps，速率对即可），保存为 `uds_demo.cfg`。
2. **导入数据库**：Simulation Setup → 右键 "Database" → Add → 选本目录 `uds.dbc`。
3. **建 ECU 仿真节点**：Simulation Setup → 右键 "Nodes" → Insert Network Node → 命名 `ECU` →
   双击进入 CAPL 编辑器 → File → Load 选 `uds_ecu.can`（自包含，AES 已内联，无需其他文件）。
4. **建诊断仪节点**：同法再 Insert 一个 Node → 命名 `Tester` → 载入 `uds_tester.can`。
5. **放面板**（可选）：Tools → Panel Editor → Open → 选 `DiagPanel.xvp`；
   或把面板拖到 Measurement 界面。**重要**：面板必须关联到 **Tester 节点**
   （在 Tester 节点的配置里 Add Panel 选 `DiagPanel.xvp`），因为按钮调用的 `DoSessionExt` 等函数都定义在 `uds_tester.can`。
   若面板因 CANoe 版本差异打不开，直接在 Panel Editor 里
   拖 6 个 Button，每个的 Event 设为 CAPL 函数 `DoSessionExt / DoSecurity / DoReadVin / DoRoutineCrc / DoFlash / DoFullSequence`。
6. **连线**：两个节点都在同一 CAN 通道（如 CAN1）即可，无需真实硬件（用 CANoe 自带的虚拟总线）。
7. **运行**：Start Measurement → 点面板"一键全流程验证"按钮，或在 Write 窗口按 `r` 键。
   结果（每步 PASS/FAIL）实时打印在 Write 窗口。

## 预期输出（Write 窗口片段）

```
==> $10 03 进入扩展会话
  [PASS] 正响应 0x50 03 (P2=50ms P2*=500ms)
==> $27 01 请求 Seed
  [PASS] 收到 16 字节 Seed
==> $27 02 提交 Key (AES-128 派生)
  [PASS] 安全访问解锁成功 (secLevel=1)
==> $22 F190 读 VIN
  [PASS] VIN = LSVUDS0000000000
==> $31 01 FF00 计算 Flash CRC32
  [PASS] 例程完成, CRC32=0x...
==> $10 02 进入编程会话
  [PASS] 编程会话
==> $34 请求下载 (addr=0x08010000, len=64)
  [PASS] 下载授权, maxBlockLength=32
==> $36 #01 传输 32 字节
==> $36 #02 传输 32 字节
  [PASS] 2 块传输完成
==> $37 退出传输
  [PASS] 刷写流程结束
```

## 面试话术要点

- "我在 CANoe 里用 CAPL 搭了一个 ECU 仿真 + 诊断仪，把自研栈的 10 项 UDS 服务和 ISO-TP 流控
  跑通，重点验证了 **$27 安全握手**——诊断仪用和 ECU 同一套 AES-128 主密钥派生 key，
  不是写死的比对值，所以 seed 随机也能解锁。"
- "ISO-TP 的多帧分段/流控在 CAPL 里也实现了（SF/FF/CF/FC），可以演示 32 字节块的刷写分段。"
- "负响应覆盖：错误会话调 $34 会回 `0x7F 34 33`（安全拒绝），未解锁调写 DID 回 `0x22`（条件不满足）。"

## 基准对账值（调试用）

C 端 `security.c` 已独立编译验证：PRNG 种子 `0xCAFEBABE` 时，首轮 `$27` 流程为

```
seed = 2A F9 87 A8 9B CD F0 A3 FA E7 3D F2 FB 93 92 96
key  = 1C E5 37 A3 B4 96 CA E4 0D F3 0C 18 5C D3 41 33
SEC_CheckKey(派生key) = 1   SEC_CheckKey(错误key) = 0
```

CANoe 里若想确认 CAPL 的 AES 与 C 栈一致，可在 `uds_tester.can` 的 `DoSecurity()` 中临时 `write` 打印 `gSeed` 与算出的 `key`，
应与上面完全一致（说明 CAPL 的 `aes_can.inc` 与 `security.c` 行为等价）。

## 注意事项 / 已知坑

- **CAPL 铁律（本次踩坑核心）**：**CAPL 不允许把数组作为函数参数传递**（如 `void f(byte x[])` 会直接 `parse error`，
  且因常被 `#include` 引用，错误会被归到第一行的 `#include` 上，表现为"第 1 行就报 parse error"。
  本工程因此**已彻底不用 `#include`、所有数组都改走全局变量传值**（如 `TpSendReq(void)` 改用全局 `g_req`/`g_reqLen`）。
  若日后你改脚本又出现"第 1 行 (1,1) parse error"，第一反应就是：检查有没有数组参数或 `#include`。
- **CANoe 版本**：当前脚本为 CAPL C89 风格（变量声明在块首、无 const、无指针、无多维数组），兼容性很好，
  CANoe v11+ 均可；极老版本若报变量超限，可把 `SBOX[256]`/`flash[1024]` 等适当缩小。
- **msWait**：ECU/诊断仪在发多帧时用 `msWait(1)` 做简化 STmin pacing（真实栈按 FC 的 stmin pacing）。
  若测量时报 "blocking call"，可改小或去掉，虚拟总线下通常无碍。
- **流控简化**：本演示 ECU 收到 FF 直接回 CTS(bs=0)，诊断仪不严格等 FC 直接按固定间隔发 CF，
  足以演示分段；真实 C 栈的 FC 状态机（`can_tp.c`）已在单元测试中验证。
- **Flash 仿真**：ECU 节点用 64KB RAM 数组模拟 Flash（先擦后写），与 `uds_io.c` 语义一致。
