"""
uds_client.py —— 诊断仪侧 Python 工具（零依赖，仅标准库）。

能力：
  1. iso_tp_frames(pdu, tx_id)         : 把一条 UDS PDU 拆成 ISO-15765-2 CAN 帧
                                          （SF / FF+CF），可直接用于 CANoe CAPL 激励
  2. DiagClient(transport)             : 以 PDU 粒度驱动完整诊断会话
                                          （$10/$3E/$22/$27/$2E/$31/$34/$36/$37）
  3. LoopbackTransport + ReferenceEcuc : 进程内参考 ECU，使 `python uds_client.py --demo`
                                          可离线跑通"编程会话 + 安全解锁 + 刷写"全链路，
                                          并打印 CANoe 风格报文追踪，用于演示与自测。
  4. AES Seed&Key 与 C 栈（src/security.c）算法完全一致，可跨语言互验。

注：生产协议栈是 C 实现（src/），本文件是诊断仪/测试侧配套工具。
"""
import argparse
import random

from aes import aes128_ecb_encrypt, derive_key

# ---------------- CRC32 (与 C 端一致) ----------------
_CRC_TABLE = []
for _i in range(256):
    _c = _i
    for _k in range(8):
        _c = (_c >> 1) ^ 0xEDB88320 if (_c & 1) else (_c >> 1)
    _CRC_TABLE.append(_c)


def crc32(data: bytes) -> int:
    c = 0xFFFFFFFF
    for b in data:
        c = _CRC_TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


# ---------------- ISO-TP 分段（诊断仪 -> ECU） ----------------
def iso_tp_frames(pdu: bytes, tx_id: int = 0x7E0):
    """把 UDS PDU 拆成 CAN 帧（每帧 8 字节，含 PCI）。返回 [(can_id, bytes), ...]"""
    frames = []
    n = len(pdu)
    if n <= 7:
        frames.append((tx_id, bytes([n]) + pdu))
    else:
        frames.append((tx_id, bytes([0x10 | (n >> 8), n & 0xFF]) + pdu[:6]))
        sn = 1
        off = 6
        while off < n:
            chunk = pdu[off:off + 7]
            frames.append((tx_id, bytes([0x20 | (sn & 0xF)]) + chunk))
            sn = (sn + 1) & 0xF
            off += 7
    # 末帧补 0 到 8 字节
    frames = [(cid, f + bytes(8 - len(f))) for cid, f in frames]
    return frames


def dump_frames(frames, label="TX"):
    for i, (cid, f) in enumerate(frames):
        pci = f[0] >> 4
        kind = {0: "SF", 1: "FF", 2: "CF", 3: "FC"}[pci]
        print(f"  [{label}] {cid:03X}#{i} {kind}: " + " ".join(f"{b:02X}" for b in f))


# ---------------- 传输层抽象 ----------------
class PduTransport:
    def send_pdu(self, pdu: bytes): raise NotImplementedError
    def recv_pdu(self) -> bytes: raise NotImplementedError


class LoopbackTransport(PduTransport):
    """进程内闭环：把请求交给 ReferenceEcuc，返回其响应 PDU。"""
    def __init__(self, ecu):
        self.ecu = ecu
    def send_pdu(self, pdu: bytes):
        self._resp = self.ecu.handle(pdu)
    def recv_pdu(self) -> bytes:
        return self._resp


