# -*- coding: utf-8 -*-
"""NCM 解密：网易云 .ncm -> .flac / .mp3

原理：
  1. 文件头 "CTENFDAM"，随后是加密的 RC4 密钥
  2. 密钥数据逐字节 ^0x64，再用固定密钥 AES-128-ECB 解密
  3. 解密结果去掉前缀 b'neteasecloudmusic'，剩下的就是音频密钥
  4. 用音频密钥构建 256 字节替换表（KSA），再对音频数据做异或流（PRGA）
  5. 输出按文件头判断格式：b'fLaC' -> flac，其他 -> mp3

用法：
  python ncm2mp3.py <输入目录> [输出目录]
"""
import sys
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

# 控制台是 GBK，日文歌名里的「・」会编码失败，这里放宽输出编码
try:
    sys.stdout.reconfigure(errors="replace")
    sys.stderr.reconfigure(errors="replace")
except Exception:
    pass

NCM_KEY = b"hzHRAmso5kInbaxW"      # 网易云固定 AES 密钥
PREFIX = b"neteasecloudmusic"      # 解密后密钥的固定前缀
MAGIC = b"CTENFDAM"


def build_key_box(key: bytes) -> bytearray:
    """KSA：由音频密钥生成 256 字节替换表"""
    box = bytearray(range(256))
    last = 0
    for i in range(256):
        c = box[i]
        last = (c + last + key[i % len(key)]) & 0xFF
        box[i] = box[last]
        box[last] = c
    return box


def build_stream(n: int, box: bytearray) -> bytes:
    """PRGA：生成长度 n 的密钥流"""
    out = bytearray(n)
    for i in range(n):
        j = (i + 1) & 0xFF
        out[i] = box[(box[j] + box[(box[j] + j) & 0xFF]) & 0xFF]
    return bytes(out)


def aes_ecb_decrypt(data: bytes) -> bytes:
    dec = Cipher(algorithms.AES(NCM_KEY), modes.ECB()).decryptor()
    return dec.update(data) + dec.finalize()


def decrypt_ncm(path: Path) -> tuple:
    """返回 (音频字节, 扩展名)"""
    raw = path.read_bytes()
    if raw[:8] != MAGIC:
        raise ValueError("不是 ncm 文件")

    pos = 10                                   # magic(8) + gap(2)
    key_len = int.from_bytes(raw[pos:pos + 4], "little")
    pos += 4
    key_data = bytes(b ^ 0x64 for b in raw[pos:pos + key_len])
    pos += key_len

    plain = aes_ecb_decrypt(key_data)
    # 去掉 PKCS7 填充
    pad = plain[-1]
    if 0 < pad <= 16:
        plain = plain[:-pad]
    if not plain.startswith(PREFIX):
        raise ValueError("密钥格式不符")
    audio_key = plain[len(PREFIX):]

    # 跳过元数据
    meta_len = int.from_bytes(raw[pos:pos + 4], "little")
    pos += 4 + meta_len
    pos += 4 + 5                               # crc(4) + gap(5)

    # 跳过封面图
    img_size = int.from_bytes(raw[pos:pos + 4], "little")
    pos += 4 + img_size

    audio = bytearray(raw[pos:])
    stream = build_stream(len(audio), build_key_box(audio_key))
    for i in range(len(audio)):
        audio[i] ^= stream[i]

    ext = ".flac" if bytes(audio[:4]) == b"fLaC" else ".mp3"
    return bytes(audio), ext


def main() -> int:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else src / "converted"
    dst.mkdir(parents=True, exist_ok=True)

    files = sorted(src.rglob("*.ncm"))
    print(f"找到 {len(files)} 个 ncm 文件")
    if not files:
        return 1

    ok = fail = 0
    total = 0
    for f in files:
        try:
            audio, ext = decrypt_ncm(f)
            out = dst / (f.stem + ext)
            out.write_bytes(audio)
            total += len(audio)
            ok += 1
            print(f"  OK  {f.name}  ->  {out.name}  ({len(audio)/1024/1024:.1f} MB)")
        except Exception as e:
            fail += 1
            print(f"  FAIL {f.name}  ({e})")

    print(f"\n完成：成功 {ok}，失败 {fail}，输出 {total/1024/1024:.1f} MB")
    print(f"输出目录：{dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
