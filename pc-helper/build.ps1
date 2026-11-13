# Build framedock-pc.exe with the C# compiler that comes with Windows (.NET Framework 4.8). No SDK, no packages.
#   powershell -ExecutionPolicy Bypass -File pc-helper\build.ps1 [-OutDir dist]
param([string]$OutDir = (Join-Path $PSScriptRoot 'out'))
$ErrorActionPreference = 'Stop'
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path $csc)) { throw "No C# compiler at $csc (.NET Framework 4.8 is part of Windows 10/11)" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$exe = Join-Path $OutDir 'framedock-pc.exe'
& $csc /nologo /target:winexe /optimize+ /r:System.Windows.Forms.dll "/out:$exe" (Join-Path $PSScriptRoot 'framedock-pc.cs')
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
# The parser's own check: exit code 0 = it reads SteamVR's lines as expected
$p = Start-Process -FilePath $exe -ArgumentList '--self-test' -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "self-test failed: $($p.ExitCode) check(s)" }
"built $exe (self-test passed)"
