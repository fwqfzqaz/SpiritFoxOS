//! SHA-256 摘要（FIPS 180-4），从 `services/sha256.c` 逐位等价迁移。
//!
//! 被 SFK 包签名域哈希、事务盘封条、只读盘校验与信任密钥指纹共同使用，
//! 是信任链的基础原语：所有输入均有界，无堆、无副作用。
//! 加法全部使用 `wrapping_*`，与 C 无符号溢出回绕语义一致。

/// 64 个轮常量（前 32 个素数立方根小数部分前 32 位）。
const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

/// 单块压缩：以大端读入 16 个字，扩展 64 个字后做 64 轮混合。
fn block(h: &mut [u32; 8], p: &[u8]) {
    let mut w = [0u32; 64];
    for i in 0..16 {
        w[i] = u32::from_be_bytes([p[4 * i], p[4 * i + 1], p[4 * i + 2], p[4 * i + 3]]);
    }
    for i in 16..64 {
        let a = w[i - 15];
        let b = w[i - 2];
        let s0 = a.rotate_right(7) ^ a.rotate_right(18) ^ (a >> 3);
        let s1 = b.rotate_right(17) ^ b.rotate_right(19) ^ (b >> 10);
        w[i] = w[i - 16].wrapping_add(s0).wrapping_add(w[i - 7]).wrapping_add(s1);
    }
    let (mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut z) =
        (h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
    for i in 0..64 {
        let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
        let ch = (e & f) ^ (!e & g);
        let t = z.wrapping_add(s1).wrapping_add(ch).wrapping_add(K[i]).wrapping_add(w[i]);
        let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
        let maj = (a & b) ^ (a & c) ^ (b & c);
        let u = s0.wrapping_add(maj);
        // 轮转顺序与 C 实现一致：先推进 f/g/z，再叠加旧值。
        z = g;
        g = f;
        f = e;
        e = d.wrapping_add(t);
        d = c;
        c = b;
        b = a;
        a = t.wrapping_add(u);
    }
    h[0] = h[0].wrapping_add(a);
    h[1] = h[1].wrapping_add(b);
    h[2] = h[2].wrapping_add(c);
    h[3] = h[3].wrapping_add(d);
    h[4] = h[4].wrapping_add(e);
    h[5] = h[5].wrapping_add(f);
    h[6] = h[6].wrapping_add(g);
    h[7] = h[7].wrapping_add(z);
}

/// 计算任意字节序列的 SHA-256 摘要。
///
/// # Safety（C 调用方契约）
/// - `out` 必须指向至少 32 字节可写内存；
/// - `p` 为空时必须 `n == 0`；否则 `p[0..n]` 必须可读。
#[no_mangle]
pub unsafe extern "C" fn sf_sha256(out: *mut u8, p: *const u8, n: usize) {
    let mut h: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
        0x5be0cd19,
    ];
    let bytes = core::slice::from_raw_parts(p, n);
    let bits = (n as u64) * 8;
    // 整块流式压缩；余量 < 64 字节进入填充尾部。
    let mut chunks = bytes.chunks_exact(64);
    for chunk in chunks.by_ref() {
        block(&mut h, chunk);
    }
    let rest = chunks.remainder();
    // 填充：0x80 终止符 + 零填充 + 末尾 64 位大端比特长度。
    let mut tail = [0u8; 128];
    tail[..rest.len()].copy_from_slice(rest);
    tail[rest.len()] = 0x80;
    // 余量 < 56 时单块收尾，否则需要第二块容纳长度字段。
    let count: usize = if rest.len() < 56 { 64 } else { 128 };
    for i in 0..8 {
        tail[count - 1 - i] = (bits >> (i * 8)) as u8;
    }
    block(&mut h, &tail[..64]);
    if count == 128 {
        block(&mut h, &tail[64..128]);
    }
    // 输出按大端序列化为 32 字节。
    for i in 0..32 {
        *out.add(i) = (h[i / 4] >> (24 - 8 * (i % 4))) as u8;
    }
}
