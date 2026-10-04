# QB-65 on Windows/MSVC, the pacing probe: cand-s2 = cand with the parent's per-pass stop-token poll put back in
# front of the QB-65 check (same signal logic, same incidental per-pass work as before QB-65). RANDOM order per round
# (seeded Fisher-Yates); ctl / ctlcopy (byte-identical binaries, the A/A) / cand / cand-s2.
param([int]$Rounds = 80, [int]$Seed = 6566)
$ErrorActionPreference = "Stop"
$S   = "<scratch>"
$QVO = "D:/repo/qb-dev/qb-vs-others"
$W   = "D:/repo/ab-qb65"
$OUT = "$QVO/results/desktop-win11-msvc19/qb-branch-qb65-signal-generations"; $ResultDir = "$OUT/s2"
if (Test-Path $ResultDir) { Remove-Item -Recurse -Force $ResultDir }
New-Item -ItemType Directory -Force $ResultDir | Out-Null
function ts { (Get-Date).ToUniversalTime().ToString("HH:mm:ss") }
. "$QVO/tools/msvc-env.ps1"
$env:VCPKG_ROOT = "D:/repo/vcpkg"
$TreeDir = "$W/cand-s2"
if (Test-Path $TreeDir) { Remove-Item -Recurse -Force $TreeDir }
New-Item -ItemType Directory -Force $TreeDir | Out-Null
Copy-Item -Recurse "$W/cand/qb" "$TreeDir/qb"
& python "$S/patch-s2.py" "$TreeDir/qb/src/qb/core/VirtualCore.cpp"
if ($LASTEXITCODE -ne 0) { exit 1 }
& cmake -S "$W/ctl/qvo" -B "$TreeDir/build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DQVO_QB_DIR=$TreeDir/qb" -DQVO_WITH_CAF=OFF `
    -DQVO_WITH_SOBJECTIZER=OFF -DQVO_WITH_BASELINE=OFF "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" *> "$ResultDir/cfg-s2.log"
& cmake --build "$TreeDir/build" --target qvo-qb-savina-ping-pong qvo-qb-savina-thread-ring qvoprobe-pass-cost *> "$ResultDir/build-s2.log"
Write-Host "=== cand-s2 build rc=$LASTEXITCODE $(ts)"
if ($LASTEXITCODE -ne 0) { Get-Content "$ResultDir/build-s2.log" -Tail 15; exit 1 }

$rng = [System.Random]::new($Seed)
Write-Host "=== quiet 60 s $(ts)"; Start-Sleep -Seconds 60
Write-Host "=== s2 $(ts) seed=$Seed"
$tags  = @("ctl", "ctlcopy", "cand", "cand-s2")
$cells = @(@("ping-pong","cores=2","wait=1"), @("thread-ring","cores=2","wait=1"), @("ping-pong","cores=1","wait=1"))
$orders = @()
for ($round = 1; $round -le $Rounds; $round++) {
    $order = [string[]]$tags.Clone()
    for ($i = $order.Count - 1; $i -gt 0; $i--) { $j = $rng.Next($i + 1); $swap = $order[$i]; $order[$i] = $order[$j]; $order[$j] = $swap }
    $orders += ($order -join ",")
    foreach ($tag in $order) {
        $B = "$W/$tag/build/bin"
        foreach ($c in $cells) {
            & "$B/qvo-qb-savina-$($c[0]).exe" --repetitions 3 --warmup 1 --cpus 0,2 --out "$ResultDir/$($c[0])-$($c[1])-$($c[2])@$tag@$round.json" --param $c[1] --param $c[2] *> $null
        }
    }
}
$orders | Set-Content "$ResultDir/orders.txt"
$probeLog = "$ResultDir/probe.txt"; "" | Set-Content $probeLog
for ($pr = 1; $pr -le 8; $pr++) {
    foreach ($tag in @("ctl", "cand", "cand-s2")) {
        "$tag pass-cost k=1 $((& "$W/$tag/build/bin/qvoprobe-pass-cost.exe" 1 2 2 2>&1 | Select-Object -Last 1))" | Add-Content $probeLog
    }
}
& python "$S/ab-summarize3.py" $ResultDir | Tee-Object -FilePath "$ResultDir/summary.txt"
Get-Content $probeLog | Where-Object { $_ } | ForEach-Object { $m = [regex]::Match($_, 'ns_per_pass=([0-9.]+)'); "$(($_ -split ' ')[0]) $($m.Groups[1].Value)" } | Group-Object { ($_ -split ' ')[0] } | ForEach-Object { $v = $_.Group | ForEach-Object { [double](($_ -split ' ')[1]) } | Sort-Object; "pass-cost k=1 {0,-8} median {1:N2}  [{2:N2}..{3:N2}]" -f $_.Name, $v[[int]($v.Count/2)], $v[0], $v[-1] }
Write-Host "=== AB-QB65-S2-DONE $(ts)"
