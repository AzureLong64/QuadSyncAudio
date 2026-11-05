param([string]$Engine = '')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if (!$Engine) {
    $Engine=Join-Path $root 'build/QuadSync.Engine.exe'
    if (!(Test-Path $Engine)) { $Engine=Join-Path $root 'build/Release/QuadSync.Engine.exe' }
}
$pipeName="QuadSyncAudio.test.$PID"
$process=Start-Process -FilePath $Engine -ArgumentList '--pipe',$pipeName -WindowStyle Hidden -PassThru
$pipe=[IO.Pipes.NamedPipeClientStream]::new('.', $pipeName, [IO.Pipes.PipeDirection]::InOut)
try {
    $pipe.Connect(6000)
    $utf8=[Text.UTF8Encoding]::new($false)
    $reader=[IO.StreamReader]::new($pipe,$utf8,$false,4096,$true)
    $writer=[IO.StreamWriter]::new($pipe,$utf8,4096,$true)
    $writer.AutoFlush=$true
    function Request($body) {
        $writer.WriteLine(($body | ConvertTo-Json -Depth 10 -Compress))
        $line=$reader.ReadLine()
        if (!$line) { throw 'Engine disconnected unexpectedly' }
        return $line | ConvertFrom-Json
    }
    function Assert($condition,$message) { if (!$condition) { throw $message } }
    $r=Request @{id=1;command='devices'}
    Assert ($r.ok -and $r.id -eq 1) 'Device enumeration or request ID failed'
    Write-Host "Device enumeration: $(@($r.data.devices).Count) endpoints"
    $channels=@(0..3 | ForEach-Object { @{gainDb=-6;distanceM=3.43} })
    $r=Request @{command='settings';master=0.5;mute=$true;channels=$channels}
    Assert $r.ok 'Valid settings rejected'
    $channels[3].gainDb=7
    Assert (!(Request @{command='settings';master=0.5;mute=$false;channels=$channels}).ok) 'Invalid gain accepted'
    Assert (!(Request @{command='start';deviceA='same';deviceB='same';source='tone'}).ok) 'Duplicate devices accepted'
    Assert (!(Request @{command='unknown'}).ok) 'Unknown command accepted'
    $writer.WriteLine('{broken')
    Assert (!(($reader.ReadLine() | ConvertFrom-Json).ok)) 'Malformed JSON accepted'
    Assert (Request @{command='status'}).ok 'Connection did not recover after invalid JSON'
    # Fragmentation and coalescing: byte-stream framing must survive both.
    $writer.Write('{"id":11,"command":')
    $writer.WriteLine('"status"}')
    Assert (($reader.ReadLine() | ConvertFrom-Json).id -eq 11) 'Fragmented request failed'
    $writer.WriteLine('{"id":12,"command":"status"}' + "`n" + '{"id":13,"command":"status"}')
    Assert (($reader.ReadLine() | ConvertFrom-Json).id -eq 12) 'First coalesced request failed'
    Assert (($reader.ReadLine() | ConvertFrom-Json).id -eq 13) 'Second coalesced request failed'
    $r=Request @{command='start';deviceA='same';deviceB='same';source='external'}
    Assert (!$r.ok) 'Duplicate external devices accepted'
    Assert (!(Request @{command='pcm';samples=@(1,2,3)}).ok) 'Misaligned PCM accepted'
    $r=Request @{command='pcm';samples=@(0.1,0.2,0.3,0.4)}
    Assert (!$r.ok) 'Stopped engine accepted and silently discarded PCM'
    Assert (Request @{command='stop'}).ok 'Stop failed'
    $reader.Dispose();$writer.Dispose();$pipe.Dispose()
    $pipe=[IO.Pipes.NamedPipeClientStream]::new('.', $pipeName, [IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect(3000)
    $reader=[IO.StreamReader]::new($pipe,$utf8,$false,4096,$true)
    $writer=[IO.StreamWriter]::new($pipe,$utf8,4096,$true);$writer.AutoFlush=$true
    Assert (Request @{command='status'}).ok 'Reconnect failed'
    Assert (Request @{command='shutdown'}).ok 'Shutdown failed'
    Assert ($process.WaitForExit(3000)) 'Engine did not exit'
    Assert ($process.ExitCode -eq 0) 'Engine returned failure'
    Write-Host 'Named-pipe integration tests passed.'
} finally {
    $pipe.Dispose()
    if (!$process.HasExited) { $process.Kill();$process.WaitForExit() }
    $process.Dispose()
}
