#Requires -Version 5.1
<#
.SYNOPSIS
    Local self-test for the feat/op3pro-2k90 fork: static contract checks, then the host build + ctest.

.DESCRIPTION
    CONTRIBUTING-FORK.md §4 requires `pwsh tools/fork_selftest.ps1` before every commit. This is that script.

    Part A — static checks (no compiler needed, a couple of seconds):
      A1  every `#if defined(__ANDROID__)` block in the changed C++ files has an #else / #elif counterpart
          (CONTRIBUTING-FORK.md §3.4: a missing one breaks the PC/macOS build)
      A2  no tab indentation and no mixed line endings in the files this branch touches
      A3  every workflow under .github/workflows/ parses as YAML and has the shape GitHub needs
      A4  every switch of docs/PERF-DESIGN.md §2 (and §4.5.2) is documented, parsed and logged
      A5  no game data, keys or private/ content staged or tracked
      A6  CMakeLists.txt still registers the unit tests, and the perf tests are among them

    Part B — host build + ctest (only when a C++20 compiler is found):
      configures with -DNX_TESTS=ON in a scratch directory under the temp folder, builds, runs ctest.
      With no generated game code CMakeLists.txt builds only the unit tests, so this needs no game data, no
      SDL3 and no Vulkan loader. If no compiler is found the step is SKIPPED with an explanation, not failed.

    Exit code: 0 when nothing FAILED (SKIP/WARN are fine), 1 when at least one check failed.

.PARAMETER RepoRoot
    Repository root. Defaults to the parent of this script's directory (tools\..).

.PARAMETER SkipBuild
    Static checks only. Useful on a machine with no compiler, and for a fast pre-commit run.

.PARAMETER BuildDir
    Where to configure the host build. Defaults to a fresh directory under the temp folder, so the repository is
    never touched and a stale CMakeCache cannot poison the result.

.PARAMETER Compiler
    The C++ compiler to use (and the C compiler: the same prefix with 'gcc'). Defaults to the first C++20-capable
    compiler found: g++, clang++, or cl on the PATH, plus the MSYS2 g++ that this project's docs use on Windows.

.EXAMPLE
    pwsh tools/fork_selftest.ps1
    pwsh tools/fork_selftest.ps1 -SkipBuild
    pwsh tools/fork_selftest.ps1 -Compiler E:\MSYS2\mingw64\bin\g++.exe -BuildDir F:\Temp\p5r-selftest

.NOTES
    Read-only with respect to the repository: every file it creates lives in -BuildDir (the temp folder by default).
    Output is ASCII so Windows PowerShell 5.1 prints it under any code page.
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [switch]$SkipBuild,
    [string]$BuildDir = '',
    [string]$Compiler = ''
)

$ErrorActionPreference = 'Stop'
# Native commands (cmake, ninja, ctest, compilers) write progress and warnings to stderr, which Windows PowerShell
# would otherwise turn into terminating errors under 'Stop'. The exit code is the signal for those, not the stream.
$PSNativeCommandUseErrorActionPreference = $false

function Invoke-Native {
    # Runs a command, streams its output through the caller's formatter, and returns its exit code. stderr is captured
    # as data, never treated as a failure: a CMake warning must not abort the self-test.
    #
    # Windows PowerShell 5.1 turns a native command's stderr lines into ErrorRecords, and under
    # $ErrorActionPreference = 'Stop' the first one becomes terminating — so cmake's "CMake Warning" would kill the
    # script. The preference is lowered for the duration of the call (and restored), and the exit code is the signal.
    param(
        [string]$Exe,
        [string[]]$Arguments,
        [string]$Indent = '    '
    )
    $saved = $ErrorActionPreference
    $lines = @()
    $code = -1
    try {
        $ErrorActionPreference = 'Continue'
        $lines = @(& $Exe @Arguments 2>&1 | ForEach-Object { "$_" })
        $code = $LASTEXITCODE
    } catch {
        $lines += "$($_.Exception.Message)"
        $code = -1
    } finally {
        $ErrorActionPreference = $saved
    }
    foreach ($line in $lines) { Write-Host ($Indent + $line) -ForegroundColor DarkGray }
    return [pscustomobject]@{ ExitCode = $code; Output = $lines }
}

# ----------------------------------------------------------------------------------------------------------------
# Result plumbing
# ----------------------------------------------------------------------------------------------------------------
$script:Results = New-Object System.Collections.ArrayList

function Add-Result {
    param(
        [string]$Check,
        [string]$Target,
        [ValidateSet('PASS', 'FAIL', 'SKIP', 'WARN')][string]$Status,
        [string]$Detail = ''
    )
    [void]$script:Results.Add([pscustomobject]@{ Check = $Check; Target = $Target; Status = $Status; Detail = $Detail })
    $colour = switch ($Status) { 'PASS' { 'Green' } 'FAIL' { 'Red' } 'WARN' { 'Yellow' } 'SKIP' { 'DarkGray' } }
    $line = '  {0,-6} {1,-40} {2}' -f $Status, $Target, $Detail
    Write-Host $line -ForegroundColor $colour
}

