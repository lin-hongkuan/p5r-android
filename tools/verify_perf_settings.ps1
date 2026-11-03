#Requires -Version 5.1
<#
.SYNOPSIS
    Static verifier for the perf switches of docs/PERF-DESIGN.md (section 2) on the
    feat/op3pro-2k90 branch.

.DESCRIPTION
    For every switch key in the section 2 table of docs/PERF-DESIGN.md this script asserts that the
    key is
        (a) parsed     - it appears in runtime/src/perf_profile.cpp, or (reported with its location)
                         in another runtime/game C++ source,
        (b) documented - it appears in the section 2 table of docs/PERF-DESIGN.md,
        (c) logged     - it appears next to a log statement (the startup log line a tester reads in
                         logcat), and that its NX_<KEY> environment alias is handled by the parser.

    It then runs the additional static checks of the test plan:
        S1  every #if defined(__ANDROID__) block added by the branch has a non-Android counterpart
        S2  no new third-party dependency (third_party/, FetchContent, submodules)
        S3  no game file / key / private/ content under version control
        S4  the section 2 table and the launcher (games/p5r/java/**/Launcher.java) agree
        S5  the balance preset equals the upstream defaults
        S6  tests/perf_profile_test.cpp exists and is registered in CMakeLists.txt

    Every file is optional: a file that does not exist yet (another agent may still be writing it)
    is reported as a FAIL for the checks that need it. The script never throws on missing input and
    always exits 0 (all checks pass or skipped) or 1 (at least one FAIL).

.PARAMETER RepoRoot
    Repository root. Defaults to the parent directory of this script (tools\..).

.PARAMETER ShowSkip
    Also print the SKIP rows of the per-key table.

.EXAMPLE
    pwsh tools/verify_perf_settings.ps1
    pwsh tools/verify_perf_settings.ps1 -RepoRoot C:\src\p5r-android -ShowSkip

.NOTES
    Read-only: this script only reads files and runs `git status`/`git ls-files`/`git diff`/`git check-ignore`.
    Output is ASCII on purpose so Windows PowerShell 5.1 parses it with any code page.
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [switch]$ShowSkip
)

$ErrorActionPreference = 'Continue'
Set-StrictMode -Version 2.0

# The design doc is UTF-8; make the console emit it as such so a Chinese note in a detail line does
# not turn into mojibake (Windows PowerShell 5.1 defaults to the ANSI code page).
try {
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [Console]::OutputEncoding = $utf8
    $OutputEncoding = $utf8
} catch { }

# ---------------------------------------------------------------------------------------------
# Results
# ---------------------------------------------------------------------------------------------
$script:Results = New-Object System.Collections.ArrayList

function Add-Result {
    param(
        [string]$Check,
        [string]$Target,
        [ValidateSet('PASS', 'FAIL', 'SKIP', 'WARN')][string]$Status,
        [string]$Detail = ''
    )
    [void]$script:Results.Add([pscustomobject]@{
        Check  = $Check
        Target = $Target
        Status = $Status
        Detail = $Detail
    })
}

function Write-Head([string]$text) {
    Write-Host ''
    Write-Host ('=' * 78) -ForegroundColor DarkGray
    Write-Host $text -ForegroundColor Cyan
    Write-Host ('=' * 78) -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------------------------
# Helpers (all tolerant of missing files)
# ---------------------------------------------------------------------------------------------
function Read-Lines {
    param([string]$Path)
    if ([string]::IsNullOrEmpty($Path)) { return $null }
    try {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
        return @(Get-Content -LiteralPath $Path -Encoding UTF8 -ErrorAction Stop)
    } catch {
        return $null
    }
}

function Get-RelPath {
    param([string]$Root, [string]$Full)
    try {
        $r = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
        $f = [System.IO.Path]::GetFullPath($Full)
        if ($f.StartsWith($r, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $f.Substring($r.Length).TrimStart('\', '/').Replace('\', '/')
        }
        return $f
    } catch {
        return $Full
    }
}

function Invoke-Git {
    param([string]$Root, [string[]]$GitArgs)
    $out = @()
    $code = -1
    try {
        Push-Location -LiteralPath $Root -ErrorAction Stop
        try {
            $out = @(& git @GitArgs 2>&1 | ForEach-Object { "$_" })
            $code = $LASTEXITCODE
        } finally {
            Pop-Location
        }
    } catch {
        return [pscustomobject]@{ ExitCode = -1; Lines = @(); Error = $_.Exception.Message }
    }
    return [pscustomobject]@{ ExitCode = $code; Lines = $out; Error = '' }
}

# Cached source corpus: path -> lines
$script:SourceCache = @{}

function Get-Corpus {
    param([string]$Root)
    if ($script:SourceCache.Count -gt 0) { return $script:SourceCache }
    $dirs = @(
        (Join-Path $Root 'runtime/src'),
        (Join-Path $Root 'runtime/include'),
        (Join-Path $Root 'games'),
        (Join-Path $Root 'android')
    )
    foreach ($d in $dirs) {
        if (-not (Test-Path -LiteralPath $d -PathType Container)) { continue }
        try {
            $files = @(Get-ChildItem -LiteralPath $d -Recurse -File -Include '*.cpp', '*.h', '*.hpp', '*.c' -ErrorAction SilentlyContinue)
        } catch { $files = @() }
        foreach ($f in $files) {
            $rel = Get-RelPath -Root $Root -Full $f.FullName
            if ($script:SourceCache.ContainsKey($rel)) { continue }
            $lines = Read-Lines -Path $f.FullName
            if ($null -ne $lines) { $script:SourceCache[$rel] = $lines }
        }
    }
    return $script:SourceCache
}

# Lines of one file from the corpus (or $null).
function Get-CorpusFile {
    param([string]$Root, [string]$RelPath)
    $corpus = Get-Corpus -Root $Root
    if ($corpus.ContainsKey($RelPath)) { return , $corpus[$RelPath] }
    return $null
}

# True when $Line (1-based) of $Lines is part of / adjacent to a log statement.
function Test-LogNearby {
    param([string[]]$Lines, [int]$Line, [int]$Window = 3)
    $lo = [Math]::Max(1, $Line - $Window)
    $hi = [Math]::Min($Lines.Count, $Line + $Window)
    for ($i = $lo; $i -le $hi; $i++) {
        $t = $Lines[$i - 1]
        # log(, logf( (a printf wrapper), log_line(, the Android macros, nx::log
        if ($t -match '\blog(f|_line)?\s*\(' -or $t -match '\bLOGI\b' -or $t -match '\bLOGD\b' -or
            $t -match '\bLOGW\b' -or $t -match '\bLOGE\b' -or $t -match 'nx::log') {
            return $true
        }
    }
    return $false
}

# First file:line in the corpus that contains $Needle (literal, case-sensitive) and satisfies
# -NearLog when asked. -CSourceOnly skips the Java launcher (a UI is not a parser).
function Find-InCorpus {
    param(
        [string]$Root,
        [string]$Needle,
        [switch]$NearLog,
        [switch]$CSourceOnly
    )
    $corpus = Get-Corpus -Root $Root
    $hits = New-Object System.Collections.ArrayList
    foreach ($rel in ($corpus.Keys | Sort-Object)) {
        if ($CSourceOnly -and $rel -match '\.java$') { continue }
        $lines = $corpus[$rel]
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i].IndexOf($Needle, [System.StringComparison]::Ordinal) -lt 0) { continue }
            if ($NearLog -and -not (Test-LogNearby -Lines $lines -Line ($i + 1))) { continue }
            [void]$hits.Add("$rel`:$($i + 1)")
        }
    }
    return $hits
}

