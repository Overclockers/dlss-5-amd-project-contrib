# Known-store library scan. No full-disk walk. Does not write library.json.
function Test-OptScalerGameExeName([string]$name) {
    return $name -match '(?i)\.exe$' -and
        $name -notmatch '(?i)^(setup|install|unins|crash|unitycrash|vc_redist|dxsetup|easyanticheat|eac_|battleye|beclient|beservice|unrealcef|webview|cefsubprocess|shadercompile|bugsplat|crs-|redprelauncher|larilauncher|2klauncher).*\.exe$' -and
        $name -notmatch '(?i)(prelauncher|launcher|crashreport|crashhandler|crs-handler|errorreporter|updater|patcher|uninstaller|benchmark|dedicatedserver)\.exe$' -and
        $name -notmatch '(?i)^(gamelaunchhelper|person-worker|activationui|cleanup|touchup|config)\.exe$'
}

function Test-OptScalerUpscalerAnchor([string]$exeDir) {
    if (-not $exeDir) { return $false }
    foreach ($marker in @(
        'amd-presr-install.txt',
        'nvngx_dlss.dll', 'nvngx_dlssd.dll', 'nvngx_dlssg.dll', 'sl.interposer.dll',
        'libxess.dll', 'libxess_fg.dll',
        'amd_fidelityfx_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_vk.dll'
    )) {
        if (Test-Path -LiteralPath (Join-Path $exeDir $marker) -PathType Leaf) { return $true }
    }
    return $false
}

