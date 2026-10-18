param(
    [ValidateSet('start', 'stop', 'exec', 'build', 'verify', 'verify-ref', 'run')]
    [string]$Action = 'verify',
    [string]$Revision = ''
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$settingsPath = Join-Path $repoRoot '.spiritfox-local.json'
if (-not (Test-Path -LiteralPath $settingsPath)) {
    throw 'Missing .spiritfox-local.json. Configure the local toolRoot and python paths; see docs/DEVELOPMENT.md.'
}
$settings = Get-Content -LiteralPath $settingsPath -Raw -Encoding UTF8 | ConvertFrom-Json
& $settings.python -B (Join-Path $settings.toolRoot 'dev.py') $Action $repoRoot $Revision
exit $LASTEXITCODE
