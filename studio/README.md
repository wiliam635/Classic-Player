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
