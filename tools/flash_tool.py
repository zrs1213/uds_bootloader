"""
flash_tool.py —— 固件刷写脚本生成器（零依赖）。

读取 Intel HEX 固件文件，按 Bootloader 下载协议（$34/$36/$37）切块，
生成可直接灌入 CANoe / 诊断仪的请求序列，并给出：
  - 每条请求的 UDS PDU 与 ISO-TP CAN 帧拆解
  - 整包 CRC32（刷写完整性校验参考）
  - 可选输出 CANoe CAPL 激励脚本（--capl out.can）

用法：
  python flash_tool.py firmware.hex
  python flash_tool.py firmware.hex --block 32 --capl download.can
"""
import argparse
import sys

from uds_client import crc32, iso_tp_frames, dump_frames  # crc32/ISO-TP 与 C 端一致


def parse_intel_hex(path: str):
    """返回 (image: dict[addr]->byte, start:int, end:int)"""
    image = {}
    ext_linear = 0  # 0x04 扩展线性地址高位
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or not line.startswith(":"):
                continue
            data = bytes.fromhex(line[1:])
            length = data[0]
            addr = (data[1] << 8) | data[2]
            rectype = data[3]
            payload = data[4:4 + length]
            if rectype == 0x00:  # 数据记录
                base = (ext_linear << 16) | addr
                for i, b in enumerate(payload):
                    image[base + i] = b
            elif rectype == 0x04:  # 扩展线性地址
                ext_linear = (payload[0] << 8) | payload[1]
            elif rectype == 0x01:  # 文件结束
                break
    if not image:
        raise ValueError("HEX 文件无数据记录")
    addrs = sorted(image)
    return image, addrs[0], addrs[-1]


def build_requests(image, start, end, base_addr, block=32):
    """生成完整下载请求 PDU 列表（不含 ISO-TP 封装）"""
    size = end - start + 1
    reqs = []
    # $34 请求下载
    lfi = 0x44
    req34 = bytes([0x34, 0x00, lfi]) + base_addr.to_bytes(4, "big") + size.to_bytes(4, "big")
    reqs.append(("RequestDownload", req34))
    # $36 数据传输（按块）
    off = 0
    bsc = 1
    while off < size:
        chunk = bytes(image[start + i] for i in range(off, min(off + block, size)))
        req36 = bytes([0x36, bsc & 0xFF]) + chunk
        reqs.append((f"TransferData#{bsc:02d}", req36))
        off += block
        bsc = (bsc + 1) & 0xFF
    # $37 退出传输
    reqs.append(("RequestTransferExit", bytes([0x37])))
    return reqs, size


def emit_capl(reqs, tx_id=0x7E0):
    lines = []
    lines.append("/* 由 flash_tool.py 生成的 CANoe CAPL 刷写激励脚本 */")
    lines.append("void FlashFirmware(void)")
    lines.append("{")
    lines.append("  message 0x7E0 msg;")
    lines.append("  int i; byte data[8];")
    for name, pdu in reqs:
        frames = iso_tp_frames(pdu, tx_id)
        lines.append(f"  /* --- {name} (UDS PDU: {' '.join(f'{b:02X}' for b in pdu)}) --- */")
        for cid, f in frames:
            hexbytes = ", ".join(f"0x{b:02X}" for b in f)
            lines.append(f"  msg.dlc = 8; data = {hexbytes};")
            lines.append("  for (i=0;i<8;i++) msg.byte(i) = data[i];")
            lines.append("  output(msg); testWaitForTimeout(5);")
    lines.append("}")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hexfile")
    ap.add_argument("--addr", type=lambda x: int(x, 0), default=None,
                    help="Flash 起始地址（默认用 HEX 首地址）")
    ap.add_argument("--block", type=int, default=32, help="每块字节数（默认 32）")
    ap.add_argument("--capl", default=None, help="输出 CANoe CAPL 脚本路径")
    args = ap.parse_args()

    image, start, end = parse_intel_hex(args.hexfile)
    base_addr = args.addr if args.addr is not None else start
    reqs, size = build_requests(image, start, end, base_addr, args.block)

    # 整包校验
    raw = bytes(image[start + i] for i in range(size))
    crc = crc32(raw)

    print(f"=== Flash 刷写脚本生成 ===")
    print(f"  HEX 范围 : 0x{start:08X} .. 0x{end:08X}")
    print(f"  下载地址 : 0x{base_addr:08X}")
    print(f"  固件大小 : {size} 字节 ({size/1024:.2f} KB)")
    print(f"  块大小   : {args.block} 字节 -> {len(reqs)-2} 个 $36 块")
    print(f"  整包CRC32: 0x{crc:08X}")
    print(f"\n--- 请求序列（UDS PDU + ISO-TP 帧）---")
    for name, pdu in reqs:
        print(f"\n[{name}] PDU: {' '.join(f'{b:02X}' for b in pdu)}")
        dump_frames(iso_tp_frames(pdu), "TX")

    if args.capl:
        with open(args.capl, "w") as f:
            f.write(emit_capl(reqs))
        print(f"\nCANoe CAPL 脚本已写出: {args.capl}")


if __name__ == "__main__":
    main()
