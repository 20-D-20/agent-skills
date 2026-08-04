<#
.SYNOPSIS
  将中央仓库 rules/ 分发到一个或多个项目的 .claude/rules 与 .agent/rules。
.EXAMPLE
  .\sync-rules.ps1 -ProjectPath "D:\project-a", "D:\project-b"
.NOTES
  同名文件会覆盖，目标目录中的额外文件不会删除；同步后请在目标项目提交。
#>
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string[]]$ProjectPath
)

$ErrorActionPreference = "Stop"
$source = Join-Path $PSScriptRoot "rules"

if (-not (Test-Path $source)) { throw "找不到源目录：$source" }

foreach ($project in $ProjectPath) {
    if (-not (Test-Path $project -PathType Container)) { throw "目标项目不存在：$project" }
    foreach ($subdirectory in ".claude\rules", ".agent\rules") {
        $destination = Join-Path $project $subdirectory
        if (-not (Test-Path $destination)) {
            New-Item -ItemType Directory -Force -Path $destination | Out-Null
        }
        Copy-Item (Join-Path $source "*") $destination -Force
        Write-Host "已同步 -> $destination"
    }
}

Write-Host "完成。请到目标项目检查并提交变更。"
