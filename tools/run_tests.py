"""
run_tests.py —— 纯 Python 协议栈逻辑验证（无 C 编译器依赖）。

在本环境（无本机 gcc/MinGW/TinyCC 链接库）即可运行，等价覆盖 C 单元测试
（test/）的关键断言点：
  1. AES-128-ECB 加解密 + FIPS-197 标准向量
  2. ISO-TP 发送分段（SF/FF/CF/PCI）与接收重组的对称性
  3. UDS 会话权限矩阵（默认/编程/扩展会话）
  4. NRC 边界（会话权限 / 安全锁 / 块序号 / 未知服务）
  5. 刷写三件套 $34/$36/$37 闭环 + CRC32 完整性
  6. $27 Seed&Key 派生与跨语言一致性基准

注：C 实现（src/）的同等断言见 test/，在具备 gcc/MinGW 或 CANoe 的环境用
`make test` 运行；本脚本验证 Python 参考实现（与 C 栈同源算法）的逻辑正确性。
"""
import sys
from aes import aes128_ecb_encrypt, aes128_ecb_decrypt, derive_key
from uds_client import (iso_tp_frames, ReferenceEcuc, DiagClient,
                        LoopbackTransport, crc32)

passed = 0
failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
        print("  [PASS] " + name)
    else:
        failed += 1
        print("  [FAIL] " + name)


# 1. AES-128-ECB -----------------------------------------------------------
print("== 1. AES-128-ECB ==")
KEY = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
PT = bytes.fromhex("00112233445566778899aabbccddeeff")
CT = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")
check("FIPS-197 加密向量一致", aes128_ecb_encrypt(KEY, PT) == CT)
check("FIPS-197 解密向量一致", aes128_ecb_decrypt(KEY, CT) == PT)
# ECB 模式：逐 16 字节块独立加解密
def ecb_enc(k, d):
    return b"".join(aes128_ecb_encrypt(k, d[i:i + 16]) for i in range(0, len(d), 16))


def ecb_dec(k, d):
    return b"".join(aes128_ecb_decrypt(k, d[i:i + 16]) for i in range(0, len(d), 16))


blk = b"0123456789abcdef" * 3   # 48 字节 = 3 块
check("多块 ECB 加解密往返", ecb_dec(KEY, ecb_enc(KEY, blk)) == blk)

# 2. ISO-TP 分段 + 重组 ----------------------------------------------------
print("== 2. ISO-TP 分段与重组 ==")


def reassemble(frames):
    data = bytearray()
    total = None
    for _cid, f in frames:
        t = f[0] >> 4
        if t == 0:            # SF
            total = f[0] & 0x0F
            data += f[1:1 + total]
        elif t == 1:          # FF
            total = ((f[0] & 0x0F) << 8) | f[1]
            data += f[2:8]
        elif t == 2:          # CF
            data += f[1:8]
    return bytes(data[:total])


sf = iso_tp_frames(b"\x22\xF1\x90")
check("短 PDU 单帧 SF (PCI=0x03)", len(sf) == 1 and sf[0][1][0] == 3)
longpdu = bytes(range(20))
ff = iso_tp_frames(longpdu)
check("长 PDU 首帧 FF", ff[0][1][0] >> 4 == 1)
check("FF 长度编码正确", ((ff[0][1][0] & 0x0F) << 8) | ff[0][1][1] == 20)
check("CF 序列号 1..N 递增", all(((ff[i][1][0] >> 4) == 2) and
      ((ff[i][1][0] & 0x0F) == i) for i in range(1, len(ff))))
check("所有帧补零至 8 字节", all(len(f) == 8 for _c, f in ff))
# 分段->重组 对称性（对应 C 端 CAN-TP 收发状态机）
big = bytes(range(31))
bf = iso_tp_frames(big)
check("分段->重组还原一致 (31B)", reassemble(bf) == big)
check("分段->重组还原一致 (20B)", reassemble(ff) == longpdu)

# 3. UDS 会话权限矩阵 ------------------------------------------------------
print("== 3. UDS 会话权限矩阵 ==")
master = bytes.fromhex("DEADBEEF0123456789ABCDEF FEDCBA98".replace(" ", ""))
ecu = ReferenceEcuc(master)
r = ecu.handle(bytes([0x27, 0x01]))           # 默认会话 $01 下请求 $27
check("默认会话 $27 被拒 (NRC 7F)", r == b"\x7F\x27\x7F")
ecu.handle(bytes([0x10, 0x02]))               # 切编程会话
r = ecu.handle(bytes([0x27, 0x01]))
check("编程会话 $27 返回 Seed", r[:2] == b"\x67\x01" and len(r) == 18)
ecu.handle(bytes([0x10, 0x01]))               # 切回默认会话
r = ecu.handle(bytes([0x99]))                 # 未知服务
check("默认会话未知服务被拒 (NRC 7F)", r[:2] == b"\x7F\x99" and r[2] == 0x7F)

# 4. NRC 边界 --------------------------------------------------------------
print("== 4. NRC 边界 ==")
ecu = ReferenceEcuc(master)
ecu.handle(bytes([0x10, 0x02]))               # 编程会话, 安全未解锁
r = ecu.handle(bytes([0x36, 0x01, 0xAA]))
check("$36 安全锁拒绝 (NRC 33)", r == b"\x7F\x36\x33")
ecu.security = 1
ecu.handle(bytes([0x34, 0x00, 0x44]) + ecu.base.to_bytes(4, "big") +
           bytes([0, 0, 0, 4]))
r = ecu.handle(bytes([0x36, 0x05, 0xAA]))     # 期望 bsc=1, 实给 5
check("$36 块序号错误 (NRC 24)", r == b"\x7F\x36\x24")

# 5. 刷写三件套闭环 + CRC32 ------------------------------------------------
print("== 5. 刷写闭环 $34/$36/$37 ==")
ecu = ReferenceEcuc(master)
c = DiagClient(LoopbackTransport(ecu), master)
c.session(0x02)
c.security_unlock()
img = bytes((i * 7 + 3) & 0xFF for i in range(40))
c.request_download(ecu.base, len(img))
c.transfer_data(1, img[:20])
c.transfer_data(2, img[20:])
c.request_exit()
written = bytes(ecu.flash[0:40])
check("刷写回读一致", written == img)
check("传输退出后 dl 清空 (闭环完成)", ecu.dl is None)
check("CRC32 标准向量 (123456789)", crc32(b"123456789") == 0xCBF43926)

# 6. $27 Seed&Key 跨语言一致性基准 ----------------------------------------
print("== 6. $27 Seed&Key ==")
ecu = ReferenceEcuc(master)
seed = ecu._gen_seed()
key = derive_key(master, seed)
ecu.seed = seed
ecu.attempts = 0
ecu.session = 0x02
r = ecu.handle(bytes([0x27, 0x02]) + key)
check("正确 Key 解锁成功 (0x67 02)", r == b"\x67\x02")
ecu.seed = seed
r2 = ecu.handle(bytes([0x27, 0x02]) + bytes(16))
check("错误 Key 拒绝 (NRC 35)", r2 == b"\x7F\x27\x35")
# 跨语言对照基准：固定 master / 固定 seed 下派生出的 key 是确定的，
# C 端 src/security.c 用相同算法 + 相同输入应得到相同 key。
base_key = derive_key(bytes(16), bytes(16))
print("  [基准] master=0/seed=0 -> key = " + base_key.hex())

print("\n=== 结果: %d passed, %d failed ===" % (passed, failed))
sys.exit(1 if failed else 0)
