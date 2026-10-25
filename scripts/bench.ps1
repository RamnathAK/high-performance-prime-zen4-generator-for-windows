# bench.ps1 - Simple benchmark for prime generator
$exe = ".\build\prime.exe"
if (-not (Test-Path $exe)) { Write-Error "Build first!"; exit 1 }

Write-Host "Starting benchmark - will measure first 100MB file"
Remove-Item primegenerated*.txt -ErrorAction SilentlyContinue

$sw = [System.Diagnostics.Stopwatch]::StartNew()
Start-Process $exe -NoNewWindow
while (-not (Test-Path "primegenerated1.txt") -or ((Get-Item "primegenerated1.txt" -ErrorAction SilentlyContinue).Length -lt 100MB)) {
  Start-Sleep -Seconds 1
  $size = (Get-Item "primegenerated1.txt" -ErrorAction SilentlyContinue).Length
  Write-Host "Current size: $([math]::Round($size/1MB,2)) MB"
}
$sw.Stop()

$count = (Get-Content primegenerated1.txt | Measure-Object -Line).Lines
Write-Host "Finished 100MB in $($sw.Elapsed.TotalSeconds) sec, $count primes"
"Time: $($sw.Elapsed.TotalSeconds) sec, Primes: $count, Size: 100MB" | Out-File bench_results.txt

# Stop prime.exe
Get-Process prime -ErrorAction SilentlyContinue | Stop-Process -Force
