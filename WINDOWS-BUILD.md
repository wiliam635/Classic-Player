# Classic Player 1.6.1 para Windows

## Artefatos produzidos

- Aplicativo standalone Windows x64.
- Instrumento VST3 Windows x64.
- Instalador `Classic-Player-1.6.1-Windows-x64-Setup.exe`.

## Compilacao recomendada

O projeto deve ser compilado em Windows 10/11 x64 ou no runner
`windows-2022` do GitHub Actions. O preset usa Visual Studio 2022 e o triplet
`x64-windows-static`, para que FluidSynth e OpenSSL sejam incorporados ao
binario e nao causem falhas por DLL ausente.

Dependencias do ambiente:

- Visual Studio 2022 com Desktop development with C++;
- CMake;
- Git;
- vcpkg, indicado pela variavel `VCPKG_ROOT`;
- Inno Setup 6 para gerar o instalador.

No PowerShell, a partir desta pasta:

```powershell
$env:VCPKG_DEFAULT_TRIPLET = "x64-windows-static"
cmake --preset windows-x64
cmake --build --preset windows-release
scripts\package-windows.ps1
```

O instalador sera gravado na pasta `outputs\installers` da raiz do projeto.

## Audio

A compilacao padrao disponibiliza os drivers de audio nativos suportados pelo
JUCE no Windows, incluindo Windows Audio/WASAPI. Para uma compilacao com ASIO,
configure tambem `ASIO_SDK_DIR` apontando para o SDK ASIO antes de executar o
CMake.

## Validacao obrigatoria

Antes da distribuicao, testar em um Windows real:

1. abertura do standalone;
2. selecao da interface e do buffer de audio;
3. entrada de um ou mais controladores MIDI;
4. carregamento e troca de SF2 em todas as layers;
5. MIDI Learn sem interferir no sustain;
6. teclado virtual, cores e detector de cifras;
7. abertura do VST3 em uma DAW de teste.
# ASIO

O suporte ASIO é ativado quando o SDK da Steinberg está disponível em
`ASIO_SDK_DIR` (ou em `third_party/asio-sdk` para um build local). O SDK possui
licença proprietária e GPLv3; escolha a licença aplicável antes de distribuir
um instalador. O workflow público não baixa nem redistribui o SDK sem essa
decisão.

Exemplo de build local:

```powershell
cmake --preset windows-x64 -DASIO_SDK_DIR="C:\SDK\ASIOSDK"
cmake --build --preset windows-release
```

O log de configuração deve conter `ASIO habilitado`. Sem o SDK, o aplicativo
continua usando WASAPI/DirectSound.
