$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$iscc = if ($env:ISCC_PATH) { $env:ISCC_PATH } else { "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" }
$artefacts = Join-Path $root "build\windows-x64\ClassicPlayer_artefacts\Release"
$standaloneCandidates = @(
    (Join-Path $artefacts "ClassicPlayerApp_artefacts\Release\Classic Player.exe"),
    (Join-Path $artefacts "Standalone\Classic Player.exe")
)
$standalone = $standaloneCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
$vst3 = Join-Path $artefacts "VST3\Classic Player.vst3"

if (-not $standalone) {
    throw "Executável Windows não encontrado nos artefatos: $($standaloneCandidates -join ', ')"
}

# JUCE may place bundle resources in a target-specific directory depending on
# the generator. Stage WebUI beside the executable so Inno Setup always sees it.
$webUiCandidates = @(
    (Join-Path (Split-Path $standalone -Parent) "Resources\WebUI"),
    (Join-Path $artefacts "Standalone\Resources\WebUI"),
    (Join-Path $artefacts "ClassicPlayerApp_artefacts\Release\Resources\WebUI"),
    (Join-Path $root "Resources\WebUI")
)
$webUiSource = $webUiCandidates | Where-Object { Test-Path (Join-Path $_ "index.html") } | Select-Object -First 1
if (-not $webUiSource) {
    throw "WebUI não encontrada nos artefatos Windows. Esperado index.html em Resources\WebUI."
}
$webUiDestination = Join-Path (Split-Path $standalone -Parent) "Resources\WebUI"
New-Item -ItemType Directory -Force -Path $webUiDestination | Out-Null
$sourceFull = [System.IO.Path]::GetFullPath($webUiSource).TrimEnd('\')
$destinationFull = [System.IO.Path]::GetFullPath($webUiDestination).TrimEnd('\')
if ($sourceFull -ne $destinationFull) {
    Copy-Item -Path (Join-Path $webUiSource "*") -Destination $webUiDestination -Recurse -Force
}
if (-not (Test-Path (Join-Path $webUiDestination "index.html"))) {
    throw "Falha ao preparar Resources\WebUI ao lado do executável."
}

# Stage the complete WebUI app in the conventional Standalone directory used
# by the Inno script, regardless of which CMake generator produced it.
$stagedStandalone = Join-Path $artefacts "Standalone"
New-Item -ItemType Directory -Force -Path $stagedStandalone | Out-Null
if ([System.IO.Path]::GetFullPath((Split-Path $standalone -Parent)) -ne [System.IO.Path]::GetFullPath($stagedStandalone)) {
    Copy-Item -Path $standalone -Destination (Join-Path $stagedStandalone "Classic Player.exe") -Force
    Copy-Item -Path $webUiDestination -Destination (Join-Path $stagedStandalone "Resources\WebUI") -Recurse -Force
    Get-ChildItem -Path (Split-Path $standalone -Parent) -Filter "*.dll" -File -ErrorAction SilentlyContinue |
        Copy-Item -Destination $stagedStandalone -Force
}
if (-not (Test-Path $iscc)) {
    throw "Inno Setup não encontrado. Defina ISCC_PATH."
}

& $iscc "$root\installer\windows\ClassicPlayer.iss"
if ($LASTEXITCODE -ne 0) { throw "Falha ao criar o instalador Windows." }
