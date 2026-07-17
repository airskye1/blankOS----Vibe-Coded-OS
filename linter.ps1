param(
    [string]$TargetDir = ".\src",
    [string]$Makefile = ".\Makefile"
)

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " Running Advanced Vibe-Coded Auto-Linter v1.3.1" -ForegroundColor Cyan
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
    $srcFiles = Get-ChildItem -Path $TargetDir -Recurse -Include *.cpp, *.c | Where-Object { $_.FullName -notmatch "boot.c" -and $_.FullName -notmatch "doom_src" -and $_.FullName -notmatch "store_apps" }
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
    # Skip library / third-party files
    if ($file.FullName -match "stb_" -or $file.FullName -match "doom_src" -or $file.FullName -match "libc_stubs" -or $file.FullName -match "store_apps") {
        continue
    }

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

    # F. Check for missing externs for dui_ and blankUI_ functions
    $checkDui = $true
    $checkBlankUI = $true
    if ($relName -match "blankDUI.cpp" -or $relName -match "compositor.cpp" -or $relName -match "stubs.cpp") {
        $checkDui = $false
        $checkBlankUI = $false
    }
    if ($relName -match "blankUI.cpp") {
        $checkBlankUI = $false
    }
    if ($checkDui -or $checkBlankUI) {
        $pattern = if ($checkDui -and $checkBlankUI) { '\b(dui_[a-zA-Z0-9_]+|blankUI_[a-zA-Z0-9_]+)\s*\(' }
                   elseif ($checkDui) { '\b(dui_[a-zA-Z0-9_]+)\s*\(' }
                   else { '\b(blankUI_[a-zA-Z0-9_]+)\s*\(' }
        
        $matches = [regex]::Matches($content, $pattern)
        foreach ($m in $matches) {
            $funcName = $m.Groups[1].Value
            if ($content -notmatch "extern\s+[^\r\n]*\b$funcName\b") {
                Report-Warning "$relName calls '$funcName' but is missing an extern declaration! This causes compilation errors."
            }
        }
    }

    # G. Auto-fix OutputString missing (CHAR16*) cast for L"..."
    if ($content -match 'OutputString\s*\([^,]+,\s*L"') {
        $content = [regex]::Replace($content, '(OutputString\s*\([^,]+,\s*)(L"[^"]*")', '$1(CHAR16*)$2')
        $changed = $true
        Write-Host "[AUTO-FIX] Added (CHAR16*) cast to OutputString in $relName" -ForegroundColor Green
    }
    
    # G2. Auto-fix wide string assignments/initializers lacking cast
    if ($content -match 'CHAR16\*\s+[a-zA-Z0-9_]+\s*=\s*L"') {
        $content = [regex]::Replace($content, '(const\s+CHAR16\*\s+[a-zA-Z0-9_]+\s*=\s*)(L"[^"]*")', '$1(const CHAR16*)$2')
        $content = [regex]::Replace($content, '(?<!const\s+)(CHAR16\*\s+[a-zA-Z0-9_]+\s*=\s*)(L"[^"]*")', '$1(CHAR16*)$2')
        $changed = $true
        Write-Host "[AUTO-FIX] Added CHAR16* cast to assignments in $relName" -ForegroundColor Green
    }
    
    # H. Auto-fix missing (char*) cast for string literals in blankUI functions to prevent -Wwrite-strings (Safe version restricted to single line)
    if ($content -match 'blankUI_[a-zA-Z0-9_]+\s*\([^)\r\n]*?"') {
        $content = [regex]::Replace($content, '(blankUI_[a-zA-Z0-9_]+\s*\([^)\r\n]*?)("[^"]*")', '$1(char*)$2')
        $changed = $true
        Write-Host "[AUTO-FIX] Added (char*) cast to blankUI string arguments in $relName" -ForegroundColor Green
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
