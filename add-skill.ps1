<#
.SYNOPSIS
  把一个 skill 收进中央仓库并分发到本机已安装的 Agent。
.DESCRIPTION
  拉取模式会先把外部 skill 导入临时隔离目录，再复制到本仓库；本地模式
  使用 skills/<name> 中的现有内容。随后脚本只提交目标 skill，默认 push，
  最后交给 skills CLI 自动识别本机 Agent 并全局分发。
.PARAMETER Skill
  skill 名称（对应 skills/<Skill> 目录）。
.PARAMETER Source
  拉取模式下的来源仓库或本地路径。本地模式可省略。
.PARAMETER Local
  skill 已在本仓库，跳过拉取步骤。
.PARAMETER NoPush
  创建本地提交但不 push，并从本地路径分发。
.PARAMETER AllAgents
  明确要求 skills CLI 分发到其支持的全部 Agent。
.EXAMPLE
  .\add-skill.ps1 -Source someone/repo -Skill cool-skill
.EXAMPLE
  .\add-skill.ps1 -Skill my-new-skill -Local -NoPush
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Skill,

    [string]$Source,

    [switch]$Local,

    [switch]$NoPush,

    [switch]$AllAgents
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

# 分发只是一次只读 clone，不需要写权限。把 GitHub 的 SSH origin 转成
# owner/repo 简写后走 HTTPS，可以完全绕开 SSH 认证；非 GitHub 或无法识别的
# 地址原样返回。设置 AGENT_SKILLS_KEEP_ORIGIN_URL=1 可强制使用原始 origin
# （例如私有仓库必须走 SSH 的场景）。
function Convert-DistributionSource([string]$Origin)
{
    if ($env:AGENT_SKILLS_KEEP_ORIGIN_URL -eq "1") { return $Origin }

    $path = $null
    if ($Origin -match '^git@github\.com:(.+)$') { $path = $Matches[1] }
    elseif ($Origin -match '^ssh://git@github\.com/(.+)$') { $path = $Matches[1] }
    elseif ($Origin -match '^https?://github\.com/(.+)$') { $path = $Matches[1] }
    else { return $Origin }

    $path = ($path -replace '\.git$', '').TrimEnd('/')
    if ($path -match '^[^/]+/[^/]+$') { return $path }
    return $Origin
}

