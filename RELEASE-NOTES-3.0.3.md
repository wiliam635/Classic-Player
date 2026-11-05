# Classic Player 3.0.3

- Correcoes na prioridade dos controles MIDI aprendidos por layer, evitando interferencia de mapeamentos antigos do master.
- Validacao de CC7, CC11 e CC86, canais MIDI, salvamento e restauracao; ciclos de mute/unmute SF2 por CC7/CC11.
- Modulation/CC1 do Hammond controla a velocidade do Leslie, respeitando canal e habilitacao da layer.
- Menos trabalho repetido nos motores Analog/Moog e Hammond, no Pad Continuo, no grafico do EQ e em paineis ocultos.
- Moog com limite fixo de 32 vozes, sem seletor de polifonia; presets antigos respeitam o mesmo limite.
- Ganho de saida do Hammond recalibrado para o nivel do DX7 de referencia, preservando volume, drawbars e drive.
- Menu de contexto da layer fecha apos excluir o canal e nao abre duplicado pelo listener de mouse.
- Preparacao de SF2 e leitura NPNO fora da trava do callback no fluxo normal de importacao. Arquivo SF2 invalido preserva o instrumento atual.
- Versao 3.0.3 no aplicativo e nos instaladores Mac/Windows, sem sufixo de teste nos downloads.

Publicacao no site somente apos o teste e aprovacao do instalador. O formato .nw2pbundle fica para uma proxima atualizacao.
