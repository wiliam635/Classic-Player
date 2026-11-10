# Classic Player 3.0.3 — Conceito D (teste)

Base: `55d068b` da branch `codex/review-fix-client-20261109`.
Preview: `codex/concept-d-complete-preview-20261110`.

## Interface

- Tema GRAFITE AMBAR: inspetor integrado acima do mixer, abas SOM/ENVELOPE/MIDI/EFEITOS/VELOCITY e navegacao PALCO/MIXER/LIVE SET.
- Fonte especifica de SF2, NPNO, NSMP/NSMP3/NSMP4, DX7, Hammond e Analog; desenho dos pads integrado ao inspetor.
- Hammond com drawbars, Leslie, percussao e carater; Analog com osciladores e controles integrados.
- Envelope grafico e curva de velocity lado a lado, com presets e arquivos de configuracao.
- Cadeia de efeitos com bypass persistente por efeito; EQ grafico e filtros integrados. Bypass preserva regulagens e neutraliza o processamento correspondente.
- MIDI Learn global, etiquetas dos CCs, novos destinos EQ/ganho e aprendizado dos drawbars.
- Mixer com imagens, miniaturas dos pads, mute/solo, medidores e faders; rolagem horizontal em janelas estreitas.
- Tres colunas em telas largas, duas nas intermediarias e uma com rolagem nas menores.
- Criacao/exclusao/reset de programacao atualiza ou descarta o inspetor para evitar controles de layers removidas.
- Timers dos novos paineis e captura do espectro seguem a visibilidade.

Os outros temas continuam disponiveis com seu layout anterior. Os arquivos de biblioteca e os formatos de instrumentos foram preservados.

## Validacao

`ClassicPlayerHammondTest --concept-ui [prefixo-das-imagens]` cria oito tipos de layer, renderiza as abas e os tamanhos 1440/1100/800, verifica navegacao, aprendizado de EQ, persistencia do bypass e descarte do inspetor. As imagens devem ser inspecionadas alem das assercoes estruturais.

`--responsive-ui` e `--live-layout` verificam explicitamente o tema legado. `--ui-preferences` verifica reabertura e separacao do estado Standalone/VST. `ClassicPlayerMasterMidiTest` cobre volume, efeitos, mute e persistencia MIDI.

A validacao de componentes nao substitui teste musical nem teste do VST em um host externo. Os pacotes CI desta branch sao identificados como `3.0.3-Conceito-D-teste`; nao sao publicacao do site.
