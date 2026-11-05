param([string]$Application = '')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if (!$Application) { $Application=Join-Path $root 'dist/QuadSync.Control.exe' }
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$p=Start-Process -FilePath $Application -WindowStyle Hidden -PassThru
try {
    $connected=$false
    for($i=0;$i -lt 30;$i++) {
        Start-Sleep -Milliseconds 500
        $p.Refresh()
        if($p.HasExited){throw 'Desktop exited during startup'}
        $windowHandle=$p.MainWindowHandle
        if($windowHandle -eq [IntPtr]::Zero){continue}
        $window=[Windows.Automation.AutomationElement]::FromHandle($windowHandle)
        $texts=$window.FindAll([Windows.Automation.TreeScope]::Descendants,[Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::ControlTypeProperty,[Windows.Automation.ControlType]::Text))
        foreach($text in $texts) {
            if($text.Current.Name -match '引擎已连接') { $connected=$true; break }
        }
        if($connected){break}
    }
    if(!$connected){throw 'Desktop did not connect to engine'}
    $sliders=$window.FindAll([Windows.Automation.TreeScope]::Descendants,[Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::ControlTypeProperty,[Windows.Automation.ControlType]::Slider))
    if($sliders.Count -ne 4){throw 'Expected four channel faders'}
    Write-Output 'Desktop connected to native engine; four faders present.'
    $window.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close()
    if(!$p.WaitForExit(15000)){throw 'Desktop shutdown timed out'}
    Write-Output "Desktop shutdown exit: $($p.ExitCode)"
} finally {if(!$p.HasExited){$p.Kill()};$p.Dispose()}
