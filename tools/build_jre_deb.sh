#!/bin/bash
set -euo pipefail

# ============================================================
# SpiritFoxOS JRE .deb Package Builder
# Builds a custom .deb with UNCOMPRESSED data.tar for
# SpiritFoxOS pkgmgr compatibility
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_DIR="$PROJECT_DIR/cs"
DEB_OUTPUT="$OUTPUT_DIR/java-jre.deb"

STAGING_DIR="/tmp/spiritfox-jre-staging"
DOWNLOAD_PATH="/tmp/spiritfox-jre21.tar.gz"
JRE_URL="https://api.adoptium.net/v3/binary/latest/21/ga/linux/x64/jre/hotspot/normal/eclipse?project=jdk"

echo "=== SpiritFoxOS JRE .deb 构建工具 ==="
echo ""

# ----------------------------------------------------------
# 步骤 1: 下载 Eclipse Temurin JRE 21
# ----------------------------------------------------------
echo "[1/5] 下载 Eclipse Temurin JRE 21..."

if [ -f "$DOWNLOAD_PATH" ]; then
    echo "  已存在缓存文件: $DOWNLOAD_PATH，跳过下载"
else
    echo "  从 Adoptium 下载中..."
    if ! curl -fSL -o "$DOWNLOAD_PATH" "$JRE_URL"; then
        echo "错误: 下载 JRE 失败！" >&2
        rm -f "$DOWNLOAD_PATH"
        exit 1
    fi
fi

# 验证下载文件不为空
if [ ! -s "$DOWNLOAD_PATH" ]; then
    echo "错误: 下载的文件为空！" >&2
    rm -f "$DOWNLOAD_PATH"
    exit 1
fi

echo "  下载完成: $DOWNLOAD_PATH ($(du -h "$DOWNLOAD_PATH" | cut -f1))"

# ----------------------------------------------------------
# 步骤 2: 清理并创建临时目录，解压 JRE
# ----------------------------------------------------------
echo ""
echo "[2/5] 解压 JRE 到临时目录..."

rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR"

if ! tar -xzf "$DOWNLOAD_PATH" -C "$STAGING_DIR"; then
    echo "错误: 解压 JRE 失败！" >&2
    exit 1
fi

# 找到解压后的 JRE 目录 (通常为 jdk-21.x.x.x+xx-jre)
JRE_DIR=$(find "$STAGING_DIR" -maxdepth 1 -type d -name "jdk-*" | head -1)
if [ -z "$JRE_DIR" ]; then
    echo "错误: 未找到解压后的 JRE 目录！" >&2
    ls -la "$STAGING_DIR"
    exit 1
fi

echo "  JRE 目录: $JRE_DIR"

# ----------------------------------------------------------
# 步骤 3: 准备 .deb 包内容
# ----------------------------------------------------------
echo ""
echo "[3/5] 准备 .deb 包结构..."

DEB_STAGING="$STAGING_DIR/deb_staging"
DATA_DIR="$DEB_STAGING/data"
CONTROL_DIR="$DEB_STAGING/control"

rm -rf "$DEB_STAGING"
mkdir -p "$DATA_DIR" "$CONTROL_DIR"

# 创建 data 目录结构
mkdir -p "$DATA_DIR/usr/local/bin"
mkdir -p "$DATA_DIR/usr/local/lib/java-jre"
mkdir -p "$DATA_DIR/usr/local/lib"

# 复制整个 JRE 内容到 usr/local/lib/java-jre/jre/
cp -a "$JRE_DIR" "$DATA_DIR/usr/local/lib/java-jre/jre"
if [ $? -ne 0 ]; then
    echo "错误: 复制 JRE 文件失败！" >&2
    exit 1
fi

# 创建符号链接: usr/local/bin/java -> ../lib/jre/bin/java
ln -sf ../lib/jre/bin/java "$DATA_DIR/usr/local/bin/java"

# 创建符号链接: usr/local/lib/jre -> java-jre/jre
ln -sf java-jre/jre "$DATA_DIR/usr/local/lib/jre"

echo "  data 目录结构:"
echo "    usr/local/bin/java -> ../lib/jre/bin/java"
echo "    usr/local/lib/java-jre/jre/ (JRE 完整内容)"
echo "    usr/local/lib/jre -> java-jre/jre"

# 创建 control 文件
cat > "$CONTROL_DIR/control" <<'EOF'
Package: java-jre
Version: 21.0
Architecture: amd64
Maintainer: SpiritFoxOS
Description: Eclipse Temurin JRE 21 for SpiritFoxOS
EOF

if [ ! -f "$CONTROL_DIR/control" ]; then
    echo "错误: 创建 control 文件失败！" >&2
    exit 1
