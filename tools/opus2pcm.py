"""opus2pcm.py — 将小智 OGG/Opus 语音转为 16kHz/16bit/双声道/小端裸 PCM。

用法:
    python tools/opus2pcm.py <input.ogg> <output.pcm>

说明:
    - 仅依赖系统自带的 libopus（Windows 下取华为 PCManager 附带的 opus.dll，
      标准 libopus 1.3.1），不安装/不联网/不引入第三方音频解码库。
    - 输出: 裸 PCM，无 WAV/OGG 头，16-bit 有符号小端、双声道、16 kHz。
    - 解码在 48 kHz 进行（Opus 固定输出采样率），再重采样到 16 kHz。
"""

import ctypes
import os
import struct
import sys

import numpy as np

# 优先用命令行指定的 DLL，否则回退到已知路径
OPUS_DLL = os.environ.get(
    "OPUS_DLL", r"C:\Program Files\Huawei\PCManager\opus.dll")

OUT_SR = 16000       # 目标采样率
DEC_SR = 48000       # Opus 内部解码采样率（固定）
OUT_CH = 2           # 目标双声道


def die(msg):
    print(f"[FAIL] {msg}", file=sys.stderr)
    sys.exit(1)


class OpusDecoder:
    """libopus 解码器薄封装（ctypes）。"""

    def __init__(self, sample_rate, channels):
        if not os.path.exists(OPUS_DLL):
            die(f"找不到 libopus DLL: {OPUS_DLL}")
        try:
            self.lib = ctypes.CDLL(OPUS_DLL)
        except OSError as e:
            die(f"加载 {OPUS_DLL} 失败: {e}")

        self.lib.opus_decoder_create.restype = ctypes.c_void_p
        self.lib.opus_decoder_create.argtypes = [
            ctypes.c_int32, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
        self.lib.opus_decode.restype = ctypes.c_int
        self.lib.opus_decode.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int32,
            ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_int]
        self.lib.opus_decoder_destroy.argtypes = [ctypes.c_void_p]

        err = ctypes.c_int(0)
        self.dec = self.lib.opus_decoder_create(
            sample_rate, channels, ctypes.byref(err))
        if err.value != 0 or not self.dec:
            die(f"opus_decoder_create 失败: code={err.value}")
        self.channels = channels
        # 最大帧 120ms@48kHz = 5760 样本；放大 4 倍留足多帧包的余量
        self.max_frame_samples = 5760 * 4

    def decode(self, packet):
        """解码一个 Opus 包，返回 int16 逐样本交织数组（n*channels）。"""
        nbuf = self.max_frame_samples * self.channels
        buf = (ctypes.c_int16 * nbuf)()
        if packet is not None and len(packet) > 0:
            pkt_arr = (ctypes.c_ubyte * len(packet)).from_buffer_copy(packet)
            pkt_len = len(packet)
        else:
            pkt_arr = None
            pkt_len = 0
        n = self.lib.opus_decode(
            self.dec, pkt_arr, pkt_len, buf, self.max_frame_samples, 0)
        if n < 0:
            return np.zeros(0, dtype=np.int16), n  # 返回错误码
        return np.ctypeslib.as_array(buf)[:n * self.channels].copy(), 0

    def close(self):
        if self.dec:
            self.lib.opus_decoder_destroy(self.dec)
            self.dec = None


def parse_ogg(path):
    """解析 OGG 容器，返回按序重组后的 Opus 包列表 (packet_bytes)。"""
    if not os.path.exists(path):
        die(f"输入文件不存在: {path}")
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 27:
        die("输入过短，非有效 OGG 文件")

    packets = []
    pos = 0
    cur = bytearray()
    last_granule = 0
    while pos < len(data):
        if data[pos:pos + 4] != b"OggS":
            break
        if pos + 27 > len(data):
            die("OGG 页头越界")
        nseg = data[pos + 26]
        seg_table = data[pos + 27:pos + 27 + nseg]
        if len(seg_table) != nseg:
            die("OGG 段表不完整")
        body_start = pos + 27 + nseg
        body_end = body_start + sum(seg_table)
        if body_end > len(data):
            die("OGG 页体越界")
        body = data[body_start:body_end]

        granule = struct.unpack("<q", data[pos + 6:pos + 14])[0]
        if granule > 0:
            last_granule = granule

        bpos = 0
        for seg in seg_table:
            cur.extend(body[bpos:bpos + seg])
            bpos += seg
            if seg < 255:  # 一个包结束
                packets.append(bytes(cur))
                cur = bytearray()
        pos = body_end

    if not packets:
        die("未解析到任何 OGG 包")
    return packets, last_granule


