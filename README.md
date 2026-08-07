# Classic Player — Standalone, VST3 e Audio Unit

Projeto nativo do Classic Player baseado em JUCE e FluidSynth. O mesmo processador de áudio é compilado como:

- aplicativo Standalone para Windows e macOS;
- instrumento VST3 para DAWs no Windows e macOS;
- Audio Unit (AU) para DAWs no macOS.

## Funcionamento do áudio

No **Standalone**, o painel padrão de áudio do JUCE permite selecionar dispositivo, sample rate e buffer. No Windows, WASAPI funciona sem SDK adicional. Para compilar suporte ASIO direto no Standalone, informe `ASIO_SDK_DIR` e respeite a licença do SDK da Steinberg. No macOS, o sistema usa Core Audio.

No **VST3/AU**, a interface de áudio, ASIO/Core Audio, sample rate e buffer são escolhidos na DAW. O plugin recebe áudio e MIDI do host; por isso não deve abrir a interface de áudio diretamente.

## Recursos desta base nativa

- quatro layers SF2 independentes;
- polifonia configurada em 4096 vozes por synth, limitada na prática por CPU e memória;
- split por nota, oitava, mono/poli e sustain individual no motor;
- volume e pan automatizáveis pela DAW por layer;
- estado do projeto salvo pela DAW, incluindo caminhos dos SF2;
- proteção offline ECDSA P-256 compatível com os 500 códigos já emitidos;
- saída estéreo com ganho conservador por layer para evitar clipping ao empilhar timbres.

## Compilar

Pré-requisitos: CMake 3.22+, compilador C++20, vcpkg, JUCE e as licenças aplicáveis.

### macOS

```bash
export VCPKG_ROOT=/caminho/para/vcpkg
cmake -S . -B build/macos-arm64-make -G "Unix Makefiles" \
  -DJUCE_DIR="$HOME/Desktop/JUCE" \
  -DCLASSIC_PLAYER_FETCH_JUCE=OFF \
  -DCMAKE_PREFIX_PATH="$VCPKG_ROOT/installed/arm64-osx" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/macos-arm64-make --parallel 4
bash scripts/package-macos.sh
```

### Windows

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
cmake --preset windows-x64
cmake --build --preset windows-release
powershell -ExecutionPolicy Bypass -File scripts\package-windows.ps1
```

O instalador coloca o VST3 em `C:\Program Files\Common Files\VST3`. No macOS, instala o VST3 em `/Library/Audio/Plug-Ins/VST3`, o AU em `/Library/Audio/Plug-Ins/Components` e o aplicativo em `/Applications`.

## Assinatura e distribuição

Defina `APPLE_SIGN_IDENTITY` e `APPLE_INSTALLER_IDENTITY` para assinatura Apple. A notarização requer uma conta Apple Developer. No Windows, assine o `.exe`, o `.vst3` e o instalador com um certificado Authenticode antes de distribuir.

A chave privada de emissão de licenças não pertence a este projeto e nunca deve ser incluída nos binários ou instaladores.

JUCE é usado sob os termos da licença escolhida pelo distribuidor. Para uma distribuição comercial fechada, obtenha a licença JUCE adequada antes do lançamento.
