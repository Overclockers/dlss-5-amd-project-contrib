# Real local artwork discovery against generated PNG/JPEG fixtures. No launcher build or store writes.
[CmdletBinding()]
param([switch]$LocalSmoke)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repo 'tools/install/library-scan.ps1')
Add-Type -AssemblyName System.Drawing
$out = Join-Path $repo ('exports/tests/library-artwork/' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($out)
function Check([bool]$condition, [string]$message) { if (-not $condition) { throw $message } }
function Image([string]$relative, [int]$width, [int]$height) {
    $path = Join-Path $out $relative
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    $bitmap = New-Object Drawing.Bitmap($width, $height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear([Drawing.Color]::DarkSlateBlue)
        $format = if ([IO.Path]::GetExtension($path) -eq '.png') { [Drawing.Imaging.ImageFormat]::Png } else { [Drawing.Imaging.ImageFormat]::Jpeg }
        $bitmap.Save($path, $format)
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
    return $path
}
function Report([string]$name, $art) {
    Write-Output "$name entries=$($art.examined) directories=$($art.directories) images=$($art.images)"
    foreach ($candidate in $art.candidates) { Write-Output ('  {0} {1}x{2} {3}' -f $candidate.kind, $candidate.width, $candidate.height, $candidate.path) }
    Write-Output "  SELECT cover=$($art.cover) hero=$($art.hero)"
}
try {
    $oldCover = Image 'old/library_600x900.jpg' 600 900
    $oldHero = Image 'old/library_hero.jpg' 1920 620
    $header = Image 'old/header.jpg' 2048 960
    $old = Get-OptScalerLocalArtwork -Source Steam -Roots @((Join-Path $out 'old')) -MaxMilliseconds 2000
    Check ($old.cover -eq $oldCover -and $old.hero -eq $oldHero) 'Old Steam cache must separate portrait and dedicated hero (not larger header)'
    Report 'Steam old' $old

    $tiny = Image 'new/15d3e861875701ec9b01f9d9b606c7c8379e6115.jpg' 16 16
    $nestedHero = Image 'new/cd5817c4f6256d206c077cb611da21a5de4a5452/library_hero_schinese.jpg' 1920 620
    $nestedCover = Image 'new/94a971a57dbfc1a8d9ddcb3ec39cfd93e2418d23/library_capsule_english.jpg' 600 900
    $blur = Image 'new/cd5817c4f6256d206c077cb611da21a5de4a5452/library_hero_blur.jpg' 2048 1024
    $logo = Image 'new/logo.png' 1024 1024
    $deep = Image 'new/94a971a57dbfc1a8d9ddcb3ec39cfd93e2418d23/deeper/library_600x900.jpg' 1200 1800
    $other = Image 'new/unrelated/library_600x900.jpg' 1200 1800
    $new = Get-OptScalerLocalArtwork -Source Steam -Roots @((Join-Path $out 'new')) -MaxMilliseconds 2000
    Check ($new.cover -eq $nestedCover -and $new.hero -eq $nestedHero) 'Hash/localized cache must beat tiny JPG without walking arbitrary/deep directories'
    Check ($new.candidates.Count -eq 2 -and $new.directories -eq 3) 'Steam must ignore blur/logo/unassociated/deeper images'
    Report 'Steam hash/localized' $new

    [void][IO.Directory]::CreateDirectory((Join-Path $out 'bad'))
    [IO.File]::WriteAllText((Join-Path $out 'bad/library_hero.jpg'), 'corrupt image')
    $truncated = Image 'bad/library_600x900.jpg' 600 900
    $bytes = [IO.File]::ReadAllBytes($truncated)
    [IO.File]::WriteAllBytes($truncated, $bytes[0..31])
    $wrongShape = Image 'bad/header.jpg' 300 600
    $bad = Get-OptScalerLocalArtwork -Source Steam -Roots @((Join-Path $out 'bad'), (Join-Path $out 'missing')) -MaxMilliseconds 2000
    Check (-not $bad.cover -and -not $bad.hero) 'Corrupt/missing/wrong-shape images must have no fabricated fallback'
    $bomb = Image 'bad/oversize.png' 64 64
    $bytes = [IO.File]::ReadAllBytes($bomb)
    $bytes[16] = 127
    [IO.File]::WriteAllBytes($bomb, $bytes)
    Check ($null -eq (Get-OptScalerImageBounds $bomb)) 'Oversized image header must be rejected without decoding'
    $oversize = Join-Path $out 'bad/too-large.jpg'
    $stream = [IO.File]::Create($oversize)
    try { $stream.SetLength(32MB + 1) } finally { $stream.Dispose() }
    Check ($null -eq (Get-OptScalerImageBounds $oversize)) 'File size bound must reject oversized containers'
    $brokenJpeg = Join-Path $out 'bad/broken-segment.jpg'
    $bytes = New-Object byte[] 64
    $bytes[0] = 255; $bytes[1] = 216; $bytes[2] = 255; $bytes[3] = 224
    $bytes[4] = 255; $bytes[5] = 255; $bytes[62] = 255; $bytes[63] = 217
    [IO.File]::WriteAllBytes($brokenJpeg, $bytes)
    Check ($null -eq (Get-OptScalerImageBounds $brokenJpeg)) 'JPEG segments cannot escape the header read bound'
    Check ($null -eq (Get-OptScalerImageBounds (Join-Path $out 'missing.png'))) 'Missing image must be harmless'
    $exclusive = [IO.File]::Open($oldHero, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $exclusive.Dispose()

    $storeLogo = Image 'xbox/StoreLogo.png' 50 50
    $smallLogo = Image 'xbox/Logo.png' 150 150
    $largeLogo = Image 'xbox/LargeLogo.png' 480 480
    $wideLogo = Image 'xbox/WideLogo.png' 1240 600
    $splash = Image 'xbox/SplashScreen.png' 1920 1080
    $squareSplash = Image 'xbox/Assets/MCSplashScreen.png' 2048 2048
    $xbox = Get-OptScalerLocalArtwork -Source Xbox -Roots @((Join-Path $out 'xbox')) -MaxMilliseconds 2000
    Check ($xbox.cover -eq $largeLogo -and $xbox.hero -eq $splash) 'Xbox must prefer large square and real landscape splash, never wide logo or square splash'
    Report 'Xbox named assets' $xbox

    $mappedCover = Image 'mapped/Visual/Tile.scale-200.png' 640 640
    $mappedHero = Image 'mapped/Visual/Backdrop.scale-200.png' 1920 1080
    $outside = Image 'outside/Tile.png' 2048 2048
    $xml = '<Game><ShellVisuals Square480x480Logo="Visual\Tile.png" SplashScreenImage="Visual\Backdrop.png" StoreLogo="..\outside\Tile.png" /></Game>'
    [IO.File]::WriteAllText((Join-Path $out 'mapped/MicrosoftGame.Config'), $xml)
    $mapped = Get-OptScalerLocalArtwork -Source Xbox -Roots @((Join-Path $out 'mapped')) -MaxMilliseconds 2000
    Report 'Xbox manifest resources' $mapped
    Check ($mapped.cover -eq $mappedCover -and $mapped.hero -eq $mappedHero) 'GDK manifest qualifiers must remain associated with this source root'

    $appxCover = Image 'appx/Assets/Tile.targetsize-256.png' 256 256
    $appxHero = Image 'appx/Assets/Backdrop.png' 1280 720
    $xml = '<Package><Applications><Application><VisualElements Square150x150Logo="Assets\Tile.png"><SplashScreen Image="Assets\Backdrop.png" /></VisualElements></Application></Applications></Package>'
    [IO.File]::WriteAllText((Join-Path $out 'appx/appxmanifest.xml'), $xml)
    $appx = Get-OptScalerLocalArtwork -Source Xbox -Roots @((Join-Path $out 'appx')) -MaxMilliseconds 2000
    Check ($appx.cover -eq $appxCover -and $appx.hero -eq $appxHero) 'Appx visual mappings must use dimensions without using launch-helper identity'
    $xml = '<!DOCTYPE Game [<!ENTITY external SYSTEM "file:///C:/Windows/win.ini">]><Game><ShellVisuals Description="&external;" /></Game>'
    [IO.File]::WriteAllText((Join-Path $out 'mapped/MicrosoftGame.Config'), $xml)
    $invalid = Get-OptScalerLocalArtwork -Source Xbox -Roots @((Join-Path $out 'mapped')) -MaxMilliseconds 2000
    Check (-not $invalid.cover -and -not $invalid.hero) 'DTD/malformed metadata must not establish resource mappings'

    $mappedBudget = Get-OptScalerLocalArtwork -Source Xbox -Roots @((Join-Path $out 'appx')) -MaxEntries 3 -MaxMilliseconds 2000
    Check ($mappedBudget.examined -le 3) 'Manifest resource attributes must consume the same entry quota'
    $mappedBudget = Get-OptScalerLocalArtwork -Source Xbox -Roots @((Join-Path $out 'appx')) -MaxDirectories 1 -MaxMilliseconds 2000
    Check ($mappedBudget.directories -eq 1 -and -not $mappedBudget.cover -and -not $mappedBudget.hero) 'Manifest-mapped directories must obey the directory quota'
    $limited = Get-OptScalerLocalArtwork -Source Steam -Roots @((Join-Path $out 'new')) -MaxEntries 1 -MaxMilliseconds 2000
    Check ($limited.examined -le 1) 'Entry quota exceeded'
    $limited = Get-OptScalerLocalArtwork -Source Steam -Roots @((Join-Path $out 'new')) -MaxDirectories 1 -MaxMilliseconds 2000
    Check ($limited.directories -eq 1 -and -not $limited.cover -and -not $limited.hero) 'Directory quota exceeded'
    $limited = Get-OptScalerLocalArtwork -Source Steam -Roots @((Join-Path $out 'old')) -MaxImages 1 -MaxMilliseconds 2000
    Check ($limited.images -eq 1) 'Image quota exceeded'
    $limited = Get-OptScalerLocalArtwork -Source Steam -Roots @((Join-Path $out 'old')) -MaxMilliseconds 0
    Check ($limited.examined -eq 0) 'Zero time budget should perform no filesystem enumeration'
    $budget = @{ RemainingMilliseconds = 0 }
    $limited = Get-OptScalerLocalArtwork -Source Steam -Roots @((Join-Path $out 'old')) -Budget $budget
    Check ($limited.examined -eq 0 -and -not $limited.cover -and -not $limited.hero) 'Exhausted shared budget must skip further games'
    $budget.RemainingMilliseconds = 2000
    $null = Get-OptScalerLocalArtwork -Source Steam -Roots @((Join-Path $out 'old')) -Budget $budget
    Check ($budget.RemainingMilliseconds -lt 2000 -and $budget.RemainingMilliseconds -ge 0) 'Artwork calls must debit shared elapsed budget'

    if ($LocalSmoke) {
        $root = 'C:/Program Files (x86)/Steam/appcache/librarycache/2344520'
        Check (Test-Path -LiteralPath $root -PathType Container) 'Local Diablo IV cache is unavailable'
        $real = Get-OptScalerLocalArtwork -Source Steam -Roots @($root)
        Report 'READ-ONLY Diablo IV 2344520' $real
        Check ($real.hero -eq (Join-Path $root 'cd5817c4f6256d206c077cb611da21a5de4a5452/library_hero.jpg')) 'Real Diablo IV nested hero not selected'
        Check ($real.cover -eq (Join-Path $root '94a971a57dbfc1a8d9ddcb3ec39cfd93e2418d23/library_capsule.jpg')) 'Real Diablo IV nested portrait not selected'
        foreach ($game in @("C:/XboxGames/Baldur's Gate 3/Content", 'C:/XboxGames/Minecraft for Windows/Content')) {
            if (Test-Path -LiteralPath $game -PathType Container) {
                $real = Get-OptScalerLocalArtwork -Source Xbox -Roots @($game)
                Report ('READ-ONLY ' + $game) $real
            }
        }
    }
    Write-Output 'library-artwork: PASS (isolated fixtures; optional read-only local smoke; no launcher build)'
} finally { Remove-Item -LiteralPath $out -Recurse -Force -ErrorAction SilentlyContinue }