function Test-LocalAgent
{
    $userProfile = [Environment]::GetFolderPath("UserProfile")
    $configHome = if ($env:XDG_CONFIG_HOME) { $env:XDG_CONFIG_HOME } else { Join-Path $userProfile ".config" }
    $claudeHome = if ($env:CLAUDE_CONFIG_DIR) { $env:CLAUDE_CONFIG_DIR } else { Join-Path $userProfile ".claude" }
    $codexHome = if ($env:CODEX_HOME) { $env:CODEX_HOME } else { Join-Path $userProfile ".codex" }
    $autohandHome = if ($env:AUTOHAND_HOME) { $env:AUTOHAND_HOME } else { Join-Path $userProfile ".autohand" }
    $grokHome = if ($env:GROK_HOME) { $env:GROK_HOME } else { Join-Path $userProfile ".grok" }
    $hermesHome = if ($env:HERMES_HOME) { $env:HERMES_HOME } else { Join-Path $userProfile ".hermes" }
    $vibeHome = if ($env:VIBE_HOME) { $env:VIBE_HOME } else { Join-Path $userProfile ".vibe" }

    $markers = @(
        $claudeHome,
        $codexHome,
        $autohandHome,
        $grokHome,
        $hermesHome,
        $vibeHome,
        (Join-Path $userProfile ".aider-desk"),
        (Join-Path $configHome "amp"),
        (Join-Path $userProfile ".gemini"),
        (Join-Path $userProfile ".astrbot"),
        (Join-Path $userProfile ".augment"),
        (Join-Path $userProfile ".bob"),
        (Join-Path $userProfile ".openclaw"),
        (Join-Path $userProfile ".clawdbot"),
        (Join-Path $userProfile ".moltbot"),
        (Join-Path $userProfile ".cline"),
        (Join-Path $userProfile ".codeartsdoer"),
        (Join-Path $userProfile ".codebuddy"),
        (Join-Path $userProfile ".codemaker"),
        (Join-Path $userProfile ".codestudio"),
        (Join-Path $userProfile ".commandcode"),
        (Join-Path $userProfile ".continue"),
        (Join-Path $userProfile ".snowflake\cortex"),
        (Join-Path $userProfile ".config\crush"),
        (Join-Path $userProfile ".cursor"),
        (Join-Path $userProfile ".deepagents"),
        (Join-Path $configHome "devin"),
        (Join-Path $userProfile ".dexto"),
        (Join-Path $userProfile ".factory"),
        (Join-Path $userProfile ".firebender"),
        (Join-Path $userProfile ".forge"),
        (Join-Path $userProfile ".copilot"),
        (Join-Path $configHome "goose"),
        (Join-Path $userProfile ".inferencesh"),
        (Join-Path $userProfile ".jazz"),
        (Join-Path $userProfile ".junie"),
        (Join-Path $userProfile ".iflow"),
        (Join-Path $userProfile ".kilocode"),
        (Join-Path $userProfile ".config\kimchi"),
        (Join-Path $userProfile ".kimi-code"),
        (Join-Path $userProfile ".kimi"),
        (Join-Path $userProfile ".kiro"),
        (Join-Path $userProfile ".kode"),
        (Join-Path $userProfile ".lingma"),
        (Join-Path $userProfile ".loaf"),
        (Join-Path $userProfile ".mcpjam"),
        (Join-Path $userProfile ".moxby"),
        (Join-Path $userProfile ".mux"),
        (Join-Path $userProfile ".openhands"),
        (Join-Path $userProfile ".ona"),
        (Join-Path $userProfile ".pi\agent"),
        (Join-Path $userProfile ".qoder"),
        (Join-Path $userProfile ".qoder-cn"),
        (Join-Path $userProfile ".qwen"),
        (Join-Path $userProfile ".reasonix"),
        (Join-Path $userProfile ".rovodev"),
        (Join-Path $userProfile ".roo"),
        (Join-Path $userProfile ".tabnine"),
        (Join-Path $userProfile ".terramind"),
        (Join-Path $userProfile ".tinycloud"),
        (Join-Path $userProfile ".trae"),
        (Join-Path $userProfile ".trae-cn"),
        (Join-Path $userProfile ".warp"),
        (Join-Path $userProfile ".codeium\windsurf"),
        (Join-Path $configHome "opencode"),
        (Join-Path $configHome "zed"),
        (Join-Path $userProfile ".zcode"),
        (Join-Path $userProfile ".zencoder"),
        (Join-Path $userProfile ".neovate"),
        (Join-Path $userProfile ".pochi"),
        (Join-Path $userProfile ".adal")
    )
    foreach ($marker in $markers) {
        if (Test-Path $marker) { return $true }
    }
    return $false
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

$nodeMajorOutput = & node -p 'Number(process.versions.node.split(".")[0])'
Assert-LastExit "无法读取 Node.js 版本"
$nodeMajor = 0
if (-not [int]::TryParse(([string]$nodeMajorOutput).Trim(), [ref]$nodeMajor)) {
    throw "无法识别 Node.js 主版本：$nodeMajorOutput"
}
if ($nodeMajor -lt 18) { throw "Node.js 版本过低：需要 18 或更高版本" }

if ($Skill -notmatch '^[a-z0-9][a-z0-9._-]*$' -or $Skill -in @('.', '..')) {
    throw "无效的 skill 名称 '$Skill'：仅允许小写字母、数字、点、下划线和连字符"
}
if ($Local -and $Source) { throw "-Local 与 -Source 不能同时使用" }
if (-not $Local -and -not $Source) { throw "拉取模式需要 -Source；若 skill 已在本仓库请加 -Local" }
if (-not $AllAgents -and -not (Test-LocalAgent)) {
    throw "未发现本机 Agent。为避免 skills CLI 在无人值守模式下选择全部 Agent，已中止；请先安装 Agent，或明确传入 -AllAgents"
}

$repo = $PSScriptRoot
$skillsDir = Join-Path $repo "skills"
$destination = Join-Path $skillsDir $Skill
$relativePath = "skills/$Skill"
if (-not (Test-Path (Join-Path $repo ".git"))) { throw "不是 Git 仓库：$repo" }
if (-not (Test-Path $skillsDir)) { throw "找不到 skills 目录：$skillsDir" }

$originOutput = & git -C $repo remote get-url origin 2>$null
if ($LASTEXITCODE -ne 0 -or -not $originOutput) { throw "本仓库没有配置 origin 远程" }
$origin = ([string]$originOutput).Trim()
if (-not $NoPush) { Assert-PushSafe $repo }

$agentArgs = @()
if ($AllAgents) { $agentArgs = @("-a", "*") }

if ($Local)
{
    if (-not (Test-Path (Join-Path $destination "SKILL.md"))) {
        throw "本地 skill 不存在或缺少 SKILL.md：$destination"
    }
    Write-Host "[1/4] 使用本仓库中的 $relativePath" -ForegroundColor Cyan
}
else
{
    Write-Host "[1/4] 从 $Source 隔离导入 $Skill ..." -ForegroundColor Cyan
    $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("agent-skills-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $tempRoot | Out-Null
    try {
        Push-Location $tempRoot
        try {
            Invoke-Skills add $Source -s $Skill -a universal -y
        }
        finally {
            Pop-Location
        }

        $imported = Join-Path $tempRoot ".agents\skills\$Skill"
        if (-not (Test-Path (Join-Path $imported "SKILL.md"))) {
            throw "隔离导入后未找到有效 skill：$imported"
        }
        if (Test-Path $destination) { Remove-Item $destination -Recurse -Force }
        Copy-Item $imported $destination -Recurse -Force
    }
    finally {
        if (Test-Path $tempRoot) { Remove-Item $tempRoot -Recurse -Force }
    }
    Write-Host "      已收录到 $relativePath"
}

Write-Host "[2/4] 创建目标路径的独立提交 ..." -ForegroundColor Cyan
& git -C $repo add -A -- $relativePath
Assert-LastExit "git add 失败"
& git -C $repo diff --cached --quiet -- $relativePath
$diffExit = $LASTEXITCODE
$commitCreated = $false
if ($diffExit -eq 0)
{
    Write-Host "      skill 内容无变化，跳过提交" -ForegroundColor Yellow
}
elseif ($diffExit -eq 1)
{
    & git -C $repo commit --only -m "feat: 收录/更新 skill $Skill" -- $relativePath
    Assert-LastExit "commit 失败"
    $commitCreated = $true
}
else
{
    throw "无法检查暂存变更 (exit=$diffExit)"
}

if ($NoPush)
{
    Write-Host "[3/4] -NoPush：跳过 push" -ForegroundColor Yellow
}
elseif ($commitCreated)
{
    Write-Host "[3/4] 推送新提交 ..." -ForegroundColor Cyan
    & git -C $repo push
    Assert-LastExit "push 失败"
}
else
{
    Write-Host "[3/4] 没有新提交，跳过 push" -ForegroundColor Yellow
}

$distributionSource = if ($NoPush) { $repo } else { Convert-DistributionSource $origin }
Write-Host "[4/4] 从 $distributionSource 分发到本机 Agent ..." -ForegroundColor Cyan
$distributionArgs = @("add", $distributionSource, "-g", "-s", $Skill) + $agentArgs + @("-y")
Invoke-Skills @distributionArgs

Write-Host "完成：$Skill 已入库并分发。" -ForegroundColor Green
