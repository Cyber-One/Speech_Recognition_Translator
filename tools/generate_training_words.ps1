param(
    [int]$LanguageId = 0,
    [string]$OutputPath = ""
)

$dictPath = "microsd/Dictionary.dat"
$languagePath = "microsd/Language.dat"

function Get-LanguageName {
    param(
        [int]$Id,
        [string]$Path
    )

    if (-not (Test-Path $Path)) {
        return ("Language_{0:X2}" -f $Id)
    }

    foreach ($line in Get-Content $Path) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -lt 3) { continue }
        if (-not $trimmed.Substring(0, 2) -match '^[0-9A-Fa-f]{2}$') { continue }

        try { $lineId = [Convert]::ToInt32($trimmed.Substring(0, 2), 16) } catch { continue }
        if ($lineId -ne $Id) { continue }

        $name = $trimmed.Substring(2).Trim()
        if ([string]::IsNullOrWhiteSpace($name)) {
            return ("Language_{0:X2}" -f $Id)
        }

        $safe = ($name -replace '[^A-Za-z0-9_-]', '_')
        if ([string]::IsNullOrWhiteSpace($safe)) {
            return ("Language_{0:X2}" -f $Id)
        }

        return $safe
    }

    return ("Language_{0:X2}" -f $Id)
}

$languageName = Get-LanguageName -Id $LanguageId -Path $languagePath
$outPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    "microsd/TrainingWords_{0}.txt" -f $languageName
} else {
    $OutputPath
}

$hexFieldChars = 45
$langFieldChars = 2
$langSepChars = 1
$wordSize = 26
$wordOffset = $hexFieldChars + $langFieldChars + $langSepChars

$targetIds = New-Object System.Collections.Generic.HashSet[int]

$entries = New-Object System.Collections.Generic.List[object]
Get-Content $dictPath | ForEach-Object {
    $line = $_
    if ($line.Length -lt $wordOffset) { return }

    $hexPart = $line.Substring(0, $hexFieldChars).Trim()
    $langHex = if ($line.Length -ge ($hexFieldChars + $langFieldChars)) { $line.Substring($hexFieldChars, $langFieldChars) } else { "00" }
    try { $entryLanguageId = [Convert]::ToInt32($langHex, 16) } catch { $entryLanguageId = 0 }
    if ($entryLanguageId -ne $LanguageId) { return }

    $maxLen = [Math]::Min($wordSize, [Math]::Max(0, $line.Length - $wordOffset))
    $wordPart = if ($maxLen -gt 0) { $line.Substring($wordOffset, $maxLen).Trim() } else { "" }

    if ([string]::IsNullOrWhiteSpace($wordPart)) { return }

    $occ = @{}
    ($hexPart -split "\s+") | ForEach-Object {
        $tok = $_
        if ($tok.Length -ne 2) { return }
        try {
            $v = [Convert]::ToInt32($tok, 16)
        } catch {
            return
        }

        if ($v -eq 0) { return }

        if ($v -ge 0x05 -and $v -le 0x2C) {
            [void]$targetIds.Add($v)
        }

        if ($targetIds.Contains($v)) {
            if (-not $occ.ContainsKey($v)) { $occ[$v] = 0 }
            $occ[$v] += 1
        }
    }

    if ($occ.Count -gt 0) {
        $entries.Add([pscustomobject]@{
            Word = $wordPart
            Occ  = $occ
        }) | Out-Null
    }
}

$required = @{}
$current = @{}
foreach ($id in $targetIds) {
    $required[$id] = 3
    $current[$id] = 0
}

$selected = New-Object System.Collections.Generic.List[string]
$used = New-Object System.Collections.Generic.HashSet[int]

while ($true) {
    $allDone = $true
    foreach ($id in $targetIds) {
        if ($current[$id] -lt $required[$id]) {
            $allDone = $false
            break
        }
    }

    if ($allDone) { break }

    $bestIndex = -1
    $bestGain = 0
    $bestSpan = 0

    for ($i = 0; $i -lt $entries.Count; $i++) {
        if ($used.Contains($i)) { continue }

        $entry = $entries[$i]
        $gain = 0
        $span = 0

        foreach ($k in $entry.Occ.Keys) {
            $def = [Math]::Max(0, $required[$k] - $current[$k])
            if ($def -gt 0) {
                $gain += [Math]::Min($entry.Occ[$k], $def)
                $span += 1
            }
        }

        if (($gain -gt $bestGain) -or (($gain -eq $bestGain) -and ($span -gt $bestSpan))) {
            $bestGain = $gain
            $bestSpan = $span
            $bestIndex = $i
        }
    }

    if ($bestIndex -lt 0 -or $bestGain -le 0) { break }

    $chosen = $entries[$bestIndex]
    $selected.Add($chosen.Word) | Out-Null
    [void]$used.Add($bestIndex)

    foreach ($k in $chosen.Occ.Keys) {
        $current[$k] += $chosen.Occ[$k]
    }
}

$missing = @()
foreach ($id in ($targetIds | Sort-Object)) {
    if ($current[$id] -lt $required[$id]) {
        $missing += $id
    }
}

$selected | Set-Content -Path $outPath -Encoding UTF8

Write-Output "Output file: $outPath"
Write-Output "Language ID filter: $('{0:X2}' -f $LanguageId)"
Write-Output "Selected words: $($selected.Count)"
Write-Output "Target phonemes: $($targetIds.Count)"
Write-Output "Missing phonemes: $($missing.Count)"

if ($missing.Count -eq 0) {
    $minCov = ($targetIds | ForEach-Object { $current[$_] } | Measure-Object -Minimum).Minimum
    Write-Output "Minimum coverage per phoneme: $minCov"
} else {
    Write-Output ("Missing IDs: " + (($missing | ForEach-Object { ('0x{0:X2}' -f $_) }) -join ' '))
}
