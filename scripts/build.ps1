param(
  [string]$BuildDir = "$PSScriptRoot\..\build-release",
  [string]$Compiler = '',
  [int]$Jobs = 12,
  [switch]$Pch,
  [switch]$NoTests
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path
Push-Location $root
try {
  $environmentJson = python -c 'import configure,json; print(json.dumps(configure.developer_environment()))'
  if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the C++ environment' }
  $developerEnvironment = $environmentJson | ConvertFrom-Json
  foreach ($property in $developerEnvironment.PSObject.Properties) {
    [Environment]::SetEnvironmentVariable($property.Name, $property.Value, 'Process')
  }
  $configureArgs = @('-S', $root, '-B', $BuildDir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release')
  if ($Compiler) { $configureArgs += "-DCMAKE_CXX_COMPILER=$Compiler" }
  if ($NoTests) { $configureArgs += '-DBUILD_TESTING=OFF' }
  if ($Pch) { $configureArgs += '-DSNINJA_PCH=ON' }
  $seed = Join-Path $root 'build-bootstrap\sninja.exe'
  if (-not (Get-Command ninja -ErrorAction SilentlyContinue) -and -not (Test-Path $seed)) {
    python configure.py --bootstrap --jobs $Jobs
    if ($LASTEXITCODE -ne 0) { throw 'Bootstrap failed' }
  }
  if (Test-Path $seed) { $configureArgs += "-DCMAKE_MAKE_PROGRAM=$seed" }
  & cmake @configureArgs
  if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
  & cmake --build $BuildDir --parallel $Jobs
  if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
  if (-not $NoTests) {
    & ctest --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
  }
} finally {
  Pop-Location
}