function Get-OptScalerExeRank([string]$fullPath, [string]$rootDir, [int]$depth) {
    $leaf = [IO.Path]::GetFileName($fullPath)
    $exeDir = [IO.Path]::GetDirectoryName($fullPath)
    $rel = if ($rootDir -and $fullPath.Length -gt $rootDir.Length) { $fullPath.Substring($rootDir.Length).TrimStart('\','/') } else { $leaf }
    if (Test-Path -LiteralPath (Join-Path $exeDir 'amd-presr-install.txt') -PathType Leaf) { return -10 }
    $rank = 3
    if ($leaf -match '(?i)WinGDK-Shipping\.exe$') { $rank = 0 }
    elseif ($leaf -match '(?i)-Win64-Shipping\.exe$') { $rank = 1 }
    elseif ($leaf -match '(?i)Shipping\.exe$') { $rank = 2 }
    elseif ($rel -match '(?i)(^|[\\/])(x64_dx12|dx12)([\\/]|$)' -or $leaf -match '(?i)dx12') { $rank = 1 }
    elseif ($rel -match '(?i)(^|[\\/])(binaries[\\/](win64|wingdk)|bin[\\/](x64|win64))([\\/]|$)') { $rank = 2 }
    elseif (Test-Path -LiteralPath (Join-Path $exeDir 'UnityPlayer.dll') -PathType Leaf) { $rank = 2 }
    if (Test-OptScalerUpscalerAnchor $exeDir) {
        $rank -= $(if ($rel -match '(?i)(^|[\\/])(x64_dx12|dx12)([\\/]|$)' -or $leaf -match '(?i)dx12') { 5 } else { 4 })
    }
    return $rank
}

function Get-OptScalerPrimaryExes([string]$dir, [int]$depth = 4) {
    $found = New-Object System.Collections.Generic.List[object]
    if (-not $dir -or -not (Test-Path -LiteralPath $dir -PathType Container)) { return @() }
    $rootFull = try { [IO.Path]::GetFullPath($dir).TrimEnd('\','/') } catch { $dir }
    $queue = New-Object System.Collections.Generic.Queue[object]
    $queue.Enqueue(@{ Path = $dir; Depth = 0 })
    $seen = 0
    while ($queue.Count -gt 0 -and $seen -lt 400) {
        $cur = $queue.Dequeue()
        if ($cur.Depth -gt $depth) { continue }
        $entries = @()
        try { $entries = [IO.Directory]::GetFileSystemEntries($cur.Path) } catch { continue }
        $subDirs = New-Object System.Collections.Generic.List[string]
        foreach ($entry in $entries) {
            if ($seen -ge 400) { break }
            $leaf = [IO.Path]::GetFileName($entry)
            $isDir = $false
            $reparse = $false
            try {
                $attr = [IO.File]::GetAttributes($entry)
                $isDir = [bool]($attr -band [IO.FileAttributes]::Directory)
                $reparse = [bool]($attr -band [IO.FileAttributes]::ReparsePoint)
            } catch { continue }
            if ($reparse) { continue }
            if ($isDir) {
                if ($leaf -notmatch '(?i)^(OptiScaler|person-model|dlssnr-amd|_CommonRedist|__Installer|redist|DirectX|Launcher|REDprelauncher|CrashReporter|Bonus|Extras|Soundtrack|OST|Artbook|Wallpapers?|Manual|Docs|Support|ModTools|SDK|Paks|ShaderCache)$') {
                    [void]$subDirs.Add($entry)
                }
                continue
            }
            if ($leaf -notmatch '(?i)\.exe$') { continue }
            $seen++
            if (-not (Test-OptScalerGameExeName $leaf)) { continue }
            $rank = Get-OptScalerExeRank $entry $rootFull $cur.Depth
            [void]$found.Add(@{ path = $entry; rank = $rank; name = $leaf })
        }
        foreach ($sd in ($subDirs | Sort-Object {
            $n = [IO.Path]::GetFileName($_)
            if ($n -match '(?i)^(bin|binaries|x64_dx12|x64|win64|wingdk|game|client|content)$') { 0 } else { 1 }
        }, { [IO.Path]::GetFileName($_) })) {
            if ($seen -ge 400) { break }
            $seen++
            $queue.Enqueue(@{ Path = $sd; Depth = $cur.Depth + 1 })
        }
    }
    return @($found | Sort-Object { $_.rank }, { $_.name }, { $_.path } | Select-Object -First 8)
}

function Get-OptScalerXboxExes([string]$dir) {
    # AppxManifest can point at GameLaunchHelper; GDK's config names the actual game.
    $found = New-Object System.Collections.Generic.List[object]
    $seen = @{}
    $reader = $null
    try {
        $config = Join-Path $dir 'MicrosoftGame.Config'
        $settings = New-Object System.Xml.XmlReaderSettings
        $settings.DtdProcessing = [System.Xml.DtdProcessing]::Prohibit
        $settings.XmlResolver = $null
        $settings.MaxCharactersInDocument = 1MB
        $reader = [System.Xml.XmlReader]::Create($config, $settings)
        $xml = New-Object System.Xml.XmlDocument
        $xml.XmlResolver = $null
        $xml.Load($reader)
        $root = [IO.Path]::GetFullPath($dir).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
        foreach ($node in $xml.SelectNodes('/*[local-name()="Game"]/*[local-name()="ExecutableList"]/*[local-name()="Executable"]')) {
            $name = $node.GetAttribute('Name')
            $family = $node.GetAttribute('TargetDeviceFamily')
            if ($family -and $family -ne 'PC') { continue }
            if ($node.GetAttribute('IsDevOnly') -eq 'true') { continue }
            if (-not $name -or [IO.Path]::IsPathRooted($name)) { continue }
            if (-not (Test-OptScalerGameExeName ([IO.Path]::GetFileName($name)))) { continue }
            try { $path = [IO.Path]::GetFullPath((Join-Path $dir $name)) } catch { continue }
            if (-not $path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { continue }
            if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or $seen.ContainsKey($path)) { continue }
            $seen[$path] = $true
            [void]$found.Add(@{ path = $path; rank = -1; name = [IO.Path]::GetFileName($path) })
        }
    } catch {
        # Missing, stale or malformed store metadata falls back to the bounded scan.
    } finally {
        if ($reader) { $reader.Dispose() }
    }
    if ($found.Count -gt 0) { return @($found | Sort-Object { $_.name }, { $_.path }) }
    return @(Get-OptScalerPrimaryExes $dir 6)
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
        exeOverride = $false
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
                $cands = @(Get-OptScalerPrimaryExes $j.InstallLocation 4)
                if ($j.LaunchExecutable) {
                    $cand = Join-Path $j.InstallLocation $j.LaunchExecutable
                    if ((Test-Path -LiteralPath $cand -PathType Leaf) -and (Test-OptScalerGameExeName ([IO.Path]::GetFileName($cand)))) {
                        $candRank = Get-OptScalerExeRank $cand $j.InstallLocation 0
                        if ($cands.Count -eq 0 -or $candRank -le $cands[0].rank) { $exe = $cand }
                    }
                }
                if (-not $exe -and $cands.Count -gt 0) { $exe = $cands[0].path }
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
                    $exe = $null
                    $cands = @(Get-OptScalerPrimaryExes ([string]$p.path) 4)
                    if ((Test-Path -LiteralPath $cand -PathType Leaf) -and (Test-OptScalerGameExeName ([IO.Path]::GetFileName($cand)))) {
                        $candRank = Get-OptScalerExeRank $cand ([string]$p.path) 0
                        if ($cands.Count -eq 0 -or $candRank -le $cands[0].rank) { $exe = $cand }
                    }
                    if (-not $exe -and $cands.Count -gt 0) { $exe = $cands[0].path }
                    if ($exe) {
                        Add-OptScalerGame $games $seen $p.gameName $exe 'GOG' ("gog:" + [string]$_.PSChildName)
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
            $cands = @(Get-OptScalerXboxExes $base)
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
