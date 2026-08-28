# Hammond nativo — integração de teste para macOS

- Nova fonte de layer: Hammond (ID 5, preservando IDs antigos).
- Editor próprio sem teclado visual e sem vibrato/chorus.
- Nove drawbars com relações corretas, foldback e atualização durante notas sustentadas.
- Soltura cosseno de 20 ms, independente do tamanho do bloco; MIDI processado na posição da amostra.
- Percussão 2nd/3rd com decaimento fixo, disparo único e supressão de 1'.
- Dois rotores com inércia, Doppler, amplitude e pan. CC1 alterna Chorale/Fast.
- 33 registros, key click, leakage, drive e nível do instrumento.
- Volume/reverb/cutoff/compressor e roteamento usam os controles existentes da layer.
- CC7 e CC11 por canal; CC64 sustain; CC120/123 desligamento; CC121 reset.
- Learn para cada drawbar, Leslie e nível, guardado junto com a programação.
- Inclusão/remoção/mudança de fonte e restauração de programa preservam configurações.
- Não depende do navegador. Não incorpora OpenB3.

O motor é uma adaptação C++ do laboratório, não uma cópia bit a bit do Web Audio:
os filtros, reverb e saturação usam DSP nativo e precisam de validação auditiva.
Modo Hammond é polifônico; velocity não altera o volume de órgão.
Para controladores diferentes no mesmo canal, use o seletor de entrada MIDI de cada layer.

Compilação local: macOS Apple Silicon (arm64), sem substituir o app instalado.
Ainda não é uma distribuição Universal/Intel nem uma release notarizada.
As alterações anteriores do DX7, Analog, restauração do último programa e Master Learn foram preservadas.

Teste: ClassicPlayerHammondTest (CTest: hammond).
As regressões de áudio existentes e de Master MIDI também devem continuar passando.