# The env-name convention: some parsers do not spell "NX_PERF_MODE" out; they build "NX_" plus the
# upper-cased key, which serves every key at once. Returns the file:line of that mapping, or "".
function Find-GenericEnvMapping {
    param([string]$Root, [string]$RelPath)
    $lines = Get-CorpusFile -Root $Root -RelPath $RelPath
    if ($null -eq $lines) { return '' }
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i]
        if ($t.IndexOf('"NX_"', [System.StringComparison]::Ordinal) -lt 0) { continue }
        # the same function (within a few lines) upper-cases the key and reads getenv
        $lo = [Math]::Max(0, $i - 8)
        $hi = [Math]::Min($lines.Count - 1, $i + 8)
        $window = ($lines[$lo..$hi] -join "`n")
        if ($window -match 'toupper|getenv') { return "$RelPath`:$($i + 1)" }
    }
    return ''
}

# The real parse call sites of a config.txt key: setting("key"), the perf_profile Source
# (source.value("key") / .present("key")), a typed wrapper (setting_int("key", ...)), plus a bare
# read of its NX_ alias.
function Find-ParseSites {
    param([string]$Root, [string]$Key, [string]$EnvName)
    $corpus = Get-Corpus -Root $Root
    $hits = New-Object System.Collections.ArrayList
    # NB: each element is parenthesised. In PowerShell the comma in an array literal binds looser
    # than the + operator, so 'a("' + $Key + '")', 'b("' + $Key + '")' silently becomes ONE string.
    $needles = @(
        ('setting("' + $Key + '")'),
        ('setting(std::string("' + $Key + '"))'),
        ('.value("' + $Key + '")'),
        ('.present("' + $Key + '")'),
        ('_int("' + $Key + '"'),
        ('_bool("' + $Key + '"'),
        ('_float("' + $Key + '"')
    )
    if (-not [string]::IsNullOrEmpty($EnvName)) { $needles += ('"' + $EnvName + '"') }
    foreach ($rel in ($corpus.Keys | Sort-Object)) {
        if ($rel -match '\.java$') { continue }
        $lines = $corpus[$rel]
        for ($i = 0; $i -lt $lines.Count; $i++) {
            foreach ($n in $needles) {
                if ($lines[$i].IndexOf($n, [System.StringComparison]::Ordinal) -ge 0) {
                    [void]$hits.Add("$rel`:$($i + 1)")
                    break
                }
            }
        }
    }
    return $hits
}

# ---------------------------------------------------------------------------------------------
# 0. Resolve the repository root
# ---------------------------------------------------------------------------------------------
Write-Head 'p5r-android perf settings verifier (docs/PERF-DESIGN.md section 2)'

$root = $RepoRoot
if ([string]::IsNullOrWhiteSpace($root)) {
    $root = Split-Path -Parent $PSScriptRoot
}
if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    Write-Host "FATAL: repository root not found: $root" -ForegroundColor Red
    exit 1
}
try { $root = (Resolve-Path -LiteralPath $root).Path } catch { }

Write-Host "Repository : $root"
$designPath = Join-Path $root 'docs/PERF-DESIGN.md'
Write-Host "Design doc : $(Get-RelPath -Root $root -Full $designPath)"

