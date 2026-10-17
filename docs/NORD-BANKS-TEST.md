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

Não é uma emulação completa do Stage 4: synth/NSMP4 e órgãos não-B3 não são
reproduzidos. Programas com synth ativo na cena ficam desabilitados no menu,
com a razão, para não omitir partes silenciosamente. Morphs, touch Nord,
compressão dinâmica, chorus/vibrato, delay, amp sim, parâmetros finos de
efeitos e crossfade de zonas não são reconstruídos nesta etapa. Seus bytes
originais permanecem guardados; não há promessa de sonoridade idêntica.
Outras versões NS4 são rejeitadas até validação própria.

Banco Filipe Martins fornecido: 30 programas, 10 NPNO, 1 NSMP4. Cena I
compatível inicialmente: Gordo Pno, Radio Pno, Pno Samba, Soft Organ,
Hard Organ, Driv Organ e Pump Piano (nomes terminam em `FLP`).

## Verificação

`ClassicPlayerNordProgramTest` testa identificação de piano, ganho, partes
inativas, cenas, zonas, checksum, tamanho e versão. Com o diretório local
dos programas como argumento, verifica os 30 programas e as 7 cenas I.

`ClassicPlayerNordBankStateTest` usa um programa B3 sintético, sem recursos
comerciais, para verificar tipo/caminho/cena/roteamento após salvar/reabrir,
isolamento da layer SF2 vizinha, remoção/compactação e troca de fonte.

`tools/nord-bank-probe` permite testar localmente o bundle fornecido:
importação, catálogo, áudio finito/não-silencioso dos 7 programas compatíveis,
falha atômica ao escolher programa incompatível, parada e unload. Não inclui
nem publica o banco ou samples. O teste remove apenas sua cópia temporária.

Mapas de parâmetros/identidades: ns4decode 2.0 (MIT); ver
`THIRD_PARTY_NORD_PROGRAM_LICENSE.txt`. Nenhum código OpenNord foi usado.
