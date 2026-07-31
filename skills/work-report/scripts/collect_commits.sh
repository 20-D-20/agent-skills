#!/usr/bin/env bash
# 采集指定日期区间内多个仓库的提交事实，供日报/周报生成使用。
# 输出纯文本，包含 subject、body 与改动文件统计；不读完整 diff。
set -uo pipefail

usage() {
  cat <<'EOF'
用法:
  collect_commits.sh --since <YYYY-MM-DD> --until <YYYY-MM-DD> [选项] <仓库路径>...

选项:
  --since <日期>    起始日（闭区间）
  --until <日期>    结束日（闭区间），缺省同 --since
  --author <正则>   按作者过滤，缺省不过滤
  --all             扫描所有分支，缺省只扫当前 HEAD
  --wip             额外输出未提交改动摘要
  -h, --help        显示本帮助

示例:
  collect_commits.sh --since 2026-07-30 --until 2026-07-30 ~/Qt_Project
  collect_commits.sh --since 2026-07-27 --until 2026-07-31 --wip ~/Qt_Project ~/MinHope_Project/MH3500c
EOF
}

SINCE=""
UNTIL=""
AUTHOR=""
SCAN_ALL=0
WITH_WIP=0
REPOS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --since)  SINCE="${2:-}"; shift 2 ;;
    --until)  UNTIL="${2:-}"; shift 2 ;;
    --author) AUTHOR="${2:-}"; shift 2 ;;
    --all)    SCAN_ALL=1; shift ;;
    --wip)    WITH_WIP=1; shift ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "未知选项: $1" >&2; usage >&2; exit 2 ;;
    *)  REPOS+=("$1"); shift ;;
  esac
done

[[ -z "$SINCE" ]] && { echo "错误: 缺少 --since" >&2; usage >&2; exit 2; }
[[ -z "$UNTIL" ]] && UNTIL="$SINCE"
[[ ${#REPOS[@]} -eq 0 ]] && { echo "错误: 未指定仓库路径" >&2; usage >&2; exit 2; }

for d in "$SINCE" "$UNTIL"; do
  [[ "$d" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]] || { echo "错误: 日期格式应为 YYYY-MM-DD，收到 '$d'" >&2; exit 2; }
done

printf '# 提交采集 %s ~ %s\n\n' "$SINCE" "$UNTIL"

total=0
for repo in "${REPOS[@]}"; do
  if ! git -C "$repo" rev-parse --git-dir >/dev/null 2>&1; then
    printf '## %s\n跳过: 不是 git 仓库或路径不存在\n\n' "$repo"
    continue
  fi

  top=$(git -C "$repo" rev-parse --show-toplevel)
  branch=$(git -C "$repo" symbolic-ref --quiet --short HEAD 2>/dev/null \
    || git -C "$repo" rev-parse --short HEAD 2>/dev/null \
    || echo '未知')
  printf '## %s\n路径: %s\n分支: %s\n\n' "$(basename "$top")" "$top" "$branch"

  # 空仓库（无任何提交）直接跳过，避免 git log 报错
  if ! git -C "$repo" rev-parse --verify --quiet HEAD >/dev/null 2>&1; then
    printf '仓库尚无提交，跳过。\n\n'
    continue
  fi

  args=(--no-merges --since="$SINCE 00:00:00" --until="$UNTIL 23:59:59")
  [[ -n "$AUTHOR" ]] && args+=(--author="$AUTHOR")
  [[ $SCAN_ALL -eq 1 ]] && args+=(--all)

  mapfile -t hashes < <(git -C "$repo" log "${args[@]}" --format=%H)

  if [[ ${#hashes[@]} -eq 0 ]]; then
    printf '区间内无提交。\n\n'
  else
    printf '提交数: %d\n\n' "${#hashes[@]}"
    total=$(( total + ${#hashes[@]} ))
    for h in "${hashes[@]}"; do
      git -C "$repo" show -s --date=short --format='--- %h %ad %an%n主题: %s%n%n%b' "$h"
      printf '改动文件:\n'
      git -C "$repo" show --stat --format='' "$h" | sed '/^[[:space:]]*$/d'
      printf '\n'
    done
  fi

  if [[ $WITH_WIP -eq 1 ]]; then
    wip=$(git -C "$repo" status --short 2>/dev/null | head -50)
    if [[ -n "$wip" ]]; then
      printf '未提交改动（进行中，未必属于本区间）:\n%s\n\n' "$wip"
    else
      printf '未提交改动: 无\n\n'
    fi
  fi
done

printf '# 合计提交数: %d\n' "$total"

# 提示凌晨提交的归属问题：跨零点的工作可能应计入前一日
if [[ "$SINCE" == "$UNTIL" ]]; then
  printf '\n提示: 如当日 00:00-05:00 存在提交，确认是否应归入前一日的日报。\n'
fi