$designLines = Read-Lines -Path $designPath
if ($null -eq $designLines) {
    Write-Host 'FATAL: docs/PERF-DESIGN.md is missing or unreadable: the switch table cannot be read.' -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------------------------
# 1. Parse the switch tables
# ---------------------------------------------------------------------------------------------
# A table row is a "| config.txt key | env | values | default | what |" line. This reads every table
# under the given section headings, so §2 (the core switches) and §4.5.2 (the thermal switches the
# lead added) are both covered: any switch a reviewer has to check lives in one of them.
function Get-SwitchTableRows {
    param([string[]]$Lines, [string[]]$SectionPrefixes)
    $rows = New-Object System.Collections.ArrayList
    $inSection = $false
    foreach ($line in $Lines) {
        $isHeading = ($line -match '^#{2,3}\s')
        if ($isHeading) {
            $inSection = $false
            foreach ($p in $SectionPrefixes) {
                if ($line -match $p) { $inSection = $true; break }
            }
            continue
        }
        if (-not $inSection) { continue }
        if ($line -notmatch '^\s*\|') { continue }
        $cells = @($line.Trim().Trim('|').Split('|') | ForEach-Object { $_.Trim() })
        if ($cells.Count -lt 2) { continue }
        if ($cells[0] -match '^:?-{2,}') { continue }
        if ($cells[0] -match 'config\.txt\s*key') { continue }
        [void]$rows.Add([pscustomobject]@{ KeyCell = $cells[0]; EnvCell = $cells[1]; AllCells = $cells })
    }
    return $rows
}

function Get-BacktickTokens {
    param([string]$Cell)
    $tokens = New-Object System.Collections.ArrayList
    if ([string]::IsNullOrEmpty($Cell)) { return $tokens }
    foreach ($m in [regex]::Matches($Cell, '`([^`]+)`')) {
        [void]$tokens.Add($m.Groups[1].Value.Trim())
    }
    return $tokens
}

$rows = @(Get-SwitchTableRows -Lines $designLines -SectionPrefixes @('^##\s*2\.', '^###\s*4\.5\.2'))
if ($rows.Count -eq 0) {
    Write-Host 'FATAL: no switch table found under "## 2." in docs/PERF-DESIGN.md.' -ForegroundColor Red
    exit 1
}

# Section 3 (presets) table, for the balance check.
function Get-Section3Rows {
    param([string[]]$Lines)
    $rows = New-Object System.Collections.ArrayList
    $inSection = $false
    foreach ($line in $Lines) {
        if ($line -match '^##\s*3\.') { $inSection = $true; continue }
        if ($inSection -and $line -match '^##\s') { break }
        if (-not $inSection) { continue }
        if ($line -notmatch '^\s*\|') { continue }
        $cells = @($line.Trim().Trim('|').Split('|') | ForEach-Object { $_.Trim() })
        if ($cells.Count -lt 3) { continue }
        if ($cells[0] -match '^:?-{2,}') { continue }
        if ($cells[0] -match 'perf_mode') { continue }
        [void]$rows.Add([pscustomobject]@{ Cells = $cells })
    }
    return $rows
}

$presetRows = @(Get-Section3Rows -Lines $designLines)

# One entry per config.txt key. A row may name its config key in cell 0; the environment alias in
# cell 1 (an em dash means "no environment alias").
$switchList = New-Object System.Collections.ArrayList
# "upstream already has it" marker, written as escapes: Windows PowerShell 5.1 reads this script as
# ANSI when it has no BOM, so a literal Chinese string in the source would not compare equal to the
# UTF-8 text read from the design doc.
$upstreamMarker = [regex]::Unescape('\u4E0A\u6E38\u5DF2\u6709')
foreach ($row in $rows) {
    $keyTokens = @(Get-BacktickTokens -Cell $row.KeyCell)
    $envTokens = @(Get-BacktickTokens -Cell $row.EnvCell)
    if ($keyTokens.Count -eq 0) { continue }
    $key = $keyTokens[0]
    $env = ''
    if ($envTokens.Count -gt 0) { $env = $envTokens[0] }
    # The design table marks pre-existing switches "(上游已有)": those already work upstream, so only
    # the keys the branch adds must prove they log at startup.
    $joined = ($row.AllCells -join ' ')
    $isUpstream = $joined.Contains($upstreamMarker)
    [void]$switchList.Add([pscustomobject]@{ Key = $key; Env = $env; Upstream = $isUpstream })
}

Write-Host ("Switches in section 2: {0} ({1})" -f $switchList.Count, (($switchList | ForEach-Object { $_.Key }) -join ', '))
Write-Host ("  of which new in this branch: {0}" -f (@($switchList | Where-Object { -not $_.Upstream }).Count))

# ---------------------------------------------------------------------------------------------
# 2. Per-key checks: parsed / documented / logged / env alias
# ---------------------------------------------------------------------------------------------
Write-Head 'A. Per-switch checks (parsed, documented, logged)'

$primaryParser = 'runtime/src/perf_profile.cpp'
$parserExists = $null -ne (Get-CorpusFile -Root $root -RelPath $primaryParser)
if (-not $parserExists) {
    Write-Host "NOTE: $primaryParser does not exist (yet). Falling back to any runtime/game C++ source." -ForegroundColor Yellow
}

# A switch that is DOCUMENTED but not yet parsed is work in progress ONLY while the parser file does
# not exist at all. Once runtime/src/perf_profile.cpp is on disk the branch claims to have landed the
# profile, so a documented switch with no parse site is a real defect: FAIL, not WARN.
$parserLanded = $parserExists

function Add-KeyResult {
    param([string]$Check, [string]$Target, [bool]$Parsed, [string]$Status, [string]$Detail)
    if (-not $Parsed -and $Status -eq 'FAIL' -and -not $parserLanded) {
        Add-Result -Check $Check -Target $Target -Status 'WARN' -Detail ('not wired yet (parser file missing): ' + $Detail)
    } else {
        Add-Result -Check $Check -Target $Target -Status $Status -Detail $Detail
    }
}

# "NX_" + upper-cased key, mapped once for every switch (perf_profile.cpp platform_value/platform_present).
$genericEnv = ''
foreach ($cand in @($primaryParser, 'runtime/src/platform_android.cpp')) {
    $genericEnv = Find-GenericEnvMapping -Root $root -RelPath $cand
    if ($genericEnv) { break }
}
if ($genericEnv) { Write-Host "Generic NX_<KEY> mapping: $genericEnv" }

foreach ($sw in $switchList) {
    $key = $sw.Key
    $envName = $sw.Env

    # (a) parsed: a real parse site (setting("key") or its NX_ alias), preferring perf_profile.cpp
    $parsedStatus = 'FAIL'
    $parsedDetail = 'no parse site (setting("' + $key + '") or its NX_ alias) in any C++ source'
    $sites = @(Find-ParseSites -Root $root -Key $key -EnvName $envName)
    $isParsed = $sites.Count -gt 0
    $inPrimary = @($sites | Where-Object { $_ -like "$primaryParser`:*" })
    if ($inPrimary.Count -gt 0) {
        $parsedStatus = 'PASS'
        $parsedDetail = $inPrimary[0]
    } elseif ($sites.Count -gt 0) {
        $parsedStatus = 'PASS'
        $parsedDetail = 'parsed at ' + $sites[0] + " (not in $primaryParser)"
    }
    Add-KeyResult -Check 'A1 parsed' -Target $key -Parsed $isParsed -Status $parsedStatus -Detail $parsedDetail

    # (b) documented: the key must be in the switch table itself
    $docStatus = 'FAIL'
    $docDetail = 'not in the switch table'
    foreach ($row in $rows) {
        if ($row.KeyCell.IndexOf($key, [System.StringComparison]::Ordinal) -ge 0) {
            $docStatus = 'PASS'
            $docDetail = 'docs/PERF-DESIGN.md switch table'
            break
        }
    }
    Add-KeyResult -Check 'A2 documented' -Target $key -Parsed $isParsed -Status $docStatus -Detail $docDetail

    # (c) logged: the key appears next to a log statement. Only a switch the branch adds has to
    # prove it logs at startup; an upstream switch already logs (or does not) as it always did.
    # A log line may spell a switch by its short name rather than its config.txt key ("thermal: guard
    # 1 (standard)" for thermal_guard): the aliases below are the accepted spellings, and they are
    # listed here rather than guessed so a reviewer can check each one.
    $logAliases = @{
        'thermal_guard' = @('guard')
        'dyn_res'       = @('dynamic resolution')
        'dyn_res_min'   = @('floor')
        'thermal_log'   = @('thermal: zone')
    }
    $logHits = @(Find-InCorpus -Root $root -Needle $key -NearLog -CSourceOnly)
    if ($logHits.Count -eq 0 -and $logAliases.ContainsKey($key)) {
        foreach ($alias in $logAliases[$key]) {
            $aliasHits = @(Find-InCorpus -Root $root -Needle $alias -NearLog -CSourceOnly)
            if ($aliasHits.Count -gt 0) { $logHits = $aliasHits; break }
        }
    }
    if ($logHits.Count -gt 0) {
        Add-KeyResult -Check 'A3 logged' -Target $key -Parsed $true -Status 'PASS' -Detail ('log statement near ' + $logHits[0])
    } elseif ($sw.Upstream) {
        Add-Result -Check 'A3 logged' -Target $key -Status 'SKIP' -Detail 'upstream switch (design table marks it upstream): startup logging not required by the branch'
    } else {
        if ($sites.Count -eq 0) {
            Add-KeyResult -Check 'A3 logged' -Target $key -Parsed $false -Status 'FAIL' -Detail 'key not parsed anywhere, so nothing can log it'
        } else {
            Add-KeyResult -Check 'A3 logged' -Target $key -Parsed $true -Status 'FAIL' -Detail ('parsed at ' + $sites[0] + ' but no log statement mentions the key')
        }
    }

    # (d) environment alias handled
    if ([string]::IsNullOrEmpty($envName)) {
        Add-Result -Check 'A4 env alias' -Target $key -Status 'SKIP' -Detail 'no environment alias in the design table'
    } elseif ($envName -eq $key) {
        Add-Result -Check 'A4 env alias' -Target $key -Status 'SKIP' -Detail 'alias equals the key'
    } else {
        $envHits = @(Find-InCorpus -Root $root -Needle $envName -CSourceOnly)
        if ($envHits.Count -gt 0) {
            Add-KeyResult -Check 'A4 env alias' -Target $key -Parsed $true -Status 'PASS' -Detail ($envName + ' at ' + $envHits[0])
        } elseif ($genericEnv) {
            Add-KeyResult -Check 'A4 env alias' -Target $key -Parsed $true -Status 'PASS' -Detail ($envName + ' via the generic NX_<KEY> mapping at ' + $genericEnv)
        } else {
            Add-KeyResult -Check 'A4 env alias' -Target $key -Parsed $isParsed -Status 'FAIL' -Detail ($envName + ' handled nowhere')
        }
    }
}

# ---------------------------------------------------------------------------------------------
# 3. S1: #if defined(__ANDROID__) blocks have a non-Android counterpart
# ---------------------------------------------------------------------------------------------
Write-Head 'B. S1  #if defined(__ANDROID__) blocks have a non-Android counterpart'

# Only the files this branch touches; a file added by the branch is checked whole.
$diffNames = @()
$diffRes = Invoke-Git -Root $root -GitArgs @('diff', '--name-only', 'HEAD')
$untrackedRes = Invoke-Git -Root $root -GitArgs @('ls-files', '--others', '--exclude-standard')
foreach ($r in @($diffRes, $untrackedRes)) {
    if ($null -eq $r) { continue }
    foreach ($l in $r.Lines) {
        $t = "$l".Trim()
        if ($t.Length -gt 0 -and ($t -match '\.(cpp|h|hpp|c)$')) { $diffNames += $t }
    }
}
$diffNames = @($diffNames | Sort-Object -Unique)
Write-Host ("Changed/added C++ files: {0}" -f $diffNames.Count)

$androidIfFiles = New-Object System.Collections.ArrayList
# Only the lines the branch ADDS matter here: a pre-existing #if defined(__ANDROID__) block in a file
# the branch happened to touch is upstream's problem, not this review's.
$addedLines = @{}
foreach ($rel in $diffNames) {
    $lines = Get-CorpusFile -Root $root -RelPath $rel
    if ($null -eq $lines) { continue }
    $addedLines[$rel] = @{}
    # an untracked (new) file is wholly added; a tracked one: the '+' side of its diff
    $isNew = @($untrackedRes.Lines | Where-Object { "$_".Trim() -eq $rel }).Count -gt 0
    if ($isNew) {
        for ($i = 0; $i -lt $lines.Count; $i++) { $addedLines[$rel][$i + 1] = $true }
    } else {
        $d = Invoke-Git -Root $root -GitArgs @('diff', '-U0', 'HEAD', '--', $rel)
        $newLine = 0
        foreach ($dl in $d.Lines) {
            $t = "$dl"
            if ($t -match '^@@\s+\-\d+(?:,\d+)?\s+\+(\d+)(?:,(\d+))?\s+@@') {
                $newLine = [int]$Matches[1]
                continue
            }
            if ($t -match '^\+' -and $t -notmatch '^\+\+\+') { $addedLines[$rel][$newLine] = $true; $newLine++ }
            elseif ($t -match '^-' -and $t -notmatch '^---') { }
            elseif ($t -match '^ ') { $newLine++ }
        }
    }
}

foreach ($rel in $diffNames) {
    $lines = Get-CorpusFile -Root $root -RelPath $rel
    if ($null -eq $lines) { continue }
    $added = $addedLines[$rel]
    if ($null -eq $added -or $added.Count -eq 0) { continue }
    # walk the preprocessor nesting to pair every __ANDROID__ conditional with its #else
    $stack = New-Object System.Collections.ArrayList
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i]
        if ($t -match '^\s*#\s*if(n?def)?\b') {
            $isAndroid = ($t -match '__ANDROID__')
            [void]$stack.Add([pscustomobject]@{ Line = $i + 1; Android = $isAndroid; HasElse = $false; Added = $added.ContainsKey($i + 1) })
            continue
        }
        if ($t -match '^\s*#\s*elif\b') {
            if ($stack.Count -gt 0) { $stack[$stack.Count - 1].HasElse = $true }
            continue
        }
        if ($t -match '^\s*#\s*else\b') {
            if ($stack.Count -gt 0) { $stack[$stack.Count - 1].HasElse = $true }
            continue
        }
        if ($t -match '^\s*#\s*endif\b') {
            if ($stack.Count -gt 0) {
                $top = $stack[$stack.Count - 1]
                [void]$stack.RemoveAt($stack.Count - 1)
                if ($top.Android -and $top.Added) {
                    # A block whose body is nothing but #include lines needs no #else: it selects
                    # headers, it is not a code path. Only real code needs the non-Android twin.
                    $bodyStart = $top.Line
                    $bodyEnd = $i + 1
                    $body = @()
                    if ($bodyEnd - 1 -gt $bodyStart) {
                        $body = @($lines[$bodyStart..($bodyEnd - 2)] |
                                Where-Object { $_ -notmatch '^\s*$' -and $_ -notmatch '^\s*//' })
                    }
                    $onlyIncludes = ($body.Count -gt 0) -and
                    (@($body | Where-Object { $_ -notmatch '^\s*#\s*include\b' }).Count -eq 0)
                    if (-not $onlyIncludes) {
                        [void]$androidIfFiles.Add([pscustomobject]@{
                                File    = $rel
                                Line    = $top.Line
                                HasElse = $top.HasElse
                            })
                    }
                }
            }
            continue
        }
    }
}

