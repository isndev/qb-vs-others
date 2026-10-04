# QB-65 A/B on Windows/MSVC -- ctl = the parent (one pending-signal slot), cand = per-signum generations.
# Same instruments as the WSL2 run: pass-cost k=1,2,4, the one-core ping-pong and ring, the two-core spin cells,
# big and fib -- interleaved rounds, pinned to CPUs 0,2, after 60 s of quiet. QUIET host: nothing else runs.
param([Parameter(Mandatory)][string]$Ctl, [Parameter(Mandatory)][string]$Cand, [int]$Rounds = 8)
$ErrorActionPreference = "Stop"
$S   = "<scratch>"
$QB  = "D:/repo/qb-dev/qb"; $QVO = "D:/repo/qb-dev/qb-vs-others"
$W   = "D:/repo/ab-qb65"
$OUT = "$QVO/results/desktop-win11-msvc19/qb-branch-qb65-signal-generations"; $R = "$OUT/census"
. "$QVO/tools/msvc-env.ps1"
$env:VCPKG_ROOT = "D:\repo\vcpkg"
if (Test-Path $R) { Remove-Item -Recurse -Force $R }
New-Item -ItemType Directory -Force $R | Out-Null
function ts { (Get-Date).ToUniversalTime().ToString("HH:mm:ss") }
$targets = @("qvo-qb-savina-ping-pong", "qvo-qb-savina-thread-ring", "qvo-qb-savina-fib", "qvo-qb-savina-big", "qvoprobe-pass-cost", "qvoprobe-ask-cost")
foreach ($pair in @(@("ctl", $Ctl), @("cand", $Cand))) {
    $tag = $pair[0]; $sha = $pair[1]; $T = "$W/$tag"
    if (Test-Path $T) { Remove-Item -Recurse -Force $T }
    New-Item -ItemType Directory -Force "$T/qb", "$T/qvo" | Out-Null
    & git -C $QB archive -o "$T/qb.tar" $sha;  & "$env:SystemRoot\System32/tar.exe" -xf "$T/qb.tar" -C "$T/qb"
    & git -C $QVO archive -o "$T/qvo.tar" HEAD; & "$env:SystemRoot\System32/tar.exe" -xf "$T/qvo.tar" -C "$T/qvo"
    $marker = (Select-String -Path "$T/qb/src/qb/core/Main.h" -Pattern "_signal_raised" | Measure-Object).Count
    Write-Host "$tag $sha signal_raised=$marker $(ts)"
    & cmake -S "$T/qvo" -B "$T/build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DQVO_QB_DIR=$T/qb" -DQVO_WITH_CAF=OFF `
        -DQVO_WITH_SOBJECTIZER=OFF -DQVO_WITH_BASELINE=OFF "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" *> "$R/cfg-$tag.log"
    & cmake --build "$T/build" --target $targets *> "$R/build-$tag.log"
    Write-Host "=== $tag build rc=$LASTEXITCODE warn=$((Select-String -Path "$R/build-$tag.log" -Pattern 'warning C' | Measure-Object).Count) $(ts)"
    if ($LASTEXITCODE -ne 0) { Get-Content "$R/build-$tag.log" -Tail 15; exit 1 }
}
Write-Host "=== quiet 60 s $(ts)"; Start-Sleep -Seconds 60
Write-Host "=== census $(ts)"
$cells = @(@("ping-pong","cores=1","wait=1"), @("thread-ring","cores=1","wait=1"), @("ping-pong","cores=2","wait=1"),
           @("thread-ring","cores=2","wait=1"), @("big","cores=2","wait=1"), @("fib","cores=1","wait=1"))
for ($round = 1; $round -le $Rounds; $round++) {
    foreach ($tag in @("cand", "ctl")) {
        $B = "$W/$tag/build/bin"
        foreach ($c in $cells) {
            & "$B/qvo-qb-savina-$($c[0]).exe" --repetitions 3 --warmup 1 --cpus 0,2 --out "$R/$($c[0])-$($c[1])-$($c[2])@$tag@$round.json" --param $c[1] --param $c[2] *> $null
        }
    }
}
$probeLog = "$OUT/probe.txt"; "" | Set-Content $probeLog
for ($r = 1; $r -le 6; $r++) {
    foreach ($tag in @("ctl", "cand")) {
        $B = "$W/$tag/build/bin"
        foreach ($k in 1, 2, 4) { "$tag pass-cost k=$k $((& "$B/qvoprobe-pass-cost.exe" $k 2 2 2>&1 | Select-Object -Last 1))" | Add-Content $probeLog }
        "$tag ask-cost ask  $((& "$B/qvoprobe-ask-cost.exe" ask 2 2 2>&1 | Select-Object -Last 1))" | Add-Content $probeLog
    }
}
& python "$S/ab-summarize.py" $R $probeLog | Tee-Object -FilePath "$OUT/summary.txt"
Write-Host "=== AB-QB65-WIN-DONE $(ts)"
