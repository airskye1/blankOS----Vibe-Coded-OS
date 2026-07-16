param(
    [string]$TargetDir = ".\src",
    [string]$Makefile = ".\Makefile"
)

Write-Host "Running Vibe-Coded Auto-Linter (Advanced OS Checks)..."
$errorFound = $false

# 1. Makefile Checks (Crucial for UEFI OS Booting)
if (Test-Path $Makefile) {
    $makeContent = Get-Content $Makefile -Raw
    
    if ($makeContent -notmatch '-maccumulate-outgoing-args') {
        Write-Host "[WARNING] Makefile is missing -maccumulate-outgoing-args in CFLAGS! This will cause UEFI stack corruption." -ForegroundColor Red
        $errorFound = $true
    }
    if ($makeContent -notmatch '-j \.rodata') {
        Write-Host "[WARNING] Makefile is missing -j .rodata in OBJCOPY! Strings will be stripped and cause crash." -ForegroundColor Red
        $errorFound = $true
    }
    if ($makeContent -notmatch '-j \.bss') {
        Write-Host "[WARNING] Makefile is missing -j .bss in OBJCOPY! Global variables might cause memory faults." -ForegroundColor Yellow
    }
}

# 2. Source Code Checks
$files = Get-ChildItem -Path $TargetDir -Recurse -Include *.c,*.h
foreach ($file in $files) {
    $content = Get-Content $file.FullName -Raw
    $changed = $false

    # Check for standard library includes (fatal in freestanding OS)
    if ($content -match '<stdio\.h>' -or $content -match '<stdlib\.h>') {
        Write-Host "[WARNING] $($file.Name) includes <stdio.h> or <stdlib.h>. This will cause fatal linker errors in a custom OS!" -ForegroundColor Red
        $errorFound = $true
    }

    # Check for improper \n without \r in OutputString
    if ($content -match 'OutputString.*L".*[^\r]\\n"') {
        Write-Host "[WARNING] $($file.Name) uses \n without \r in OutputString. UEFI requires \r\n for proper formatting." -ForegroundColor Yellow
    }

    # Check for bool
    if ($content -match '\bbool\b' -and $content -notmatch 'stdbool\.h') {
        $content = "#include <stdbool.h>`r`n" + $content
        $changed = $true
        Write-Host "[FIXED] Added <stdbool.h> to $($file.Name)"
    }

    # Check for NULL
    if ($content -match '\bNULL\b' -and $content -notmatch 'stddef\.h') {
        $content = "#include <stddef.h>`r`n" + $content
        $changed = $true
        Write-Host "[FIXED] Added <stddef.h> to $($file.Name)"
    }

    # Check for standard integers
    if ($content -match '\buint(8|16|32|64)_t\b' -and $content -notmatch 'stdint\.h') {
        $content = "#include <stdint.h>`r`n" + $content
        $changed = $true
        Write-Host "[FIXED] Added <stdint.h> to $($file.Name)"
    }

    # Save if changed
    if ($changed) {
        [System.IO.File]::WriteAllText($file.FullName, $content)
    }
}

if ($errorFound) {
    Write-Host "`n[!] Linting finished with warnings/errors. Please review." -ForegroundColor Red
} else {
    Write-Host "`n[OK] Linting Complete! Codebase is ready for safe OS compilation." -ForegroundColor Green
}
