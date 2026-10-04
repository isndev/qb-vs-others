# QB-65 bisect on Windows/MSVC: is the 2-core spin regression the CODE or the LAYOUT?
#   ctl  = 9dfe1e08 (existing build), cand = 72f1fb5f (existing build),
#   lay  = ctl + ONLY a VirtualCore member of _signal_seen's size, appended after _stop_token (pure layout).
# Rotating order per round (no variant always first), 2c spin cells + one 1c control, quiet host.
param([int]$Rounds = 10)
$ErrorActionPreference = "Stop"
$S   = "<scratch>"
$QVO = "D:/repo/qb-dev/qb-vs-others"
$W   = "D:/repo/ab-qb65"
$OUT = "$QVO/results/desktop-win11-msvc19/qb-branch-qb65-signal-generations"; $R = "$OUT/bisect"
. "$QVO/tools/msvc-env.ps1"
$env:VCPKG_ROOT = "D:\repo\vcpkg"
if (Test-Path $R) { Remove-Item -Recurse -Force $R }
New-Item -ItemType Directory -Force $R | Out-Null
function ts { (Get-Date).ToUniversalTime().ToString("HH:mm:ss") }

$T = "$W/lay"
if (Test-Path $T) { Remove-Item -Recurse -Force $T }
New-Item -ItemType Directory -Force $T | Out-Null
Copy-Item -Recurse "$W/ctl/qb" "$T/qb"
& python "$S/patch-lay.py" "$T/qb/src/qb/core/VirtualCore.h"
if ($LASTEXITCODE -ne 0) { exit 1 }
& cmake -S "$W/ctl/qvo" -B "$T/build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DQVO_QB_DIR=$T/qb" -DQVO_WITH_CAF=OFF `
    -DQVO_WITH_SOBJECTIZER=OFF -DQVO_WITH_BASELINE=OFF "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" *> "$R/cfg-lay.log"
& cmake --build "$T/build" --target qvo-qb-savina-ping-pong qvo-qb-savina-thread-ring *> "$R/build-lay.log"
Write-Host "=== lay build rc=$LASTEXITCODE $(ts)"
if ($LASTEXITCODE -ne 0) { Get-Content "$R/build-lay.log" -Tail 15; exit 1 }

Write-Host "=== quiet 60 s $(ts)"; Start-Sleep -Seconds 60
Write-Host "=== bisect $(ts)"
$tags  = @("ctl", "cand", "lay")
$cells = @(@("thread-ring","cores=2","wait=1"), @("ping-pong","cores=2","wait=1"), @("thread-ring","cores=1","wait=1"))
for ($round = 1; $round -le $Rounds; $round++) {
    for ($i = 0; $i -lt 3; $i++) {
        $tag = $tags[($round + $i) % 3]
        $B = "$W/$tag/build/bin"
        foreach ($c in $cells) {
            & "$B/qvo-qb-savina-$($c[0]).exe" --repetitions 3 --warmup 1 --cpus 0,2 --out "$R/$($c[0])-$($c[1])-$($c[2])@$tag@$round.json" --param $c[1] --param $c[2] *> $null
        }
    }
}
& python "$S/ab-summarize3.py" $R | Tee-Object -FilePath "$R/summary.txt"
Write-Host "=== AB-QB65-BISECT-DONE $(ts)"
