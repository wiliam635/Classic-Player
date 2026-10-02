# Classic Player Studio

Primeiro esqueleto da futura DAW desktop para Windows e macOS. O aplicativo
contém a fundação da interface, o estado de transporte, a serialização de uma
sessão e um caminho de áudio real para o instrumento hospedado. Ainda não
grava nem exporta áudio.

O alvo é deliberadamente separado do `ClassicPlayer` existente. A fundação já
tem sessão versionada, transporte baseado em amostras, estado de mixer
independente do volume interno das layers e uma ponte de plug-in. A Studio
processa atualmente uma pista de instrumento por vez; a arquitetura deixa a
expansão para pistas de áudio/MIDI e gravação para os próximos marcos.

## Estado do Marco 1

- O transporte expõe posição em samples para ser avançado pelo callback de
  áudio; o timer da interface apenas atualiza a visualização.
- O mixer já representa canais, ganho em dB, pan, mute, solo, armamento de
  gravação e pico pré/pós-fader, além do ganho e pico do master.
- O callback de áudio aplica ao primeiro bus de instrumento ganho, pan, mute,
  solo e ganho master, além de expor picos pré/pós-fader para a interface.
- Ainda não há gravação, timeline, roteamento de múltiplas pistas ou
  carregamento do `ClassicPlayerAudioProcessor`; esses itens entram nas
  integrações seguintes.

## Ponte de instrumento

O `InstrumentHost` já define a fronteira para uma pista de instrumento: ele
carrega uma `PluginDescription` via JUCE, prepara o plug-in, processa buffers
de áudio/MIDI e salva/restaura o estado. A primeira implementação usa o
Classic Player VST3 (e AU no macOS) instalado no computador. A descoberta do
plug-in agora pode ser executada pelo botão `SCAN INSTRUMENTS` da janela: a
varredura consulta as pastas padrão de VST3 e AU e filtra somente instrumentos.
O botão `LOAD FIRST INSTRUMENT` exercita o carregamento da primeira descrição
encontrada, e os botões de sessão já permitem criar, abrir e salvar arquivos
`.cpsession`. O `AudioEngine` já abre o dispositivo padrão, conecta o callback
de áudio ao `InstrumentHost` e avança o transporte em tempo real. Os botões
`START AUDIO` e `STOP AUDIO` controlam esse ciclo; a seleção visual de várias
descrições e gravação ainda entram nos próximos marcos. As entradas MIDI
habilitadas no sistema são encaminhadas ao instrumento carregado por um
  `MidiMessageCollector`, mantendo a conversão entre a thread MIDI e o callback
  de áudio. O primeiro canal do mixer agora é copiado para o callback por um
  snapshot atômico, evitando que controles da interface sejam lidos diretamente
  pela thread de áudio. Ao salvar uma sessão, o identificador/formato e o estado binário do
instrumento carregado também são persistidos; ao abrir, a Studio tenta
reencontrar o mesmo plug-in e restaurar esse estado. O alvo existente do
Classic Player continua sem alterações.
