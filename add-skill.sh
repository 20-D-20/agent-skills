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
用法：
  ./add-skill.sh --source <仓库或路径> --skill <名称> [--no-push] [--all-agents]
  ./add-skill.sh --local --skill <名称> [--no-push] [--all-agents]

把一个 skill 收进中央仓库，独立提交并分发到本机已安装的 Agent。

选项：
  -s, --skill NAME    skill 名称，对应 skills/NAME
  -r, --source SRC    外部 skills 仓库或本地来源
  -l, --local         使用仓库中已经存在的 skill
  --no-push           创建本地提交但不推送，并从本地仓库分发
  --all-agents        明确分发到 skills CLI 支持的全部 Agent
  -h, --help          显示帮助
EOF
}

skill=''
source_repo=''
local_mode=false
no_push=false
all_agents=false
while (($# > 0)); do
  case $1 in
    -s|--skill)
      (($# >= 2)) || die "$1 需要一个值"
      skill=$2
      shift
      ;;
    -r|--source)
      (($# >= 2)) || die "$1 需要一个值"
      source_repo=$2
      shift
      ;;
    -l|--local) local_mode=true ;;
    --no-push) no_push=true ;;
    --all-agents) all_agents=true ;;
    -h|--help) usage; exit 0 ;;
    *) die "未知参数：$1" ;;
  esac
  shift
done

[[ -n $skill ]] || die "必须通过 --skill 指定 skill 名称"
validate_skill_name "$skill"
if [[ $local_mode == true && -n $source_repo ]]; then
  die "--local 与 --source 不能同时使用"
fi
if [[ $local_mode == false && -z $source_repo ]]; then
  die "外部拉取模式需要 --source；若 skill 已在本仓库，请使用 --local"
fi

require_common_dependencies
require_supported_node
require_repo_layout
require_detectable_agent_or_all "$all_agents"
origin=$(require_origin)
if [[ $no_push == false ]]; then
  ensure_push_safe
fi

destination="$REPO_DIR/skills/$skill"
relative_path="skills/$skill"

if [[ $local_mode == true ]]; then
  validate_skill_directory "$destination"
  info "使用本仓库中的 $relative_path"
else
  temp_dir=$(mktemp -d)
  cleanup() {
    rm -rf -- "$temp_dir"
  }
  trap cleanup EXIT

  info "从 $source_repo 获取 $skill（先隔离导入，不写入全局 Agent 目录）"
  (
    cd -- "$temp_dir"
    run_skills add "$source_repo" -s "$skill" -a universal -y
  )
  imported="$temp_dir/.agents/skills/$skill"
  validate_skill_directory "$imported"

  rm -rf -- "$destination"
  cp -a -- "$imported" "$destination"
  validate_skill_directory "$destination"
  info "已收录到 $relative_path"
fi

git -C "$REPO_DIR" add -A -- "$relative_path"
commit_created=false
if git -C "$REPO_DIR" diff --cached --quiet -- "$relative_path"; then
  info "skill 内容无变化，跳过提交"
else
  git -C "$REPO_DIR" commit --only -m "feat: 收录/更新 skill $skill" -- "$relative_path"
  commit_created=true
  info "已创建仅包含 $relative_path 的提交"
fi

if [[ $no_push == true ]]; then
  info "--no-push：跳过推送"
elif [[ $commit_created == true ]]; then
  git -C "$REPO_DIR" push
  info "提交已推送"
else
  info "没有新提交，跳过推送"
fi

agent_args "$all_agents"
if [[ $no_push == true ]]; then
  distribution_source=$REPO_DIR
  info "从本地仓库分发 $skill"
else
  distribution_source=$(distribution_source_from_origin "$origin")
  info "从 $distribution_source 分发 $skill"
fi
run_skills add "$distribution_source" -g -s "$skill" "${AGENT_ARGS[@]}" -y
info "$skill 已入库并分发"
