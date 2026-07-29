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
  ./remove-skill.sh <名称>
  ./remove-skill.sh <名称> --from-repo [--no-push] [--yes]

默认只从本机全局 Agent 目录卸载指定 skill。
--from-repo 会同时删除中央仓库源文件、创建提交并默认推送。

选项：
  --from-repo  同时从中央仓库删除
  --no-push    创建本地删除提交但不推送（仅与 --from-repo 一起使用）
  -y, --yes    跳过中央仓库删除确认
  -h, --help   显示帮助
EOF
}

skill=''
from_repo=false
no_push=false
assume_yes=false
while (($# > 0)); do
  case $1 in
    --from-repo) from_repo=true ;;
    --no-push) no_push=true ;;
    -y|--yes) assume_yes=true ;;
    -h|--help) usage; exit 0 ;;
    --*) die "未知参数：$1" ;;
    *)
      [[ -z $skill ]] || die "只能指定一个 skill 名称"
      skill=$1
      ;;
  esac
  shift
done

[[ -n $skill ]] || die "必须指定 skill 名称"
validate_skill_name "$skill"
if [[ $from_repo == false && $no_push == true ]]; then
  die "--no-push 只能与 --from-repo 一起使用"
fi

require_common_dependencies
require_supported_node
require_repo_layout

if [[ $from_repo == true ]]; then
  destination="$REPO_DIR/skills/$skill"
  relative_path="skills/$skill"
  validate_skill_directory "$destination"
  [[ -n $(git -C "$REPO_DIR" ls-files -- "$relative_path") ]] ||
    die "$relative_path 未被 Git 跟踪；为保证可恢复性，拒绝自动删除"

  if [[ $assume_yes == false ]]; then
    [[ -t 0 ]] || die "非交互环境中删除中央仓库内容必须传入 --yes"
    printf "将删除 %s 并创建 Git 提交。输入 skill 名称确认：" "$relative_path" >&2
    read -r confirmation
    [[ $confirmation == "$skill" ]] || die "确认不匹配，已取消"
  fi

  if [[ $no_push == false ]]; then
    ensure_push_safe
  fi

  rm -rf -- "$destination"
  git -C "$REPO_DIR" add -A -- "$relative_path"
  git -C "$REPO_DIR" commit --only -m "chore: 移除 skill $skill" -- "$relative_path"
  info "已从仓库删除 $relative_path；文件仍可从 Git 历史恢复"

  if [[ $no_push == true ]]; then
    info "--no-push：跳过推送"
  else
    git -C "$REPO_DIR" push
    info "删除提交已推送"
  fi
fi

info "从本机全局 Agent 目录卸载 $skill"
run_skills remove "$skill" -g -a '*' -y
info "$skill 已从本机卸载"
