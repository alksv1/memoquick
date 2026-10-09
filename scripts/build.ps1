param([switch]$Test, [string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $taskVswhere)) { throw 'Install Visual Studio with Desktop development with C++ first.' }
$taskVs = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$taskVs) { throw 'Visual Studio C++ compiler was not found.' }
$taskCmake = Join-Path $taskVs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$taskCtest = Join-Path (Split-Path $taskCmake) 'ctest.exe'
if (!(Test-Path -LiteralPath $taskCmake)) { $taskCmake = (Get-Command cmake -ErrorAction Stop).Source; $taskCtest = (Get-Command ctest -ErrorAction Stop).Source }
$taskVsVersion = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
$taskGenerator = if ([int]($taskVsVersion.Split('.')[0]) -ge 18) { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }
& $taskCmake -S $taskRoot -B (Join-Path $taskRoot 'build') -G $taskGenerator -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $taskCmake --build (Join-Path $taskRoot 'build') --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
if ($Test) {
    & $taskCtest --test-dir (Join-Path $taskRoot 'build') -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
$taskDist = Join-Path $taskRoot 'dist'
New-Item -ItemType Directory -Force -Path $taskDist | Out-Null
Copy-Item -LiteralPath (Join-Path $taskRoot "build\$Configuration\MemoQuick.exe") -Destination (Join-Path $taskDist 'MemoQuick.exe') -Force
Write-Host "Ready: $taskDist\MemoQuick.exe"
