# -*- coding: utf-8 -*-
"""
doip_client.py —— DoIP 诊断仪（ISO 13400-2 子集），零依赖，仅标准库。

与 uds_client.py 的关系：uds_client.py 把 UDS PDU 拆成 CAN 帧（ISO 15765-2），
本文件把同样的 UDS PDU 装进 DoIP 报文走 TCP —— 同一套应用层，两条传输层。

能力：
  1. DoIPConnection        : TCP 连接 + 路由激活 + 诊断消息(先 ACK 后响应) + AliveCheck
  2. demo 全链路           : python doip_client.py --tcp 127.0.0.1
                             会话 -> $27 解锁 -> 预编程检查 -> 擦除 -> 34/36/37 刷写
                             -> 整片 CRC32 比对 -> ECU 复位，全程打印报文追踪
  3. --announce            : 向 UDP 13400 发 VIN 车辆识别请求并解析 0x0002 应答
                             （供真实 DoIP ECU 使用；TCP 演示端不监听 UDP）

载荷类型/响应码与 src/doip.h 保持一致。
"""
import argparse
import socket
import struct
import sys

from aes import derive_key

# 与 test_uds.c / examples/doip_ecu_tcp.c 内置主密钥一致
MASTER_KEY = bytes.fromhex("DEADBEEF0123456789ABCDEF" "FEDCBA98")

# ---------------- DoIP 消息编解码 ----------------
PT_VEH_ID_REQ_VIN   = 0x0001
PT_VEH_ID_RESP      = 0x0002
PT_ROUTING_ACT_REQ  = 0x0005
PT_ROUTING_ACT_RESP = 0x0006
PT_ALIVE_CHECK_REQ  = 0x0007
PT_ALIVE_CHECK_RESP = 0x0008
PT_DIAG_MESSAGE     = 0x8001
PT_DIAG_ACK         = 0x8002
PT_DIAG_NACK        = 0x8003

PT_NAME = {
    PT_VEH_ID_RESP: "VehIDResp", PT_ROUTING_ACT_REQ: "RoutingActReq",
    PT_ROUTING_ACT_RESP: "RoutingActResp", PT_ALIVE_CHECK_REQ: "AliveChkReq",
    PT_ALIVE_CHECK_RESP: "AliveChkResp", PT_DIAG_MESSAGE: "DiagMessage",
    PT_DIAG_ACK: "DiagACK", PT_DIAG_NACK: "DiagNACK",
}

RAR_CODE = {0x00: "rejected:unspecified", 0x01: "rejected:unknownSA",
            0x04: "rejected:format", 0x10: "success", 0x11: "already-active"}


def enc_msg(ptype: int, payload: bytes = b"") -> bytes:
    return struct.pack(">BBHI", 0x02, 0xFD, ptype, len(payload)) + payload


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("对端在传输中关闭连接")
        buf += chunk
    return buf


def recv_msg(sock: socket.socket):
    ver, inv, ptype, plen = struct.unpack(">BBHI", recv_exact(sock, 8))
    if (ver, inv) != (0x02, 0xFD):
        raise ValueError(f"DoIP 头非法: {ver:02X} {inv:02X}")
    return ptype, (recv_exact(sock, plen) if plen else b"")


def hexs(b: bytes) -> str:
    return " ".join(f"{x:02X}" for x in b[:32]) + (" ..." if len(b) > 32 else "")


# ---------------- CRC32（与 C 端 crc32_update(0, ...) 等价） ----------------
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


# ---------------- 诊断仪连接 ----------------
class DoIPConnection:
    def __init__(self, host, port=13400, tester_addr=0x0F00, ecu_addr=0x0E80,
                 verbose=True, timeout=5.0):
        self.host, self.port = host, port
        self.tester, self.ecu = tester_addr, ecu_addr
        self.verbose = verbose
        self.timeout = timeout
        self.sock = None

    def _log(self, arrow, ptype, payload):
        if self.verbose:
            print(f"{arrow} {PT_NAME.get(ptype, hex(ptype)):<14} {hexs(payload)}")

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self.sock.settimeout(self.timeout)

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def routing_activate(self) -> int:
        req = struct.pack(">H4s", self.tester, b"\x00" * 4)
        self._log("TX->", PT_ROUTING_ACT_REQ, req)
        self.sock.sendall(enc_msg(PT_ROUTING_ACT_REQ, req))
        ptype, p = recv_msg(self.sock)
        if ptype != PT_ROUTING_ACT_RESP:
            raise RuntimeError(f"期望 0x0006，收到 {PT_NAME.get(ptype, hex(ptype))}")
        self._log("<-RX", ptype, p)
        return p[4]  # response code

    def diag(self, pdu: bytes) -> bytes:
        """发送一条 UDS PDU：先收 0x8002 ACK，再收 0x8001 响应。"""
        msg = struct.pack(">HH", self.tester, self.ecu) + pdu
        self._log("TX->", PT_DIAG_MESSAGE, msg)
        self.sock.sendall(enc_msg(PT_DIAG_MESSAGE, msg))
        while True:
            ptype, p = recv_msg(self.sock)
            if ptype == PT_DIAG_ACK:
                self._log("<-RX", ptype, p)
                if p[4] != 0x00:
                    raise RuntimeError(f"诊断 ACK 异常码 {p[4]:02X}")
                continue
            if ptype == PT_DIAG_NACK:
                self._log("<-RX", ptype, p)
                raise RuntimeError(f"诊断 NACK code={p[4]:02X}")
            if ptype == PT_DIAG_MESSAGE:
                self._log("<-RX", ptype, p)
                return p[4:]
            raise RuntimeError(f"意外消息类型 {ptype:04X}")

    def alive_check(self):
        self.sock.sendall(enc_msg(PT_ALIVE_CHECK_REQ))
        ptype, p = recv_msg(self.sock)
        self._log("<-RX", ptype, p)
        return ptype == PT_ALIVE_CHECK_RESP


