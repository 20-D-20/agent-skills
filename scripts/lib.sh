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
  # skills CLI 在自己的 TUI 里执行 git clone，交互式认证提示（SSH key passphrase、
  # 未知 host key、HTTPS 用户名密码）会被 TUI 吞掉，进程就永久挂在 "Cloning
  # repository…"。BatchMode 让认证失败立即报错退出，而不是等待一个看不见的输入。
  DISABLE_TELEMETRY=1 \
    GIT_SSH_COMMAND="${GIT_SSH_COMMAND:-ssh -o BatchMode=yes}" \
    GIT_TERMINAL_PROMPT=0 \
    npx -y "$SKILLS_CLI_PACKAGE" "$@"
}

# 分发只是一次只读 clone，不需要写权限。把 GitHub 的 SSH origin 转成
# owner/repo 简写后走 HTTPS，可以完全绕开 SSH 认证；非 GitHub 或无法识别的
# 地址原样返回。设置 AGENT_SKILLS_KEEP_ORIGIN_URL=1 可强制使用原始 origin
# （例如私有仓库必须走 SSH 的场景）。
distribution_source_from_origin() {
  local origin=$1 path

  if [[ ${AGENT_SKILLS_KEEP_ORIGIN_URL:-0} == 1 ]]; then
    printf '%s\n' "$origin"
    return 0
  fi

  case $origin in
    git@github.com:*) path=${origin#git@github.com:} ;;
    ssh://git@github.com/*) path=${origin#ssh://git@github.com/} ;;
    https://github.com/*) path=${origin#https://github.com/} ;;
    http://github.com/*) path=${origin#http://github.com/} ;;
    *)
      printf '%s\n' "$origin"
      return 0
      ;;
  esac

  path=${path%.git}
  path=${path%/}
  if [[ $path =~ ^[^/]+/[^/]+$ ]]; then
    printf '%s\n' "$path"
  else
    printf '%s\n' "$origin"
  fi
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

has_antigravity_install() {
  local gemini_home=$HOME/.gemini
  local marker
  local -a markers=(
    "$gemini_home/antigravity"
    "$gemini_home/antigravity-ide"
    "$gemini_home/antigravity-cli"
  )

  for marker in "${markers[@]}"; do
    if [[ -e "$marker" ]]; then
      return 0
    fi
  done

  return 1
}

sync_antigravity_global_skills() {
  # Antigravity IDE currently discovers global skills from
  # ~/.gemini/config/skills. skills@1 treats Antigravity as a universal Agent
  # and only writes global installs to ~/.agents/skills, so mirror this repo to
  # Antigravity's documented directory until the CLI behavior is aligned.
  has_antigravity_install || return 0

  local requested_skill=${1:-}
  local skill_dir skill_name target_dir source_real target_real
  local target_root=${ANTIGRAVITY_SKILLS_DIR:-$HOME/.gemini/config/skills}
  local synced_count=0
  local -a skill_dirs

  if [[ -n $requested_skill ]]; then
    validate_skill_name "$requested_skill"
    skill_dirs=("$REPO_DIR/skills/$requested_skill")
  else
    skill_dirs=("$REPO_DIR"/skills/*)
  fi

  mkdir -p -- "$target_root"

  for skill_dir in "${skill_dirs[@]}"; do
    [[ -d "$skill_dir" && -f "$skill_dir/SKILL.md" ]] || continue
    skill_name=${skill_dir##*/}
    validate_skill_name "$skill_name"
    target_dir="$target_root/$skill_name"

    if [[ -e "$target_dir" || -L "$target_dir" ]]; then
      [[ -d "$target_dir" ]] || die "Antigravity skill 目标不是目录：$target_dir"
      source_real=$(readlink -f -- "$skill_dir")
      target_real=$(readlink -f -- "$target_dir")
      if [[ $source_real == "$target_real" ]]; then
        ((synced_count += 1))
        continue
      fi
    else
      mkdir -p -- "$target_dir"
    fi

    cp -a -- "$skill_dir/." "$target_dir/"
    ((synced_count += 1))
  done

  info "已同步 $synced_count 个 skills 到 Antigravity 全局目录：$target_root"
}

remove_antigravity_global_skill() {
  local skill_name=$1
  local target_root=${ANTIGRAVITY_SKILLS_DIR:-$HOME/.gemini/config/skills}
  local target_dir

  validate_skill_name "$skill_name"
  target_dir="$target_root/$skill_name"
  if [[ -e "$target_dir" || -L "$target_dir" ]]; then
    rm -rf -- "$target_dir"
    info "已从 Antigravity 全局目录卸载 $skill_name"
  fi
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
