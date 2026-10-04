# QB-65 on Windows/MSVC: the frame-alignment probe -- cand-fa = cand with __workflow__ frame 64-aligned as in ctl.
# pass (every later loop instruction moves N bytes, no logic changes). If the 2c cells swing across N as much as
# cand differs from ctl, the difference is alignment, not the signal check. ctlcopy = A/A noise floor.
param([int]$Rounds = 60)
$ErrorActionPreference = "Stop"
$S   = "<scratch>"
$QVO = "D:/repo/qb-dev/qb-vs-others"
$W   = "D:/repo/ab-qb65"
$OUT = "$QVO/results/desktop-win11-msvc19/qb-branch-qb65-signal-generations"; $R = "$OUT/frame"
. "$QVO/tools/msvc-env.ps1"
$env:VCPKG_ROOT = "D:\repo\vcpkg"
if (Test-Path $R) { Remove-Item -Recurse -Force $R }
New-Item -ItemType Directory -Force $R | Out-Null
function ts { (Get-Date).ToUniversalTime().ToString("HH:mm:ss") }

$variants = @(,@("cand-fa", "cand", 0))
foreach ($v in $variants) {
    $tag = $v[0]; $T = "$W/$tag"
    if (Test-Path $T) { Remove-Item -Recurse -Force $T }
    New-Item -ItemType Directory -Force $T | Out-Null
    Copy-Item -Recurse "$W/$($v[1])/qb" "$T/qb"
    & python "$S/patch-frame.py" "$T/qb/src/qb/core/VirtualCore.cpp"
    if ($LASTEXITCODE -ne 0) { exit 1 }
    & cmake -S "$W/ctl/qvo" -B "$T/build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DQVO_QB_DIR=$T/qb" -DQVO_WITH_CAF=OFF `
        -DQVO_WITH_SOBJECTIZER=OFF -DQVO_WITH_BASELINE=OFF "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" *> "$R/cfg-$tag.log"
    & cmake --build "$T/build" --target qvo-qb-savina-ping-pong qvo-qb-savina-thread-ring *> "$R/build-$tag.log"
    Write-Host "=== $tag build rc=$LASTEXITCODE $(ts)"
    if ($LASTEXITCODE -ne 0) { Get-Content "$R/build-$tag.log" -Tail 15; exit 1 }
}

Write-Host "=== quiet 60 s $(ts)"; Start-Sleep -Seconds 60
Write-Host "=== frame $(ts)"
$tags  = @("ctl", "ctlcopy", "cand", "cand-fa")
$cells = @(@("ping-pong","cores=2","wait=1"), @("thread-ring","cores=2","wait=1"), @("ping-pong","cores=1","wait=1"))
for ($round = 1; $round -le $Rounds; $round++) {
    for ($i = 0; $i -lt $tags.Count; $i++) {
        $tag = $tags[($round + $i) % $tags.Count]
        $B = "$W/$tag/build/bin"
        foreach ($c in $cells) {
            & "$B/qvo-qb-savina-$($c[0]).exe" --repetitions 3 --warmup 1 --cpus 0,2 --out "$R/$($c[0])-$($c[1])-$($c[2])@$tag@$round.json" --param $c[1] --param $c[2] *> $null
        }
    }
}
& python "$S/ab-summarize3.py" $R | Tee-Object -FilePath "$R/summary.txt"
Write-Host "=== AB-QB65-FRAME-DONE $(ts)"
