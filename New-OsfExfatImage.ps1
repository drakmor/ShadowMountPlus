<#  New-OsfExfatImage.ps1

    PURPOSE
    -------
    Create a RAW image file, mount it via OSFMount as a logical volume,
    format it as exFAT, and either:
      - format + copy + dismount (default), or
      - create + mount only for manual steps.

    USAGE (run PowerShell as Administrator)
    --------------------------------------
    1) Auto-size (recommended):
       powershell.exe -ExecutionPolicy Bypass -File .\New-OsfExfatImage.ps1 `
         -ImagePath "C:\images\data.img" `
         -SourceDir "C:\payload" `
         -Label "DATA" `
         -ForceOverwrite

    2) Fixed size:
       powershell.exe -ExecutionPolicy Bypass -File .\New-OsfExfatImage.ps1 `
         -ImagePath "C:\images\data.img" `
         -SourceDir "C:\payload" `
         -Size 8G `
         -Label "DATA" `
         -ForceOverwrite

    2b) Custom temp directory (large copies / slow system drive):
       powershell.exe -ExecutionPolicy Bypass -File .\New-OsfExfatImage.ps1 `
         -ImagePath "D:\images\data.img" `
         -SourceDir "C:\payload" `
         -TempDir "D:\temp" `
         -ForceOverwrite

    3) Create empty image and keep mounted (manual format/copy):
       powershell.exe -ExecutionPolicy Bypass -File .\New-OsfExfatImage.ps1 `
         -ImagePath "C:\images\data.exfat" `
         -SourceDir "C:\payload\APPXXXX" `
         -CreateEmptyAndMount `
         -ForceOverwrite

    PARAMETERS
    ----------
    -ImagePath       Output image file path.
    -SourceDir       Folder to copy into the new volume.
    -Size            Optional. If omitted, an optimal size is computed to fit all files.
                     Suffixes: K/M/G/T (1024), k/m/g/t (1000), b (512-byte blocks), or bytes.
    -Label           Volume label.
    -TempDir         Optional. Directory for temporary files (format.com logs, etc.).
                     Aliases: -t. When omitted, Windows default temp is used.
    -ForceOverwrite  Recreate image if it already exists.
    -CreateEmptyAndMount
                     Create and mount image only. Skip format/copy and leave mounted.

    NOTES
    -----
    - This script does NOT auto-elevate. Start PowerShell as Administrator.
    - Filesystem is always exFAT.
    - Cluster size is auto-selected:
      - large-file sets: 65536
      - small/mixed-file sets: 32768
#>

[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$ImagePath,

  [Parameter(Mandatory = $true)]
  [string]$SourceDir,

  [Parameter(Mandatory = $false)]
  [string]$Size,

  [string]$Label = "OSFIMG",

  [Alias("t")]
  [string]$TempDir,

  [switch]$ForceOverwrite,

  [switch]$CreateEmptyAndMount
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$scriptCulture = [System.Globalization.CultureInfo]::GetCultureInfo("en-US")
[System.Threading.Thread]::CurrentThread.CurrentCulture = $scriptCulture
[System.Threading.Thread]::CurrentThread.CurrentUICulture = $scriptCulture
[System.Globalization.CultureInfo]::DefaultThreadCurrentCulture = $scriptCulture
[System.Globalization.CultureInfo]::DefaultThreadCurrentUICulture = $scriptCulture

function Test-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  $p  = New-Object Security.Principal.WindowsPrincipal($id)
  return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Find-OSFMountCom {
  $cmd = Get-Command "osfmount.com" -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }

  $candidates = @(
    "${env:ProgramFiles}\OSFMount\osfmount.com",
    "${env:ProgramFiles(x86)}\OSFMount\osfmount.com",
    "${env:ProgramFiles}\PassMark\OSFMount\osfmount.com",
    "${env:ProgramFiles(x86)}\PassMark\OSFMount\osfmount.com"
  ) | Where-Object { $_ -and (Test-Path $_) }

  $candidates = @($candidates)
  if ($candidates.Count -gt 0) { return $candidates[0] }

  throw "osfmount.com not found. Add OSFMount to PATH or install it to a standard location."
}

function Parse-SizeToBytes([string]$s) {
  $s = $s.Trim()
  if ($s -match '^\s*(\d+)\s*([bBkKmMgGtT]?)\s*$') {
    $num = [Int64]$matches[1]
    $u   = $matches[2]
    switch ($u) {
      ''  { return $num }
      'b' { return $num * 512 }
      'B' { return $num * 512 }
      'K' { return $num * 1024 }
      'M' { return $num * 1024 * 1024 }
      'G' { return $num * 1024 * 1024 * 1024 }
      'T' { return $num * 1024 * 1024 * 1024 * 1024 }
      'k' { return $num * 1000 }
      'm' { return $num * 1000 * 1000 }
      'g' { return $num * 1000 * 1000 * 1000 }
      't' { return $num * 1000 * 1000 * 1000 * 1000 }
      default { throw "Unknown size suffix: '$u'" }
    }
  }
  throw "Failed to parse size string: '$s'"
}

$script:ScriptTempDir = $null

function Initialize-ScriptTempDir([string]$dir) {
  if ([string]::IsNullOrWhiteSpace($dir)) { return }
  $resolved = $dir.Trim()
  if (-not (Test-Path -LiteralPath $resolved -PathType Container)) {
    throw "Temp directory not found: $resolved"
  }
  $resolvedPath = (Resolve-Path -LiteralPath $resolved).Path
  $probe = Join-Path $resolvedPath ([System.IO.Path]::GetRandomFileName())
  try {
    Set-Content -LiteralPath $probe -Value "" -NoNewline
    Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
  } catch {
    throw "Temp directory is not writable: $resolvedPath"
  }
  $script:ScriptTempDir = $resolvedPath
  $env:TEMP = $script:ScriptTempDir
  $env:TMP = $script:ScriptTempDir
  Write-Host "[Info] Temp directory: $script:ScriptTempDir"
}

function New-ScriptTempFile {
  if ($script:ScriptTempDir) {
    $name = [System.IO.Path]::GetRandomFileName()
    return (Join-Path $script:ScriptTempDir $name)
  }
  return [System.IO.Path]::GetTempFileName()
}

function Format-Bytes([Int64]$bytes) {
  if ($bytes -ge 1TB) { return "{0:N2} TB" -f ($bytes/1TB) }
  if ($bytes -ge 1GB) { return "{0:N2} GB" -f ($bytes/1GB) }
  if ($bytes -ge 1MB) { return "{0:N2} MB" -f ($bytes/1MB) }
  if ($bytes -ge 1KB) { return "{0:N2} KB" -f ($bytes/1KB) }
  return "$bytes B"
}

function Get-FreeDriveLetter {
  $used = (Get-PSDrive -PSProvider FileSystem).Name
  foreach ($code in 68..90) {
    $letter = [char]$code
    if ($used -notcontains [string]$letter) { return [string]$letter }
  }
  throw "No free drive letters available (D:..Z:)."
}

function Get-OptimalImageSizeBytes([string]$dir, [int]$clusterBytes) {
  $cluster = [Int64]$clusterBytes
  [Int64]$metaFixed = 32MB
  [Int64]$minSlack = 64MB
  [Int64]$spareMin = 64MB
  [Int64]$spareMax = 512MB
  [Int64]$entryMetaBytes = 256

  $files = @(Get-ChildItem -LiteralPath $dir -Recurse -File -Force)
  $dirs = @(Get-ChildItem -LiteralPath $dir -Recurse -Directory -Force)

  [Int64]$rawFileBytes = 0
  [Int64]$dataBytes = 0
  foreach ($f in $files) {
    $len = [Int64]$f.Length
    $rawFileBytes += $len
    $dataBytes += [Int64]([Math]::Ceiling($len / [double]$cluster) * $cluster)
  }

  [Int64]$dataClusters = [Int64]([Math]::Ceiling($dataBytes / [double]$cluster))
  [Int64]$fatBytes = $dataClusters * 4
  [Int64]$bitmapBytes = [Int64]([Math]::Ceiling($dataClusters / 8.0))
  [Int64]$entryBytes =
      (([Int64]$files.Count + [Int64]$dirs.Count) * $entryMetaBytes)

  [Int64]$baseTotal =
      $dataBytes + $fatBytes + $bitmapBytes + $entryBytes + $metaFixed
  [Int64]$spareBytes = [Int64]([Math]::Ceiling($baseTotal / 200.0))
  if ($spareBytes -lt $spareMin) { $spareBytes = $spareMin }
  if ($spareBytes -gt $spareMax) { $spareBytes = $spareMax }
  [Int64]$total = $baseTotal + $spareBytes
  [Int64]$minTotal = $rawFileBytes + $minSlack
  if ($total -lt $minTotal) { $total = $minTotal }

  [Int64]$align = 1MB
  $total = [Int64]([Math]::Ceiling($total / [double]$align) * $align)
  return $total
}

function Wait-ForLogicalDrive([string]$driveLetter, [int]$timeoutSeconds = 20) {
  $target = "${driveLetter}:"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt $timeoutSeconds) {
    $logical = Get-CimInstance -ClassName Win32_LogicalDisk -Filter "DeviceID='$target'" -ErrorAction SilentlyContinue
    if ($logical) { return $true }
    Start-Sleep -Milliseconds 300
  }
  return $false
}

function Get-LogicalDriveFileSystem([string]$driveLetter) {
  $target = "${driveLetter}:"
  $logical = Get-CimInstance -ClassName Win32_LogicalDisk -Filter "DeviceID='$target'" -ErrorAction SilentlyContinue
  if ($logical) { return [string]$logical.FileSystem }
  return ""
}

function Get-OptimalExfatClusterSize([string]$dir) {
  [Int64]$largeFileThreshold = 1MB

  $files = @(Get-ChildItem -LiteralPath $dir -Recurse -File -Force)
  if ($files.Count -eq 0) { return 32768 }

  [Int64]$rawFileBytes = 0
  foreach ($f in $files) {
    $rawFileBytes += [Int64]$f.Length
  }

  [Int64]$avgFileBytes = [Int64]($rawFileBytes / [Int64]$files.Count)
  if ($avgFileBytes -ge $largeFileThreshold) { return 65536 }
  return 32768
}

function Format-AllocationUnitArg([int]$clusterSize) {
  if ($clusterSize -ge 1MB -and ($clusterSize % 1MB) -eq 0) {
    return "{0}M" -f ($clusterSize / 1MB)
  }
  if ($clusterSize -ge 1KB -and ($clusterSize % 1KB) -eq 0) {
    return "{0}K" -f ($clusterSize / 1KB)
  }
  return "$clusterSize"
}

function Dismount-OsfVolume([string]$osfPath, [string]$mountPoint, [int]$maxAttempts = 6) {
  if ([string]::IsNullOrWhiteSpace($mountPoint)) { return $false }

  $targets = @($mountPoint)
  if (-not $mountPoint.EndsWith("\")) { $targets += "$mountPoint\" }

  for ($i = 1; $i -le $maxAttempts; $i++) {
    foreach ($target in $targets) {
      try {
        & $osfPath -d -m $target 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) { return $true }
      } catch {
        # Retry: volume can remain busy for a short time after format/copy.
      }
    }
    Start-Sleep -Milliseconds 500
  }

  return $false
}

function Invoke-FormatVolume([string]$driveLetter, [int]$clusterSize, [string]$label) {
  $target = "${driveLetter}:"
  $fileSystem = "exFAT"
  $clusterArg = Format-AllocationUnitArg -clusterSize $clusterSize
  $attempts = @(
    @{ Name = "$fileSystem quick with requested allocation unit"; Args = @($target, "/FS:$fileSystem", "/A:$clusterArg", "/Q", "/V:$label", "/X", "/Y") }
  )

  $lastFormatExitCode = -1
  foreach ($attempt in $attempts) {
    Write-Host "[Info] format attempt: $($attempt.Name)"
    $stdoutPath = New-ScriptTempFile
    $stderrPath = New-ScriptTempFile
    try {
      $proc = Start-Process -FilePath "format.com" -ArgumentList $attempt.Args -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
      $lastFormatExitCode = [int]$proc.ExitCode

      # Show native format.com output in current console.
      if (Test-Path -LiteralPath $stdoutPath) {
        Get-Content -LiteralPath $stdoutPath -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
      }
      if (Test-Path -LiteralPath $stderrPath) {
        Get-Content -LiteralPath $stderrPath -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
      }
    } finally {
      Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue
    }

    # Some environments may report a non-zero exit code even when formatting completed.
    # Validate resulting filesystem as the source of truth.
    if (Wait-ForLogicalDrive -driveLetter $driveLetter -timeoutSeconds 5) {
      $actualFs = Get-LogicalDriveFileSystem -driveLetter $driveLetter
      if ($actualFs -and $actualFs.ToUpperInvariant() -eq $fileSystem.ToUpperInvariant()) {
        Write-Host "[Info] format result: detected filesystem '$actualFs' on $target."
        return
      }
    }

    Write-Host "[Info] format attempt failed (exit code $lastFormatExitCode), retrying..."
  }

  throw "format.com failed for $target after all retry strategies. Last exit code: $lastFormatExitCode"
}

# -------------------- Main --------------------

if (-not (Test-Admin)) { throw "Please run PowerShell as Administrator." }
if (-not (Test-Path $SourceDir -PathType Container)) { throw "Source directory not found: $SourceDir" }
if (-not (Test-Path (Join-Path $SourceDir "eboot.bin") -PathType Leaf)) { throw "eboot.bin not found in source directory: $SourceDir" }

Initialize-ScriptTempDir -dir $TempDir

# Ensure output directory exists
$outDir = Split-Path -Parent $ImagePath
if ($outDir -and -not (Test-Path $outDir)) {
  New-Item -ItemType Directory -Path $outDir | Out-Null
}

if (Test-Path $ImagePath) {
  if (-not $ForceOverwrite) { throw "Image file already exists: $ImagePath. Use -ForceOverwrite to replace it." }
  Remove-Item -Force $ImagePath
}

[Int64]$expectedBytes = 0
[string]$osfSizeArg = $null
[int]$ExfatClusterSize = Get-OptimalExfatClusterSize -dir $SourceDir
[bool]$sizeProvided = -not [string]::IsNullOrWhiteSpace($Size)
[int]$TargetClusterSize = $ExfatClusterSize

if (-not $sizeProvided) {
  Write-Host "[Info] Size not provided. Computing an optimal image size from '$SourceDir'..."
  $expectedBytes = Get-OptimalImageSizeBytes -dir $SourceDir -clusterBytes $TargetClusterSize
  $osfSizeArg = "$expectedBytes"
} else {
  $expectedBytes = Parse-SizeToBytes $Size
  $osfSizeArg = $Size
}

if (-not $sizeProvided) {
  Write-Host "[Info] Computed image size: $(Format-Bytes $expectedBytes) ($expectedBytes bytes)."
}
Write-Host "[Info] Selected filesystem: exFAT (cluster=$TargetClusterSize) for image size $(Format-Bytes $expectedBytes)."

$osf = Find-OSFMountCom

[string]$DriveLetter = ""
[string]$MountPoint = ""
[bool]$Mounted = $false
[bool]$LeaveMounted = $CreateEmptyAndMount.IsPresent

try {
  $DriveLetter = Get-FreeDriveLetter
  $MountPoint = "${DriveLetter}:"

  if ($CreateEmptyAndMount) {
    Write-Host "[1/2] Creating & mounting the image via OSFMount as a logical volume on $MountPoint ..."
  } else {
    Write-Host "[1/4] Creating & mounting the image via OSFMount as a logical volume on $MountPoint ..."
  }
  $out = & $osf -a -t file -f $ImagePath -s $osfSizeArg -m $MountPoint -o rw,rem 2>&1
  Write-Host ($out | Out-String).Trim()
  if ($LASTEXITCODE -ne 0) { throw "osfmount.com failed with exit code $LASTEXITCODE." }
  $Mounted = $true
  if (-not (Wait-ForLogicalDrive -driveLetter $DriveLetter -timeoutSeconds 20)) {
    throw "Mounted drive $MountPoint did not appear in time."
  }

  $dest = "${DriveLetter}:\"
  if ($CreateEmptyAndMount) {
    Write-Host "[2/2] Done. Empty image is mounted at $dest."
    Write-Host "Manual steps:"
    Write-Host "  1) Format $dest as exFAT (recommended cluster: 64KB for large-file sets, 32KB for small/mixed sets)."
    Write-Host "  2) Copy contents of '$SourceDir' to $dest."
    Write-Host "  3) Dismount: `"$osf`" -d -m $MountPoint"
    return
  }

  Write-Host "[2/4] Formatting $dest as exFAT (cluster=$TargetClusterSize, label='$Label') via format.com ..."
  Invoke-FormatVolume -driveLetter $DriveLetter -clusterSize $TargetClusterSize -label $Label

  if (-not (Test-Path $dest)) { throw "Drive $dest is not accessible after formatting." }

  Write-Host "[3/4] Copying contents of '$SourceDir' -> '$dest' ..."
  $roboArgs = @(
    $SourceDir, $dest,
    "/E", "/COPY:DAT", "/DCOPY:DAT",
    "/R:1", "/W:1",
    "/NFL", "/NDL",
    "/ETA"
  )
  & robocopy.exe @roboArgs
  $robocopyExitCode = $LASTEXITCODE
  if ($robocopyExitCode -gt 7) { throw "robocopy failed. Exit code: $robocopyExitCode" }

  Write-Host "[4/4] Done. Dismounting OSFMount volume..."
}
finally {
  if ($Mounted -and -not $LeaveMounted -and -not [string]::IsNullOrWhiteSpace($MountPoint)) {
    try {
      $currentPath = (Get-Location).Path
      if ($currentPath -like "$MountPoint*") {
        Set-Location "$env:SystemDrive\"
      }
    } catch {
      # Best effort only.
    }

    if (-not (Dismount-OsfVolume -osfPath $osf -mountPoint $MountPoint -maxAttempts 6)) {
      Write-Warning "Failed to dismount OSFMount volume ($MountPoint): access denied or volume busy."
    }
  }
}

Write-Host "OK: Image created at: $ImagePath"
