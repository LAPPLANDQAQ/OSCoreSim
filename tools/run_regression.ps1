# run_regression.ps1 - OSCoreSim diagnostic regression runner
param(
    [string]$ExePath = "",
    [string]$TestsDir = ""
)
if (-not $ExePath) { $ExePath = Join-Path $PSScriptRoot "..\x64\Release\OSCoreSim.exe" }
if (-not $TestsDir) { $TestsDir = Join-Path $PSScriptRoot "..\tests\diagnostics" }

$ErrorActionPreference = "Stop"
$tests = @(
    "00_clean_boot_test.txt", "01_user_session_test.txt",
    "02_process_test.txt", "03_memory_test.txt",
    "04_scheduler_test.txt", "05_persistence_test.txt",
    "06_vfs_test.txt", "07_full_regression_test.txt",
    "08_vfs_unicode_multiline_test.txt", "09_visualization_format_test.txt"
)

Write-Host "OSCoreSim Regression Runner" -ForegroundColor Cyan
Write-Host "Exe: $ExePath"
$passed = 0; $failed = 0

foreach ($test in $tests) {
    $testPath = Join-Path $TestsDir $test
    if (-not (Test-Path $testPath)) {
        Write-Host "SKIP: $test" -ForegroundColor Yellow; continue
    }
    Write-Host "Running: $test ... " -NoNewline

    $stateFile = Join-Path $PSScriptRoot "..\data\os_state.bin"
    Remove-Item $stateFile -Force -ErrorAction SilentlyContinue

    $raw = cmd /c "`"$ExePath`" < `"$testPath`" 2>&1"
    $text = if ($raw -is [array]) { $raw -join "`n" } else { "$raw" }
    $exitCode = $LASTEXITCODE
    $errorFound = $false

    if ($text -match "Unknown command") {
        $errorFound = $true; Write-Host "`n  Unknown command" -ForegroundColor Red
    }
    if ($text -match "Command execution failed") {
        $errorFound = $true; Write-Host "`n  Execution failed" -ForegroundColor Red
    }
    if ($exitCode -ne 0) {
        $errorFound = $true; Write-Host "`n  Exit=$exitCode" -ForegroundColor Red
    }

    if ($test -eq "08_vfs_unicode_multiline_test.txt") {
        # Check for 3-line content output with separator dashes
        if ($text -notmatch "Content:" -or $text -notmatch "----") {
            $errorFound = $true; Write-Host "`n  VFS format missing" -ForegroundColor Red
        }
        # Check file size appears (content was written)
        if ($text -notmatch "Size:.*bytes") {
            $errorFound = $true; Write-Host "`n  VFS size missing" -ForegroundColor Red
        }
    }

    if ($test -eq "09_visualization_format_test.txt") {
        $kw = @("Process Tree", "Memory Layout", "Memory Map", "OSCoreSim Overview")
        foreach ($k in $kw) {
            if ($text -notmatch $k) {
                $errorFound = $true; Write-Host "`n  Missing: $k" -ForegroundColor Red
            }
        }
    }

    if (-not $errorFound) { Write-Host "PASS" -ForegroundColor Green; $passed++ } else { $failed++ }
}

Write-Host "`nResults: $passed passed, $failed failed" -ForegroundColor $(if ($failed -eq 0) { "Green" } else { "Red" })
if ($failed -gt 0) { exit 1 } else { Write-Host "All tests passed." -ForegroundColor Green; exit 0 }
