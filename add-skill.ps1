<#
.SYNOPSIS
  一条命令：把一个 skill 收进中央仓库并分发到全局。
.DESCRIPTION
  两种用法：
    1) 拉取模式（默认）：从 skills.sh 上某个来源仓库拉一个公开 skill，
       搬进本仓库 skills/，commit + push，再从本仓库重新分发到全局
       （让日后 npx skills update 从"你的"仓库拉，而不是原作者仓库）。
    2) 本地模式（-Local）：skill 已经写在本仓库 skills/<name> 下，
       跳过拉取，只做 commit + push + 分发。
.PARAMETER Skill
  skill 名称（对应 skills/<Skill> 目录）。
.PARAMETER Source
  拉取模式下的来源仓库，如 "vercel-labs/agent-skills"。本地模式可省略。
.PARAMETER Local
  skill 已在本仓库，跳过拉取步骤。
.PARAMETER NoPush
  只做本地 commit、不 push，并从本地路径分发（用于 push 前测试）。
.EXAMPLE
  .\add-skill.ps1 -Source someone/repo -Skill cool-skill
.EXAMPLE
  .\add-skill.ps1 -Skill my-new-skill -Local
.EXAMPLE
  .\add-skill.ps1 -Skill my-new-skill -Local -NoPush
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Skill,

    [string]$Source,

    [switch]$Local,

    [switch]$NoPush
)

$ErrorActionPreference = "Stop"

function Assert-LastExit([string]$Msg)
{
    if ($LASTEXITCODE -ne 0) { throw "$Msg (exit=$LASTEXITCODE)" }
}

$repo = $PSScriptRoot
$skillsDir = Join-Path $repo "skills"
$dst = Join-Path $skillsDir $Skill

# 读取本仓库的 GitHub 远程作为分发来源
$origin = (git -C $repo remote get-url origin 2>$null).Trim()
if (-not $origin) { throw "本仓库没有配置 origin 远程" }

# ---- 步骤 1：取得 skill 文件 ----
if (-not $Local)
{
    if (-not $Source) { throw "拉取模式需要 -Source；若 skill 已在本仓库请加 -Local" }
    Write-Host "[1/4] 从 $Source 拉取 $Skill ..." -ForegroundColor Cyan
    npx -y skills add $Source -g -a claude-code -s $Skill -y
    Assert-LastExit "从 $Source 拉取失败"
    $pulled = Join-Path $env:USERPROFILE ".claude\skills\$Skill"
    if (-not (Test-Path $pulled)) { throw "拉取后未找到 $pulled，检查 skill 名是否正确" }
    Copy-Item $pulled $skillsDir -Recurse -Force
    Write-Host "      已搬入 $dst"
}
else
{
    if (-not (Test-Path $dst)) { throw "本地模式下 $dst 不存在" }
    Write-Host "[1/4] 本地模式：使用已存在的 $dst" -ForegroundColor Cyan
}

# ---- 步骤 2：commit ----
Write-Host "[2/4] 提交到中央仓库 ..." -ForegroundColor Cyan
git -C $repo add "skills/$Skill"
$staged = git -C $repo diff --cached --name-only
if (-not $staged)
{
    Write-Host "      无变更（skill 内容与仓库一致），跳过提交" -ForegroundColor Yellow
}
else
{
    git -C $repo commit -m "feat: 收录/更新 skill $Skill"
    Assert-LastExit "commit 失败"
}

# ---- 步骤 3：push ----
if ($NoPush)
{
    Write-Host "[3/4] -NoPush：跳过 push" -ForegroundColor Yellow
}
else
{
    Write-Host "[3/4] 推送到远程 ..." -ForegroundColor Cyan
    git -C $repo push
    Assert-LastExit "push 失败"
}

# ---- 步骤 4：分发到全局 ----
# NoPush 时远程还没这个 skill，从本地路径分发；否则从 GitHub 远程分发
if ($NoPush)
{
    Write-Host "[4/4] 从本地仓库分发到全局（未 push）..." -ForegroundColor Cyan
    npx -y skills add $repo -g -a claude-code -s $Skill -y
}
else
{
    Write-Host "[4/4] 从 $origin 分发到全局 ..." -ForegroundColor Cyan
    npx -y skills add $origin -g -a claude-code -s $Skill -y
}
Assert-LastExit "分发到全局失败"

Write-Host "完成：$Skill 已入库并分发到全局。" -ForegroundColor Green
