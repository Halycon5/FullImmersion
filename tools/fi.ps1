# Talk to the room as an agent:  tools\fi.ps1 GET /scene   |   tools\fi.ps1 POST /move '{"panel": 0, "yaw": -30}'
#   tools\fi.ps1 GET "/look?yaw=0&w=1280&h=720" -Out room.png      Name yourself with -Agent (default "agent").
param([string]$Method = "GET", [string]$Path = "/scene", [string]$Body = "", [string]$Agent = "agent",
      [string]$Out = "")
$door = Get-Content "$env:APPDATA\FullImmersion\agent_door.json" | ConvertFrom-Json
$req = @{ Uri = "http://127.0.0.1:$($door.port)$Path"; Method = $Method; UseBasicParsing = $true;
           Headers = @{ Authorization = "Bearer $($door.token)"; "X-Agent" = $Agent } }
if ($Body) { $req.Body = [Text.Encoding]::UTF8.GetBytes($Body); $req.ContentType = "application/json" }
if ($Out) { $req.OutFile = $Out }
try {
  $r = Invoke-WebRequest @req
  if (-not $Out) { $r.Content }
} catch {
  $resp = $_.Exception.Response
  if ($resp) { "HTTP $([int]$resp.StatusCode)"; (New-Object IO.StreamReader($resp.GetResponseStream())).ReadToEnd() }
  else { throw }
}
