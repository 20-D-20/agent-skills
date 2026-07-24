<#
.SYNOPSIS
  将中央仓库 rules/ 分发到目标项目的 .claude/rules 与 .agent/rules。
.EXAMPLE
  .\sync-rules.ps1 -ProjectPath "D:\MinHope_GitLab\MH3500C_NEW\mh3500c_new"
.NOTES
  rules 是项目规范，走复制（不做全局、不做 symlink），复制后需在目标项目 git 提交。
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectPath
)

$ErrorActionPreference = "Stop"
$src = Join-Path $PSScriptRoot "rules"

if (-not (Test-Path $src)) { throw "找不到源目录: $src" }
if (-not (Test-Path $ProjectPath)) { throw "目标项目不存在: $ProjectPath" }

foreach ($sub in ".claude\rules", ".agent\rules") {
    $dst = Join-Path $ProjectPath $sub
    if (-not (Test-Path $dst)) { New-Item -ItemType Directory -Force -Path $dst | Out-Null }
    Copy-Item "$src\*" $dst -Force
    Write-Host "已同步 -> $dst"
}

Write-Host "完成。请到目标项目 git 提交变更。"
