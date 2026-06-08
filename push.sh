#!/bin/bash
# SpiritFoxOS 一键推送脚本
# 用法: ./push.sh "提交信息"

REPO="/media/fwqfzqaz/王峥/SpiritFoxOS"
cd "$REPO" || { echo "❌ 目录不存在: $REPO"; exit 1; }

MSG="${1:-auto: sync $(date '+%Y-%m-%d %H:%M')}"

echo "📦 暂存文件..."
git add -A

echo "📝 提交: $MSG"
git commit -m "$MSG" || { echo "⚠️ 没有变更需要提交"; exit 0; }

echo "🚀 推送到 GitHub..."
git push origin main && echo "✅ 推送成功！" || { echo "❌ 推送失败"; exit 1; }
