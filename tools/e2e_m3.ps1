# M3 end to end: an agent arranges a workspace through the door. The room must be running with the S3 test page
# open (tools\e2e_m3.bat does all of it). Writes PASS/FAIL lines; exits 1 if anything failed.
$ErrorActionPreference = "Stop"
$failed = 0
function Check($ok, $what) { if ($ok) { "PASS  $what" } else { "FAIL  $what"; $script:failed++ } }
$door = Get-Content "$env:APPDATA\FullImmersion\agent_door.json" | ConvertFrom-Json
$base = "http://127.0.0.1:$($door.port)"
function Call($method, $path, $body = "", $headers = $null) {
  if (-not $headers) { $headers = @{ Authorization = "Bearer $($door.token)"; "X-Agent" = "Tester" } }
  $a = @{ Uri = "$base$path"; Method = $method; UseBasicParsing = $true; Headers = $headers }
  if ($body) { $a.Body = [Text.Encoding]::UTF8.GetBytes($body); $a.ContentType = "application/json" }
  try { $r = Invoke-WebRequest @a; return @{ status = [int]$r.StatusCode; body = $r.Content; raw = $r.RawContentStream } }
  catch { $x = $_.Exception.Response; if ($x) { return @{ status = [int]$x.StatusCode; body = "" } } else { throw } }
}

Check ((Call GET /scene "" @{}).status -eq 401) "no token: refused"
Check ((Call GET /scene "" @{ Authorization = "Bearer $($door.token)"; Origin = "https://example.com" }).status -eq 403) "a web page: refused"
$scene = (Call GET /scene).body | ConvertFrom-Json
Check ($scene.panels.Count -eq 1) "the scene lists the one panel ($($scene.panels.Count))"
$wins = (Call GET /windows).body | ConvertFrom-Json
Check ($wins.Count -gt 0) "open windows are listed ($($wins.Count))"

Check ((Call POST /pin '{"match": "no window is called this"}').status -eq 404) "pinning a window that isn't there: 404"
Check ((Call POST /pin '{"match": "FullImmersion input test"}').status -eq 409) "pinning one already pinned: 409"
$r = Call POST /arrange '{"panels": [0], "around": -20, "distance": 1.6}'
Check ($r.status -eq 200) "arrange"
$scene = (Call GET /scene).body | ConvertFrom-Json
Check ([math]::Abs($scene.panels[0].yaw - (-20)) -lt 0.5 -and $scene.panels[0].distance -eq 1.6) "the panel went where it was put (yaw $($scene.panels[0].yaw))"

Check ((Call POST /presence '{"state": "testing", "says": "Arranging your desk, Milk.", "looking_at": 0}').status -eq 200) "presence"
$r = Call POST /note '{"text": "This panel is the S3 test page.", "near": 0}'
Check ($r.status -eq 200) "a note"
Check ((Call POST /click '{"panel": 0, "u": 0.5, "v": 0.5}').status -eq 403) "clicking into a window: refused"
Check ((Call POST /type '{"text": "hi"}').status -eq 403) "typing into a window: refused"

Start-Sleep -Milliseconds 400
$png = "$PSScriptRoot\..\out\e2e_m3_look.png"
Invoke-WebRequest -Uri "$base/look?yaw=-10&w=1280&h=720" -Headers @{ Authorization = "Bearer $($door.token)" } -OutFile $png -UseBasicParsing
$bytes = [IO.File]::ReadAllBytes($png)
Check ($bytes.Length -gt 10000 -and $bytes[1] -eq 0x50 -and $bytes[2] -eq 0x4E) "look gives a PNG ($($bytes.Length) bytes)"
$scene = (Call GET /scene).body | ConvertFrom-Json
Check ($scene.agents.Count -eq 1 -and $scene.agents[0].name -eq "Tester" -and $scene.notes.Count -eq 1) "the agent and the note are in the scene"

if ($failed) { "FAILED: $failed" ; exit 1 } else { "ALL PASSED" ; exit 0 }
