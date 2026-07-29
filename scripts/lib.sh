#!/usr/bin/env bash

# Shared helpers for the Linux maintenance scripts. This file is sourced, not run.

SKILLS_CLI_PACKAGE="${SKILLS_CLI_PACKAGE:-skills@1}"

info() {
  printf '[agent-skills] %s\n' "$*"
}

warn() {
  printf '[agent-skills] warning: %s\n' "$*" >&2
}

die() {
  printf '[agent-skills] error: %s\n' "$*" >&2
  exit 1
}

require_command() {
  local command_name=$1
  command -v "$command_name" >/dev/null 2>&1 || die "缺少依赖：$command_name（脚本只检测依赖，不会自动安装）"
}

require_common_dependencies() {
  local command_name
  for command_name in git node npm npx readlink; do
    require_command "$command_name"
  done
}

require_supported_node() {
  local node_major
  node_major=$(node -p 'Number(process.versions.node.split(".")[0])') || die "无法读取 Node.js 版本"
  [[ $node_major =~ ^[0-9]+$ ]] || die "无法识别 Node.js 主版本：$node_major"
  ((node_major >= 18)) || die "Node.js 版本过低：需要 18 或更高版本，当前为 $(node --version)"
}

require_repo_layout() {
  [[ -d "$REPO_DIR/.git" ]] || die "不是 Git 仓库：$REPO_DIR"
  [[ -d "$REPO_DIR/skills" ]] || die "找不到 skills 目录：$REPO_DIR/skills"
}

validate_skill_name() {
  local skill_name=$1
  [[ $skill_name =~ ^[a-z0-9][a-z0-9._-]*$ ]] ||
    die "无效的 skill 名称 '$skill_name'：仅允许小写字母、数字、点、下划线和连字符"
  [[ $skill_name != '.' && $skill_name != '..' ]] || die "无效的 skill 名称：$skill_name"
}

validate_skill_directory() {
  local skill_dir=$1
  [[ -d "$skill_dir" ]] || die "找不到 skill 目录：$skill_dir"
  [[ -f "$skill_dir/SKILL.md" ]] || die "skill 缺少 SKILL.md：$skill_dir"
}

run_skills() {
  DISABLE_TELEMETRY=1 npx -y "$SKILLS_CLI_PACKAGE" "$@"
}

has_local_agent_marker() {
  # This is deliberately a conservative preflight guard. The skills CLI remains
  # responsible for the real Agent selection; these markers only prevent its
  # `-y` zero-detection fallback from targeting every registered Agent.
  local marker
  local config_home=${XDG_CONFIG_HOME:-$HOME/.config}
  local -a markers=(
    "${CLAUDE_CONFIG_DIR:-$HOME/.claude}"
    "${CODEX_HOME:-$HOME/.codex}"
    "${AUTOHAND_HOME:-$HOME/.autohand}"
    "${GROK_HOME:-$HOME/.grok}"
    "${HERMES_HOME:-$HOME/.hermes}"
    "${VIBE_HOME:-$HOME/.vibe}"
    "$HOME/.aider-desk"
    "$config_home/amp"
    "$HOME/.gemini"
    "$HOME/.astrbot"
    "$HOME/.augment"
    "$HOME/.bob"
    "$HOME/.openclaw"
    "$HOME/.clawdbot"
    "$HOME/.moltbot"
    "$HOME/.cline"
    "$HOME/.codeartsdoer"
    "$HOME/.codebuddy"
    "$HOME/.codemaker"
    "$HOME/.codestudio"
    "$HOME/.commandcode"
    "$HOME/.continue"
    "$HOME/.snowflake/cortex"
    "$HOME/.config/crush"
    "$HOME/.cursor"
    "$HOME/.deepagents"
    "$config_home/devin"
    "$HOME/.dexto"
    "$HOME/.factory"
    "$HOME/.firebender"
    "$HOME/.forge"
    "$HOME/.copilot"
    "$config_home/goose"
    "$HOME/.inferencesh"
    "$HOME/.jazz"
    "$HOME/.junie"
    "$HOME/.iflow"
    "$HOME/.kilocode"
    "$HOME/.config/kimchi"
    "$HOME/.kimi-code"
    "$HOME/.kimi"
    "$HOME/.kiro"
    "$HOME/.kode"
    "$HOME/.lingma"
    "$HOME/.loaf"
    "$HOME/.mcpjam"
    "$HOME/.moxby"
    "$HOME/.mux"
    "$config_home/opencode"
    "$HOME/.openhands"
    "$HOME/.ona"
    "$HOME/.pi/agent"
    "$HOME/.qoder"
    "$HOME/.qoder-cn"
    "$HOME/.qwen"
    "$HOME/.reasonix"
    "$HOME/.rovodev"
    "$HOME/.roo"
    "$HOME/.tabnine"
    "$HOME/.terramind"
    "$HOME/.tinycloud"
    "$HOME/.trae"
    "$HOME/.trae-cn"
    "$HOME/.warp"
    "$HOME/.codeium/windsurf"
    "$config_home/zed"
    "$HOME/.zcode"
    "$HOME/.zencoder"
    "$HOME/.neovate"
    "$HOME/.pochi"
    "$HOME/.adal"
    "/etc/codex"
  )

  for marker in "${markers[@]}"; do
    if [[ -e "$marker" ]]; then
      return 0
    fi
  done

  return 1
}

require_detectable_agent_or_all() {
  local all_agents=$1
  if [[ $all_agents == true ]]; then
    return 0
  fi

  has_local_agent_marker || die \
    "未发现本机 Agent。为避免 skills CLI 在无人值守模式下选择全部 Agent，已中止；请先安装 Agent，或明确传入 --all-agents"
}

require_origin() {
  local origin
  origin=$(git -C "$REPO_DIR" remote get-url origin 2>/dev/null) || die "本仓库没有配置 origin 远程"
  [[ -n $origin ]] || die "本仓库的 origin 远程为空"
  printf '%s\n' "$origin"
}

ensure_push_safe() {
  local upstream ahead_count
  upstream=$(git -C "$REPO_DIR" rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null) ||
    die "当前分支没有 upstream；请先配置 upstream，或使用 --no-push 后手动处理"
  ahead_count=$(git -C "$REPO_DIR" rev-list --count "${upstream}..HEAD") || die "无法检查当前分支的推送状态"
  if ((ahead_count > 0)); then
    die "当前分支在 $upstream 之前已有 $ahead_count 个未推送提交；为避免代推旧提交，请使用 --no-push 或先手动处理"
  fi
}

agent_args() {
  local all_agents=$1
  AGENT_ARGS=()
  if [[ $all_agents == true ]]; then
    AGENT_ARGS=(-a '*')
  fi
}
