<#  New-OsfExfatImage.ps1

    PURPOSE
    -------
    Create a RAW image file, mount it via OSFMount as a logical volume,
    auto-select FAT32/exFAT by image size, format it, copy a folder into it, then dismount it.

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

    PARAMETERS
    ----------
    -ImagePath       Output image file path.
    -SourceDir       Folder to copy into the new volume.
    -Size            Optional. If omitted, an optimal size is computed to fit all files.
                     Suffixes: K/M/G/T (1024), k/m/g/t (1000), b (512-byte blocks), or bytes.
    -Label           Volume label.
    -ForceOverwrite  Recreate image if it already exists.

    NOTES
    -----
    - This script does NOT auto-elevate. Start PowerShell as Administrator.
    - FS selection is automatic by image size:
      * < 4 GB  -> FAT32, cluster 4096
      * >= 4 GB -> exFAT, cluster 32768
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

  [switch]$ForceOverwrite
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
    "$env:ProgramFiles\OSFMount\osfmount.com",
    "${env:ProgramFiles(x86)}\OSFMount\osfmount.com",
    "$env:ProgramFiles\PassMark\OSFMount\osfmount.com",
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

  $files = @(Get-ChildItem -LiteralPath $dir -Recurse -File -Force)
  $dirs  = @(Get-ChildItem -LiteralPath $dir -Recurse -Directory -Force)

  [Int64]$sumAllocated = 0
  foreach ($f in $files) {
    $len = [Int64]$f.Length
    $alloc = (($len + $cluster - 1) / $cluster) * $cluster
    $sumAllocated += $alloc
  }

  # Metadata estimate (moderate, not bloated)
  [Int64]$metaEntries = ([Int64]$files.Count * 512) + ([Int64]$dirs.Count * 256)

  [Int64]$clustersData = [Int64]([Math]::Ceiling($sumAllocated / [double]$cluster))
  [Int64]$bitmap = [Int64]([Math]::Ceiling($clustersData / 8.0))   # 1 bit per cluster
  [Int64]$fat    = $clustersData * 4                                # rough 4 bytes per cluster

  [Int64]$baseline = 16MB
  [Int64]$raw = $sumAllocated + $metaEntries + $bitmap + $fat + $baseline

  # Safety margin: 2% capped to 512 MiB
  [Int64]$margin = [Int64]([Math]::Ceiling($raw * 0.02))
  if ($margin -gt 512MB) { $margin = 512MB }
  if ($margin -lt 8MB)   { $margin = 8MB }

  [Int64]$total = $raw + $margin
  if ($total -lt 64MB) { $total = 64MB }

  # Align to 1 MiB
  [Int64]$align = 1MB
  $total = (($total + $align - 1) / $align) * $align

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

function Invoke-FormatVolume([string]$driveLetter, [string]$fileSystem, [int]$clusterSize, [string]$label) {
  $target = "${driveLetter}:"
  if ($fileSystem -notin @("FAT32", "exFAT")) {
    throw "Unsupported file system '$fileSystem'. Expected FAT32 or exFAT."
  }

  $attempts = @(
    @{ Name = "$fileSystem quick with requested allocation unit"; Args = @($target, "/FS:$fileSystem", "/A:$clusterSize", "/Q", "/V:$label", "/X", "/Y") },
  )

  $lastFormatExitCode = -1
  foreach ($attempt in $attempts) {
    Write-Host "[Info] format attempt: $($attempt.Name)"
    $stdoutPath = [System.IO.Path]::GetTempFileName()
    $stderrPath = [System.IO.Path]::GetTempFileName()
    try {
      $proc = Start-Process -FilePath "format.com" -ArgumentList $attempt.Args -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
      $lastFormatExitCode = [int]$proc.ExitCode
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
[Int64]$FsSwitchThresholdBytes = 4GB
[int]$Fat32ClusterSize = 4096
[int]$ExfatClusterSize = 32768
[string]$TargetFs = ""
[int]$TargetClusterSize = 0

if ([string]::IsNullOrWhiteSpace($Size)) {
  Write-Host "[Info] Size not provided. Computing an optimal image size from '$SourceDir'..."
  $fat32Candidate = Get-OptimalImageSizeBytes -dir $SourceDir -clusterBytes $Fat32ClusterSize
  if ($fat32Candidate -lt $FsSwitchThresholdBytes) {
    $expectedBytes = $fat32Candidate
  } else {
    $expectedBytes = Get-OptimalImageSizeBytes -dir $SourceDir -clusterBytes $ExfatClusterSize
  }
  $osfSizeArg = "$expectedBytes"
  Write-Host "[Info] Computed image size: $(Format-Bytes $expectedBytes) ($expectedBytes bytes)."
} else {
  $expectedBytes = Parse-SizeToBytes $Size
  $osfSizeArg = $Size
}

if ($expectedBytes -lt $FsSwitchThresholdBytes) {
  $TargetFs = "FAT32"
  $TargetClusterSize = $Fat32ClusterSize
} else {
  $TargetFs = "exFAT"
  $TargetClusterSize = $ExfatClusterSize
}
Write-Host "[Info] Selected filesystem: $TargetFs (cluster=$TargetClusterSize) for image size $(Format-Bytes $expectedBytes)."

$osf = Find-OSFMountCom

[string]$DriveLetter = ""
[string]$MountPoint = ""
[bool]$Mounted = $false

try {
  $DriveLetter = Get-FreeDriveLetter
  $MountPoint = "${DriveLetter}:"

  Write-Host "[1/4] Creating & mounting the image via OSFMount as a logical volume on $MountPoint ..."
  $out = & $osf -a -t file -f $ImagePath -s $osfSizeArg -m $MountPoint -o rw 2>&1
  Write-Host ($out | Out-String).Trim()
  if ($LASTEXITCODE -ne 0) { throw "osfmount.com failed with exit code $LASTEXITCODE." }
  $Mounted = $true
  if (-not (Wait-ForLogicalDrive -driveLetter $DriveLetter -timeoutSeconds 20)) {
    throw "Mounted drive $MountPoint did not appear in time."
  }

  $dest = "${DriveLetter}:\"
  Write-Host "[2/4] Formatting $dest as $TargetFs (cluster=$TargetClusterSize, label='$Label') via format.com ..."
  Invoke-FormatVolume -driveLetter $DriveLetter -fileSystem $TargetFs -clusterSize $TargetClusterSize -label $Label

  if (-not (Test-Path $dest)) { throw "Drive $dest is not accessible after formatting." }

  Write-Host "[3/4] Copying '$SourceDir' -> '$dest' ..."
  $roboArgs = @(
    $SourceDir, $dest,
    "/E", "/COPY:DAT", "/DCOPY:DAT",
    "/R:1", "/W:1",
    "/NFL", "/NDL", "/NP", "/NJH", "/NJS"
  )
  & robocopy.exe @roboArgs | Out-Null
  $robocopyExitCode = $LASTEXITCODE
  if ($robocopyExitCode -gt 7) { throw "robocopy failed. Exit code: $robocopyExitCode" }

  Write-Host "[4/4] Done. Dismounting OSFMount volume..."
}
finally {
  if ($Mounted -and -not [string]::IsNullOrWhiteSpace($MountPoint)) {
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