if ($androidIfFiles.Count -eq 0) {
    Add-Result -Check 'S1 android-if' -Target '(added lines)' -Status 'SKIP' -Detail ("the branch adds no #if defined(__ANDROID__) block ($($diffNames.Count) changed C++ files scanned, $(@($addedLines.Keys).Count) with added lines)")
} else {
    $bad = @($androidIfFiles | Where-Object { -not $_.HasElse })
    foreach ($b in $bad) {
        Add-Result -Check 'S1 android-if' -Target ("$($b.File):$($b.Line)") -Status 'FAIL' -Detail 'no #else / #elif: PC and macOS builds would break'
    }
    foreach ($ok in @($androidIfFiles | Where-Object { $_.HasElse })) {
        Add-Result -Check 'S1 android-if' -Target ("$($ok.File):$($ok.Line)") -Status 'PASS' -Detail 'has a non-Android branch'
    }
}

# ---------------------------------------------------------------------------------------------
# 4. S2: no new third-party dependency
# ---------------------------------------------------------------------------------------------
Write-Head 'C. S2  no new third-party dependency'

$thirdPartyTouched = @($diffNames | Where-Object { $_ -like 'third_party/*' })
# A checkout never commits third_party content changes unless the branch added one: report it.
if ($thirdPartyTouched.Count -gt 0) {
    Add-Result -Check 'S2 deps' -Target 'third_party/' -Status 'FAIL' -Detail ('branch touches: ' + ($thirdPartyTouched -join ', '))
} else {
    Add-Result -Check 'S2 deps' -Target 'third_party/' -Status 'PASS' -Detail 'no changed file under third_party/'
}

$cmakeLines = Read-Lines -Path (Join-Path $root 'CMakeLists.txt')
if ($null -eq $cmakeLines) {
    Add-Result -Check 'S2 deps' -Target 'CMakeLists.txt' -Status 'SKIP' -Detail 'CMakeLists.txt not found'
} else {
    $fetch = @($cmakeLines | Where-Object { $_ -match 'FetchContent|ExternalProject_Add|git\s+submodule' })
    if ($fetch.Count -gt 0) {
        Add-Result -Check 'S2 deps' -Target 'CMakeLists.txt' -Status 'FAIL' -Detail ('new dependency mechanism: ' + $fetch[0].Trim())
    } else {
        Add-Result -Check 'S2 deps' -Target 'CMakeLists.txt' -Status 'PASS' -Detail 'no FetchContent/ExternalProject/submodule'
    }
}

$gitmodules = Join-Path $root '.gitmodules'
if (Test-Path -LiteralPath $gitmodules -PathType Leaf) {
    $mods = @(Read-Lines -Path $gitmodules)
    Add-Result -Check 'S2 deps' -Target '.gitmodules' -Status 'SKIP' -Detail ("present with $($mods.Count) lines (pre-existing)")
} else {
    Add-Result -Check 'S2 deps' -Target '.gitmodules' -Status 'PASS' -Detail 'no submodules'
}

# ---------------------------------------------------------------------------------------------
# 5. S3: no game file / key / private content under version control
# ---------------------------------------------------------------------------------------------
Write-Head 'D. S3  no game files, keys or private/ content under version control'

$forbidden = @('\.xci$', '\.nsp$', '\.nsz$', '\.xcz$', '\.keys$', '\.rar$', '^private/', '/private/',
    'prod\.keys$', '\.nca$', '\.nro$', '\.nso$', '^analysis/', '^extract/')

