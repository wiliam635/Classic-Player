# Classic Player Studio

Primeiro esqueleto da futura DAW desktop para Windows e macOS. O aplicativo
contém a fundação da interface, o estado de transporte e a serialização de uma
sessão. Ainda não grava nem processa áudio.

O alvo é deliberadamente separado do `ClassicPlayer` existente. A fundação já
tem sessão versionada, transporte baseado em amostras e estado de mixer
independente do volume interno das layers. A integração do motor do
instrumento será o próximo marco.

## Estado do Marco 1

- O transporte expõe posição em samples para ser avançado pelo callback de
  áudio; o timer da interface apenas atualiza a visualização.
- O mixer já representa canais, ganho em dB, pan, mute, solo, armamento de
  gravação e pico pré/pós-fader, além do ganho e pico do master.
- Ainda não há processamento de áudio real, gravação ou carregamento do
  `ClassicPlayerAudioProcessor`; esses itens entram na integração seguinte.

## Ponte de instrumento

O `InstrumentHost` já define a fronteira para uma pista de instrumento: ele
carrega uma `PluginDescription` via JUCE, prepara o plug-in, processa buffers
de áudio/MIDI e salva/restaura o estado. A primeira implementação usa o
Classic Player VST3 (e AU no macOS) instalado no computador. A descoberta do
plug-in e a interface de seleção entram no próximo commit; o alvo existente do
Classic Player continua sem alterações.
