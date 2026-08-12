<#
.SYNOPSIS
  从本机全局 Agent 目录卸载一个 skill，可选同时从中央仓库删除。
.DESCRIPTION
  默认只从本机已安装的 Agent 卸载指定 skill，不改动中央仓库。加上
  -FromRepo 后，会先删除仓库内 skills/<name>、创建独立提交并默认 push，
  再执行本机卸载。
.PARAMETER Skill
  skill 名称（对应 skills/<Skill> 目录）。
.PARAMETER FromRepo
  同时从中央仓库删除。
.PARAMETER NoPush
  只与 -FromRepo 一起使用：创建本地删除提交但不 push。
.PARAMETER Yes
  跳过中央仓库删除确认。
.EXAMPLE
  .\remove-skill.ps1 -Skill cool-skill
.EXAMPLE
  .\remove-skill.ps1 -Skill cool-skill -FromRepo
.EXAMPLE
  .\remove-skill.ps1 -Skill cool-skill -FromRepo -NoPush -Yes
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Skill,

    [switch]$FromRepo,

    [switch]$NoPush,

    [switch]$Yes
)

$ErrorActionPreference = "Stop"
$env:DISABLE_TELEMETRY = "1"
$SkillsCliPackage = if ($env:SKILLS_CLI_PACKAGE) { $env:SKILLS_CLI_PACKAGE } else { "skills@1" }

function Assert-LastExit([string]$Message)
{
    if ($LASTEXITCODE -ne 0) { throw "$Message (exit=$LASTEXITCODE)" }
}

function Assert-Command([string]$Name)
{
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "缺少依赖：$Name（脚本只检测依赖，不会自动安装）"
    }
}

function Invoke-Skills
{
    param(
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$CliArguments
    )

    # skills CLI 在自己的 TUI 里执行 git clone，交互式认证提示（SSH key passphrase、
    # 未知 host key、HTTPS 用户名密码）会被 TUI 吞掉，进程就永久挂在 "Cloning
    # repository…"。BatchMode 让认证失败立即报错退出，而不是等待一个看不见的输入。
    $previousSshCommand = $env:GIT_SSH_COMMAND
    $previousTerminalPrompt = $env:GIT_TERMINAL_PROMPT
    if (-not $previousSshCommand) { $env:GIT_SSH_COMMAND = "ssh -o BatchMode=yes" }
    $env:GIT_TERMINAL_PROMPT = "0"
    try {
        & npx -y $SkillsCliPackage @CliArguments
        Assert-LastExit "skills CLI 执行失败"
    }
    finally {
        $env:GIT_SSH_COMMAND = $previousSshCommand
        $env:GIT_TERMINAL_PROMPT = $previousTerminalPrompt
    }
}

function Assert-PushSafe([string]$Repository)
{
    $upstreamOutput = & git -C $Repository rev-parse --abbrev-ref --symbolic-full-name "@{upstream}" 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $upstreamOutput) {
        throw "当前分支没有 upstream；请先配置 upstream，或使用 -NoPush 后手动处理"
    }
    $upstream = ([string]$upstreamOutput).Trim()

    $aheadOutput = & git -C $Repository rev-list --count "$upstream..HEAD"
    Assert-LastExit "无法检查当前分支的推送状态"
    $ahead = 0
    if (-not [int]::TryParse(([string]$aheadOutput).Trim(), [ref]$ahead)) {
        throw "无法识别未推送提交数量：$aheadOutput"
    }
    if ([int]$ahead -gt 0) {
        throw "当前分支在 $upstream 之前已有 $ahead 个未推送提交；为避免代推旧提交，请使用 -NoPush 或先手动处理"
    }
}

foreach ($dependency in @("git", "node", "npm", "npx")) { Assert-Command $dependency }

$nodeMajorOutput = & node -p 'parseInt(process.versions.node)'
Assert-LastExit "无法读取 Node.js 版本"
$nodeMajor = 0
if (-not [int]::TryParse(([string]$nodeMajorOutput).Trim(), [ref]$nodeMajor)) {
    throw "无法识别 Node.js 主版本：$nodeMajorOutput"
}
if ($nodeMajor -lt 18) { throw "Node.js 版本过低：需要 18 或更高版本" }

if ($Skill -notmatch '^[a-z0-9][a-z0-9._-]*$' -or $Skill -in @('.', '..')) {
    throw "无效的 skill 名称 '$Skill'：仅允许小写字母、数字、点、下划线和连字符"
}
if (-not $FromRepo -and $NoPush) { throw "-NoPush 只能与 -FromRepo 一起使用" }

$repo = $PSScriptRoot
$skillsDir = Join-Path $repo "skills"
if (-not (Test-Path (Join-Path $repo ".git"))) { throw "不是 Git 仓库：$repo" }
if (-not (Test-Path $skillsDir)) { throw "找不到 skills 目录：$skillsDir" }

if ($FromRepo)
{
    $destination = Join-Path $skillsDir $Skill
    $relativePath = "skills/$Skill"

    if (-not (Test-Path $destination -PathType Container)) { throw "找不到 skill 目录：$destination" }
    if (-not (Test-Path (Join-Path $destination "SKILL.md"))) { throw "skill 缺少 SKILL.md：$destination" }

    $trackedOutput = & git -C $repo ls-files -- $relativePath
    if (-not $trackedOutput) { throw "$relativePath 未被 Git 跟踪；为保证可恢复性，拒绝自动删除" }

    if (-not $Yes)
    {
        if (-not [Environment]::UserInteractive) {
            throw "非交互环境中删除中央仓库内容必须传入 -Yes"
        }
        $confirmation = Read-Host "将删除 $relativePath 并创建 Git 提交。输入 skill 名称确认"
        if ($confirmation -ne $Skill) { throw "确认不匹配，已取消" }
    }

    if (-not $NoPush) { Assert-PushSafe $repo }

    Write-Host "[1/2] 删除 $relativePath 并创建提交 ..." -ForegroundColor Cyan
    Remove-Item $destination -Recurse -Force
    & git -C $repo add -A -- $relativePath
    Assert-LastExit "git add 失败"
    & git -C $repo commit --only -m "chore: 移除 skill $Skill" -- $relativePath
    Assert-LastExit "commit 失败"
    Write-Host "      已从仓库删除 $relativePath；文件仍可从 Git 历史恢复"

    if ($NoPush)
    {
        Write-Host "[2/2] -NoPush：跳过 push" -ForegroundColor Yellow
    }
    else
    {
        Write-Host "[2/2] 推送删除提交 ..." -ForegroundColor Cyan
        & git -C $repo push
        Assert-LastExit "push 失败"
        Write-Host "      删除提交已推送"
    }
}

Write-Host "从本机全局 Agent 目录卸载 $Skill ..." -ForegroundColor Cyan
Invoke-Skills remove $Skill -g -a '*' -y
Write-Host "完成：$Skill 已从本机卸载。" -ForegroundColor Green
