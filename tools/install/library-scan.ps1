# Known-store library scan. No full-disk walk. Does not write library.json.
function Get-OptScalerPrimaryExes([string]$dir, [int]$depth = 4) {
    $found = New-Object System.Collections.Generic.List[object]
    if (-not $dir -or -not (Test-Path -LiteralPath $dir -PathType Container)) { return @() }
    $stack = New-Object System.Collections.Generic.Stack[object]
    $stack.Push(@{ Path = $dir; Depth = 0 })
    $seen = 0
    while ($stack.Count -gt 0 -and $seen -lt 400 -and $found.Count -lt 8) {
        $cur = $stack.Pop()
        if ($cur.Depth -gt $depth) { continue }
        $entries = @()
        try { $entries = [IO.Directory]::GetFileSystemEntries($cur.Path) } catch { continue }
        foreach ($entry in $entries) {
            $seen++
            $leaf = [IO.Path]::GetFileName($entry)
            $isDir = $false
            $reparse = $false
            try {
                $attr = [IO.File]::GetAttributes($entry)
                $isDir = [bool]($attr -band [IO.FileAttributes]::Directory)
                $reparse = [bool]($attr -band [IO.FileAttributes]::ReparsePoint)
            } catch { continue }
            if ($reparse) { continue }
            if ($isDir) { $stack.Push(@{ Path = $entry; Depth = $cur.Depth + 1 }); continue }
            if ($leaf -notmatch '(?i)\.exe$') { continue }
            if ($leaf -match '(?i)^(setup|install|unins|crash|unitycrash|vc_redist|easyanticheat|battleye).*\.exe$') { continue }
            $rank = 3
            if ($leaf -match '(?i)WinGDK-Shipping\.exe$') { $rank = 0 }
            elseif ($leaf -match '(?i)-Win64-Shipping\.exe$') { $rank = 1 }
            elseif ($leaf -match '(?i)Shipping\.exe$') { $rank = 2 }
            [void]$found.Add(@{ path = $entry; rank = $rank; name = $leaf })
        }
    }
    return @($found | Sort-Object rank, name)
}

function Add-OptScalerGame($list, $seen, [string]$name, [string]$exe, [string]$source, [string]$id, [string]$cover = '', [string]$hero = '') {
    if (-not $exe) { return }
    try { $exe = [IO.Path]::GetFullPath($exe) } catch { return }
    $key = $exe.ToLowerInvariant()
    if ($seen.ContainsKey($key)) { return }
    $seen[$key] = $true
    $dir = [IO.Path]::GetDirectoryName($exe)
    $installed = Test-Path -LiteralPath (Join-Path $dir 'amd-presr-install.txt')
    [void]$list.Add(@{
        id = $(if ($id) { $id } else { $key })
        name = $name
        exe = $exe
        source = $source
        installed = [bool]$installed
        selectionRequired = $false
        cover = $cover
        hero = $hero
    })
}

