#!/usr/bin/env bash
set -Eeuo pipefail

command -v readlink >/dev/null 2>&1 || {
  printf '[agent-skills] error: 缺少依赖：readlink\n' >&2
  exit 1
}
SCRIPT_PATH=$(readlink -f -- "${BASH_SOURCE[0]}")
REPO_DIR=$(cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)
# shellcheck source=scripts/lib.sh
source "$REPO_DIR/scripts/lib.sh"

usage() {
  cat <<'EOF'
用法：./sync-skills.sh [--pull] [--all-agents]

从当前仓库全局同步全部 skills。默认由 skills CLI 自动识别本机 Agent。

选项：
  --pull        同步前执行 git pull --ff-only
  --all-agents  明确同步到 skills CLI 支持的全部 Agent
  -h, --help    显示帮助
EOF
}

pull_first=false
all_agents=false
while (($# > 0)); do
  case $1 in
    --pull) pull_first=true ;;
    --all-agents) all_agents=true ;;
    -h|--help) usage; exit 0 ;;
    *) die "未知参数：$1" ;;
  esac
  shift
done

require_common_dependencies
require_supported_node
require_repo_layout
require_detectable_agent_or_all "$all_agents"
agent_args "$all_agents"

if [[ $pull_first == true ]]; then
  info "更新中央仓库（仅允许 fast-forward）"
  git -C "$REPO_DIR" pull --ff-only
fi

info "同步全部 skills，并由 skills CLI 处理本机 Agent 目录"
run_skills add "$REPO_DIR" -g -s '*' "${AGENT_ARGS[@]}" -y
sync_antigravity_global_skills
info "skills 同步完成"
