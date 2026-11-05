param([ValidateSet('Debug','Release')][string]$Configuration='Release', [switch]$FrameworkDependent)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $portableCmake=Get-ChildItem "$root/.tools/cmake-*/bin/cmake.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    $portableClang=Get-ChildItem "$root/.tools/llvm-mingw-*/bin/clang++.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($portableCmake) { $cmake=$portableCmake.FullName } else { $cmake=(Get-Command cmake -ErrorAction Stop).Source }
    if (Test-Path "$root/.tools/dotnet/dotnet.exe") { $dotnet="$root/.tools/dotnet/dotnet.exe" } else { $dotnet=(Get-Command dotnet -ErrorAction Stop).Source }
    if ($portableClang) {
        $env:PATH="$($portableClang.DirectoryName);$root/.tools/ninja;$env:PATH"
        & $cmake -S . -B build -G Ninja "-DCMAKE_CXX_COMPILER=$($portableClang.FullName)" "-DCMAKE_BUILD_TYPE=$Configuration"
    } else { & $cmake -S . -B build -A x64 }
    if ($LASTEXITCODE) { throw 'CMake configure failed' }
    & $cmake --build build --config $Configuration
    if ($LASTEXITCODE) { throw 'Native build failed' }
    & (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir build -C $Configuration --output-on-failure
    if ($LASTEXITCODE) { throw 'Core tests failed' }
    $env:DOTNET_CLI_TELEMETRY_OPTOUT='1'
    $selfContained=if($FrameworkDependent){'false'}else{'true'}
    & $dotnet publish desktop/QuadSync.Control -c $Configuration -r win-x64 --self-contained $selfContained -o dist -p:PublishSingleFile=true --configfile "$root/NuGet.Config"
    if ($LASTEXITCODE) { throw 'Desktop build failed' }
    $engine=if(Test-Path "build/$Configuration/QuadSync.Engine.exe"){ "build/$Configuration/QuadSync.Engine.exe" }else{'build/QuadSync.Engine.exe'}
    Copy-Item -LiteralPath $engine -Destination 'dist/QuadSync.Engine.exe' -Force
    Copy-Item README.md,LICENSE -Destination dist -Force
    Copy-Item native/third_party/LICENSE.json -Destination dist/LICENSE.nlohmann-json -Force
    Copy-Item docs -Destination dist -Recurse -Force
    New-Item -ItemType Directory -Force dist/licenses | Out-Null
    if($portableClang){
        Copy-Item (Join-Path (Split-Path $portableClang.DirectoryName) 'LICENSE.TXT') -Destination dist/licenses/LLVM-MinGW.txt -Force
    }
    $packageCache=if($env:NUGET_PACKAGES){$env:NUGET_PACKAGES}else{Join-Path $env:USERPROFILE '.nuget/packages'}
    foreach($package in @('microsoft.windowsdesktop.app.runtime.win-x64','microsoft.netcore.app.runtime.win-x64')) {
        Get-ChildItem "$packageCache/$package/*" -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            $versionPath=$_.FullName
            Get-ChildItem $versionPath -File | Where-Object Name -Match '^(LICENSE|THIRD.PARTY.NOTICES|ThirdPartyNotices)' | ForEach-Object {
                Copy-Item $_.FullName -Destination "dist/licenses/$package-$($_.Name)" -Force
            }
        }
    }
    Write-Host "Built: $root/dist/QuadSync.Control.exe"
} finally { Pop-Location }
