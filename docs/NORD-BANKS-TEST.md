# Bancos Nord — teste isolado 2.0.5

Base preservada: `19ff30c2d4c8cdaf840ddefc4437d835b9013823`, branch
`codex/checkpoint-205-before-ns4-20261017`. Experimento:
`codex/nord-bank-layer-205-20261017`. Android não foi alterado.

## Uso

1. `+ LAYER` → `Bancos Nord (.ns4pbundle) - teste`.
2. No canal, clique em `BANCOS` → `IMPORTAR BANCO NORD`.
3. `BANCOS IMPORTADOS` → banco → programa → Cena I ou II.
4. Salve a programação normalmente. O tipo da layer, caminho do programa,
   cena, controles e roteamento são restaurados junto com ela.

O banco é copiado para `NordBankEngine::libraryRoot()` (subpasta
`Classic Player/Nord Banks` dos dados do usuário). Não é necessário reimportar
nem manter o ZIP original conectado. Os samples são carregados ao escolher
um programa, não todos na RAM na importação. O arquivo original não é alterado.

## Escopo real

Importa o contêiner `.ns4pbundle`, cataloga `.ns4p` versão 3.08 e suas
dependências; usa os motores próprios de NPNO e Hammond para adaptar piano A/B
e B3 A/B. Respeita cenas, partes ativas, ganho relativo, oitavas, transposição,
zonas e sustain; timbre, EQ, reverb, drawbars, Leslie e percussão são adaptados.
Os controles externos da layer continuam disponíveis. Nenhum novo limiter,
compressor automático ou normalização por número de notas foi adicionado.

Agora também decodifica NSMP4 V4/mapa 21 (40 zonas estéreo no arquivo do
banco), incluindo PCM, ganho, detune, notas, velocity e loops. O sample aparece
separadamente no menu como `NSMP4 - ...`, além de poder compor o programa.
O rompler tem 128 vozes por parte, sustain que não solta teclas ainda pressionadas,
crossfade de loop, envelope e controles externos de volume, attack/release,
cutoff, EQ, reverb e compressor manual. Não aplica aos strings o limite de 15 s
dos pianos. O sintetizador analógico é uma adaptação ao motor nativo existente:
formas básicas, camadas, níveis, pitch, envelopes e filtro, não uma emulação Nord.

FM/WAVE, arpeggiadores e órgãos não-B3 ainda não são reproduzidos. Programas
que os exigem ficam desabilitados no menu, com a razão. Morphs, touch Nord,
compressão dinâmica, chorus/vibrato, delay, amp sim, parâmetros finos de
efeitos e crossfade de zonas não são reconstruídos nesta etapa. Seus bytes
originais permanecem guardados; não há promessa de sonoridade idêntica.
Outras versões NS4 são rejeitadas até validação própria.

Banco Filipe Martins fornecido: 30 programas, 10 NPNO, 1 NSMP4. Cena I
reproduzível no teste: 24 dos 30 programas, mais o NSMP4 selecionável sozinho.
Permanecem bloqueados na cena I: Pluck Penta, Three Lead, Verb Lead, Bell Lead,
B3 Syn Bass e Clima (FM/WAVE, modelo de órgão ou arpeggiador não implementado).
Os programas com synth analógico mostram explicitamente `adaptado`.

## Verificação

`ClassicPlayerNordProgramTest` testa identificação de piano, ganho, partes
inativas, cenas, zonas, checksum, tamanho e versão. Com o diretório local
dos programas como argumento, verifica os 30 programas e as cenas I.

`ClassicPlayerNordSampleTest` usa NSMP4 sintético sem conteúdo comercial:
cabeçalho/checksum, PCM assinado, escala, notas/loops, silêncio em repouso,
note-off, pedal sem matar tecla pressionada, panic e dados corrompidos,
em 44,1 e 48 kHz. A captura sintética flagrou e impediu que a voz terminasse
um frame antes de voltar ao início do loop.

`ClassicPlayerNordBankStateTest` usa um programa B3 sintético, sem recursos
comerciais, para verificar tipo/caminho/cena/roteamento após salvar/reabrir,
isolamento da layer SF2 vizinha, remoção/compactação e troca de fonte.

`tools/nord-bank-probe` permite testar localmente o bundle fornecido:
importação, catálogo, áudio finito/não-silencioso dos 24 programas e do NSMP4,
falha atômica ao escolher programa incompatível, parada e unload. Não inclui
nem publica o banco ou samples. O teste remove apenas sua cópia temporária.

Mapas de parâmetros/identidades: ns4decode 2.0 (MIT); ver
`THIRD_PARTY_NORD_PROGRAM_LICENSE.txt`. Nenhum código OpenNord foi usado.
Codec de strokes NSMP4 adaptado de nord-format 0.8.0, BSD-3-Clause,
referência `jmoo/drawbar` commit `046a178e125c1ac115981cf84de92dbb16122bd5`;
ver `THIRD_PARTY_NORD_SAMPLE_LICENSE.txt`. A descrição V4 dessa referência
é inferida de arquivos, sem validação de reprodução em hardware V4.

## Investigação do chiado

Retirados key click e leakage sintéticos herdados dos defaults do Hammond
somente nos órgãos importados; isso não altera a layer Hammond normal.
Nos pianos, a comparação amostra a amostra de Gordo Pno com os mesmos NPNO,
ganhos, EQ/timbre e reverb em dois slots independentes deu erro máximo zero,
em 44,1 e 48 kHz. Todos os testes sem notas ficaram silenciosos. Não foi
demonstrada a origem do ruído relatado durante as notas do piano; sem gravação
não se deve declarar resolvido. Não foram adicionados gate/denoiser/limiter
automático nem alterados o master e a layer NPNO original.
