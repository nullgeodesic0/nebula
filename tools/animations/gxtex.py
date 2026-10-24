"""GameCube/Wii texture encoding: CMPR (S3TC/DXT1 in 8x8 tiles, big-endian)."""
import numpy as np


def _rgb565(c):
    c = np.clip(np.rint(c), 0, 255).astype(np.uint32)
    return ((c[..., 0] >> 3) << 11) | ((c[..., 1] >> 2) << 5) | (c[..., 2] >> 3)


def _expand565(v):
    r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float32)


def _encode_blocks(blocks):
    """blocks: (n, 16, 4) RGBA uint8 -> (n, 8) bytes of DXT1, GX byte order. Texels with alpha < 128
    use the transparent index (3-colour mode) when the block has any."""
    px = blocks[..., :3].astype(np.float32)
    opaque = blocks[..., 3] >= 128
    n = len(blocks)
    w = opaque[..., None].astype(np.float32)
    cnt = np.maximum(w.sum(1), 1)
    mean = (px * w).sum(1) / cnt
    centered = (px - mean[:, None]) * w
    cov = np.einsum('nki,nkj->nij', centered, centered)
    axis = np.ones((n, 3), np.float32)
    for _ in range(8):  # power iteration for the principal colour axis
        axis = np.einsum('nij,nj->ni', cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-6)
    proj = np.einsum('nki,ni->nk', px - mean[:, None], axis)
    proj = np.where(opaque, proj, np.nan)
    lo = np.nan_to_num(np.nanmin(proj, 1)); hi = np.nan_to_num(np.nanmax(proj, 1))
    c_hi = mean + axis * hi[:, None]
    c_lo = mean + axis * lo[:, None]
    e0 = _rgb565(c_hi); e1 = _rgb565(c_lo)
    has_alpha = ~opaque.all(1)
    # 4-colour mode needs e0 > e1, 3-colour + transparent needs e0 <= e1.
    swap = np.where(has_alpha, e0 > e1, e0 < e1)
    e0, e1 = np.where(swap, e1, e0), np.where(swap, e0, e1)
    same = (e0 == e1) & ~has_alpha
    e1 = np.where(same & (e0 > 0), e0 - 1, e1)
    e0 = np.where(same & (e0 == 0), 1, e0)
    p0, p1 = _expand565(e0), _expand565(e1)
    four = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)
    three = np.stack([p0, p1, (p0 + p1) / 2, p0], 1)
    pal = np.where(has_alpha[:, None, None], three, four)
    d = ((px[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)
    d[:, :, 3] = np.where(has_alpha[:, None], np.inf, d[:, :, 3])
    idx = d.argmin(-1)
    idx = np.where(has_alpha[:, None] & ~opaque, 3, idx)
    out = np.zeros((n, 8), np.uint8)
    out[:, 0] = e0 >> 8; out[:, 1] = e0 & 255; out[:, 2] = e1 >> 8; out[:, 3] = e1 & 255
    for row in range(4):
        v = np.zeros(n, np.uint32)
        for col in range(4):
            v = (v << 2) | idx[:, row * 4 + col].astype(np.uint32)
        out[:, 4 + row] = v
    return out


def encode_cmpr(img):
    """img: (h, w, 4) uint8, h and w multiples of 8 -> CMPR bytes."""
    h, w = img.shape[:2]
    # tiles of 8x8, each of four 4x4 blocks in Z order
    t = img.reshape(h // 8, 2, 4, w // 8, 2, 4, 4).transpose(0, 3, 1, 4, 2, 5, 6)
    blocks = t.reshape(-1, 16, 4)
    return _encode_blocks(blocks).tobytes()


def decode_cmpr(data, w, h):
    """CMPR bytes -> (h, w, 4) uint8 (for checks)."""
    b = np.frombuffer(data, np.uint8).reshape(-1, 8)
    e0 = (b[:, 0].astype(np.uint32) << 8) | b[:, 1]
    e1 = (b[:, 2].astype(np.uint32) << 8) | b[:, 3]
    p0, p1 = _expand565(e0), _expand565(e1)
    four = e0 > e1
    c2 = np.where(four[:, None], (2 * p0 + p1) / 3, (p0 + p1) / 2)
    c3 = np.where(four[:, None], (p0 + 2 * p1) / 3, 0)
    pal = np.stack([p0, p1, c2, c3], 1)
    alpha = np.stack([np.full(len(b), 255), np.full(len(b), 255), np.full(len(b), 255),
                      np.where(four, 255, 0)], 1)
    idx = np.zeros((len(b), 16), np.int64)
    for row in range(4):
        v = b[:, 4 + row]
        for col in range(4):
            idx[:, row * 4 + col] = (v >> (6 - 2 * col)) & 3
    rgb = np.take_along_axis(pal, idx[..., None].repeat(3, -1), 1)
    a = np.take_along_axis(alpha, idx, 1)
    px = np.concatenate([rgb, a[..., None]], -1).astype(np.uint8)
    t = px.reshape(h // 8, w // 8, 2, 2, 4, 4, 4).transpose(0, 2, 4, 1, 3, 5, 6)
    return t.reshape(h, w, 4)
