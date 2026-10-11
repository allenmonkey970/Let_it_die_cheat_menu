# Deploy the overlay into the game: dinput8.dll + lid_mod\ (tools, item catalog). Game must be closed.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$win64 = 'C:\Users\antho\Downloads\LET.IT.DIE-SteamRIP.com\LET.IT.DIE\Binaries\Win64'
$mod = Join-Path $win64 'lid_mod'

if (Get-Process BrgGame-Steam -ErrorAction SilentlyContinue) { throw 'LET IT DIE is running - close it first.' }

New-Item -ItemType Directory -Force (Join-Path $mod 'tools') | Out-Null
Copy-Item (Join-Path $root 'build\dinput8.dll') (Join-Path $win64 'dinput8.dll') -Force
foreach ($f in 'lid_save.py', 'lid_fighter.py', 'lid_apply_queue.py') {
    Copy-Item (Join-Path $root "db\$f") (Join-Path $mod "tools\$f") -Force
}
foreach ($f in 'items.tsv', 'icons_atlas.png') {
    $src = Join-Path $root "assets\$f"
    if (Test-Path $src) { Copy-Item $src (Join-Path $mod $f) -Force } else { Write-Warning "assets\$f not built yet" }
}
Get-ChildItem $mod -Recurse -File | Select-Object @{n='File';e={$_.FullName.Substring($win64.Length + 1)}}, Length, LastWriteTime