function Write-Head {
    param([string]$Text)
    Write-Host ''
    Write-Host ('=' * 100) -ForegroundColor DarkGray
    Write-Host $Text -ForegroundColor Cyan
    Write-Host ('=' * 100) -ForegroundColor DarkGray
}

# ----------------------------------------------------------------------------------------------------------------
# 0. Root and environment
# ----------------------------------------------------------------------------------------------------------------
$root = if ($RepoRoot) { $RepoRoot } else { Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    Write-Host "FATAL: repository root not found: $root" -ForegroundColor Red
    exit 1
}
$root = (Resolve-Path -LiteralPath $root).Path

Write-Head 'p5r-android fork self-test (feat/op3pro-2k90)'
Write-Host "Repository : $root"
Write-Host "PowerShell : $($PSVersionTable.PSVersion)"

# The files this branch owns. Used by A1/A2 so the checks are about *our* changes, not the whole upstream tree.
function Get-ChangedCppFiles {
    $files = New-Object System.Collections.ArrayList
    foreach ($gitArgs in @(@('diff', '--name-only', 'HEAD'), @('ls-files', '--others', '--exclude-standard'))) {
        try {
            $out = & git -C $root @gitArgs 2>&1
            if ($LASTEXITCODE -ne 0) { continue }
        } catch { continue }
        foreach ($line in @($out)) {
            $t = "$line".Trim().Replace('\', '/')
            if ($t -match '\.(cpp|h|hpp|c)$' -and -not $files.Contains($t)) { [void]$files.Add($t) }
        }
    }
    return @($files | Sort-Object)
}

$changed = @(Get-ChangedCppFiles)
Write-Host "Changed C++: $($changed.Count) file(s)"

# ================================================================================================================
# A1. #if defined(__ANDROID__) blocks need a non-Android branch
# ================================================================================================================
Write-Head 'A1  #if defined(__ANDROID__) blocks have an #else / #elif counterpart'

if ($changed.Count -eq 0) {
    Add-Result -Check 'A1 android-if' -Target '(changed files)' -Status 'SKIP' -Detail 'no changed C++ file'
} else {
    $found = 0
    foreach ($rel in $changed) {
        $path = Join-Path $root $rel
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $lines = @(Get-Content -LiteralPath $path -Encoding UTF8)
        # Walk the preprocessor nesting: push on #if*, mark the frame on #else/#elif, pop on #endif.
        $stack = New-Object System.Collections.ArrayList
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $t = $lines[$i]
            if ($t -match '^\s*#\s*if(n?def)?\b') {
                [void]$stack.Add([pscustomobject]@{ Line = $i + 1; Android = ($t -match '__ANDROID__'); HasElse = $false })
                continue
            }
            if ($t -match '^\s*#\s*(elif|else)\b') {
                if ($stack.Count -gt 0) { $stack[$stack.Count - 1].HasElse = $true }
                continue
            }
            if ($t -match '^\s*#\s*endif\b') {
                if ($stack.Count -gt 0) {
                    $top = $stack[$stack.Count - 1]
                    [void]$stack.RemoveAt($stack.Count - 1)
                    if ($top.Android) {
                        $found++
                        if ($top.HasElse) {
                            Add-Result -Check 'A1 android-if' -Target "${rel}:$($top.Line)" -Status 'PASS' -Detail 'has a non-Android branch'
                        } else {
                            Add-Result -Check 'A1 android-if' -Target "${rel}:$($top.Line)" -Status 'FAIL' -Detail 'no #else / #elif: the PC/macOS build would break'
                        }
                    }
                }
                continue
            }
        }
    }
    if ($found -eq 0) {
        Add-Result -Check 'A1 android-if' -Target '(changed files)' -Status 'SKIP' -Detail 'no __ANDROID__ conditional in the changed files'
    }
}

# ================================================================================================================
# A2. No tab indentation, no mixed line endings, no BOM in the files this branch touches
# ================================================================================================================
Write-Head 'A2  indentation and line endings of the changed files'

