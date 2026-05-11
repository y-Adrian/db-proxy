#!/usr/bin/env bash
# 结束本仓库 build/db-proxy 进程（避免误杀系统里其它同名程序）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT}/build/db-proxy"
if [[ ! -x "${BIN}" && ! -f "${BIN}" ]]; then
    echo "未找到 ${BIN}（若尚未编译可忽略）"
fi
if pgrep -f "${ROOT}/build/db-proxy" >/dev/null 2>&1; then
    pkill -f "${ROOT}/build/db-proxy" || true
    echo "已结束匹配 ${ROOT}/build/db-proxy 的进程。"
else
    echo "当前没有运行中的本仓库 db-proxy 进程。"
fi