# ---------------- 全链路演示 ----------------
def demo(host: str, port: int):
    conn = DoIPConnection(host, port)
    conn.connect()

    code = conn.routing_activate()
    print(f"[路由激活] code=0x{code:02X} ({RAR_CODE.get(code, '?')})")
    assert code in (0x10, 0x11), "路由激活失败"

    def expect(req, ok_prefix, label):
        resp = conn.diag(req)
        assert resp and resp[0] == ok_prefix, f"{label} 失败: {resp.hex(' ').upper()}"
        print(f"[{label}] OK  resp={hexs(resp)}")
        return resp

    expect(b"\x10\x02", 0x50, "10 02 进编程会话")

    seed = expect(b"\x27\x01", 0x67, "27 01 请求种子")[2:]
    key = derive_key(MASTER_KEY, seed)
    expect(b"\x27\x02" + key, 0x67, "27 02 发送密钥(AES-128 派生)")

    expect(b"\x31\x01\xFF\x01", 0x71, "31 FF01 预编程条件检查")
    expect(b"\x31\x01\x02\x01", 0x71, "31 0201 擦除 APP 区")

    # 128 字节演示固件镜像
    image = bytes((i * 7 + 0x33) & 0xFF for i in range(128))
    req34 = bytes([0x34, 0x00, 0x44]) + (0x08010000).to_bytes(4, "big") + \
            len(image).to_bytes(4, "big")
    r = expect(req34, 0x74, "34 请求下载")
    max_block = struct.unpack(">H", r[2:4])[0]

    bsc = 1
    off = 0
    n36 = 0
    while off < len(image):
        chunk = image[off:off + max_block]
        r = conn.diag(bytes([0x36, bsc]) + chunk)
        assert r[0] == 0x76, f"36 失败: {r.hex(' ').upper()}"
        bsc = (bsc + 1) & 0xFF
        off += len(chunk)
        n36 += 1
    print(f"[36 数据传输] OK  {n36} 块 x <= {max_block}B，BSC 递增回卷")

    expect(b"\x37", 0x77, "37 退出传输")

    r = expect(b"\x31\x01\xFF\x00", 0x71, "31 FF00 整片 CRC32")
    ecu_crc = struct.unpack(">I", r[5:9])[0]
    model = image + b"\xFF" * (64 * 1024 - len(image))   # 擦除态 + 镜像落位
    exp_crc = crc32(model)
    assert ecu_crc == exp_crc, f"CRC 不一致 ECU={ecu_crc:08X} 预期={exp_crc:08X}"
    print(f"[CRC 比对]  ECU=0x{ecu_crc:08X} == 预期=0x{exp_crc:08X}  一致")

    conn.alive_check()
    expect(b"\x11\x01", 0x51, "11 01 ECU 复位")

    conn.close()
    print("\n=== DoIP 全链路演示通过：路由激活 / $27 解锁 / 34-36-37 刷写 / CRC 校验 / 复位 ===")


# ---------------- UDP 车辆识别请求 ----------------
def announce(vin: str, broadcast="255.255.255.255", port=13400, timeout=2.0):
    pkt = enc_msg(PT_VEH_ID_REQ_VIN, vin.encode("ascii", "ignore").ljust(17, b"\x00")[:17])
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(timeout)
    s.sendto(pkt, (broadcast, port))
    try:
        data, addr = s.recvfrom(2048)
    except socket.timeout:
        print("未收到应答（真实 DoIP ECU 才会响应 UDP 广播）")
        return
    ver, inv, ptype, plen = struct.unpack(">BBHI", data[:8])
    p = data[8:8 + plen]
    if ptype == PT_VEH_ID_RESP:
        resp_vin = p[0:17].decode("ascii", "ignore").rstrip("\x00")
        la = struct.unpack(">H", p[17:19])[0]
        ipv4 = ".".join(str(b) for b in p[56:60])
        print(f"[{addr[0]}] 车辆应答: VIN={resp_vin} 逻辑地址=0x{la:04X} IPv4={ipv4}")
    s.close()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="DoIP 诊断仪（配套 build/doip_ecu.exe）")
    ap.add_argument("--tcp", metavar="HOST", help="连接 ECU 演示端并跑全链路，如 127.0.0.1")
    ap.add_argument("--port", type=int, default=13400)
    ap.add_argument("--announce", metavar="VIN", help="UDP 广播车辆识别请求")
    args = ap.parse_args()
    if args.tcp:
        demo(args.tcp, args.port)
    elif args.announce:
        announce(args.announce)
    else:
        ap.print_help()
        sys.exit(1)