$textChanged = @($changed)
try {
    $other = & git -C $root diff --name-only HEAD 2>&1
    foreach ($line in @($other)) {
        $t = "$line".Trim().Replace('\', '/')
        if ($t -match '\.(ps1|sh|yml|yaml|md|java|cmd)$' -and -not $textChanged.Contains($t)) { $textChanged += $t }
    }
} catch { }

if ($textChanged.Count -eq 0) {
    Add-Result -Check 'A2 whitespace' -Target '(changed files)' -Status 'SKIP' -Detail 'no changed text file'
} else {
    foreach ($rel in ($textChanged | Sort-Object -Unique)) {
        $path = Join-Path $root $rel
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $bytes = [System.IO.File]::ReadAllBytes($path)

        # BOM: the C++ sources of this project have none, and a BOM breaks some compilers and `patch`.
        $bom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)

        # Line endings: count CRLF and bare LF. Both are fine on their own (the checkout may be either); a *mix* is not.
        $crlf = 0
        $lf = 0
        for ($i = 0; $i -lt $bytes.Length; $i++) {
            if ($bytes[$i] -eq 10) {
                if ($i -gt 0 -and $bytes[$i - 1] -eq 13) { $crlf++ } else { $lf++ }
            }
        }
        $mixed = ($crlf -gt 0 -and $lf -gt 0)

        # Tab indentation: this project indents with spaces everywhere (verified over runtime/src).
        $tabs = 0
        foreach ($l in (Get-Content -LiteralPath $path -Encoding UTF8)) {
            if ($l -match '^\t') { $tabs++ }
        }

        $problems = @()
        if ($bom) { $problems += 'UTF-8 BOM' }
        if ($mixed) { $problems += "mixed line endings ($crlf CRLF, $lf LF)" }
        if ($tabs -gt 0) { $problems += "$tabs tab-indented line(s)" }

        if ($problems.Count -gt 0) {
            Add-Result -Check 'A2 whitespace' -Target $rel -Status 'FAIL' -Detail ($problems -join '; ')
        } else {
            Add-Result -Check 'A2 whitespace' -Target $rel -Status 'PASS' -Detail 'no BOM, consistent line endings, spaces'
        }
    }
}

# ================================================================================================================
# A3. Workflows parse and have the shape GitHub needs
# ================================================================================================================
Write-Head 'A3  .github/workflows YAML validity'

$workflowDir = Join-Path $root '.github/workflows'
$python = $null
foreach ($candidate in @(
        'C:\Users\lin20\AppData\Local\Programs\Python\Python312\python.exe',
        'python', 'python3', 'py')) {
    try {
        $cmd = Get-Command $candidate -ErrorAction Stop
        $null = & $cmd.Source -c 'import yaml' 2>&1
        if ($LASTEXITCODE -eq 0) { $python = $cmd.Source; break }
    } catch { }
}

if (-not (Test-Path -LiteralPath $workflowDir -PathType Container)) {
    Add-Result -Check 'A3 workflows' -Target '.github/workflows' -Status 'SKIP' -Detail 'no workflow directory'
} else {
    $workflows = @(Get-ChildItem -LiteralPath $workflowDir -File -Filter '*.yml') +
                 @(Get-ChildItem -LiteralPath $workflowDir -File -Filter '*.yaml')
    if ($workflows.Count -eq 0) {
        Add-Result -Check 'A3 workflows' -Target '.github/workflows' -Status 'FAIL' -Detail 'no workflow file: the fork has no cloud build'
    } elseif (-not $python) {
        # No PyYAML anywhere: fall back to a shape check that needs no parser.
        foreach ($w in $workflows) {
            $text = Get-Content -LiteralPath $w.FullName -Raw -Encoding UTF8
            $ok = ($text -match '(?m)^on:') -and ($text -match '(?m)^jobs:')
            if ($ok) {
                Add-Result -Check 'A3 workflows' -Target $w.Name -Status 'WARN' -Detail 'shape looks right, but PyYAML is missing: not fully validated'
            } else {
                Add-Result -Check 'A3 workflows' -Target $w.Name -Status 'FAIL' -Detail 'no top-level on: and jobs:'
            }
        }
        Write-Host '  (install PyYAML for a real parse: python -m pip install pyyaml)' -ForegroundColor Yellow
    } else {
        # The check runs in python so a YAML error is reported by the parser that GitHub uses, not by a regex.
        $script = @'
import sys, yaml
problems = []
for path in sys.argv[1:]:
    try:
        doc = yaml.safe_load(open(path, encoding="utf-8"))
    except Exception as e:
        problems.append("%s: YAML error: %s" % (path, e))
        continue
    if not isinstance(doc, dict):
        problems.append("%s: not a mapping" % path); continue
    # PyYAML reads the bare key `on:` as the boolean True (YAML 1.1), so both spellings are accepted.
    if "jobs" not in doc:
        problems.append("%s: no jobs:" % path); continue
    if not any(k in doc for k in ("on", True)):
        problems.append("%s: no on: trigger" % path); continue
    for name, job in doc["jobs"].items():
        if not isinstance(job, dict) or "steps" not in job:
            problems.append("%s: job %s has no steps" % (path, name)); continue
        if "runs-on" not in job:
            problems.append("%s: job %s has no runs-on" % (path, name))
        for i, step in enumerate(job["steps"]):
            if not isinstance(step, dict):
                problems.append("%s: job %s step %d is not a mapping" % (path, name, i)); continue
            if "uses" not in step and "run" not in step:
                problems.append("%s: job %s step %d has neither uses nor run" % (path, name, i))
print("\n".join(problems))
sys.exit(1 if problems else 0)
'@
        $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("p5r-yamlcheck-{0}.py" -f [guid]::NewGuid().ToString('N'))
        Set-Content -LiteralPath $tmp -Value $script -Encoding UTF8
        try {
            $out = & $python $tmp @($workflows | ForEach-Object { $_.FullName }) 2>&1
            $code = $LASTEXITCODE
        } finally {
            Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
        }
        if ($code -eq 0) {
            foreach ($w in $workflows) {
                Add-Result -Check 'A3 workflows' -Target $w.Name -Status 'PASS' -Detail 'valid YAML, has on/jobs/runs-on/steps'
            }
        } else {
            foreach ($problem in @($out)) {
                if ("$problem".Trim()) {
                    Add-Result -Check 'A3 workflows' -Target 'workflow' -Status 'FAIL' -Detail "$problem".Trim()
                }
            }
        }
    }
}

