# Pads contínuos e drum pads

- Adicione **Pad Continuo (12)** pelo menu **+ LAYER**.
- Em **EDITAR**, carregue o áudio de cada pad e atribua seu CC com LEARN.
- Um clique ou uma nova pressão do CC inicia o pad. Soltar o controle não para a reprodução; pressionar o pad atual não reinicia o áudio.
- Apenas um pad fica selecionado por layer. Na troca, o anterior desaparece gradualmente enquanto o novo entra.
- **STOP** faz uma saída gradual. Seu CC pode ser aprendido no editor.
- O ajuste de crossfade (0,02–10 segundos) é usado na emenda do loop, nas trocas e na parada. Em arquivos curtos, a emenda é limitada à metade do arquivo.
- O fader controla o banco inteiro; seu LEARN fica no editor. Os medidores dos dois tipos de pad mostram o áudio após o fader.
- Caminhos dos arquivos, CCs e crossfade são salvos na programação. Carregar uma programação não inicia os pads automaticamente. Mantenha os arquivos de áudio em seus caminhos originais.
- Cada pad contínuo aceita até 10 minutos, independentemente da taxa de amostragem. O banco aceita até 256 MB de áudio decodificado: um arquivo estéreo de 48 kHz com 10 minutos ocupa aproximadamente 220 MB, portanto cabe sozinho; para usar vários pads, prefira trechos menores.
- CC64 (sustain) e CC120–127 não são aprendidos como disparadores. Pads contínuos não respondem a notas; drum pads mantêm as notas no canal 10.

## Validação

`ClassicPlayerHammondTest` verifica emendas e repetição do mesmo pad, áudio 44,1 kHz em saída 48 kHz, STOP, CC, restauração sem autoplay, movimentação de layers e layout de 8/12 pads em 1280 × 700.
Os testes de MIDI master e regressão de áudio existentes também devem passar antes da publicação.
