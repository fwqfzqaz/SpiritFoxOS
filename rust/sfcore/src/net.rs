//! 有界网络帧构造与解析，从 `services/net_proto.c` 等价迁移。
//!
//! 覆盖 ARP 请求、ICMP 回显、一次性 UDP/TCP 交换的帧构造与回包解析，
//! 以及 IPv4 头校验和、伪头 UDP/TCP 校验和。全部函数为纯帧逻辑：
//! 不发起系统调用、不触碰设备状态，被 ring-3 virtio-net 驱动与
//! 宿主边界测试共同使用。
//!
//! 多字节整数一律按网络序（大端）读写；校验和采用反码求和，
//! 加法使用 `wrapping_*` 与 C 回绕语义一致。

use crate::ffi::{
    SF_INVALID, SF_NET_ARP, SF_NET_FRAME_MAX, SF_NET_ICMP, SF_NET_ICMP_ID, SF_NET_IPV4,
    SF_NET_PAYLOAD_MAX, SF_NET_TCP, SF_NET_UDP, SF_OK,
};
use core::ffi::{c_char, c_int, c_uint};

// ---------- 字节序辅助 ----------

unsafe fn put16(p: *mut u8, v: u16) {
    *p = (v >> 8) as u8;
    *p.add(1) = v as u8;
}
unsafe fn put32(p: *mut u8, v: u32) {
    *p = (v >> 24) as u8;
    *p.add(1) = (v >> 16) as u8;
    *p.add(2) = (v >> 8) as u8;
    *p.add(3) = v as u8;
}
unsafe fn get16(p: *const u8) -> u16 {
    ((*p as u16) << 8) | *p.add(1) as u16
}
unsafe fn get32(p: *const u8) -> u32 {
    ((*p as u32) << 24) | ((*p.add(1) as u32) << 16) | ((*p.add(2) as u32) << 8) | *p.add(3) as u32
}

// ---------- 校验和 ----------

/// 原始反码累加器（不折叠、不取反），供伪头校验和复用。
unsafe fn add_words(mut sum: u32, mut p: *const u8, mut bytes: u32) -> u32 {
    while bytes > 1 {
        sum = sum.wrapping_add(((*p as u32) << 8) | *p.add(1) as u32);
        p = p.add(2);
        bytes -= 2;
    }
    if bytes != 0 {
        sum = sum.wrapping_add((*p as u32) << 8);
    }
    sum
}