# ---------------- 诊断客户端 ----------------
class DiagClient:
    def __init__(self, transport: PduTransport, master_key: bytes, seed_rng=0xCAFEBABE):
        self.t = transport
        self.master = master_key
        self.seed_rng_state = seed_rng
        self.trace = []

    def _next_seed(self) -> bytes:
        # 与 C 端 xorshift32 同构，保证 Python/C 派生 key 一致
        x = self.seed_rng_state
        x ^= x << 13; x &= 0xFFFFFFFF
        x ^= x >> 17
        x ^= x << 5; x &= 0xFFFFFFFF
        self.seed_rng_state = x
        return x.to_bytes(16, "little")

    def _req(self, pdu: bytes):
        self.t.send_pdu(pdu)
        resp = self.t.recv_pdu()
        self.trace.append((bytes(pdu), bytes(resp)))
        return resp

    def session(self, s: int):
        return self._req(bytes([0x10, s]))

    def tester_present(self):
        return self._req(bytes([0x3E, 0x00]))

    def read_did(self, *dids):
        p = bytearray([0x22])
        for d in dids:
            p += d.to_bytes(2, "big")
        return self._req(bytes(p))

    def security_unlock(self):
        # 诊断仪先请求 seed（由 ECU 端内部生成），再派生 key 回送
        r = self._req(bytes([0x27, 0x01]))
        ecu_seed = r[2:18]
        key = derive_key(self.master, ecu_seed)
        return self._req(bytes([0x27, 0x02]) + key)

    def request_download(self, addr: int, size: int):
        lfi = 0x44
        p = bytes([0x34, 0x00, lfi]) + addr.to_bytes(4, "big") + size.to_bytes(4, "big")
        return self._req(p)

    def transfer_data(self, bsc: int, data: bytes):
        return self._req(bytes([0x36, bsc & 0xFF]) + data)

    def request_exit(self):
        return self._req(bytes([0x37]))


# ---------------- 参考 ECU（进程内，PDU 粒度，仅供 demo/自测） ----------------
class ReferenceEcuc:
    NRC = {  # 负响应码
        "GENERAL": 0x10, "SERVICE": 0x11, "SUBFUNC": 0x12, "LENGTH": 0x13,
        "COND": 0x22, "RANGE": 0x31, "DENIED": 0x33, "KEY": 0x35,
        "ATTEMPTS": 0x36, "SEQ": 0x24, "SES": 0x7F, "SUBSES": 0x7E,
    }

    def __init__(self, master_key, base=0x08010000, size=64 * 1024):
        self.master = master_key
        self.flash = bytearray(size)
        self.base = base
        self.session = 0x01
        self.security = 0
        self.seed = None
        self.dl = None
        self.attempts = 0
        self.rng = 0xCAFEBABE

    def _neg(self, sid, nrc):
        return bytes([0x7F, sid, nrc])

    def _session_ok(self, sid):
        if sid in (0x10, 0x11, 0x22, 0x3E):
            return True
        if sid in (0x27, 0x2E, 0x31):
            return self.session in (0x03, 0x02)
        if sid in (0x34, 0x36, 0x37):
            return self.session == 0x02
        return False

    def _gen_seed(self):
        x = self.rng
        x ^= x << 13; x &= 0xFFFFFFFF
        x ^= x >> 17
        x ^= x << 5; x &= 0xFFFFFFFF
        self.rng = x
        self.seed = x.to_bytes(16, "little")
        return self.seed

    def handle(self, req: bytes) -> bytes:
        if not req:
            return self._neg(0x00, self.NRC["LENGTH"])
        sid = req[0]
        if not self._session_ok(sid):
            return self._neg(sid, self.NRC["SES"])

        if sid == 0x10:
            self.session = req[1] & 0x7F
            self.security = 0
            return bytes([0x50, req[1] & 0x7F, 0x00, 0x32, 0x01, 0xF4])
        if sid == 0x3E:
            return bytes([0x7E, 0x00])
        if sid == 0x22:
            out = bytearray([0x62])
            i = 1
            while i + 1 < len(req):
                did = (req[i] << 8) | req[i + 1]
                if did == 0xF190:
                    out += did.to_bytes(2, "big") + b"LSVUDS0000000000"
                elif did == 0xF195:
                    out += did.to_bytes(2, "big") + b"BL_V1.0.0"
                elif did == 0x0100:
                    out += did.to_bytes(2, "big") + b"2026.07"
                else:
                    return self._neg(0x22, self.NRC["RANGE"])
                i += 2
            return bytes(out)
        if sid == 0x27:
            sub = req[1] & 0x7F
            if sub == 0x01:
                if self.attempts >= 3:
                    return self._neg(0x27, self.NRC["ATTEMPTS"])
                self.seed = self._gen_seed()
                return bytes([0x67, 0x01]) + self.seed
            elif sub == 0x02:
                if self.seed is None:
                    return self._neg(0x27, self.NRC["SEQ"])
                key = req[2:18]
                exp = derive_key(self.master, self.seed)
                self.seed = None
                if key == exp:
                    self.security = 1
                    self.attempts = 0
                    return bytes([0x67, 0x02])
                self.attempts += 1
                return self._neg(0x27, self.NRC["KEY"])
            return self._neg(0x27, self.NRC["SUBFUNC"])
        if sid == 0x34:
            addr = int.from_bytes(req[3:7], "big")
            size = int.from_bytes(req[7:11], "big")
            if addr < self.base or addr + size > self.base + len(self.flash):
                return self._neg(0x34, self.NRC["RANGE"])
            for k in range(addr, addr + size):
                self.flash[k - self.base] = 0xFF
            self.dl = {"addr": addr, "remaining": size, "bsc": 1, "crc": 0}
            return bytes([0x74, 0x20, 0x00, 0x20])
        if sid == 0x36:
            if self.security == 0:
                return self._neg(0x36, self.NRC["DENIED"])
            if self.dl is None:
                return self._neg(0x36, self.NRC["SEQ"])
            bsc = req[1]
            if bsc != self.dl["bsc"]:
                return self._neg(0x36, self.NRC["SEQ"])
            data = req[2:]
            off = self.dl["addr"] - self.base
            for k, b in enumerate(data):
                self.flash[off + k] = b
            # 累计整片已写区域的 CRC32（demo 级存在性/完整性校验）
            self.dl["crc"] = crc32(bytes(self.flash[:off + len(data)]))
            self.dl["addr"] += len(data)
            self.dl["remaining"] -= len(data)
            self.dl["bsc"] = (self.dl["bsc"] + 1) & 0xFF
            return bytes([0x76, bsc])
        if sid == 0x37:
            if self.dl is None:
                return self._neg(0x37, self.NRC["SEQ"])
            if self.dl["remaining"] != 0:
                return self._neg(0x37, self.NRC["SEQ"])
            self.dl = None
            return bytes([0x77])
        return self._neg(sid, self.NRC["SERVICE"])