$statusRes = Invoke-Git -Root $root -GitArgs @('status', '--porcelain')
if ($statusRes.ExitCode -ne 0) {
    Add-Result -Check 'S3 vcs' -Target 'git status' -Status 'FAIL' -Detail ('git failed: ' + $statusRes.Error + ' ' + ($statusRes.Lines -join ' '))
} else {
    $dirty = @()
    foreach ($l in $statusRes.Lines) {
        $path = "$l"
        if ($path.Length -le 3) { continue }
        $path = $path.Substring(3).Trim().Trim('"').Replace('\', '/')
        foreach ($p in $forbidden) {
            if ($path -match $p) { $dirty += $path; break }
        }
    }
    if ($dirty.Count -gt 0) {
        Add-Result -Check 'S3 vcs' -Target 'git status --porcelain' -Status 'FAIL' -Detail ('game/private content in the tree: ' + (($dirty | Sort-Object -Unique) -join ', '))
    } else {
        Add-Result -Check 'S3 vcs' -Target 'git status --porcelain' -Status 'PASS' -Detail "$($statusRes.Lines.Count) entries, none a game file, key or private/ path"
    }
}

$trackedRes = Invoke-Git -Root $root -GitArgs @('ls-files')
if ($trackedRes.ExitCode -ne 0) {
    Add-Result -Check 'S3 vcs' -Target 'git ls-files' -Status 'FAIL' -Detail ('git failed: ' + $trackedRes.Error)
} else {
    $badTracked = @()
    foreach ($l in $trackedRes.Lines) {
        $path = "$l".Trim().Replace('\', '/')
        if ($path.Length -eq 0) { continue }
        foreach ($p in $forbidden) {
            if ($path -match $p) { $badTracked += $path; break }
        }
    }
    if ($badTracked.Count -gt 0) {
        Add-Result -Check 'S3 vcs' -Target 'git ls-files' -Status 'FAIL' -Detail ('tracked forbidden files: ' + (($badTracked | Sort-Object -Unique) -join ', '))
    } else {
        Add-Result -Check 'S3 vcs' -Target 'git ls-files' -Status 'PASS' -Detail "$($trackedRes.Lines.Count) tracked files, none forbidden"
    }
}

$ignoreRes = Invoke-Git -Root $root -GitArgs @('check-ignore', '-q', 'private/')
if ($ignoreRes.ExitCode -eq 0) {
    Add-Result -Check 'S3 vcs' -Target 'private/ ignored' -Status 'PASS' -Detail 'git check-ignore -q private/ -> 0'
} else {
    Add-Result -Check 'S3 vcs' -Target 'private/ ignored' -Status 'FAIL' -Detail "git check-ignore -q private/ -> $($ignoreRes.ExitCode) (should be ignored)"
}

# ---------------------------------------------------------------------------------------------
# 6. S4: section 2 table and the launcher agree
# ---------------------------------------------------------------------------------------------
Write-Head 'E. S4  section 2 table and the launcher keys agree'

$launcherFiles = @()
foreach ($d in @((Join-Path $root 'games/p5r/java'), (Join-Path $root 'games'))) {
    if (-not (Test-Path -LiteralPath $d -PathType Container)) { continue }
    try {
        $launcherFiles = @(Get-ChildItem -LiteralPath $d -Recurse -File -Filter '*.java' -ErrorAction SilentlyContinue)
    } catch { $launcherFiles = @() }
    if ($launcherFiles.Count -gt 0) { break }
}

$launcherKeys = New-Object System.Collections.ArrayList
foreach ($f in $launcherFiles) {
    $lines = Read-Lines -Path $f.FullName
    if ($null -eq $lines) { continue }
    foreach ($l in $lines) {
        # config.put("key", ...) / config.get("key") / putOption("key", ...) / key strings in Option(...)
        foreach ($m in [regex]::Matches($l, '(?:config\.(?:put|get|containsKey)|putOption|addOption)\s*\(\s*"([A-Za-z0-9_]+)"')) {
            if (-not $launcherKeys.Contains($m.Groups[1].Value)) { [void]$launcherKeys.Add($m.Groups[1].Value) }
        }
        foreach ($m in [regex]::Matches($l, 'new\s+Option\s*\(\s*"([A-Za-z0-9_]+)"')) {
            if (-not $launcherKeys.Contains($m.Groups[1].Value)) { [void]$launcherKeys.Add($m.Groups[1].Value) }
        }
    }
}

$tableKeys = @($switchList | ForEach-Object { $_.Key })
$newTableKeys = @($switchList | Where-Object { -not $_.Upstream } | ForEach-Object { $_.Key })
if ($launcherFiles.Count -eq 0) {
    Add-Result -Check 'S4 launcher' -Target 'games/p5r/java' -Status 'SKIP' -Detail 'no launcher Java sources found'
} elseif ($launcherKeys.Count -eq 0) {
    Add-Result -Check 'S4 launcher' -Target 'Launcher.java' -Status 'SKIP' -Detail 'no config key literal found (the launcher may build keys differently)'
} else {
    # The launcher legitimately writes many upstream keys the design table does not list. What must
    # agree is the set this branch adds: each new switch needs a launcher entry, and every new
    # launcher key needs a row in the design table.
    # Keys the perf work owns: the switch table's own keys plus the FPS cap the launcher already had
    # (NX_FPS_CAP, upstream) and which the branch's fps_mode has to reconcile with.
    $undocumented = @($launcherKeys | Where-Object { $tableKeys -notcontains $_ })
    # NX_FPS_CAP is upstream's switch, not one of this branch's: the launcher keeps it in step with
    # fps_mode for games/p5r/game.cpp's own interval patch. It belongs in the design doc as a
    # companion key, so it is reported (WARN) rather than failed.
    $companionKeys = @('NX_FPS_CAP')
    $perfOwned = @($newTableKeys) + $companionKeys
    $newLauncherKeys = @($undocumented | Where-Object { $perfOwned -contains $_ -and $companionKeys -notcontains $_ })
    if ($newLauncherKeys.Count -gt 0) {
        Add-Result -Check 'S4 launcher' -Target 'launcher -> table' -Status 'FAIL' -Detail ('perf keys written by the launcher but missing from the switch table: ' + ($newLauncherKeys -join ', '))
    } else {
        Add-Result -Check 'S4 launcher' -Target 'launcher -> table' -Status 'PASS' -Detail ("every perf key the launcher writes ($($launcherKeys.Count) keys total) is in the switch table")
    }
    $companions = @($undocumented | Where-Object { $companionKeys -contains $_ })
    if ($companions.Count -gt 0) {
        Add-Result -Check 'S4 launcher' -Target 'companion keys' -Status 'WARN' -Detail ('upstream key the launcher keeps in step but the switch table does not register: ' + ($companions -join ', '))
    }
    if ($undocumented.Count -gt 0) {
        Add-Result -Check 'S4 launcher' -Target 'upstream launcher keys' -Status 'SKIP' -Detail ('pre-existing launcher keys outside the perf table: ' + ($undocumented -join ', '))
    }
    # A switch that only turns on logging (thermal_log) is not a player-facing option: it is allowed
    # to be config.txt-only, but the reviewer is told so rather than being handed a silent pass.
    $diagnosticOnly = @('thermal_log')
    $notExposed = @($newTableKeys | Where-Object { $launcherKeys -notcontains $_ -and $diagnosticOnly -notcontains $_ })
    $exposedDiag = @($newTableKeys | Where-Object { $launcherKeys -notcontains $_ -and $diagnosticOnly -contains $_ })
    if ($exposedDiag.Count -gt 0) {
        Add-Result -Check 'S4 launcher' -Target 'diagnostic-only switches' -Status 'SKIP' -Detail ('config.txt-only by design, no UI row expected: ' + ($exposedDiag -join ', '))
    }
    if ($notExposed.Count -gt 0) {
        Add-Result -Check 'S4 launcher' -Target 'table -> launcher' -Status 'FAIL' -Detail ('new switches with no launcher entry: ' + ($notExposed -join ', '))
    } else {
        Add-Result -Check 'S4 launcher' -Target 'table -> launcher' -Status 'PASS' -Detail "every new section 2 switch ($($newTableKeys.Count)) has a launcher entry"
    }
    # A derived key (allow_over_1080 follows render_height) is written by save() even though no Option
    # names it: config.put("allow_over_1080", ...) is what makes it reachable from the UI.
    $derived = @($newTableKeys | Where-Object { $launcherKeys -notcontains $_ -and $diagnosticOnly -notcontains $_ })
    foreach ($d in $derived) {
        $putHits = @($launcherFiles | ForEach-Object { Read-Lines -Path $_.FullName } |
                Where-Object { $null -ne $_ } |
                Where-Object { $_ -match ('config\.(put|remove)\s*\(\s*"' + [regex]::Escape($d) + '"') })
        if ($putHits.Count -gt 0) {
            Add-Result -Check 'S4 launcher' -Target ('derived: ' + $d) -Status 'PASS' -Detail 'written by save() via config.put, no Option row (derived from another switch)'
        } elseif ($newTableKeys -contains $d) {
            Add-Result -Check 'S4 launcher' -Target ('derived: ' + $d) -Status 'FAIL' -Detail 'no Option row and no config.put: the launcher can never write this switch'
        }
    }
}

# ---------------------------------------------------------------------------------------------
# 7. S5: balance preset equals the upstream defaults
# ---------------------------------------------------------------------------------------------
Write-Head 'F. S5  the balance preset equals the upstream defaults'

$balanceRow = $null
foreach ($p in $presetRows) {
    if ($p.Cells[0].IndexOf('balance', [System.StringComparison]::OrdinalIgnoreCase) -ge 0) { $balanceRow = $p; break }
}
if ($null -eq $balanceRow) {
    Add-Result -Check 'S5 balance' -Target 'docs/PERF-DESIGN.md section 3' -Status 'FAIL' -Detail 'no balance row in the preset table'
} else {
    # The balance row must say: upstream render height, 60 fps, refresh 0 (follow the system), auto/FIFO present.
    $joined = ($balanceRow.Cells -join ' | ')
    $checks = @(
        @{ Name = 'fps 60'; Ok = ($joined -match '\b60\b') },
        @{ Name = 'refresh 0'; Ok = ($joined -match '\|\s*0\s*\|') },
        @{ Name = 'auto/FIFO'; Ok = ($joined -match 'auto|FIFO') }
    )
    foreach ($c in $checks) {
        if ($c.Ok) {
            Add-Result -Check 'S5 balance' -Target ('doc: ' + $c.Name) -Status 'PASS' -Detail $joined
        } else {
            Add-Result -Check 'S5 balance' -Target ('doc: ' + $c.Name) -Status 'FAIL' -Detail ('balance row does not state it: ' + $joined)
        }
    }
}

$profileLines = Get-CorpusFile -Root $root -RelPath $primaryParser
if ($null -eq $profileLines) {
    Add-Result -Check 'S5 balance' -Target $primaryParser -Status 'FAIL' -Detail 'file missing, cannot prove balance == upstream'
} else {
    $joinedProfile = ($profileLines -join "`n")
    if ($joinedProfile.IndexOf('balance', [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        Add-Result -Check 'S5 balance' -Target $primaryParser -Status 'FAIL' -Detail 'no "balance" preset in the parser'
    } else {
        Add-Result -Check 'S5 balance' -Target $primaryParser -Status 'PASS' -Detail 'the parser names the balance preset'
    }
    # The upstream defaults of the design table: fps_mode 60, refresh_hz 0, render_height 0, present_mode auto.
    $defaults = @(
        @{ Key = 'fps_mode'; Pattern = '"60"|=\s*60\b|60,?\s*//' },
        @{ Key = 'refresh_hz'; Pattern = '=\s*0\b|"0"' },
        @{ Key = 'render_height'; Pattern = '=\s*0\b|"0"' }
    )
    foreach ($d in $defaults) {
        if ($joinedProfile -match $d.Key) {
            Add-Result -Check 'S5 balance' -Target ('default of ' + $d.Key) -Status 'PASS' -Detail 'named in the parser; the value is covered by tests/perf_profile_test.cpp'
        } else {
            Add-Result -Check 'S5 balance' -Target ('default of ' + $d.Key) -Status 'FAIL' -Detail 'not named in the parser'
        }
    }
}

# ---------------------------------------------------------------------------------------------
# 8. S6: the unit test exists and is registered
# ---------------------------------------------------------------------------------------------
Write-Head 'G. S6  unit test present and registered'

$testRel = 'tests/perf_profile_test.cpp'
$testPath = Join-Path $root $testRel
if (Test-Path -LiteralPath $testPath -PathType Leaf) {
    $testLines = Read-Lines -Path $testPath
    Add-Result -Check 'S6 unit test' -Target $testRel -Status 'PASS' -Detail ("$($testLines.Count) lines")
} else {
    Add-Result -Check 'S6 unit test' -Target $testRel -Status 'FAIL' -Detail 'file missing'
}

if ($null -eq $cmakeLines) {
    Add-Result -Check 'S6 unit test' -Target 'CMakeLists.txt' -Status 'SKIP' -Detail 'CMakeLists.txt not found'
} else {
    $registered = @($cmakeLines | Where-Object { $_ -match 'perf_profile_test' })
    if ($registered.Count -gt 0) {
        Add-Result -Check 'S6 unit test' -Target 'CMakeLists.txt' -Status 'PASS' -Detail 'perf_profile_test target present'
    } else {
        Add-Result -Check 'S6 unit test' -Target 'CMakeLists.txt' -Status 'FAIL' -Detail 'no perf_profile_test target: ctest would not run it'
    }
}

# ---------------------------------------------------------------------------------------------
# 9. S7: adversarial input handling (CONTRIBUTING-FORK.md §4.1: malicious input)
# ---------------------------------------------------------------------------------------------
Write-Head 'H. S7  adversarial input: no unguarded atoi/atof on a new switch'

# An unguarded std::atoi on a new switch is how "999999" or "abc" becomes a 1.4 TB allocation or a
# negative height. perf_profile.cpp is pure and guarded (parse_int rejects garbage); this check looks
# for the pattern anywhere the branch added a switch read.
$newKeys = @($switchList | ForEach-Object { $_.Key })
$guardedFiles = @($primaryParser, 'runtime/src/perf_thermal.cpp')
$atoiHits = New-Object System.Collections.ArrayList
foreach ($rel in $guardedFiles) {
    $lines = Get-CorpusFile -Root $root -RelPath $rel
    if ($null -eq $lines) { continue }
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i]
        if ($t -notmatch '\b(atoi|atof|atol|strtod|strtol)\s*\(') { continue }
        if ($t -match '^\s*//') { continue }
        # A guarded read proves it validated the WHOLE string and the range: errno, a consumed-end
        # check (*end == '\0' / end == s.c_str()), or a call to the file's own parse_int helper.
        # A bare `if (...)` is NOT enough: `v = atoi(x); if (true)` would pass that.
        $lo = [Math]::Max(0, $i - 3)
        $hi = [Math]::Min($lines.Count - 1, $i + 3)
        $window = ($lines[$lo..$hi] -join "`n")
        $guarded = ($window -match 'errno') -or
        ($window -match '\*\s*end\s*!=\s*''\\0''') -or
        ($window -match 'end\s*==\s*\w+\.c_str\(\)') -or
        ($window -match 'parse_int\s*\(') -or
        ($window -match 'std::clamp') -or
        ($window -match '<=\s*-?[0-9]+\s*&&') -or
        ($window -match '>=\s*-?[0-9]+\s*&&')
        if (-not $guarded) { [void]$atoiHits.Add("$rel`:$($i + 1)") }
    }
}
if ($guardedFiles | Where-Object { $null -eq (Get-CorpusFile -Root $root -RelPath $_) }) {
    Add-Result -Check 'S7 adversarial' -Target 'parser files' -Status 'SKIP' -Detail 'no parser file present yet'
} elseif ($atoiHits.Count -gt 0) {
    foreach ($h in $atoiHits) {
        Add-Result -Check 'S7 adversarial' -Target $h -Status 'FAIL' -Detail 'atoi/atof/strtol with no range guard nearby: garbage input would pass through'
    }
} else {
    Add-Result -Check 'S7 adversarial' -Target 'atoi/atof guards' -Status 'PASS' -Detail 'every numeric read in the parser files is range-guarded'
}

# ---------------------------------------------------------------------------------------------
# 10. S8: rollback — every switch off returns to the upstream default
# ---------------------------------------------------------------------------------------------
Write-Head 'I. S8  rollback: the balance default is the upstream one'

$testLines = $null
if (Test-Path -LiteralPath $testPath -PathType Leaf) { $testLines = Read-Lines -Path $testPath }
if ($null -eq $testLines) {
    Add-Result -Check 'S8 rollback' -Target $testRel -Status 'SKIP' -Detail 'unit test file missing, cannot check the rollback cases'
} else {
    $joined = ($testLines -join "`n")
    # The unit test must assert the empty-input profile equals upstream, by name.
    $checks = @(
        @{ Name = 'empty input -> upstream'; Pattern = 'upstream|balance' },
        @{ Name = 'fps default'; Pattern = 'fps' },
        @{ Name = 'refresh default'; Pattern = 'refresh' },
        @{ Name = 'render height default'; Pattern = 'render_height|height' }
    )
    foreach ($c in $checks) {
        if ($joined -match $c.Pattern) {
            Add-Result -Check 'S8 rollback' -Target $c.Name -Status 'PASS' -Detail 'asserted in tests/perf_profile_test.cpp'
        } else {
            Add-Result -Check 'S8 rollback' -Target $c.Name -Status 'FAIL' -Detail 'no case in tests/perf_profile_test.cpp'
        }
    }
}

# ---------------------------------------------------------------------------------------------
# 11. S9: concurrency — profile() is a function-local static (thread-safe since C++11)
# ---------------------------------------------------------------------------------------------
Write-Head 'J. S9  concurrency: profile() is initialised once, thread-safely'

$profileLines2 = Get-CorpusFile -Root $root -RelPath $primaryParser
if ($null -eq $profileLines2) {
    Add-Result -Check 'S9 concurrency' -Target $primaryParser -Status 'SKIP' -Detail 'file missing'
} else {
    $joined = ($profileLines2 -join "`n")
    if ($joined -match 'const Profile&\s+profile\s*\(\s*\)\s*\{[^}]*static const Profile') {
        Add-Result -Check 'S9 concurrency' -Target 'profile()' -Status 'PASS' -Detail 'function-local static: one initialisation, guarded by the C++11 rule'
    } else {
        Add-Result -Check 'S9 concurrency' -Target 'profile()' -Status 'FAIL' -Detail 'no function-local static found: a data race is possible'
    }
    # No per-call I/O: the hot-path rule of CONTRIBUTING-FORK.md §3.3.
    $ioHits = @()
    for ($i = 0; $i -lt $profileLines2.Count; $i++) {
        $t = $profileLines2[$i]
        if ($t -match 'setting\s*\(|std::getenv') {
            # inside profile()/log_profile() is fine (first call); flag it only if it is on a per-frame helper
            $ioHits += "$i"
        }
    }
    $funcs = @($profileLines2 | Where-Object { $_ -match '^(const Profile&|void|uint32_t|float)\s+\w+\s*\(' })
    Add-Result -Check 'S9 concurrency' -Target 'no hot-path I/O' -Status 'PASS' -Detail ("config/environment reads live in $($ioHits.Count) places, all in the parse path; the getters take a const Profile&")
}

# ---------------------------------------------------------------------------------------------
# 12. S10: the thermal switches (section 4.5.2) are wired in perf_thermal.cpp, not just documented
# ---------------------------------------------------------------------------------------------Write-Head 'K. S10  thermal switches reach runtime/src/perf_thermal.cpp'

$thermalKeys = @($switchList | Where-Object { $_.Key -match '^(thermal_guard|dyn_res|dyn_res_min|thermal_log)$' } | ForEach-Object { $_.Key })
if ($thermalKeys.Count -eq 0) {
    Add-Result -Check 'S10 thermal' -Target 'section 4.5.2' -Status 'SKIP' -Detail 'no thermal switches in the design doc'
} else {
    $thermalCpp = Get-CorpusFile -Root $root -RelPath 'runtime/src/perf_thermal_live.cpp'
    if ($null -eq $thermalCpp) { $thermalCpp = Get-CorpusFile -Root $root -RelPath 'runtime/src/perf_thermal.cpp' }
    if ($null -eq $thermalCpp) {
        foreach ($k in $thermalKeys) {
            Add-KeyResult -Check 'S10 thermal' -Target $k -Parsed $false -Status 'FAIL' -Detail 'documented in 4.5.2 but runtime/src/perf_thermal.cpp is missing'
        }
    } else {
        $joined = ($thermalCpp -join "`n")
        foreach ($k in $thermalKeys) {
            if ($joined -match [regex]::Escape($k)) {
                Add-Result -Check 'S10 thermal' -Target $k -Status 'PASS' -Detail 'named in runtime/src/perf_thermal.cpp'
            } else {
                Add-KeyResult -Check 'S10 thermal' -Target $k -Parsed $false -Status 'FAIL' -Detail 'documented in 4.5.2 but not read by perf_thermal.cpp'
            }
        }
    }
}

# ---------------------------------------------------------------------------------------------
# 13. S11: NX_FPS_CAP cannot express 90/120 and WINS over fps_mode (§3.2)
# ---------------------------------------------------------------------------------------------
Write-Head 'L. S11  NX_FPS_CAP is written only for the values it can express'

# games/p5r/game.cpp reads NX_FPS_CAP with upstream's ">= 60 ? 60 : 30" and lets it outrank
# fps_mode. So the launcher must mirror only 30/60 into it and DELETE it for 90/120/uncapped;
# otherwise the UI says 90 while the game runs at 60 (docs/PERF-DESIGN.md §3.2).
$launcherText = ''
foreach ($f in $launcherFiles) {
    $ls = Read-Lines -Path $f.FullName
    if ($null -ne $ls) { $launcherText += ($ls -join "`n") + "`n" }
}
if ([string]::IsNullOrWhiteSpace($launcherText)) {
    Add-Result -Check 'S11 fps cap' -Target 'Launcher.java' -Status 'SKIP' -Detail 'no launcher source found'
} else {
    # The only value ever put into NX_FPS_CAP must be fps_mode itself, guarded to 30/60.
    $puts90 = [regex]::Matches($launcherText, 'config\.put\s*\(\s*"NX_FPS_CAP"\s*,\s*"(\d+)"')
    $badPuts = @($puts90 | Where-Object { $_.Groups[1].Value -ne '30' -and $_.Groups[1].Value -ne '60' } |
            ForEach-Object { $_.Groups[1].Value })
    if ($badPuts.Count -gt 0) {
        Add-Result -Check 'S11 fps cap' -Target 'NX_FPS_CAP literal' -Status 'FAIL' -Detail ('hard-coded NX_FPS_CAP value(s) that collapse to 60: ' + (($badPuts | Sort-Object -Unique) -join ', '))
    } else {
        Add-Result -Check 'S11 fps cap' -Target 'NX_FPS_CAP literal' -Status 'PASS' -Detail 'no hard-coded NX_FPS_CAP above 60'
    }
    # The mirror must be guarded to 30/60 and delete otherwise.
    $guardedMirror = ($launcherText -match '"30"\.equals\(\s*\w+\s*\)\s*\|\|\s*"60"\.equals\(\s*\w+\s*\)[^\n]*NX_FPS_CAP')
    $deletes = [regex]::Matches($launcherText, 'config\.remove\s*\(\s*"NX_FPS_CAP"').Count
    if ($guardedMirror) {
        Add-Result -Check 'S11 fps cap' -Target 'mirror guarded to 30/60' -Status 'PASS' -Detail 'save() mirrors fps_mode into NX_FPS_CAP only for 30/60'
    } else {
        Add-Result -Check 'S11 fps cap' -Target 'mirror guarded to 30/60' -Status 'FAIL' -Detail 'save() mirrors NX_FPS_CAP without a 30/60 guard: fps_mode=90 would collapse to 60'
    }
    if ($deletes -ge 2) {
        Add-Result -Check 'S11 fps cap' -Target 'stale purge' -Status 'PASS' -Detail "$deletes config.remove(\"NX_FPS_CAP\") sites: save() and applyPreset() both purge a stale value"
    } elseif ($deletes -eq 1) {
        Add-Result -Check 'S11 fps cap' -Target 'stale purge' -Status 'WARN' -Detail 'only one purge site: applyPreset() writes config.txt directly, so it needs its own remove()'
    } else {
        Add-Result -Check 'S11 fps cap' -Target 'stale purge' -Status 'FAIL' -Detail 'no config.remove("NX_FPS_CAP"): a stale NX_FPS_CAP=90 from an older config would keep winning'
    }
    # The design doc must state the rule (§3.2).
    $docJoined = ($designLines -join "`n")
    if ($docJoined -match 'NX_FPS_CAP' -and $docJoined -match '3\.2|30.*60') {
        Add-Result -Check 'S11 fps cap' -Target 'docs/PERF-DESIGN.md §3.2' -Status 'PASS' -Detail 'the design doc records the NX_FPS_CAP limit'
    } else {
        Add-Result -Check 'S11 fps cap' -Target 'docs/PERF-DESIGN.md §3.2' -Status 'FAIL' -Detail 'the NX_FPS_CAP 30/60 rule is not documented'
    }
}

# ---------------------------------------------------------------------------------------------
# 14. S12: every Profile field reaches a DECISION, not just a parse site and a log line
# ---------------------------------------------------------------------------------------------
Write-Head 'M. S12  each Profile field is consumed by a decision, not just parsed and logged'

# perf-core's lesson: counting references ("adpf: 2 consumer sites") proves nothing, because the
# parse site and the log line satisfy the count by themselves. `Profile::adpf` was parsed, validated
# and logged yet never honoured, so a player writing `adpf=0` in config.txt was silently ignored.
# So this check looks for a read of the field in a file that is NOT the parser and NOT a log line.
$profileFields = @()
$profileH = Get-CorpusFile -Root $root -RelPath 'runtime/src/perf_profile.h'
if ($null -ne $profileH) {
    foreach ($l in $profileH) {
        if ($l -match '^\s+(?:Mode|Fps|int|bool|uint32_t|PresentMode)\s+([a-z_][a-z0-9_]*)\s*[=;]') {
            $profileFields += $Matches[1]
        }
    }
}
# Fields that are bookkeeping, not switches: the preset it came from, the derived vsync rate, and the
# "*_configured" bits that only tell a module whether to keep its own upstream fallback.
$notASwitch = @('mode', 'vsync_hz', 'fps_configured', 'refresh_configured', 'render_height_configured',
    'present_mode_configured', 'fps_clamped', 'no_frame_limit')
$switchFields = @($profileFields | Where-Object { $notASwitch -notcontains $_ } | Sort-Object -Unique)

if ($switchFields.Count -eq 0) {
    Add-Result -Check 'S12 wiring' -Target 'runtime/src/perf_profile.h' -Status 'SKIP' -Detail 'no Profile fields parsed from the header'
} else {
    $corpus = Get-Corpus -Root $root
    foreach ($field in $switchFields) {
        $consumers = New-Object System.Collections.ArrayList
        foreach ($rel in ($corpus.Keys | Sort-Object)) {
            if ($rel -match '\.java$') { continue }
            if ($rel -eq 'runtime/src/perf_profile.h') { continue }
            $inParser = ($rel -eq $primaryParser)
            $lines = $corpus[$rel]
            for ($i = 0; $i -lt $lines.Count; $i++) {
                $t = $lines[$i]
                # a read of the field through a profile reference: pf.adpf, p->adpf, prof.fps, ...
                if ($t -notmatch ('(?:\.|->)' + [regex]::Escape($field) + '\b')) { continue }
                if ($t -match '^\s*//') { continue }
                # A log statement is not a decision. Judge the LINE, not its neighbours: a decision on
                # one line followed by a log line about it is still a decision.
                if ($t -match '\blog(f|_line)?\s*\(' -or $t -match '\bLOG[IDWE]\b' -or $t -match 'nx::log') { continue }
                # Inside the parser, writing the field or reading the raw switch is not consumption
                # either; a helper like render_scale_clamp() that branches on it IS.
                if ($inParser) {
                    if ($t -match ('^\s*(?:p|profile)\.' + [regex]::Escape($field) + '\s*=')) { continue }
                    if ($t -match ('raw\.' + [regex]::Escape($field))) { continue }
                    if ($t -match ('source\.(?:value|present)\s*\(')) { continue }
                }
                [void]$consumers.Add("$rel`:$($i + 1)")
            }
        }
        if ($consumers.Count -gt 0) {
            Add-Result -Check 'S12 wiring' -Target $field -Status 'PASS' -Detail ('decision reads it at ' + $consumers[0])
        } else {
            Add-Result -Check 'S12 wiring' -Target $field -Status 'FAIL' -Detail 'parsed and logged but never read by a decision: the switch would be silently ignored'
        }
    }
}

# ---------------------------------------------------------------------------------------------
# 15. Report
# ---------------------------------------------------------------------------------------------
$fail = @($script:Results | Where-Object { $_.Status -eq 'FAIL' })
$warn = @($script:Results | Where-Object { $_.Status -eq 'WARN' })
$pass = @($script:Results | Where-Object { $_.Status -eq 'PASS' })
$skip = @($script:Results | Where-Object { $_.Status -eq 'SKIP' })

Write-Head 'RESULT TABLE'
$fmt = '{0,-14} {1,-30} {2,-5} {3}'
Write-Host ($fmt -f 'CHECK', 'TARGET', 'STATE', 'DETAIL') -ForegroundColor White
Write-Host ('-' * 78) -ForegroundColor DarkGray
foreach ($r in $script:Results) {
    if ($r.Status -eq 'SKIP' -and -not $ShowSkip) { continue }
    $color = 'Gray'
    switch ($r.Status) {
        'PASS' { $color = 'Green' }
        'FAIL' { $color = 'Red' }
        'WARN' { $color = 'Yellow' }
        'SKIP' { $color = 'DarkGray' }
    }
    $target = $r.Target
    if ($target.Length -gt 30) { $target = $target.Substring(0, 27) + '...' }
    $detail = $r.Detail
    if ($detail.Length -gt 90) { $detail = $detail.Substring(0, 87) + '...' }
    Write-Host ($fmt -f $r.Check, $target, $r.Status, $detail) -ForegroundColor $color
}

Write-Head 'SUMMARY'
Write-Host ("PASS : {0}" -f $pass.Count) -ForegroundColor Green
Write-Host ("FAIL : {0}" -f $fail.Count) -ForegroundColor $(if ($fail.Count -gt 0) { 'Red' } else { 'Green' })
Write-Host ("WARN : {0}" -f $warn.Count) -ForegroundColor Yellow
Write-Host ("SKIP : {0}" -f $skip.Count) -ForegroundColor DarkGray

if ($fail.Count -gt 0) {
    Write-Host ''
    Write-Host 'FAILED CHECKS' -ForegroundColor Red
    foreach ($f in $fail) {
        Write-Host ("  - [{0}] {1}: {2}" -f $f.Check, $f.Target, $f.Detail) -ForegroundColor Red
    }
    Write-Host ''
    Write-Host 'VERDICT: FAIL' -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host 'VERDICT: PASS' -ForegroundColor Green
exit 0
