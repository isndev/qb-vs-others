# QB-65 on Windows/MSVC, the final census: RANDOM order per round (seeded Fisher-Yates), so no tag always runs right
# after another -- the fixed rotation of the earlier scripts put ctlcopy behind ctl in every round and its A/A came
# out negative four times out of four. ctl / ctlcopy (byte-identical binaries) / cand, existing builds, 100 rounds.
param([int]$Rounds = 100, [int]$Seed = 6565)
$ErrorActionPreference = "Stop"
$S   = "<scratch>"
$QVO = "D:/repo/qb-dev/qb-vs-others"
$W   = "D:/repo/ab-qb65"
$OUT = "$QVO/results/desktop-win11-msvc19/qb-branch-qb65-signal-generations"; $ResultDir = "$OUT/random"
if (Test-Path $ResultDir) { Remove-Item -Recurse -Force $ResultDir }
New-Item -ItemType Directory -Force $ResultDir | Out-Null
function ts { (Get-Date).ToUniversalTime().ToString("HH:mm:ss") }
$rng = [System.Random]::new($Seed)
Write-Host "=== quiet 60 s $(ts)"; Start-Sleep -Seconds 60
Write-Host "=== random $(ts) seed=$Seed"
$tags  = @("ctl", "ctlcopy", "cand")
$cells = @(@("ping-pong","cores=2","wait=1"), @("thread-ring","cores=2","wait=1"), @("ping-pong","cores=1","wait=1"))
$orders = @()
for ($round = 1; $round -le $Rounds; $round++) {
    $order = [string[]]$tags.Clone()
    for ($i = $order.Count - 1; $i -gt 0; $i--) { $j = $rng.Next($i + 1); $t = $order[$i]; $order[$i] = $order[$j]; $order[$j] = $t }
    $orders += ($order -join ",")
    foreach ($tag in $order) {
        $B = "$W/$tag/build/bin"
        foreach ($c in $cells) {
            & "$B/qvo-qb-savina-$($c[0]).exe" --repetitions 3 --warmup 1 --cpus 0,2 --out "$ResultDir/$($c[0])-$($c[1])-$($c[2])@$tag@$round.json" --param $c[1] --param $c[2] *> $null
        }
    }
}
$orders | Set-Content "$ResultDir/orders.txt"
& python "$S/ab-summarize3.py" $ResultDir | Tee-Object -FilePath "$ResultDir/summary.txt"
Write-Host "=== AB-QB65-RANDOM-DONE $(ts)"
