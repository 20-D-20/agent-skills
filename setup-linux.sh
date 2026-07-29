#!/usr/bin/env bash
set -Eeuo pipefail

if ! command -v readlink >/dev/null 2>&1; then
  printf '[agent-skills] error: 缺少依赖：readlink（脚本只检测依赖，不会自动安装）\n' >&2
  exit 1
fi

SCRIPT_PATH=$(readlink -f -- "${BASH_SOURCE[0]}")
REPO_DIR=$(cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)
# shellcheck source=scripts/lib.sh
source "$REPO_DIR/scripts/lib.sh"

usage() {
  cat <<'EOF'
用法：./setup-linux.sh [--install-commands] [--all-agents]

检查 Linux 依赖并将本仓库全部 skills 同步到本机已安装的 Agent。
脚本只检测依赖，不会调用 apt、sudo 或自动安装软件。

选项：
  --install-commands  在 ~/.local/bin 创建 agent-skills-* 命令符号链接
  --all-agents        明确要求 skills CLI 同步到其支持的全部 Agent
  -h, --help          显示帮助
EOF
}

install_commands=false
all_agents=false
while (($# > 0)); do
  case $1 in
    --install-commands) install_commands=true ;;
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

info "依赖检查通过（Node.js $(node --version)，skills CLI 包：$SKILLS_CLI_PACKAGE）"

sync_args=()
if [[ $all_agents == true ]]; then
  sync_args+=(--all-agents)
fi
"$REPO_DIR/sync-skills.sh" "${sync_args[@]}"

if [[ $install_commands == true ]]; then
  bin_dir="$HOME/.local/bin"
  aliases=(
    'agent-skills-setup:setup-linux.sh'
    'agent-skills-sync:sync-skills.sh'
    'agent-skills-add:add-skill.sh'
    'agent-skills-remove:remove-skill.sh'
    'agent-rules-sync:sync-rules.sh'
  )

  mkdir -p -- "$bin_dir"

  for mapping in "${aliases[@]}"; do
    alias_name=${mapping%%:*}
    script_name=${mapping#*:}
    destination="$bin_dir/$alias_name"
    source_path="$REPO_DIR/$script_name"

    if [[ -e "$destination" || -L "$destination" ]]; then
      if [[ -L "$destination" ]] && [[ $(readlink -f -- "$destination" 2>/dev/null || true) == "$source_path" ]]; then
        continue
      fi
      die "不会覆盖已有路径：$destination"
    fi
  done

  for mapping in "${aliases[@]}"; do
    alias_name=${mapping%%:*}
    script_name=${mapping#*:}
    destination="$bin_dir/$alias_name"
    source_path="$REPO_DIR/$script_name"
    if [[ ! -L "$destination" ]]; then
      ln -s -- "$source_path" "$destination"
      info "已安装命令：$destination"
    fi
  done

  case ":$PATH:" in
    *":$bin_dir:"*) ;;
    *) warn "$bin_dir 不在 PATH 中；请把它加入 shell 的 PATH" ;;
  esac
fi

info "Linux 初始化完成"
