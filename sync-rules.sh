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
RULES_DIR="$REPO_DIR/rules"

usage() {
  cat <<'EOF'
用法：./sync-rules.sh <项目路径> [更多项目路径 ...]

将 rules/ 复制到每个项目的 .claude/rules 与 .agent/rules。
同名文件会覆盖，目标中额外存在的文件不会删除。
EOF
}

if (($# == 0)); then
  usage >&2
  exit 1
fi
if [[ ${1:-} == '-h' || ${1:-} == '--help' ]]; then
  usage
  exit 0
fi

[[ -d "$RULES_DIR" ]] || die "找不到源目录：$RULES_DIR"

for project_path in "$@"; do
  [[ -d "$project_path" ]] || die "目标项目不存在：$project_path"
  for relative_destination in '.claude/rules' '.agent/rules'; do
    destination="$project_path/$relative_destination"
    mkdir -p -- "$destination"
    cp -a -- "$RULES_DIR/." "$destination/"
    info "已同步 -> $destination"
  done
done

info "rules 同步完成；请在目标项目中检查并提交变更"
