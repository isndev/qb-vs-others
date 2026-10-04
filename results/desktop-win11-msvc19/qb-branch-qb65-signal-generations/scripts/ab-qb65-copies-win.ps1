# QB-65 on Windows/MSVC, the multi-copy census: 4 byte-identical copies of the parent's binaries and 4 of the
# candidate's, each its own file (so its own physical pages -- the A/A pair showed a single copy can sit +-3-4 % off
# on ping-pong 2c from placement alone), random order per round over all 8, 40 rounds. Group = median of the copies.
param([int]$Rounds = 40, [int]$Seed = 6568)
$ErrorActionPreference = "Stop"
$S   = "<scratch>"
$QVO = "D:/repo/qb-dev/qb-vs-others"
$W   = "D:/repo/ab-qb65"
$OUT = "$QVO/results/desktop-win11-msvc19/qb-branch-qb65-signal-generations"; $ResultDir = "$OUT/copies"
if (Test-Path $ResultDir) { Remove-Item -Recurse -Force $ResultDir }
New-Item -ItemType Directory -Force $ResultDir | Out-Null
function ts { (Get-Date).ToUniversalTime().ToString("HH:mm:ss") }
$tags = @()
foreach ($g in "ctl", "cand") {
    for ($c = 1; $c -le 4; $c++) {
        $tag = "$($g)_$c"; $tags += $tag
        $D = "$W/copies/$tag"
        if (Test-Path $D) { Remove-Item -Recurse -Force $D }
        New-Item -ItemType Directory -Force $D | Out-Null
        Copy-Item "$W/$g/build/bin/*" "$D/"
    }
}
$rng = [System.Random]::new($Seed)
Write-Host "=== quiet 60 s $(ts)"; Start-Sleep -Seconds 60
Write-Host "=== copies $(ts) seed=$Seed"
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
Write-Host "=== AB-QB65-COPIES-DONE $(ts)"
