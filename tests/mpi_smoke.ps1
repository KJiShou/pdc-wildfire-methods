param(
    [Parameter(Mandatory = $true)][string]$MpiExe,
    [Parameter(Mandatory = $true)][string]$SerialExe,
    [Parameter(Mandatory = $true)][string]$Mpiexec,
    [Parameter(Mandatory = $true)][string]$WorkingDirectory
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $WorkingDirectory | Out-Null

function Set-U16([byte[]]$Bytes, [int]$Offset, [int]$Value) {
    [BitConverter]::GetBytes([uint16]$Value).CopyTo($Bytes, $Offset)
}

function Set-U32([byte[]]$Bytes, [int]$Offset, [uint32]$Value) {
    [BitConverter]::GetBytes($Value).CopyTo($Bytes, $Offset)
}

function Write-TestBmp([string]$Path, [int]$Width, [int]$Height, [int]$BitsPerPixel) {
    $bytesPerPixel = $BitsPerPixel / 8
    $rowBytes = [int]([math]::Ceiling(($Width * $bytesPerPixel) / 4.0) * 4)
    $pixelOffset = 54
    $bytes = New-Object byte[] ($pixelOffset + $rowBytes * $Height)
    $bytes[0] = 0x42
    $bytes[1] = 0x4d
    Set-U32 $bytes 2 $bytes.Length
    Set-U32 $bytes 10 $pixelOffset
    Set-U32 $bytes 14 40
    Set-U32 $bytes 18 $Width
    Set-U32 $bytes 22 $Height
    Set-U16 $bytes 26 1
    Set-U16 $bytes 28 $BitsPerPixel
    Set-U32 $bytes 34 ($rowBytes * $Height)
    Set-U32 $bytes 38 2835
    Set-U32 $bytes 42 2835
    for ($y = 0; $y -lt $Height; $y++) {
        for ($x = 0; $x -lt $Width; $x++) {
            $offset = $pixelOffset + $y * $rowBytes + $x * $bytesPerPixel
            $bytes[$offset] = [byte](($x * 31 + $y * 17) % 256)
            $bytes[$offset + 1] = [byte](($x * 11 + $y * 43) % 256)
            $bytes[$offset + 2] = [byte](($x * 7 + $y * 59) % 256)
            if ($bytesPerPixel -eq 4) { $bytes[$offset + 3] = 255 }
        }
    }
    [IO.File]::WriteAllBytes($Path, $bytes)
}

function Invoke-Checked([string]$Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}

$cases = @(
    @{ Name = 'rgba'; Width = 17; Height = 9; Bits = 32; Ranks = @(1, 2, 4); Blocks = 6 },
    @{ Name = 'rgb'; Width = 19; Height = 7; Bits = 24; Ranks = @(2, 4); Blocks = 5 },
    @{ Name = 'tiny'; Width = 1; Height = 1; Bits = 32; Ranks = @(4); Blocks = 1 }
)

foreach ($case in $cases) {
    $input = Join-Path $WorkingDirectory "$($case.Name).bmp"
    Write-TestBmp $input $case.Width $case.Height $case.Bits
    $serialOutput = Join-Path $WorkingDirectory "$($case.Name)-serial.qoi"
    $serialResult = Join-Path $WorkingDirectory "$($case.Name)-serial.json"
    Invoke-Checked $SerialExe @('--input', $input, '--output', $serialOutput, '--result', $serialResult, '--no-preview', '--blocks', "$($case.Blocks)", '--validate')

    foreach ($rankCount in $case.Ranks) {
        $mpiOutput = Join-Path $WorkingDirectory "$($case.Name)-mpi-$rankCount.qoi"
        $mpiResult = Join-Path $WorkingDirectory "$($case.Name)-mpi-$rankCount.json"
        Invoke-Checked $Mpiexec @('-n', "$rankCount", $MpiExe, '--input', $input, '--output', $mpiOutput, '--result', $mpiResult, '--no-preview', '--blocks', "$($case.Blocks)", '--validate')
        $result = Get-Content -Raw $mpiResult | ConvertFrom-Json
        if ($result.status -ne 'success' -or -not $result.validation.passed -or -not $result.validation.pixel_match) {
            throw "MPI validation failed for $($case.Name) with $rankCount ranks"
        }
    }
}

Write-Output 'MPI smoke tests passed.'