# ================================================================================================================
# A4. Every documented perf switch is documented, parsed and logged
# ================================================================================================================
Write-Head 'A4  perf switches: documented, parsed, logged (docs/PERF-DESIGN.md section 2 + 4.5.2)'

$designPath = Join-Path $root 'docs/PERF-DESIGN.md'
if (-not (Test-Path -LiteralPath $designPath -PathType Leaf)) {
    Add-Result -Check 'A4 switches' -Target 'docs/PERF-DESIGN.md' -Status 'FAIL' -Detail 'the authoritative design doc is missing'
} else {
    $design = @(Get-Content -LiteralPath $designPath -Encoding UTF8)

    # The switch tables: section 2, plus the thermal block in 4.5.2. Both are `| `key` | `NX_KEY` | ... |` rows.
    $switches = New-Object System.Collections.ArrayList
    # "upstream already had it", written as an escape: Windows PowerShell 5.1 reads a BOM-less script as ANSI, so a
    # literal Chinese string here would not compare equal to the UTF-8 text read from the design doc.
    $upstreamMarker = [regex]::Unescape('\u4E0A\u6E38\u5DF2\u6709')
    $inTable = $false
    foreach ($line in $design) {
        if ($line -match '^##\s*2\.' -or $line -match '^###\s*4\.5\.2') { $inTable = $true; continue }
        if ($inTable -and $line -match '^#{2,3}\s') { $inTable = $false; continue }
        if (-not $inTable) { continue }
        if ($line -notmatch '^\s*\|') { continue }
        $cells = @($line.Trim().Trim('|').Split('|') | ForEach-Object { $_.Trim() })
        if ($cells.Count -lt 2) { continue }
        $m = [regex]::Match($cells[0], '^`([^`]+)`$')
        if (-not $m.Success) { continue }
        $key = $m.Groups[1].Value
        $envMatch = [regex]::Match($cells[1], '`([^`]+)`')
        $envName = if ($envMatch.Success) { $envMatch.Groups[1].Value } else { '' }
        if (-not ($switches | Where-Object { $_.Key -eq $key })) {
            [void]$switches.Add([pscustomobject]@{
                    Key      = $key
                    Env      = $envName
                    Upstream = ($cells -join ' ').Contains($upstreamMarker)
                })
        }
    }

    if ($switches.Count -eq 0) {
        Add-Result -Check 'A4 switches' -Target 'docs/PERF-DESIGN.md' -Status 'FAIL' -Detail 'no switch table found'
    } else {
        Write-Host "  $($switches.Count) switches in the design doc: $(($switches | ForEach-Object { $_.Key }) -join ', ')" -ForegroundColor Gray

        # The C++ corpus: the whole runtime and the game glue (not just the changed files: a switch may be read anywhere).
        $corpus = @()
        foreach ($dir in @('runtime/src', 'runtime/include', 'games')) {
            $full = Join-Path $root $dir
            if (Test-Path -LiteralPath $full -PathType Container) {
                $corpus += @(Get-ChildItem -LiteralPath $full -Recurse -File -Include '*.cpp', '*.h', '*.hpp', '*.c' -ErrorAction SilentlyContinue)
            }
        }
        $corpusText = @{}
        foreach ($f in $corpus) {
            $corpusText[$f.FullName] = @(Get-Content -LiteralPath $f.FullName -Encoding UTF8)
        }

        foreach ($sw in $switches) {
            $key = $sw.Key
            $parseHits = New-Object System.Collections.ArrayList
            $logHits = New-Object System.Collections.ArrayList
            # The log lines are written for a human, so they rarely repeat the config key verbatim ("render height 1440"
            # for render_height, "dynamic resolution on" for dyn_res). Both spellings count as "logged".
            $phrases = @($key, ($key -replace '_', ' '))
            switch ($key) {
                'dyn_res' { $phrases += 'dynamic resolution' }
                'dyn_res_min' { $phrases += 'floor' }
                'thermal_guard' { $phrases += 'guard ' }
                'render_height' { $phrases += 'render height' }
                'NX_RENDER_RES' { $phrases += 'render res' }
                'resolution' { $phrases += 'resolution' }
            }
            foreach ($file in $corpusText.Keys) {
                $lines = $corpusText[$file]
                $rel = $file.Substring($root.Length).TrimStart('\', '/').Replace('\', '/')
                for ($i = 0; $i -lt $lines.Count; $i++) {
                    $l = $lines[$i]
                    if ($l.Contains('"' + $key + '"') -or ($sw.Env -and $l.Contains('"' + $sw.Env + '"'))) {
                        [void]$parseHits.Add("${rel}:$($i + 1)")
                    }
                    # A log call usually spans several lines (a format string, then its arguments), so the window is
                    # the statement's own lines: from here back to the nearest log*/( and forward a few lines.
                    $window = New-Object System.Collections.ArrayList
                    for ($k = [Math]::Max(0, $i - 3); $k -le [Math]::Min($lines.Count - 1, $i + 6); $k++) {
                        [void]$window.Add($lines[$k])
                    }
                    $windowText = ($window -join "`n")
                    if ($windowText -notmatch '\blog\w*\s*\(|\blog_line\s*\(|\bLOGI\b|\bLOGD\b|\bLOGW\b') { continue }
                    $mentioned = $false
                    foreach ($p in $phrases) { if ($windowText.IndexOf($p, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $mentioned = $true; break } }
                    if ($mentioned) { [void]$logHits.Add("${rel}:$($i + 1)") }
                }
            }

            if ($parseHits.Count -gt 0) {
                Add-Result -Check 'A4 switches' -Target "$key parsed" -Status 'PASS' -Detail $parseHits[0]
            } else {
                Add-Result -Check 'A4 switches' -Target "$key parsed" -Status 'FAIL' -Detail 'documented but no C++ file reads it'
            }

            if ($logHits.Count -gt 0) {
                # Prefer a log line in the file that actually reads the switch: a hit in an unrelated file (a word
                # that happens to appear in another module's log) is not evidence that this switch is observable.
                # A switch parsed through the shared profile (perf_profile.cpp) is logged there as well, so a log line
                # in either the parsing file or perf_profile.cpp counts.
                $parsedFiles = @($parseHits | ForEach-Object { ($_ -split ':')[0] } | Sort-Object -Unique)
                $parsedFiles += 'runtime/src/perf_profile.cpp'
                $sameFile = @($logHits | Where-Object { $parsedFiles -contains (($_ -split ':')[0]) })
                if ($sameFile.Count -gt 0) {
                    Add-Result -Check 'A4 switches' -Target "$key logged" -Status 'PASS' -Detail $sameFile[0]
                } else {
                    Add-Result -Check 'A4 switches' -Target "$key logged" -Status 'WARN' -Detail "logged only outside the file that reads it ($($logHits[0])): check the device log really shows it"
                }
            } elseif ($sw.Upstream) {
                # The design table marks pre-existing switches "(上游已有)": they behave as they always did, and the
                # branch is not required to add a log line for them (that would change upstream's output).
                Add-Result -Check 'A4 switches' -Target "$key logged" -Status 'SKIP' -Detail 'upstream switch: logging not required by this branch'
            } elseif ($parseHits.Count -eq 0) {
                Add-Result -Check 'A4 switches' -Target "$key logged" -Status 'FAIL' -Detail 'not read anywhere, so it cannot be logged either'
            } else {
                # A warning, not a failure: the log text is prose and this is a heuristic over it. It still flags a
                # switch a tester would be unable to confirm on the device.
                Add-Result -Check 'A4 switches' -Target "$key logged" -Status 'WARN' -Detail "read at $($parseHits[0]) but no log line seems to print it: a tester may not be able to confirm it took effect"
            }
        }
    }
}

# ================================================================================================================
# A5. No game data, keys or private/ content in the tree
# ================================================================================================================
Write-Head 'A5  no game data, keys or private/ content under version control'

$forbidden = @('\.xci$', '\.nsp$', '\.nsz$', '\.xcz$', '\.keys$', '\.rar$', '\.nca$', '\.nro$', '\.nso$',
    '(^|/)private/', 'prod\.keys$')
$offenders = New-Object System.Collections.ArrayList

try {
    $status = & git -C $root status --porcelain 2>&1
    if ($LASTEXITCODE -eq 0) {
        foreach ($line in @($status)) {
            $t = "$line"
            if ($t.Length -le 3) { continue }
            $path = $t.Substring(3).Trim().Trim('"').Replace('\', '/')
            foreach ($p in $forbidden) { if ($path -match $p) { [void]$offenders.Add("staged: $path"); break } }
        }
    }
} catch { }

try {
    $tracked = & git -C $root ls-files 2>&1
    if ($LASTEXITCODE -eq 0) {
        foreach ($line in @($tracked)) {
            $path = "$line".Trim().Replace('\', '/')
            if (-not $path) { continue }
            foreach ($p in $forbidden) { if ($path -match $p) { [void]$offenders.Add("tracked: $path"); break } }
        }
    }
} catch { }

if ($offenders.Count -gt 0) {
    foreach ($o in ($offenders | Sort-Object -Unique)) {
        Add-Result -Check 'A5 secrets' -Target $o -Status 'FAIL' -Detail 'never commit game data, keys or private/ content'
    }
} else {
    Add-Result -Check 'A5 secrets' -Target 'git status + git ls-files' -Status 'PASS' -Detail 'no game file, key or private/ path'
}

# ================================================================================================================
# A6. The unit tests are still registered
# ================================================================================================================
Write-Head 'A6  CMakeLists.txt registers the unit tests'

$cmakePath = Join-Path $root 'CMakeLists.txt'
if (-not (Test-Path -LiteralPath $cmakePath -PathType Leaf)) {
    Add-Result -Check 'A6 tests' -Target 'CMakeLists.txt' -Status 'FAIL' -Detail 'missing'
} else {
    $cmake = @(Get-Content -LiteralPath $cmakePath -Encoding UTF8)
    $testTargets = @($cmake | Where-Object { $_ -match '^\s*add_executable\((\w+_test)\b' } |
            ForEach-Object { [regex]::Match($_, 'add_executable\((\w+_test)').Groups[1].Value })
    $testCases = @($cmake | Where-Object { $_ -match '^\s*add_test\(' })

    if ($testCases.Count -eq 0) {
        Add-Result -Check 'A6 tests' -Target 'CMakeLists.txt' -Status 'FAIL' -Detail 'no add_test(): ctest would run nothing'
    } else {
        Add-Result -Check 'A6 tests' -Target 'CMakeLists.txt' -Status 'PASS' -Detail "$($testTargets.Count) test target(s), $($testCases.Count) add_test()"
    }
    # A target with no add_test() is built but never run: the usual way a test silently stops running.
    foreach ($t in $testTargets) {
        if ($cmake -match ('add_test\(NAME\s+\w+\s+COMMAND\s+' + [regex]::Escape($t))) {
            Add-Result -Check 'A6 tests' -Target $t -Status 'PASS' -Detail 'registered with add_test'
        } else {
            Add-Result -Check 'A6 tests' -Target $t -Status 'FAIL' -Detail 'built but not registered with add_test: ctest never runs it'
        }
    }
    # The perf work's own tests (PERF-DESIGN.md §6.2). Missing now only means the file is not written yet.
    foreach ($required in @('perf_profile_test')) {
        if ($testTargets -contains $required) {
            Add-Result -Check 'A6 tests' -Target $required -Status 'PASS' -Detail 'present'
        } else {
            Add-Result -Check 'A6 tests' -Target $required -Status 'WARN' -Detail 'not registered yet (may still be being written)'
        }
    }
}

# ================================================================================================================
# B. Host build + ctest
# ================================================================================================================
Write-Head 'B   host build (-DNX_TESTS=ON) and ctest'

if ($SkipBuild) {
    Add-Result -Check 'B build' -Target '(host build)' -Status 'SKIP' -Detail '-SkipBuild'
} else {
    # Find a C++20 compiler. Two traps this guards against:
    #   * g++ 8.1 (the mingw64 that is often first on the PATH) has no -std=c++20 at all;
    #   * a g++/clang++ started by full path without its own directory on the PATH cannot find its runtime DLLs and
    #     exits 1 *printing nothing* — which looks exactly like "compiler rejects C++20" if the exit code alone is
    #     trusted. So the compiler's directory is prepended to the PATH for the probe and for the build.
    function Resolve-CompilerPath {
        param([string]$Name)
        if (Test-Path -LiteralPath $Name -PathType Leaf) { return (Resolve-Path -LiteralPath $Name).Path }
        $cmd = Get-Command $Name -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
        return $null
    }

    function Test-Cxx20 {
        param([string]$Exe)
        $exePath = Resolve-CompilerPath -Name $Exe
        if (-not $exePath) { return $false }
        $probe = Join-Path ([System.IO.Path]::GetTempPath()) ("p5r-probe-{0}.cpp" -f [guid]::NewGuid().ToString('N'))
        Set-Content -LiteralPath $probe -Value "#include <version>`nint main() { return 0; }" -Encoding ASCII
        $saved = $env:PATH
        try {
            # Its own directory first, so the toolchain finds its DLLs and its cc1plus.
            $env:PATH = (Split-Path -Parent $exePath) + [System.IO.Path]::PathSeparator + $saved
            $out = & $exePath -std=c++20 -fsyntax-only $probe 2>&1
            $code = $LASTEXITCODE
            if ($code -eq 0) { return $true }
            if (-not $out) {
                Write-Host "  (not usable: $exePath -> exit $code with no output; the toolchain cannot run)" -ForegroundColor DarkGray
            }
            return $false
        } catch {
            return $false
        } finally {
            $env:PATH = $saved
            Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
        }
    }

    $candidates = New-Object System.Collections.ArrayList
    if ($Compiler) {
        [void]$candidates.Add($Compiler)
    } else {
        foreach ($name in @('g++', 'clang++', 'cl')) {
            $cmd = Get-Command $name -ErrorAction SilentlyContinue
            if ($cmd) { [void]$candidates.Add($cmd.Source) }
        }
        # The MSYS2 g++ this project's docs use on Windows (15.2, C++20). Added explicitly because the PATH often
        # resolves g++ to an old mingw64 8.1 that cannot compile this project at all.
        foreach ($p in @('E:\MSYS2\mingw64\bin\g++.exe', 'C:\msys64\mingw64\bin\g++.exe')) {
            if ((Test-Path -LiteralPath $p -PathType Leaf) -and -not $candidates.Contains($p)) { [void]$candidates.Add($p) }
        }
    }

    $cxx = $null
    foreach ($candidate in $candidates) {
        if (Test-Cxx20 -Exe $candidate) { $cxx = (Resolve-CompilerPath -Name $candidate); break }
    }

    if (-not $cxx) {
        Add-Result -Check 'B build' -Target '(host build)' -Status 'SKIP' -Detail 'no C++20 compiler found: install MSYS2 g++ 15, clang 16+, or MSVC 2022'
        Write-Host ''
        Write-Host '  The static checks above still ran. To get the build:' -ForegroundColor Yellow
        Write-Host '    Windows : winget install MSYS2.MSYS2, then pacman -S mingw-w64-x86_64-gcc ninja cmake' -ForegroundColor Yellow
        Write-Host '    or      : install "Desktop development with C++" from Visual Studio 2022 and run this in a dev prompt' -ForegroundColor Yellow
        Write-Host '    Linux   : sudo apt install g++ cmake ninja-build' -ForegroundColor Yellow
        Write-Host '    macOS   : xcode-select --install; brew install cmake ninja' -ForegroundColor Yellow
    } else {
        Write-Host "  compiler: $cxx" -ForegroundColor Gray
        if (-not $BuildDir) {
            $BuildDir = Join-Path ([System.IO.Path]::GetTempPath()) ("p5r-selftest-{0}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
        }
        Write-Host "  build dir: $BuildDir" -ForegroundColor Gray

        # The C compiler that goes with the C++ one: CMakeLists.txt enables C and CXX, and mixing a mingw g++ with an
        # MSVC gcc cannot work.
        $cc = $null
        $dir = Split-Path -Parent $cxx
        foreach ($name in @('gcc.exe', 'gcc', 'clang.exe', 'clang')) {
            $p = Join-Path $dir $name
            if (Test-Path -LiteralPath $p -PathType Leaf) { $cc = $p; break }
        }

        $cmake = Get-Command cmake -ErrorAction SilentlyContinue
        if (-not $cmake) {
            Add-Result -Check 'B build' -Target '(host build)' -Status 'SKIP' -Detail 'cmake not on the PATH'
        } else {
            # A fresh directory every run: a stale CMakeCache from another compiler is the classic false failure.
            if (Test-Path -LiteralPath $BuildDir) { Remove-Item -LiteralPath $BuildDir -Recurse -Force -ErrorAction SilentlyContinue }

            # The toolchain's own directory goes on the PATH for the whole build: g++/clang++ started by full path
            # cannot find their DLLs (Windows) or their assembler/linker helpers otherwise, and then fail silently.
            $savedPath = $env:PATH
            $env:PATH = $dir + [System.IO.Path]::PathSeparator + $savedPath

            $configureArgs = @('-S', $root, '-B', $BuildDir, '-DCMAKE_BUILD_TYPE=Release', '-DNX_TESTS=ON', '-DNX_GAME=p5r')
            if ($cxx -match 'cl(\.exe)?$') {
                # MSVC needs no -G: the default generator works and needs no ninja on the PATH.
                Write-Host '  generator: default (MSVC)' -ForegroundColor Gray
            } else {
                $ninja = Get-Command ninja -ErrorAction SilentlyContinue
                if ($ninja) { $configureArgs = @('-G', 'Ninja') + $configureArgs }
                if ($cc) { $configureArgs += @("-DCMAKE_C_COMPILER=$cc") }
                $configureArgs += @("-DCMAKE_CXX_COMPILER=$cxx")
            }

            try {
                $sw = [System.Diagnostics.Stopwatch]::StartNew()
                $configure = Invoke-Native -Exe $cmake.Source -Arguments $configureArgs
                $configured = ($configure.ExitCode -eq 0)
                if ($configured) {
                    Add-Result -Check 'B build' -Target 'cmake configure' -Status 'PASS' -Detail "NX_TESTS=ON, $([int]$sw.Elapsed.TotalSeconds)s"
                } else {
                    Add-Result -Check 'B build' -Target 'cmake configure' -Status 'FAIL' -Detail 'see the output above'
                }

                if ($configured) {
                    $build = Invoke-Native -Exe $cmake.Source -Arguments @('--build', $BuildDir, '--parallel', '4')
                    $built = ($build.ExitCode -eq 0)
                    if ($built) {
                        Add-Result -Check 'B build' -Target 'cmake --build' -Status 'PASS' -Detail "$([int]$sw.Elapsed.TotalSeconds)s"
                    } else {
                        Add-Result -Check 'B build' -Target 'cmake --build' -Status 'FAIL' -Detail 'compilation failed: see the output above'
                    }

                    if ($built) {
                        $ctest = Get-Command ctest -ErrorAction SilentlyContinue
                        if (-not $ctest) {
                            Add-Result -Check 'B build' -Target 'ctest' -Status 'SKIP' -Detail 'ctest not on the PATH'
                        } else {
                            $test = Invoke-Native -Exe $ctest.Source -Arguments @('--test-dir', $BuildDir, '--output-on-failure', '--timeout', '300')
                            # ctest prints one "N/M Test #k: <name> ... Passed" line per test, plus a summary line
                            # ("100% tests passed"). Match the per-test lines only, so the count is the real one.
                            $passed = @($test.Output | Select-String -Pattern 'Test\s+#\d+:\s+\S+\s+\.+\s+Passed').Count
                            $failed = @($test.Output | Select-String -Pattern 'Test\s+#\d+:\s+\S+\s+\.+\s+\*\*\*').Count
                            if ($test.ExitCode -eq 0) {
                                Add-Result -Check 'B build' -Target 'ctest' -Status 'PASS' -Detail "$passed test(s) passed"
                            } else {
                                Add-Result -Check 'B build' -Target 'ctest' -Status 'FAIL' -Detail "$failed test(s) failed: see the output above"
                            }
                        }
                    }
                }
            } finally {
                # Always put the caller's PATH back: this script must not leak state into the session it runs in.
                $env:PATH = $savedPath
            }
        }
    }
}

# ================================================================================================================
# Report
# ================================================================================================================
$fail = @($script:Results | Where-Object { $_.Status -eq 'FAIL' })
$warn = @($script:Results | Where-Object { $_.Status -eq 'WARN' })
$pass = @($script:Results | Where-Object { $_.Status -eq 'PASS' })
$skip = @($script:Results | Where-Object { $_.Status -eq 'SKIP' })

Write-Head 'SUMMARY'
Write-Host ("PASS : {0}" -f $pass.Count) -ForegroundColor Green
Write-Host ("FAIL : {0}" -f $fail.Count) -ForegroundColor $(if ($fail.Count) { 'Red' } else { 'Green' })
Write-Host ("WARN : {0}" -f $warn.Count) -ForegroundColor Yellow
Write-Host ("SKIP : {0}" -f $skip.Count) -ForegroundColor DarkGray

if ($fail.Count -gt 0) {
    Write-Host ''
    Write-Host 'FAILED CHECKS' -ForegroundColor Red
    foreach ($f in $fail) { Write-Host ("  - [{0}] {1}: {2}" -f $f.Check, $f.Target, $f.Detail) -ForegroundColor Red }
    Write-Host ''
    Write-Host 'VERDICT: FAIL' -ForegroundColor Red
    exit 1
}

if ($warn.Count -gt 0) {
    Write-Host ''
    Write-Host 'WARNINGS (not fatal)' -ForegroundColor Yellow
    foreach ($w in $warn) { Write-Host ("  - [{0}] {1}: {2}" -f $w.Check, $w.Target, $w.Detail) -ForegroundColor Yellow }
}

Write-Host ''
Write-Host 'VERDICT: PASS' -ForegroundColor Green
exit 0