fi

echo "  control 文件已创建"

# ----------------------------------------------------------
# 步骤 4: 构建 .deb 包 (AR 格式, 未压缩 tar)
# ----------------------------------------------------------
echo ""
echo "[4/5] 构建 .deb 包 (AR 格式, 未压缩 tar)..."

DEB_WORK="$DEB_STAGING/assemble"
rm -rf "$DEB_WORK"
mkdir -p "$DEB_WORK"

# --- 创建 debian-binary ---
printf '2.0\n' > "$DEB_WORK/debian-binary"

# --- 创建 control.tar (未压缩) ---
# 使用 tar 创建未压缩的 control.tar
# 必须在 CONTROL_DIR 的父目录中操作，使路径为 ./control
(cd "$CONTROL_DIR" && tar cf "$DEB_WORK/control.tar" ./control)
if [ ! -f "$DEB_WORK/control.tar" ]; then
    echo "错误: 创建 control.tar 失败！" >&2
    exit 1
fi

# --- 创建 data.tar (未压缩, 关键!) ---
# 使用 tar 创建未压缩的 data.tar
(cd "$DATA_DIR" && tar cf "$DEB_WORK/data.tar" .)
if [ ! -f "$DEB_WORK/data.tar" ]; then
    echo "错误: 创建 data.tar 失败！" >&2
    exit 1
fi

echo "  debian-binary: $(wc -c < "$DEB_WORK/debian-binary") 字节"
echo "  control.tar:   $(wc -c < "$DEB_WORK/control.tar") 字节"
echo "  data.tar:      $(wc -c < "$DEB_WORK/data.tar") 字节"

# --- 使用 ar 格式组装 .deb ---
# AR 文件格式:
#   全局头: "!<arch>\n" (8 字节)
#   每个成员:
#     头: 60 字节 (name[16] + mtime[12] + uid[6] + gid[6] + mode[8] + size[10] + fmag[2])
#     数据: size 字节
#     填充: 如果 size 为奇数，追加一个 '\n' 使对齐到 2 字节边界

assemble_ar() {
    local output="$1"
    shift

    # 写入 AR 全局头
    printf '!<arch>\n' > "$output"

    for member in "$@"; do
        local basename
        basename="$(basename "$member")"
        local size
        size="$(stat -c '%s' "$member")"

        # AR 头部字段
        # name: 16 字节 (左对齐, 用空格填充, 以 "/ " 结尾表示文件)
        # mtime: 12 字节 (十进制 UNIX 时间戳)
        # uid: 6 字节
        # gid: 6 字节
        # mode: 8 字节 (八进制)
        # size: 10 字节 (十进制)
        # fmag: 2 字节 (0x60 0x0A = "`\n")

        printf '%-16s%-12s%-6s%-6s%-8s%-10s`\n' \
            "${basename}/" \
            "0" \
            "0" \
            "0" \
            "100644" \
            "$size" >> "$output"

        # 写入成员数据
        cat "$member" >> "$output"

        # 2 字节对齐填充 (如果 size 为奇数)
        if [ $((size % 2)) -ne 0 ]; then
            printf '\n' >> "$output"
        fi
    done
}

assemble_ar "$DEB_WORK/java-jre.deb" \
    "$DEB_WORK/debian-binary" \
    "$DEB_WORK/control.tar" \
    "$DEB_WORK/data.tar"

if [ ! -f "$DEB_WORK/java-jre.deb" ]; then
    echo "错误: 组装 .deb 文件失败！" >&2
    exit 1
fi

echo "  .deb 包已组装: $(wc -c < "$DEB_WORK/java-jre.deb") 字节"

# ----------------------------------------------------------
# 步骤 5: 复制到输出目录
# ----------------------------------------------------------
echo ""
echo "[5/5] 复制到输出目录..."

mkdir -p "$OUTPUT_DIR"

cp "$DEB_WORK/java-jre.deb" "$DEB_OUTPUT"
if [ $? -ne 0 ]; then
    echo "错误: 复制 .deb 到输出目录失败！" >&2
    exit 1
fi

# 确认 server.jar 存在
if [ -f "$OUTPUT_DIR/server.jar" ]; then
    echo "  server.jar 已存在于 cs/ 目录"
else
    echo "  警告: cs/server.jar 未找到！"
fi

# 清理临时文件
echo ""
echo "清理临时文件..."
rm -rf "$STAGING_DIR"

echo ""
echo "=== 构建完成 ==="
echo "  输出文件: $DEB_OUTPUT"
echo "  文件大小: $(du -h "$DEB_OUTPUT" | cut -f1)"
echo ""