def parse_opush_head(head):
    """解析 OpusHead，返回 (channels, pre_skip, input_sr, mapping_family)。"""
    if len(head) < 19 or head[:8] != b"OpusHead":
        die(f"首包不是 OpusHead: {head[:8]!r}")
    version = head[8]
    channels = head[9]
    pre_skip = struct.unpack("<H", head[10:12])[0]
    input_sr = struct.unpack("<I", head[12:16])[0]
    mapping_family = head[18]
    if version != 1:
        die(f"不支持的 OpusHead 版本: {version}")
    if mapping_family != 0:
        die(f"不支持 channel mapping family {mapping_family}（仅支持 0）")
    return channels, pre_skip, input_sr, mapping_family


def resample_48k_to_16k(pcm48, channels):
    """pcm48: int16 交织数组（n*channels）。返回 16kHz 交织数组（n16*channels）。"""
    n = len(pcm48) // channels
    if n == 0:
        return np.zeros(0, dtype=np.int16)
    x = pcm48[:n * channels].reshape(-1, channels).astype(np.float64)
    # 48000 -> 16000 精确 3:1；用线性插值按比例取点，兼容非整数倍场景
    n_out = max(1, int(round(n * OUT_SR / DEC_SR)))
    t_in = np.linspace(0.0, n, n, endpoint=False)
    t_out = np.linspace(0.0, n, n_out, endpoint=False)
    out = np.empty((n_out, channels), dtype=np.float64)
    for c in range(channels):
        out[:, c] = np.interp(t_out, t_in, x[:, c])
    return out


def main():
    if len(sys.argv) != 3:
        die("用法: python opus2pcm.py <input.ogg> <output.pcm>")
    in_path, out_path = sys.argv[1], sys.argv[2]

    packets, last_granule = parse_ogg(in_path)
    channels, pre_skip, input_sr, _ = parse_opush_head(packets[0])

    # 第 2 个包是 OpusTags（注释头），其后为音频包
    audio_packets = packets[2:]
    if not audio_packets:
        die("OGG 中没有音频包")

    dec = OpusDecoder(DEC_SR, channels)
    frames = []
    try:
        for idx, pkt in enumerate(audio_packets):
            samples, code = dec.decode(pkt)
            if code != 0:
                die(f"第 {idx} 个包 opus_decode 出错 code={code}")
            if len(samples) > 0:
                frames.append(samples)
    finally:
        dec.close()

    if not frames:
        die("解码结果为空")

    pcm48 = np.concatenate(frames)
    n_dec = len(pcm48) // channels

    # 剪掉 pre_skip（解码器时延），并按最后一页 granule 收尾（去掉末帧多余重叠）
    total_samples = last_granule if last_granule > 0 else n_dec
    valid = total_samples - pre_skip
    if valid <= 0:
        die(f"有效样本数非法: granule={total_samples} pre_skip={pre_skip}")
    start = pre_skip
    end = min(n_dec, pre_skip + valid)
    if end <= start:
        die("截取区间为空（数据异常）")
    pcm48 = pcm48[start * channels:end * channels]

    # 重采样到 16kHz
    pcm16 = resample_48k_to_16k(pcm48, channels)

    # 单声道 -> 双声道（左右声道复制）
    if channels == 1:
        pcm16 = np.column_stack([pcm16[:, 0], pcm16[:, 0]])

    # 裁到 [-32768, 32767]，转 int16 小端
    pcm16 = np.clip(np.round(pcm16), -32768, 32767).astype("<i2")

    out_dir = os.path.dirname(os.path.abspath(out_path))
    os.makedirs(out_dir, exist_ok=True)

    raw = pcm16.reshape(-1).tobytes()  # int16 交织，小端
    try:
        with open(out_path, "wb") as f:
            f.write(raw)
    except OSError as e:
        die(f"写输出文件失败: {e}")

    duration = len(raw) / (OUT_SR * OUT_CH * 2)
    print(f"[OK]  输入: {in_path}  (Opus ch={channels} pre_skip={pre_skip} "
          f"input_sr={input_sr})")
    print(f"[OK]  输出: {out_path}")
    print(f"[OK]  大小: {len(raw)} 字节")
    print(f"[OK]  格式: {OUT_SR}Hz / 16bit signed / {OUT_CH}ch / little-endian")
    print(f"[OK]  时长: {duration:.3f} s")


if __name__ == "__main__":
    main()