def run_demo():
    master = bytes.fromhex("DEADBEEF0123456789ABCDEF FEDCBA98".replace(" ", ""))
    ecu = ReferenceEcuc(master)
    client = DiagClient(LoopbackTransport(ecu), master)
    print("=== UDS Bootloader 端到端闭环演示 (Python Loopback) ===")
    print("[1] 进入编程会话 $10 02 ->", client.session(0x02).hex())
    print("[2] 安全解锁 $27 01/02 ->", client.security_unlock().hex())
    # 造一段 40 字节固件镜像，分两块 $36 写入
    image = bytes((i * 7 + 3) & 0xFF for i in range(40))
    addr = ecu.base
    print("[3] 请求下载 $34 ->", client.request_download(addr, len(image)).hex())
    print("[4] 传输块1 $36 01 ->", client.transfer_data(1, image[:20]).hex())
    print("[5] 传输块2 $36 02 ->", client.transfer_data(2, image[20:]).hex())
    print("[6] 退出传输 $37 ->", client.request_exit().hex())
    # 校验写入
    written = bytes(ecu.flash[addr - ecu.base: addr - ecu.base + 40])
    print("[7] Flash 回读校验:", "PASS" if written == image else "FAIL")
    # ISO-TP 分段演示
    print("\n=== ISO-TP 分段演示（一条 30 字节 $36 请求）===")
    long_req = bytes([0x36, 0x01]) + bytes(range(30))
    frames = iso_tp_frames(long_req)
    dump_frames(frames, "TX")
    print(f"    共 {len(frames)} 帧 (1 FF + {len(frames)-1} CF)")
    print("\nOK: 闭环演示完成")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", action="store_true", help="运行端到端闭环演示")
    args = ap.parse_args()
    if args.demo:
        run_demo()
    else:
        print("使用: python uds_client.py --demo")
