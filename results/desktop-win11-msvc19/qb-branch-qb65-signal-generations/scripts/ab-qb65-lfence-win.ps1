# Idle pacing on Windows/MSVC: ctllf / candlf = parent / QB-65 with an lfence before the idle pass's clock read.
# Multi-copy (3 physical copies per group), random order per round, 30 rounds.
param([int]$Rounds = 30, [int]$Seed = 6569)
$ErrorActionPreference = "Stop"
$S   = "<scratch>"
$QVO = "D:/repo/qb-dev/qb-vs-others"
$W   = "D:/repo/ab-qb65"
$OUT = "$QVO/results/desktop-win11-msvc19/qb-branch-qb65-signal-generations"; $ResultDir = "$OUT/lfence"
if (Test-Path $ResultDir) { Remove-Item -Recurse -Force $ResultDir }
New-Item -ItemType Directory -Force $ResultDir | Out-Null
function ts { (Get-Date).ToUniversalTime().ToString("HH:mm:ss") }
. "$QVO/tools/msvc-env.ps1"
$env:VCPKG_ROOT = "D:/repo/vcpkg"
foreach ($v in @(@("ctllf", "ctl"), @("candlf", "cand"))) {
    $TreeDir = "$W/$($v[0])"
    if (Test-Path $TreeDir) { Remove-Item -Recurse -Force $TreeDir }
    New-Item -ItemType Directory -Force $TreeDir | Out-Null
    Copy-Item -Recurse "$W/$($v[1])/qb" "$TreeDir/qb"
    & python "$S/patch-lfence.py" "$TreeDir/qb/src/qb/core/VirtualCore.cpp"
    if ($LASTEXITCODE -ne 0) { exit 1 }
    & cmake -S "$W/ctl/qvo" -B "$TreeDir/build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DQVO_QB_DIR=$TreeDir/qb" -DQVO_WITH_CAF=OFF `
        -DQVO_WITH_SOBJECTIZER=OFF -DQVO_WITH_BASELINE=OFF "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" *> "$ResultDir/cfg-$($v[0]).log"
    & cmake --build "$TreeDir/build" --target qvo-qb-savina-ping-pong qvo-qb-savina-thread-ring qvoprobe-pass-cost *> "$ResultDir/build-$($v[0]).log"
    Write-Host "=== $($v[0]) build rc=$LASTEXITCODE $(ts)"
    if ($LASTEXITCODE -ne 0) { Get-Content "$ResultDir/build-$($v[0]).log" -Tail 15; exit 1 }
}
$tags = @()
foreach ($g in "ctl", "cand", "ctllf", "candlf") {
    for ($c = 1; $c -le 3; $c++) {
        $tag = "$($g)_$c"; $tags += $tag
        $D = "$W/copies/$tag"
        if (Test-Path $D) { Remove-Item -Recurse -Force $D }
        New-Item -ItemType Directory -Force $D | Out-Null
        Copy-Item "$W/$g/build/bin/*" "$D/"
    }
}
$rng = [System.Random]::new($Seed)
Write-Host "=== quiet 60 s $(ts)"; Start-Sleep -Seconds 60
Write-Host "=== lfence $(ts) seed=$Seed"
$cells = @(@("ping-pong","cores=2","wait=1"), @("thread-ring","cores=2","wait=1"), @("ping-pong","cores=1","wait=1"))
$orders = @()
for ($round = 1; $round -le $Rounds; $round++) {
    $order = [string[]]$tags.Clone()
    for ($i = $order.Count - 1; $i -gt 0; $i--) { $j = $rng.Next($i + 1); $swap = $order[$i]; $order[$i] = $order[$j]; $order[$j] = $swap }
    $orders += ($order -join ",")
    foreach ($tag in $order) {
        $B = "$W/copies/$tag"
        foreach ($c in $cells) {
            & "$B/qvo-qb-savina-$($c[0]).exe" --repetitions 3 --warmup 1 --cpus 0,2 --out "$ResultDir/$($c[0])-$($c[1])-$($c[2])@$tag@$round.json" --param $c[1] --param $c[2] *> $null
        }
    }
}
$orders | Set-Content "$ResultDir/orders.txt"
& python "$S/ab-summarize-groups.py" $ResultDir | Tee-Object -FilePath "$ResultDir/summary.txt"
Write-Host "=== AB-QB65-LFENCE-DONE $(ts)"
