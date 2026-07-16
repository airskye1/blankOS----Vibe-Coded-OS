param(
    [string]$TargetDir = ".\src",
    [string]$Makefile = ".\Makefile"
)

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " Running Advanced Vibe-Coded Auto-Linter v1.2.6" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
$errorFound = $false

# Helper to report warnings
function Report-Warning([string]$msg, [string]$level = "RED") {
    $color = if ($level -eq "RED") { "Red" } else { "Yellow" }
    Write-Host "[LINT $level] $msg" -ForegroundColor $color
    if ($level -eq "RED") {
        $global:errorFound = $true
    }
}

# 1. Makefile & Project Integrity Checks
if (Test-Path $Makefile) {
    $makeContent = Get-Content $Makefile -Raw
    
    # Check compilation flags
    if ($makeContent -notmatch '-maccumulate-outgoing-args') {
        Report-Warning "Makefile is missing -maccumulate-outgoing-args in CFLAGS! This will cause UEFI stack corruption."
    }
    if ($makeContent -notmatch '-DGNU_EFI_USE_MS_ABI') {
        Report-Warning "Makefile is missing -DGNU_EFI_USE_MS_ABI! Standard UEFI calls like OutputString will use System V and crash the firmware."
    }
    if ($makeContent -notmatch '-j \.rodata') {
        Report-Warning "Makefile is missing -j .rodata in OBJCOPY! Strings will be stripped and cause crash."
    }
    if ($makeContent -notmatch '-j \.bss') {
        Report-Warning "Makefile is missing -j .bss in OBJCOPY! Global variables might cause memory faults." "YELLOW"
    }

    # Verify that all source files are compiled and linked (prevent orphan files like compositor.cpp)
    $srcFiles = Get-ChildItem -Path $TargetDir -Recurse -Include *.cpp, *.c | Where-Object { $_.FullName -notmatch "boot.c" }
    foreach ($src in $srcFiles) {
        # Get relative path with forward slashes (e.g. src/ui/compositor.o)
        $relPath = $src.FullName.Replace((Get-Item .).FullName + "\", "").Replace("\", "/")
        $objPath = $relPath -replace '\.c(pp)?$', '.o'
        
        if ($makeContent -notmatch [regex]::Escape($objPath)) {
            Report-Warning "Source file '$relPath' is NOT registered in the Makefile KERNEL_OBJS! It will not be compiled or linked."
        }
    }
}

# 2. Source Code static analysis
$files = Get-ChildItem -Path $TargetDir -Recurse -Include *.cpp,*.c,*.h
foreach ($file in $files) {
    $content = Get-Content $file.FullName -Raw
    $changed = $false
    $relName = $file.FullName.Replace((Get-Item .).FullName + "\", "")

    # A. Check for standard library headers (Freestanding Violations)
    $stdlibHeaders = @('<iostream>', '<string>', '<vector>', '<map>', '<memory>', '<algorithm>', '<stdio.h>', '<stdlib.h>', '<malloc.h>')
    foreach ($hdr in $stdlibHeaders) {
        if ($content -match [regex]::Escape($hdr)) {
            Report-Warning "$relName includes standard library header '$hdr'. Freestanding OS kernels cannot use standard host runtimes!"
        }
    }

    # B. Check for standard C functions that should be freestanding stubs
    if ($content -match '\b(printf|sprintf|malloc|free|strlen|strcpy)\b' -and $relName -notmatch "stubs.cpp") {
        Report-Warning "$relName references standard library symbol (printf/malloc/etc). Ensure these are custom stubs or freestanding versions." "YELLOW"
    }

    # C. Check for OutputString UTF-16 wide string compliance (requires L prefix)
    # Match OutputString(...) calls where the second arg is a plain string e.g. "Text" instead of L"Text"
    if ($content -match 'OutputString\s*\([^,]+,\s*"') {
        Report-Warning "$relName has OutputString calls passing standard ASCII string literals. UEFI expects wide UTF-16 strings prefixed with L (e.g. L`\"Text`\")."
    }

    # D. Check for designated array initializers in C++ code (unimplemented in older G++ versions)
    if ($relName -like "*.cpp" -and $content -match '(?<=^|[,{])\s*\[\s*[''\w\s"]+\s*\]\s*=') {
        Report-Warning "$relName contains C99 designated array initializers (e.g., [index] = value). This is not standard-supported in C++ and will fail compiling under G++."
    }

    # E. Check for missing carriage return in output logs
    if ($content -match 'OutputString.*L".*[^\r]\\n"') {
        Report-Warning "$relName uses \n without \r in OutputString. UEFI text output requires \r\n for proper formatting." "YELLOW"
    }

    # E. Check for stdbool.h, stddef.h, stdint.h auto-inclusions
    if ($content -match '\bbool\b' -and $content -notmatch 'stdbool\.h' -and $relName -notmatch "boot.c" -and $relName -notmatch "stubs.cpp") {
        $content = "#include <stdbool.h>`r`n" + $content
        $changed = $true
        Write-Host "[AUTO-FIX] Added <stdbool.h> to $relName" -ForegroundColor Green
    }
    if ($content -match '\bNULL\b' -and $content -notmatch 'stddef\.h' -and $relName -notmatch "boot.c" -and $relName -notmatch "stubs.cpp") {
        $content = "#include <stddef.h>`r`n" + $content
        $changed = $true
        Write-Host "[AUTO-FIX] Added <stddef.h> to $relName" -ForegroundColor Green
    }
    if ($content -match '\buint(8|16|32|64)_t\b' -and $content -notmatch 'stdint\.h' -and $relName -notmatch "boot.c" -and $relName -notmatch "stubs.cpp") {
        $content = "#include <stdint.h>`r`n" + $content
        $changed = $true
        Write-Host "[AUTO-FIX] Added <stdint.h> to $relName" -ForegroundColor Green
    }

    # Save if auto-fixes were applied
    if ($changed) {
        [System.IO.File]::WriteAllText($file.FullName, $content)
    }
}

# 3. Final Report
Write-Host "----------------------------------------------------------" -ForegroundColor Cyan
if ($errorFound) {
    Write-Host "[!] Linting finished with warnings/errors. Please resolve them." -ForegroundColor Red
    Exit 1
} else {
    Write-Host "[OK] Linting Complete! All checks passed. OS is safe to build." -ForegroundColor Green
    Exit 0
}
