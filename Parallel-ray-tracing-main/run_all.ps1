# ARTI 503 - Assignment 2 - runs every experiment and saves the results.
# Run from the VS Code terminal, in the folder with raytracer.exe:
#   powershell -ExecutionPolicy Bypass -File run_all.ps1
# Takes about 10-15 minutes. Keep the laptop plugged in and close other apps.

param([string]$Best = "auto")   # auto = use your fastest correct version from step 5

$env:Path = "C:\msys64\ucrt64\bin;" + $env:Path
$exe = ".\raytracer.exe"
$csv = "results.csv"

if (-not (Test-Path $exe)) {
    Write-Host "raytracer.exe not found. Compile it first." -ForegroundColor Red
    exit 1
}

"step,version,threads,time_s,pixels_done,correct_pixels,lost_pixels,samples_done,correct_samples,lost_samples,result" | Out-File $csv -Encoding ascii

function Run-One($step, $version, $threads) {
    # stdout (the image) goes to image.ppm; stderr (the results) is captured here
    $lines = cmd /c "$exe $version $threads 2>&1 1>image.ppm"
    $row = $lines | Where-Object { $_ -like "CSV,*" }
    if ($row) {
        $data = $row.Substring(4)
        "$step,$data" | Out-File $csv -Append -Encoding ascii
        Write-Host ("{0,-6} {1,-10} {2} threads -> {3}" -f $step, $version, $threads, $data)
    } else {
        Write-Host "Run failed: $version $threads" -ForegroundColor Red
        $lines | Write-Host
    }
}

# Step 3: race version many times (10 runs each) with 2, 4 and 8 threads
foreach ($t in 2, 4, 8) { for ($k = 1; $k -le 10; $k++) { Run-One "step3" "race" $t } }

# Step 5: time every version, 5 runs each (sequential baseline once with 1 thread)
for ($k = 1; $k -le 5; $k++) { Run-One "step5" "seq" 1 }
foreach ($v in "race", "critical", "atomic", "reduction", "lock") {
    foreach ($t in 2, 4, 8) { for ($k = 1; $k -le 5; $k++) { Run-One "step5" $v $t } }
}

# Step 6: the agreed fastest correct version with 1, 2, 4 and 8 threads, 5 runs each
# Picks your fastest correct version at 8 threads from step 5, unless you give one:
#   powershell -ExecutionPolicy Bypass -File run_all.ps1 -Best lock
if ($Best -eq "auto") {
    $s5 = Import-Csv $csv | Where-Object { $_.step -eq "step5" -and $_.threads -eq "8" -and $_.result -eq "CORRECT" -and $_.version -ne "seq" }
    $best = ($s5 | Group-Object version | ForEach-Object {
        [pscustomobject]@{ v = $_.Name; t = ($_.Group | ForEach-Object { [double]$_.time_s } | Measure-Object -Average).Average }
    } | Sort-Object t | Select-Object -First 1).v
    Write-Host "Your fastest correct version at 8 threads: $best" -ForegroundColor Cyan
} else { $best = $Best }
foreach ($t in 1, 2, 4, 8) { for ($k = 1; $k -le 5; $k++) { Run-One "step6" $best $t } }

# Summary: average time per version and thread count
Write-Host ""
Write-Host "===== SUMMARY (average of each group of runs) =====" -ForegroundColor Cyan
Import-Csv $csv | Group-Object step, version, threads | ForEach-Object {
    $avg  = ($_.Group | Measure-Object -Property time_s -Average).Average
    $lost = ($_.Group | ForEach-Object { [long]$_.lost_pixels } | Measure-Object -Average).Average
    $wrong = ($_.Group | Where-Object { $_.result -eq "WRONG" }).Count
    "{0,-28} runs={1,2}  avg time={2,8:N3} s  wrong runs={3,2}  avg lost pixels={4,8:N0}" -f $_.Name, $_.Count, $avg, $wrong, $lost
} | Tee-Object -FilePath summary.txt

# Ready-to-copy values for the shared tables
$all = Import-Csv $csv
function Avg($rows) { ($rows | ForEach-Object { [double]$_.time_s } | Measure-Object -Average).Average }
$out = @("")

$r8 = @($all | Where-Object { $_.step -eq "step3" -and $_.threads -eq "8" })
$vals = $r8 | ForEach-Object { [long]$_.pixels_done } | Measure-Object -Minimum -Maximum
$bad = @($r8 | Where-Object { $_.result -eq "WRONG" }).Count
$out += "===== TABLE 4.3 (race version, 8 threads) ====="
$out += "Threads used: 8 | Runs tried: $($r8.Count) | Runs wrong: $bad | Smallest value: $($vals.Minimum) | Largest value: $($vals.Maximum) | Correct value: $($r8[0].correct_pixels)"

$out += ""
$out += "===== TABLE 5.2 (correct runs per technique, 2/4/8 threads) ====="
foreach ($v in "critical", "atomic", "reduction", "lock") {
    $g = @($all | Where-Object { $_.step -eq "step5" -and $_.version -eq $v })
    $ok = @($g | Where-Object { $_.result -eq "CORRECT" }).Count
    $t8 = Avg ($g | Where-Object { $_.threads -eq "8" })
    $out += "{0,-10} correct {1}/{2} at 2/4/8 threads | 8 threads: {3:N5} s" -f $v, $ok, $g.Count, $t8
}

$seq = Avg ($all | Where-Object { $_.step -eq "step5" -and $_.version -eq "seq" })
$t = @{}
foreach ($n in 1, 2, 4, 8) { $t[$n] = Avg ($all | Where-Object { $_.step -eq "step6" -and $_.threads -eq "$n" }) }
$sp = $seq / $t[8]
$eff = $sp / 8
$out += ""
$out += "===== TABLE 6.2 ($best version, 5 runs each) ====="
$out += "1 thr {0:N5} | 2 thr {1:N5} | 4 thr {2:N5} | 8 thr {3:N5} | speed-up at 8 {4:N3} (seq. baseline {5:N5}) | efficiency at 8 {6:N3} ({7:N2}%)" -f $t[1], $t[2], $t[4], $t[8], $sp, $seq, $eff, ($eff * 100)

$a8 = @{}
foreach ($v in "critical", "lock", "atomic", "reduction") {
    $a8[$v] = Avg ($all | Where-Object { $_.step -eq "step5" -and $_.version -eq $v -and $_.threads -eq "8" })
}
$fast = ($a8.GetEnumerator() | Sort-Object Value | Select-Object -First 1).Key
$out += ""
$out += "===== TABLE 6.3 (average time with 8 threads) ====="
$out += "Critical: {0:N5} | Lock: {1:N5} | Atomic: {2:N5} | Reduction: {3:N5} | Fastest: {4}" -f $a8["critical"], $a8["lock"], $a8["atomic"], $a8["reduction"], $fast

$out | Out-File summary.txt -Append -Encoding ascii
$out | ForEach-Object { Write-Host $_ }

Write-Host ""
Write-Host "Done. Send results.csv and summary.txt." -ForegroundColor Green