param(
    [Parameter(Mandatory = $true)][string]$MpiExe,
    [Parameter(Mandatory = $true)][string]$SerialExe,
    [Parameter(Mandatory = $true)][string]$Mpiexec,
    [Parameter(Mandatory = $true)][string]$WorkingDirectory
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $WorkingDirectory | Out-Null

function Set-U16([byte[]]$Bytes, [int]$Offset, [int]$Value) { [BitConverter]::GetBytes([uint16]$Value).CopyTo($Bytes, $Offset) }
function Set-U32([byte[]]$Bytes, [int]$Offset, [uint32]$Value) { [BitConverter]::GetBytes($Value).CopyTo($Bytes, $Offset) }
function Write-TestBmp([string]$Path) {
    $width = 23; $height = 11; $rowBytes = $width * 4; $bytes = New-Object byte[] (54 + $rowBytes * $height)
    $bytes[0] = 0x42; $bytes[1] = 0x4d
    Set-U32 $bytes 2 $bytes.Length; Set-U32 $bytes 10 54; Set-U32 $bytes 14 40
    Set-U32 $bytes 18 $width; Set-U32 $bytes 22 $height; Set-U16 $bytes 26 1; Set-U16 $bytes 28 32
    Set-U32 $bytes 34 ($rowBytes * $height)
    for ($y = 0; $y -lt $height; $y++) {
        for ($x = 0; $x -lt $width; $x++) {
            $offset = 54 + $y * $rowBytes + $x * 4
            $bytes[$offset] = [byte](($x * 31 + $y * 17) % 256)
            $bytes[$offset + 1] = [byte](($x * 11 + $y * 43) % 256)
            $bytes[$offset + 2] = [byte](($x * 7 + $y * 59) % 256)
            $bytes[$offset + 3] = 255
        }
    }
    [IO.File]::WriteAllBytes($Path, $bytes)
}
function Invoke-Checked([string]$Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE" }
}
$inputPath = Join-Path $WorkingDirectory 'server-input.bmp'
$serialOutput = Join-Path $WorkingDirectory 'server-serial.qoi'
$serialResult = Join-Path $WorkingDirectory 'server-serial.json'
Write-TestBmp $inputPath
Invoke-Checked $SerialExe @('--input', $inputPath, '--output', $serialOutput, '--result', $serialResult, '--no-preview', '--blocks', '7', '--validate')

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Mpiexec
$psi.Arguments = "-n 2 `"$MpiExe`" --server"
$psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
$psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $psi
[void]$process.Start()
try {
    function Invoke-Server([string]$RequestId, [int]$Blocks, [string]$Output, [string]$Result, [string]$Preview) {
        $request = @{ request_id = $RequestId; input = $inputPath; output = $Output; result = $Result; preview = $Preview; blocks = $Blocks; segment_length = 1024; validate = $true } | ConvertTo-Json -Compress
        $process.StandardInput.WriteLine($request)
        $line = $process.StandardOutput.ReadLine()
        if ([string]::IsNullOrWhiteSpace($line)) { throw "MPI server returned no response for $RequestId" }
        return $line | ConvertFrom-Json
    }
    $outputOne = Join-Path $WorkingDirectory 'server-one.qoi'; $resultOne = Join-Path $WorkingDirectory 'server-one.json'; $previewOne = Join-Path $WorkingDirectory 'server-one.bmp'
    $outputTwo = Join-Path $WorkingDirectory 'server-two.qoi'; $resultTwo = Join-Path $WorkingDirectory 'server-two.json'; $previewTwo = Join-Path $WorkingDirectory 'server-two.bmp'
    $first = Invoke-Server 'server-1' 7 $outputOne $resultOne $previewOne
    $second = Invoke-Server 'server-2' 11 $outputTwo $resultTwo $previewTwo
    foreach ($response in @($first, $second)) {
        if ($response.status -ne 'success') { throw "MPI server request failed: $($response.error)" }
    }
    foreach ($resultPath in @($resultOne, $resultTwo)) {
        $validation = (Get-Content -Raw $resultPath | ConvertFrom-Json).validation
        if (-not $validation.passed -or -not $validation.pixel_match) { throw "MPI server validation failed: $resultPath" }
    }
    if (-not (Get-Content -Raw $resultTwo | ConvertFrom-Json).configuration.persistent_context_reused) { throw 'MPI worker context was not reused' }
    if (-not (Get-Content -Raw $resultTwo | ConvertFrom-Json).configuration.input_cache_reused) { throw 'MPI input cache was not reused' }
    $process.StandardInput.Close()
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "MPI server exited with code $($process.ExitCode)" }
} finally {
    if (-not $process.HasExited) { $process.Kill() }
    $process.Dispose()
}
Write-Output 'MPI persistent server smoke test passed.'
