# -*- coding: utf-8 -*-
"""NCM 解密核心：网易云 .ncm -> .mp3 / .flac

不依赖界面，可单独被 GUI 或命令行调用。

原理：
  1. 文件以 "CTENFDAM" 开头，第 10 字节起是加密的密钥
  2. 密钥数据逐字节 ^0x64，再用固定密钥 AES-128-ECB 解密
  3. 明文去掉前缀 b'neteasecloudmusic'，剩下的是音频密钥
  4. 由音频密钥构建 256 字节替换表（KSA），再对音频数据做异或流（PRGA）
  5. 按文件头判断格式：b'fLaC' -> flac，否则 -> mp3
"""
from __future__ import annotations

import os
import warnings
from pathlib import Path
from typing import Optional, Tuple

# cryptography 在 Python 3.8 上会打弃用警告，屏蔽掉免得刷屏
warnings.filterwarnings("ignore", category=DeprecationWarning)
warnings.filterwarnings("ignore", message=".*Python 3.8.*")

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

#: 网易云客户端里写死的 AES 密钥
NCM_KEY = b"hzHRAmso5kInbaxW"

#: 解密后密钥的固定前缀
PREFIX = b"neteasecloudmusic"

#: 文件魔数
MAGIC = b"CTENFDAM"


class NcmError(Exception):
    """解密失败"""


def _build_key_box(key: bytes) -> bytearray:
    """KSA：由音频密钥生成 256 字节替换表"""
    box = bytearray(range(256))
    last = 0
    for i in range(256):
        c = box[i]
        last = (c + last + key[i % len(key)]) & 0xFF
        box[i] = box[last]
        box[last] = c
    return box


def _build_stream(size: int, box: bytearray) -> bytes:
    """PRGA：生成长度 size 的密钥流"""
    out = bytearray(size)
    for i in range(size):
        j = (i + 1) & 0xFF
        out[i] = box[(box[j] + box[(box[j] + j) & 0xFF]) & 0xFF]
    return bytes(out)


def _aes_ecb_decrypt(data: bytes) -> bytes:
    dec = Cipher(algorithms.AES(NCM_KEY), modes.ECB()).decryptor()
    return dec.update(data) + dec.finalize()


def decrypt_bytes(path: Path) -> Tuple[bytes, str]:
    """解密一个 ncm 文件，返回 (音频字节, 扩展名)"""
    raw = path.read_bytes()
    if len(raw) < 32 or raw[:8] != MAGIC:
        raise NcmError("不是有效的 ncm 文件")

    pos = 10  # 魔数 8 字节 + 间隔 2 字节

    # ---- 取出并解密音频密钥 ----
    key_len = int.from_bytes(raw[pos:pos + 4], "little")
    pos += 4
    if key_len <= 0 or pos + key_len > len(raw):
        raise NcmError("密钥长度异常")
    key_data = bytes(b ^ 0x64 for b in raw[pos:pos + key_len])
    pos += key_len

    plain = _aes_ecb_decrypt(key_data)
    pad = plain[-1]
    if 0 < pad <= 16:
        plain = plain[:-pad]
    if not plain.startswith(PREFIX):
        raise NcmError("密钥格式不符")
    audio_key = plain[len(PREFIX):]
    if not audio_key:
        raise NcmError("音频密钥为空")

    # ---- 跳过元数据、校验和、封面图 ----
    meta_len = int.from_bytes(raw[pos:pos + 4], "little")
    pos += 4 + meta_len
    pos += 4 + 5  # crc(4) + 间隔(5)

    if pos + 4 > len(raw):
        raise NcmError("文件被截断")
    img_size = int.from_bytes(raw[pos:pos + 4], "little")
    pos += 4 + img_size
    if pos >= len(raw):
        raise NcmError("文件被截断")

    # ---- 异或还原音频 ----
    audio = bytearray(raw[pos:])
    stream = _build_stream(len(audio), _build_key_box(audio_key))
    for i in range(len(audio)):
        audio[i] ^= stream[i]

    ext = ".flac" if bytes(audio[:4]) == b"fLaC" else ".mp3"
    return bytes(audio), ext


def convert_file(src: Path, out_dir: Optional[Path] = None,
                 delete_source: bool = False) -> Path:
    """转换单个文件，返回输出路径。

    src           : 源 .ncm
    out_dir       : 输出目录；为 None 表示原地生成
    delete_source : 成功后是否删除源文件
    """
    src = Path(src)
    audio, ext = decrypt_bytes(src)

    target_dir = Path(out_dir) if out_dir else src.parent
    out = target_dir / (src.stem + ext)

    # 同名文件已存在时加序号，避免覆盖
    if out.exists() and out.resolve() != src.resolve():
        n = 2
        while True:
            cand = target_dir / f"{src.stem} ({n}){ext}"
            if not cand.exists():
                out = cand
                break
            n += 1

    target_dir.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(out.suffix + ".part")
    tmp.write_bytes(audio)
    os.replace(tmp, out)

    if delete_source:
        try:
            src.unlink()
        except OSError:
            pass  # 删不掉不影响转换结果

    return out


def collect_ncm(paths) -> list:
    """把文件/文件夹列表展开成 .ncm 文件列表（去重、排序）"""
    found = []
    seen = set()
    for p in paths:
        p = Path(p)
        if p.is_dir():
            for f in sorted(p.rglob("*.ncm")):
                if f not in seen:
                    seen.add(f)
                    found.append(f)
        elif p.is_file() and p.suffix.lower() == ".ncm":
            if p not in seen:
                seen.add(p)
                found.append(p)
    return found