/// 折叠进位并取反，得到最终校验和。
fn fold(mut sum: u32) -> u16 {
    while sum >> 16 != 0 {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    (!sum & 0xffff) as u16
}

/// Internet 校验和：从 `sum` 初值累加 16 位大端字后折叠取反。
///
/// # Safety（C 调用方契约）
/// `data` 必须可读 `bytes` 字节。
#[no_mangle]
pub unsafe extern "C" fn sf_net_checksum(sum: u32, data: *const u8, bytes: u32) -> u16 {
    fold(add_words(sum, data, bytes))
}

/// UDP 伪头校验和（协议号 17）。
///
/// # Safety（C 调用方契约）
/// `segment` 必须可读 `bytes` 字节。
#[no_mangle]
pub unsafe extern "C" fn sf_net_udp_checksum(src: u32, dst: u32, segment: *const u8, bytes: u32) -> u16 {
    pseudo_checksum(SF_NET_UDP, src, dst, segment, bytes)
}

/// TCP 伪头校验和（协议号 6）。
///
/// # Safety（C 调用方契约）
/// `segment` 必须可读 `bytes` 字节。
#[no_mangle]
pub unsafe extern "C" fn sf_net_tcp_checksum(src: u32, dst: u32, segment: *const u8, bytes: u32) -> u16 {
    pseudo_checksum(SF_NET_TCP, src, dst, segment, bytes)
}

/// 伪头：源/目的地址 + 零 + 协议号 + TCP 长度，共 12 字节。
unsafe fn pseudo_checksum(proto: u8, src: u32, dst: u32, segment: *const u8, bytes: u32) -> u16 {
    let mut pseudo = [0u8; 12];
    put32(pseudo.as_mut_ptr(), src);
    put32(pseudo.as_mut_ptr().add(4), dst);
    pseudo[9] = proto;
    put16(pseudo.as_mut_ptr().add(10), bytes as u16);
    let sum = add_words(0, pseudo.as_ptr(), 12);
    fold(add_words(sum, segment, bytes))
}

// ---------- IPv4 点分文本 ----------

/// 解析 `a.b.c.d` 形式的 IPv4 地址，每段 0–255 且不允许前导零段溢出。
///
/// # Safety（C 调用方契约）
/// `text` 必须指向 NUL 终止字符串；`out` 非空且可写。
#[no_mangle]
pub unsafe extern "C" fn sf_net_parse_ipv4(text: *const c_char, out: *mut u32) -> c_int {
    if text.is_null() || out.is_null() {
        return SF_INVALID;
    }
    let mut ip: u32 = 0;
    let mut part: u32 = 0;
    let mut t = text;
    while part < 4 {
        if *t < b'0' as c_char || *t > b'9' as c_char {
            return SF_INVALID;
        }
        let mut value: u32 = 0;
        let mut digits: u32 = 0;
        while *t >= b'0' as c_char && *t <= b'9' as c_char {
            if digits == 3 {
                return SF_INVALID;
            }
            value = value * 10 + *t as u32 - b'0' as u32;
            if value > 255 {
                return SF_INVALID;
            }
            digits += 1;
            t = t.add(1);
        }
        if digits == 0 {
            return SF_INVALID;
        }
        ip = (ip << 8) | value;
        part += 1;
        if part < 4 {
            if *t != b'.' as c_char {
                return SF_INVALID;
            }
            t = t.add(1);
        }
    }
    if *t != 0 {
        return SF_INVALID;
    }
    *out = ip;
    SF_OK
}

/// 格式化点分十进制地址到 16 字节缓冲（最长 `255.255.255.255` + NUL）。
///
/// # Safety（C 调用方契约）
/// `out` 必须可写至少 16 字节。
#[no_mangle]
pub unsafe extern "C" fn sf_net_format_ipv4(ip: u32, out: *mut c_char) {
    let shifts = [24u32, 16, 8, 0];
    let mut n = 0usize;
    for i in 0..4 {
        let mut value = (ip >> shifts[i]) & 255;
        let mut digits = 0usize;
        let mut reversed = [0u8; 3];
        // 逆序产出十进制位（do-while 保证 0 也有输出）。
        loop {
            reversed[digits] = b'0' + (value % 10) as u8;
            digits += 1;
            value /= 10;
            if value == 0 {
                break;
            }
        }
        while digits > 0 {
            digits -= 1;
            *out.add(n) = reversed[digits] as c_char;
            n += 1;
        }
        if i != 3 {
            *out.add(n) = b'.' as c_char;
            n += 1;
        }
    }
    *out.add(n) = 0;
}

// ---------- ARP ----------

/// 构造 42 字节 ARP 请求帧（广播目的 MAC，目标地址待解析）。
///
/// # Safety（C 调用方契约）
/// `frame` 必须可写 42 字节；`mac` 必须可读 6 字节本机 MAC。
#[no_mangle]
pub unsafe extern "C" fn sf_net_build_arp_request(frame: *mut u8, mac: *const u8, src: u32, dst: u32) -> c_uint {
    core::ptr::write_bytes(frame, 0, 42);
    for i in 0..6 {
        *frame.add(i) = 0xff;
        *frame.add(6 + i) = *mac.add(i);
    }
    put16(frame.add(12), SF_NET_ARP);
    let a = frame.add(14);
    // 硬件类型=以太网、协议=IPv4、长度 6/4、操作=请求。
    put16(a, 1);
    put16(a.add(2), SF_NET_IPV4);
    *a.add(4) = 6;
    *a.add(5) = 4;
    put16(a.add(6), 1);
    for i in 0..6 {
        *a.add(8 + i) = *mac.add(i);
    }
    put32(a.add(14), src);
    put32(a.add(24), dst);
    42
}

/// 解析 ARP 应答：命中期望 IP 时导出对方 MAC，返回 1。
///
/// # Safety（C 调用方契约）
/// `frame` 必须可读 `bytes` 字节；`mac_out` 可为空，非空时必须可写 6 字节。
#[no_mangle]
pub unsafe extern "C" fn sf_net_parse_arp_reply(
    frame: *const u8,
    bytes: u32,
    expect_ip: u32,
    mac_out: *mut u8,
) -> c_int {
    if bytes < 42 || get16(frame.add(12)) != SF_NET_ARP {
        return 0;
    }
    let a = frame.add(14);
    if get16(a) != 1
        || get16(a.add(2)) != SF_NET_IPV4
        || *a.add(4) != 6
        || *a.add(5) != 4
        || get16(a.add(6)) != 2
    {
        return 0;
    }
    if get32(a.add(14)) != expect_ip {
        return 0;
    }
    if !mac_out.is_null() {
        core::ptr::copy_nonoverlapping(a.add(8), mac_out, 6);
    }
    1
}

// ---------- ICMP ----------

/// 构造 58 字节 ICMP 回显请求帧（IPv4 头 + 24 字节 ICMP）。
///
/// # Safety（C 调用方契约）
/// `frame` 必须可写 58 字节；两个 MAC 各可读 6 字节。
#[no_mangle]
pub unsafe extern "C" fn sf_net_build_icmp_echo(
    frame: *mut u8,
    src_mac: *const u8,
    dst_mac: *const u8,
    src: u32,
    dst: u32,
    seq: u16,
) -> c_uint {
    core::ptr::write_bytes(frame, 0, 58);
    core::ptr::copy_nonoverlapping(dst_mac, frame, 6);
    core::ptr::copy_nonoverlapping(src_mac, frame.add(6), 6);
    put16(frame.add(12), SF_NET_IPV4);
    let ip = frame.add(14);
    *ip = 0x45; // 版本 4、IHL 5（无可选段）。
    put16(ip.add(2), 44);
    put16(ip.add(4), 1); // 标识。
    put16(ip.add(6), 0x4000); // DF 标志。
    *ip.add(8) = 64; // TTL。
    *ip.add(9) = SF_NET_ICMP;
    put32(ip.add(12), src);
    put32(ip.add(16), dst);
    put16(ip.add(10), sf_net_checksum(0, ip, 20));
    let icmp = ip.add(20);
    *icmp = 8; // 回显请求。
    put16(icmp.add(4), SF_NET_ICMP_ID);
    put16(icmp.add(6), seq);
    for i in 0..16 {
        *icmp.add(8 + i) = 0x40 + i as u8;
    }
    put16(icmp.add(2), sf_net_checksum(0, icmp, 24));
    58
}

/// 解析 ICMP 回显应答：校验 IPv4 头校验和、协议与标识/序号，返回 1。
///
/// # Safety（C 调用方契约）
/// `frame` 必须可读 `bytes` 字节。
#[no_mangle]
pub unsafe extern "C" fn sf_net_parse_icmp_reply(
    frame: *const u8,
    bytes: u32,
    expect_src: u32,
    seq: u16,
) -> c_int {
    if bytes < 42 || get16(frame.add(12)) != SF_NET_IPV4 {
        return 0;
    }
    let ip = frame.add(14);
    let ihl = (*ip & 15) as u32;
    if (*ip >> 4) != 4 || ihl < 5 || 14 + ihl * 4 + 8 > bytes {
        return 0;
    }
    let total = get16(ip.add(2)) as u32;
    if total < ihl * 4 + 8 || total > bytes - 14 {
        return 0;
    }
    if (get16(ip.add(6)) & 0x3fff) != 0
        || *ip.add(9) != SF_NET_ICMP
        || get32(ip.add(12)) != expect_src
        || sf_net_checksum(0, ip, ihl * 4) != 0
    {
        return 0;
    }
    let icmp = ip.add(ihl as usize * 4);
    if *icmp != 0
        || *icmp.add(1) != 0
        || get16(icmp.add(4)) != SF_NET_ICMP_ID
        || get16(icmp.add(6)) != seq
    {
        return 0;
    }
    1
}

// ---------- UDP ----------

/// 构造 UDP 数据报帧，返回帧长度；载荷越界返回 0。
///
/// # Safety（C 调用方契约）
/// `frame` 必须可写 [`SF_NET_FRAME_MAX`] 字节；`payload` 可为空
/// （仅当 `bytes==0`），否则必须可读 `bytes` 字节。
#[no_mangle]
pub unsafe extern "C" fn sf_net_build_udp(
    frame: *mut u8,
    src_mac: *const u8,
    dst_mac: *const u8,
    src: u32,
    sport: u16,
    dst: u32,
    dport: u16,
    payload: *const u8,
    bytes: u32,
) -> c_uint {
    if bytes > SF_NET_PAYLOAD_MAX as u32 || bytes > SF_NET_FRAME_MAX as u32 - (14 + 20 + 8) {
        return 0;
    }
    core::ptr::write_bytes(frame, 0, (14 + 20 + 8 + bytes) as usize);
    core::ptr::copy_nonoverlapping(dst_mac, frame, 6);
    core::ptr::copy_nonoverlapping(src_mac, frame.add(6), 6);
    put16(frame.add(12), SF_NET_IPV4);
    let ip = frame.add(14);
    *ip = 0x45;
    put16(ip.add(2), (28 + bytes) as u16);
    put16(ip.add(4), 1);
    put16(ip.add(6), 0x4000);
    *ip.add(8) = 64;
    *ip.add(9) = SF_NET_UDP;
    put32(ip.add(12), src);
    put32(ip.add(16), dst);
    put16(ip.add(10), sf_net_checksum(0, ip, 20));
    let udp = ip.add(20);
    put16(udp, sport);
    put16(udp.add(2), dport);
    put16(udp.add(4), (8 + bytes) as u16);
    if bytes != 0 && !payload.is_null() {
        core::ptr::copy_nonoverlapping(payload, udp.add(8), bytes as usize);
    }
    // 校验和为 0 需按线路规则改写为 0xffff（全零会被当作"无校验和"）。
    let checksum = sf_net_udp_checksum(src, dst, udp, 8 + bytes);
    put16(udp.add(6), if checksum != 0 { checksum } else { 0xffff });
    14 + 20 + 8 + bytes
}

/// 解析 UDP 数据报帧；载荷写入 248 字节缓冲，返回 1 表示命中。
///
/// IPv4 允许 UDP 校验和缺席（0）；存在时必须通过伪头校验。
///
/// # Safety（C 调用方契约）
/// `frame` 必须可读 `bytes` 字节；`payload` 必须可写
/// [`SF_NET_PAYLOAD_MAX`] 字节；`payload_bytes` 可为空。
#[no_mangle]
pub unsafe extern "C" fn sf_net_parse_udp(
    frame: *const u8,
    bytes: u32,
    expect_src: u32,
    expect_sport: u16,
    expect_dport: u16,
    payload: *mut u8,
    payload_bytes: *mut u32,
) -> c_int {
    if bytes < 42 || get16(frame.add(12)) != SF_NET_IPV4 {
        return 0;
    }
    let ip = frame.add(14);
    let ihl = (*ip & 15) as u32;
    if (*ip >> 4) != 4 || ihl < 5 || 14 + ihl * 4 + 8 > bytes {
        return 0;
    }
    let total = get16(ip.add(2)) as u32;
    if total < ihl * 4 + 8 || total > bytes - 14 {
        return 0;
    }
    if (get16(ip.add(6)) & 0x3fff) != 0
        || *ip.add(9) != SF_NET_UDP
        || get32(ip.add(12)) != expect_src
        || sf_net_checksum(0, ip, ihl * 4) != 0
    {
        return 0;
    }
    let udp = ip.add(ihl as usize * 4);
    let length = get16(udp.add(4)) as u32;
    if length < 8 || length > total - ihl * 4 {
        return 0;
    }
    let count = length - 8;
    if count > SF_NET_PAYLOAD_MAX as u32 {
        return 0;
    }
    if get16(udp) != expect_sport || get16(udp.add(2)) != expect_dport {
        return 0;
    }
    let checksum = get16(udp.add(6));
    // IPv4 允许 UDP 校验和缺席；存在则必须验证通过。
    if checksum != 0 && sf_net_udp_checksum(get32(ip.add(12)), get32(ip.add(16)), udp, length) != 0 {
        return 0;
    }
    if count != 0 {
        core::ptr::copy_nonoverlapping(udp.add(8), payload, count as usize);
    }
    if !payload_bytes.is_null() {
        *payload_bytes = count;
    }
    1
}

// ---------- DHCP ----------

const DHCP_CLIENT_PORT: u16 = 68;
const DHCP_SERVER_PORT: u16 = 67;
const DHCP_MAGIC: u32 = 0x6382_5363;
const DHCP_REQUEST: u8 = 3;

/// 构造 DHCP 客户端帧（DISCOVER 或 REQUEST），返回帧长度。
///
/// 以太网广播 + IPv4 0.0.0.0 → 255.255.255.255 + UDP 68→67；BOOTP 固定段
/// 携带本机 MAC，选项包含消息类型/客户端标识/参数请求，REQUEST 额外携带
/// 请求地址与服务端标识。整帧不经过 [`SF_NET_PAYLOAD_MAX`] 限制。
///
/// # Safety（C 调用方契约）
/// `frame` 必须可写 [`SF_NET_FRAME_MAX`] 字节；`mac` 必须可读 6 字节。
#[no_mangle]
pub unsafe extern "C" fn sf_net_build_dhcp(
    frame: *mut u8,
    mac: *const u8,
    xid: u32,
    msg_type: u8,
    requested_ip: u32,
    server_ip: u32,
) -> c_uint {
    // 固定段 236 + magic cookie 4 + 选项（最多 6+6+9+5+8+1）。
    let mut dhcp = [0u8; 320];
    dhcp[0] = 1; // BOOTREQUEST
    dhcp[1] = 1; // 以太网
    dhcp[2] = 6;
    put32(dhcp.as_mut_ptr().add(4), xid);
    put16(dhcp.as_mut_ptr().add(10), 0x8000); // 广播应答
    core::ptr::copy_nonoverlapping(mac, dhcp.as_mut_ptr().add(28), 6);
    put32(dhcp.as_mut_ptr().add(236), DHCP_MAGIC);
    let mut o = 240usize;
    dhcp[o] = 53;
    dhcp[o + 1] = 1;
    dhcp[o + 2] = msg_type;
    o += 3;
    // 客户端标识：类型 1（MAC）+ 6 字节硬件地址。
    dhcp[o] = 61;
    dhcp[o + 1] = 7;
    dhcp[o + 2] = 1;
    core::ptr::copy_nonoverlapping(mac, dhcp.as_mut_ptr().add(o + 3), 6);
    o += 9;
    // 参数请求：子网掩码(1)、路由器(3)、域名服务器(6)。
    dhcp[o] = 55;
    dhcp[o + 1] = 3;
    dhcp[o + 2] = 1;
    dhcp[o + 3] = 3;
    dhcp[o + 4] = 6;
    o += 5;
    if msg_type == DHCP_REQUEST {
        dhcp[o] = 50;
        dhcp[o + 1] = 4;
        put32(dhcp.as_mut_ptr().add(o + 2), requested_ip);
        o += 6;
        dhcp[o] = 54;
        dhcp[o + 1] = 4;
        put32(dhcp.as_mut_ptr().add(o + 2), server_ip);
        o += 6;
    }
    dhcp[o] = 255;
    o += 1;
    let total = 14 + 20 + 8 + o;
    if total > SF_NET_FRAME_MAX as usize {
        return 0;
    }
    core::ptr::write_bytes(frame, 0, total);
    for i in 0..6 {
        *frame.add(i) = 0xff;
        *frame.add(6 + i) = *mac.add(i);
    }
    put16(frame.add(12), SF_NET_IPV4);
    let ip = frame.add(14);
    *ip = 0x45;
    put16(ip.add(2), (28 + o) as u16);
    put16(ip.add(4), 1);
    put16(ip.add(6), 0x4000);
    *ip.add(8) = 64;
    *ip.add(9) = SF_NET_UDP;
    put32(ip.add(12), 0); // 尚无地址
    put32(ip.add(16), 0xffff_ffffu32); // 受限广播
    put16(ip.add(10), sf_net_checksum(0, ip, 20));
    let udp = ip.add(20);
    put16(udp, DHCP_CLIENT_PORT);
    put16(udp.add(2), DHCP_SERVER_PORT);
    put16(udp.add(4), (8 + o) as u16);
    core::ptr::copy_nonoverlapping(dhcp.as_ptr(), udp.add(8), o);
    let checksum = sf_net_udp_checksum(0, 0xffff_ffffu32, udp, (8 + o) as u32);
    put16(udp.add(6), if checksum != 0 { checksum } else { 0xffff });
    total as c_uint
}

/// 解析 DHCP 服务端应答（OFFER/ACK），返回 1 表示命中期望消息类型。
///
/// 导出 yiaddr 及 option 1/3/54（地址/网关/服务端）；全部输出指针可空。
///
/// # Safety（C 调用方契约）
/// `frame` 必须可读 `bytes` 字节；输出指针非空时必须可写 4 字节。
#[no_mangle]
pub unsafe extern "C" fn sf_net_parse_dhcp(
    frame: *const u8,
    bytes: u32,
    xid: u32,
    expect_type: u8,
    ip_out: *mut u32,
    gateway_out: *mut u32,
    netmask_out: *mut u32,
    server_out: *mut u32,
) -> c_int {
    if bytes < 14 + 20 + 8 + 240 || get16(frame.add(12)) != SF_NET_IPV4 {
        return 0;
    }
    let ip = frame.add(14);
    let ihl = (*ip & 15) as u32;
    if (*ip >> 4) != 4 || ihl < 5 || 14 + ihl * 4 + 8 + 240 > bytes {
        return 0;
    }
    let total = get16(ip.add(2)) as u32;
    if total < ihl * 4 + 8 + 240 || total > bytes - 14 {
        return 0;
    }
    if *ip.add(9) != SF_NET_UDP || sf_net_checksum(0, ip, ihl * 4) != 0 {
        return 0;
    }
    let udp = ip.add(ihl as usize * 4);
    let length = get16(udp.add(4)) as u32;
    if length < 8 + 240 || length > total - ihl * 4 {
        return 0;
    }
    if get16(udp) != DHCP_SERVER_PORT || get16(udp.add(2)) != DHCP_CLIENT_PORT {
        return 0;
    }
    let checksum = get16(udp.add(6));
    if checksum != 0 && sf_net_udp_checksum(get32(ip.add(12)), get32(ip.add(16)), udp, length) != 0 {
        return 0;
    }
    let dhcp = udp.add(8);
    let payload = length - 8;
    if *dhcp != 2 || *dhcp.add(1) != 1 || *dhcp.add(2) != 6 || get32(dhcp.add(4)) != xid {
        return 0;
    }
    if get32(dhcp.add(236)) != DHCP_MAGIC {
        return 0;
    }
    let mut message_type = 0u8;
    let mut mask = 0u32;
    let mut gateway = 0u32;
    let mut server = 0u32;
    let mut o = 240usize;
    while o < payload as usize {
        let code = *dhcp.add(o);
        if code == 0 {
            o += 1;
            continue;
        }
        if code == 255 {
            break;
        }
        if o + 1 >= payload as usize {
            break;
        }
        let len = *dhcp.add(o + 1) as usize;
        if o + 2 + len > payload as usize {
            break;
        }
        let value = dhcp.add(o + 2);
        match code {
            53 if len >= 1 => message_type = *value,
            1 if len == 4 => mask = get32(value),
            3 if len == 4 => gateway = get32(value),
            54 if len == 4 => server = get32(value),
            _ => {}
        }
        o += 2 + len;
    }
    if message_type != expect_type {
        return 0;
    }
    if !ip_out.is_null() {
        *ip_out = get32(dhcp.add(16));
    }
    if !gateway_out.is_null() {
        *gateway_out = gateway;
    }
    if !netmask_out.is_null() {
        *netmask_out = mask;
    }
    if !server_out.is_null() {
        *server_out = server;
    }
    1
}


// ---------- TCP ----------

/// 构造一次性 TCP 段帧（IHL 5、数据偏移 5，无可选段），返回帧长度。
///
/// # Safety（C 调用方契约）
/// 同 [`sf_net_build_udp`]。
#[no_mangle]
pub unsafe extern "C" fn sf_net_build_tcp(
    frame: *mut u8,
    src_mac: *const u8,
    dst_mac: *const u8,
    src: u32,
    sport: u16,
    dst: u32,
    dport: u16,
    seq: u32,
    ack: u32,
    flags: u8,
    window: u16,
    payload: *const u8,
    bytes: u32,
) -> c_uint {
    if bytes > SF_NET_PAYLOAD_MAX as u32 || bytes > SF_NET_FRAME_MAX as u32 - (14 + 20 + 20) {
        return 0;
    }
    core::ptr::write_bytes(frame, 0, (14 + 20 + 20 + bytes) as usize);
    core::ptr::copy_nonoverlapping(dst_mac, frame, 6);
    core::ptr::copy_nonoverlapping(src_mac, frame.add(6), 6);
    put16(frame.add(12), SF_NET_IPV4);
    let ip = frame.add(14);
    *ip = 0x45;
    put16(ip.add(2), (40 + bytes) as u16);
    put16(ip.add(4), 1);
    put16(ip.add(6), 0x4000);
    *ip.add(8) = 64;
    *ip.add(9) = SF_NET_TCP;
    put32(ip.add(12), src);
    put32(ip.add(16), dst);
    put16(ip.add(10), sf_net_checksum(0, ip, 20));
    let tcp = ip.add(20);
    put16(tcp, sport);
    put16(tcp.add(2), dport);
    put32(tcp.add(4), seq);
    put32(tcp.add(8), ack);
    *tcp.add(12) = 0x50; // 数据偏移 5（20 字节头）。
    *tcp.add(13) = flags;
    put16(tcp.add(14), window);
    if bytes != 0 && !payload.is_null() {
        core::ptr::copy_nonoverlapping(payload, tcp.add(20), bytes as usize);
    }
    // TCP 校验和是强制的，不存在"缺席"编码。
    put16(tcp.add(16), sf_net_tcp_checksum(src, dst, tcp, 20 + bytes));
    14 + 20 + 20 + bytes
}

/// 解析 TCP 段帧；导出序号/确认号/标志/窗口与载荷，返回 1 表示命中。
///
/// # Safety（C 调用方契约）
/// `frame` 必须可读 `bytes` 字节；`payload` 必须可写
/// [`SF_NET_PAYLOAD_MAX`] 字节；其余导出指针均可为空。
#[no_mangle]
pub unsafe extern "C" fn sf_net_parse_tcp(
    frame: *const u8,
    bytes: u32,
    expect_src: u32,
    expect_sport: u16,
    expect_dport: u16,
    seq: *mut u32,
    ack: *mut u32,
    flags: *mut u8,
    window: *mut u16,
    payload: *mut u8,
    payload_bytes: *mut u32,
) -> c_int {
    if bytes < 54 || get16(frame.add(12)) != SF_NET_IPV4 {
        return 0;
    }
    let ip = frame.add(14);
    let ihl = (*ip & 15) as u32;
    if (*ip >> 4) != 4 || ihl < 5 || 14 + ihl * 4 + 20 > bytes {
        return 0;
    }
    let total = get16(ip.add(2)) as u32;
    if total < ihl * 4 + 20 || total > bytes - 14 {
        return 0;
    }
    if (get16(ip.add(6)) & 0x3fff) != 0
        || *ip.add(9) != SF_NET_TCP
        || get32(ip.add(12)) != expect_src
        || sf_net_checksum(0, ip, ihl * 4) != 0
    {
        return 0;
    }
    let tcp = ip.add(ihl as usize * 4);
    let segment = total - ihl * 4;
    let offset = ((*tcp.add(12) >> 4) * 4) as u32;
    if offset < 20 || ihl * 4 + offset > total {
        return 0;
    }
    if get16(tcp) != expect_sport || get16(tcp.add(2)) != expect_dport {
        return 0;
    }
    if sf_net_tcp_checksum(get32(ip.add(12)), get32(ip.add(16)), tcp, segment) != 0 {
        return 0;
    }
    let count = segment - offset;
    if count > SF_NET_PAYLOAD_MAX as u32 {
        return 0;
    }
    if count != 0 {
        core::ptr::copy_nonoverlapping(tcp.add(offset as usize), payload, count as usize);
    }
    if !seq.is_null() {
        *seq = get32(tcp.add(4));
    }
    if !ack.is_null() {
        *ack = get32(tcp.add(8));
    }
    if !flags.is_null() {
        *flags = *tcp.add(13);
    }
    if !window.is_null() {
        *window = get16(tcp.add(14));
    }
    if !payload_bytes.is_null() {
        *payload_bytes = count;
    }
    1
}
