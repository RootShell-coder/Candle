$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$includeExtensions = @(
  '.cpp',
  '.h',
  '.hpp',
  '.ino',
  '.js',
  '.html',
  '.css',
  '.md',
  '.json',
  '.jsonc',
  '.ini',
  '.yml',
  '.yaml',
  '.csv',
  '.ps1'
)
$excludeDirs = @('.git', '.pio')
$utf8Strict = [System.Text.UTF8Encoding]::new($false, $true)

$invalidUtf8 = New-Object System.Collections.Generic.List[string]
$utf8Bom = New-Object System.Collections.Generic.List[string]

Get-ChildItem -Path $root -Recurse -File |
  Where-Object {
    $includeExtensions -contains $_.Extension.ToLowerInvariant() -and
    -not ($_.FullName -split [regex]::Escape([System.IO.Path]::DirectorySeparatorChar) |
      Where-Object { $excludeDirs -contains $_ })
  } |
  ForEach-Object {
    $bytes = [System.IO.File]::ReadAllBytes($_.FullName)

    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
      $utf8Bom.Add($_.FullName)
    }

    try {
      $null = $utf8Strict.GetString($bytes)
    } catch {
      $invalidUtf8.Add($_.FullName)
    }
  }

if ($invalidUtf8.Count -gt 0) {
  Write-Host 'Invalid UTF-8 files:'
  $invalidUtf8 | ForEach-Object { Write-Host "  $_" }
}

if ($utf8Bom.Count -gt 0) {
  Write-Host 'UTF-8 BOM files:'
  $utf8Bom | ForEach-Object { Write-Host "  $_" }
}

if ($invalidUtf8.Count -gt 0 -or $utf8Bom.Count -gt 0) {
  exit 1
}

Write-Host 'All project text files are valid UTF-8 without BOM.'
