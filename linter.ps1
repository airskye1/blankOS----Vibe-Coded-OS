param(
    [string]$TargetDir = ".\src"
)

Write-Host "Running Vibe-Coded Auto-Linter..."

$files = Get-ChildItem -Path $TargetDir -Recurse -Include *.c,*.h

foreach ($file in $files) {
    $content = Get-Content $file.FullName -Raw
    $changed = $false

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

Write-Host "Linting Complete!"
