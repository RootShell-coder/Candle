$s = Get-Content -Raw -Path "include\matrix16x8.h"
$m = [regex]::Match($s, 'anim\[\]\s*=\s*\{([\s\S]*?)\};')
if (-not $m.Success) { Write-Output 'anim not found'; exit 1 }
$body = $m.Groups[1].Value
# find tokens like 0x.. or decimal
$tokens = [regex]::Matches($body, '0x[0-9A-Fa-f]+|\d+') | ForEach-Object { $_.Value }
$data = @()
foreach ($t in $tokens) {
    if ($t.StartsWith('0x')) { $data += [Convert]::ToInt32($t,16) } else { $data += [int]$t }
}
$p=0
$blocks=@()
while ($p + 1 -lt $data.Count) {
    $a = $data[$p]
    if ($a -ge 0x90) { $p += 1; continue }
    $b = $data[$p+1]
    $x1 = $a -shr 4; $y1 = $a -band 0x0F
    $x2 = $b -shr 4; $y2 = $b -band 0x0F
    if ($x2 -lt $x1 -or $y2 -lt $y1 -or $x1 -gt 15 -or $x2 -gt 15 -or $y1 -gt 15 -or $y2 -gt 15) { break }
    $count = ($x2 - $x1 + 1) * ($y2 - $y1 + 1)
    if ($p + 2 + $count -gt $data.Count) { break }
    $payload = @()
    for ($k = 0; $k -lt $count; $k++) { $payload += $data[$p+2+$k] }
    $blocks += ,@{pos=$p; x1=$x1; y1=$y1; x2=$x2; y2=$y2; count=$count; payload=$payload}
    $p = $p + 2 + $count
}
Write-Output "total bytes: $($data.Count), blocks parsed: $($blocks.Count)"
Write-Output "Blocks that include x=8:"
$found = $false
for ($i=0; $i -lt $blocks.Count; $i++) {
    $blk = $blocks[$i]
    if ($blk.x1 -le 8 -and $blk.x2 -ge 8) {
        $found = $true
        $sample = $blk.payload[0..([Math]::Min(7,$blk.payload.Count-1))] -join ','
        Write-Output ("block#{0} at byte {1}: x1={2} y1={3} x2={4} y2={5} count={6} payload_sample=[{7}]" -f $i, $blk.pos, $blk.x1, $blk.y1, $blk.x2, $blk.y2, $blk.count, $sample)
    }
}
Write-Output "\nFound blocks covering x=8: $found"
# columns coverage
$ctr = @{}
foreach ($blk in $blocks) {
    for ($x=$blk.x1; $x -le $blk.x2; $x++) {
        if (-not $ctr.ContainsKey($x)) { $ctr[$x]=0 }
        $ctr[$x] = $ctr[$x] + 1
    }
}
Write-Output "\nColumns coverage (x:count blocks):"
$keys = $ctr.Keys | Sort-Object
foreach ($k in $keys) { Write-Output "${k}: $($ctr[$k])" }
