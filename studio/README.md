# Classic Player Studio

Primeiro esqueleto da futura DAW desktop para Windows e macOS. O aplicativo
contém a fundação da interface, o estado de transporte, a serialização de uma
sessão e um caminho de áudio real para o instrumento hospedado. A gravação de
entrada já gera WAV, registra cada take como clipe da sessão e pode reproduzir
esses clipes durante o transporte; ainda não há exportação final.

O alvo é deliberadamente separado do `ClassicPlayer` existente. A fundação já
tem sessão versionada, transporte baseado em amostras, estado de mixer
independente do volume interno das layers e uma ponte de plug-in. A Studio
processa atualmente uma pista de instrumento por vez; a arquitetura deixa a
expansão para pistas de áudio/MIDI e timeline para os próximos marcos.

## Estado do Marco 1

- O transporte expõe posição em samples para ser avançado pelo callback de
  áudio; o timer da interface apenas atualiza a visualização.
- O mixer já representa canais, ganho em dB, pan, mute, solo, armamento de
  gravação e pico pré/pós-fader, além do ganho e pico do master.
- O callback de áudio aplica ao primeiro bus de instrumento ganho, pan, mute,
  solo e ganho master, além de expor picos pré/pós-fader para a interface.
- O botão `RECORD INPUT` grava a entrada do dispositivo em WAV de 24 bits por
  meio de um `ThreadedWriter` em segundo plano; `STOP RECORDING` fecha o arquivo
  com segurança sem escrever diretamente no disco dentro do callback. O arquivo
  também é adicionado à pista principal como um `AudioClip`, com caminho,
  posição inicial, duração, taxa de amostragem e número de canais.
- A sessão está na versão 2 e continua aceitando arquivos da versão 1. A
  interface mostra um resumo da timeline (quantidade, duração, clipes
  carregados e último clipe) e uma prévia visual por pista. Ao iniciar o áudio,
  os WAVs da sessão são carregados em um snapshot imutável e reproduzidos na
  posição do transporte, com conversão linear simples de taxa de amostragem.
- É possível adicionar e remover pistas de áudio na sessão e escolher a pista
  de destino antes de gravar. A primeira pista permanece reservada ao
  instrumento hospedado; as demais já podem receber takes e aparecem como
  linhas independentes na timeline.
- As pistas adicionais já são somadas no callback com ganho, pan, mute e solo
  próprios. O carregamento dos WAVs acontece na thread da interface para não
  fazer I/O no callback; por isso, arquivos ausentes aparecem como aviso e os
  clipes válidos continuam disponíveis. A implementação mantém os clipes
  inteiros em memória durante a sessão, uma solução adequada para o primeiro
  marco mas que deverá ser substituída por streaming para projetos longos.
- Ainda não há exportação final, edição de regiões, formas de onda ou
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
descrições e a timeline ainda entram nos próximos marcos. As entradas MIDI
habilitadas no sistema são encaminhadas ao instrumento carregado por um
`MidiMessageCollector`, mantendo a conversão entre a thread MIDI e o callback
  de áudio. O primeiro canal do mixer agora é copiado para o callback por um
  snapshot atômico, evitando que controles da interface sejam lidos diretamente
  pela thread de áudio. Ao salvar uma sessão, o identificador/formato e o estado binário do
instrumento carregado também são persistidos; ao abrir, a Studio tenta
reencontrar o mesmo plug-in e restaurar esse estado. O alvo existente do
Classic Player continua sem alterações.