function Get-OptScalerGameLibrary {
    $games = New-Object System.Collections.Generic.List[object]
    $seen = @{}
    $steamRoots = New-Object System.Collections.Generic.List[string]
    foreach ($reg in @(
        'HKCU:\Software\Valve\Steam',
        'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam',
        'HKLM:\SOFTWARE\Valve\Steam'
    )) {
        if (-not (Test-Path $reg)) { continue }
        try {
            $p = Get-ItemProperty $reg
            $path = if ($p.SteamPath) { $p.SteamPath } else { $p.InstallPath }
            if ($path) { [void]$steamRoots.Add($path) }
        } catch { }
    }
    foreach ($r in @("${env:ProgramFiles(x86)}\Steam", "$env:ProgramFiles\Steam")) {
        if ($r -and (Test-Path -LiteralPath $r)) { [void]$steamRoots.Add($r) }
    }
    foreach ($steamRoot in ($steamRoots | Select-Object -Unique)) {
        $libs = @($steamRoot)
        $vdf = Join-Path $steamRoot 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            try {
                $raw = Get-Content -LiteralPath $vdf -Raw -Encoding UTF8 -ErrorAction Stop
                [regex]::Matches($raw, '"path"\s+"([^"]+)"') | ForEach-Object { $libs += ($_.Groups[1].Value -replace '\\\\','\') }
            } catch { }
        }
        foreach ($lib in ($libs | Select-Object -Unique)) {
            $apps = Join-Path $lib 'steamapps'
            if (-not (Test-Path -LiteralPath $apps)) { continue }
            Get-ChildItem -LiteralPath $apps -Filter 'appmanifest_*.acf' -File -ErrorAction SilentlyContinue | ForEach-Object {
                try {
                    $raw = Get-Content -LiteralPath $_.FullName -Raw -Encoding UTF8
                    $n = [regex]::Match($raw, '"name"\s+"([^"]+)"').Groups[1].Value
                    $d = [regex]::Match($raw, '"installdir"\s+"([^"]+)"').Groups[1].Value
                    $appid = [regex]::Match($raw, '"appid"\s+"([^"]+)"').Groups[1].Value
                    if (-not $n -or $n -match '(?i)(dedicated server|sdk|soundtrack|benchmark|editor|steamworks|redistributable)') { return }
                    if ($d -match '(?i)Steamworks') { return }
                    $n = ($n -replace '[®™©]','').Trim()
                    $dir = Join-Path $lib "steamapps\common\$d"
                    $cands = @(Get-OptScalerPrimaryExes $dir 4)
                    if ($cands.Count -gt 0) {
                        $cover = ''
                        $hero = ''
                        $grid = Join-Path $steamRoot "appcache\librarycache\$appid"
                        if (Test-Path -LiteralPath $grid) {
                            $c1 = Join-Path $grid 'library_600x900.jpg'
                            if (Test-Path -LiteralPath $c1 -PathType Leaf) { $cover = $c1 }
                            elseif (Test-Path -LiteralPath (Join-Path $grid 'header.jpg') -PathType Leaf) { $cover = Join-Path $grid 'header.jpg' }
                            else {
                                $candsJpg = @(Get-ChildItem -LiteralPath $grid -Filter '*.jpg' -File -ErrorAction SilentlyContinue | Sort-Object Length -Descending)
                                if ($candsJpg.Count -gt 0) { $cover = $candsJpg[0].FullName }
                            }
                            $h1 = Join-Path $grid 'library_hero.jpg'
                            if (Test-Path -LiteralPath $h1 -PathType Leaf) { $hero = $h1 }
                        }
                        Add-OptScalerGame $games $seen $n $cands[0].path 'Steam' ("steam:" + $appid) $cover $hero
                        if (@($cands | Where-Object { $_.rank -eq $cands[0].rank }).Count -gt 1) {
                            $games[$games.Count - 1].selectionRequired = $true
                        }
                    }
                } catch { }
            }
        }
    }
    $epic = "$env:ProgramData\Epic\EpicGamesLauncher\Data\Manifests"
    if (Test-Path -LiteralPath $epic) {
        Get-ChildItem -LiteralPath $epic -Filter '*.item' -File -ErrorAction SilentlyContinue | ForEach-Object {
            try {
                $j = Get-Content -LiteralPath $_.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
                if (-not $j.InstallLocation -or -not $j.DisplayName) { return }
                if ($j.DisplayName -match '(?i)(dedicated server|editor|prerequisite)') { return }
                $exe = $null
                if ($j.LaunchExecutable) {
                    $cand = Join-Path $j.InstallLocation $j.LaunchExecutable
                    if (Test-Path -LiteralPath $cand -PathType Leaf) { $exe = $cand }
                }
                if (-not $exe) {
                    $cands = @(Get-OptScalerPrimaryExes $j.InstallLocation 4)
                    if ($cands.Count -gt 0) { $exe = $cands[0].path }
                }
                if ($exe) { Add-OptScalerGame $games $seen $j.DisplayName $exe 'Epic' ("epic:" + [string]$j.CatalogItemId) }
            } catch { }
        }
    }
    foreach ($regRoot in @('HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games','HKLM:\SOFTWARE\GOG.com\Games','HKCU:\SOFTWARE\GOG.com\Games')) {
        if (-not (Test-Path $regRoot)) { continue }
        Get-ChildItem $regRoot -ErrorAction SilentlyContinue | ForEach-Object {
            try {
                $p = Get-ItemProperty $_.PSPath
                if ($p.path -and $p.exe -and $p.gameName) {
                    $cand = if ([IO.Path]::IsPathRooted([string]$p.exe)) { [string]$p.exe } else { Join-Path $p.path $p.exe }
                    if (Test-Path -LiteralPath $cand -PathType Leaf) {
                        Add-OptScalerGame $games $seen $p.gameName $cand 'GOG' ("gog:" + [string]$_.PSChildName)
                    }
                }
            } catch { }
        }
    }
    foreach ($drive in [IO.DriveInfo]::GetDrives()) {
        if (-not $drive.IsReady -or $drive.DriveType -ne [IO.DriveType]::Fixed) { continue }
        $root = Join-Path $drive.Name 'XboxGames'
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            $content = Join-Path $_.FullName 'Content'
            $base = if (Test-Path -LiteralPath $content -PathType Container) { $content } else { $_.FullName }
            $cands = @(Get-OptScalerPrimaryExes $base 6)
            if ($cands.Count -gt 0) {
                $cover = ''
                $hero = ''
                $candDirs = @($base, (Join-Path $base 'Assets'), $_.FullName, (Join-Path $_.FullName 'Assets'), (Join-Path $_.FullName 'Content\Assets'))
                foreach ($cd in $candDirs) {
                    if (-not (Test-Path -LiteralPath $cd -PathType Container)) { continue }
                    foreach ($candImg in @('StoreLogo.png','Logo.png','Square150x150Logo.png','LargeLogo.png','SmallLogo.png','Wide310x150Logo.png','WideLogo.png','SplashScreen.png')) {
                        $p = Join-Path $cd $candImg
                        if (Test-Path -LiteralPath $p -PathType Leaf) { $cover = $p; break }
                    }
                    if ($cover) { break }
                }
                foreach ($cd in $candDirs) {
                    if (-not (Test-Path -LiteralPath $cd -PathType Container)) { continue }
                    foreach ($candHero in @('SplashScreen.png','MCSplashScreen.png','WideLogo.png','Wide310x150Logo.png','LargeLogo.png')) {
                        $p = Join-Path $cd $candHero
                        if (Test-Path -LiteralPath $p -PathType Leaf) { $hero = $p; break }
                    }
                    if ($hero) { break }
                }
                Add-OptScalerGame $games $seen $_.Name $cands[0].path 'Xbox' ("xbox:" + $_.Name.ToLowerInvariant()) $cover $hero
            }
        }
    }
    return @($games | Sort-Object { $_.name }, { $_.exe })
}
