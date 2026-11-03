# Bancos Nord Stage 4

Acesso: IMPORTAR > BANCOS NORD / IMPORTAR BUNDLE. Nas layers NPNO e NSMP, BANCOS IMPORTADOS abre a mesma biblioteca.

Importe um `.ns4pbundle`, escolha o banco e o preset, e clique CARREGAR PRESET COMPLETO. Depois da confirmacao, a Classic prepara os instrumentos em segundo plano e substitui as layers atuais somente se a preparacao terminar sem erros. Salve como uma performance Classic para conservar a configuracao junto das demais performances.

O arquivo do banco e extraido para a biblioteca persistente Nord Bundles. A lista de presets usa meta.xml e inclui programas so de orgao ou synth. Listar/importar nao decodifica o banco inteiro. Sons repetidos no preset compartilham a preparacao; os caches existentes permanecem limitados, e o preset tem limite de 768 MB de PCM preparado. Os arquivos permanecem no disco para restauracao das performances.

## Compatibilidade

Leitor independente de NS4P CBIN, layout de 24 blocos (Stage 4 OS 1.66), com extracao por descritores de campos. Os identificadores de nome resolvem as dependencias por hash, sem escolher instrumentos pela ordem do XML. Se um recurso necessario estiver ausente ou falhar na decodificacao, o carregamento e recusado e a performance atual permanece ativa.

Pianos usam NPNO; samples usam NSMP, NSMP3 ou NSMP4. B3 e encaminhado ao Hammond; outros modelos de orgao usam o motor Nord Organ; synth analogico/FM/wave recebe uma aproximacao pelo motor Moog. Ate sete partes da cena ativa sao criadas separadamente, com niveis, zonas, oitavas, pedal e configuracao de efeitos.

Nao ha promessa de equivalencia sonora com o teclado: envelopes, filtros, efeitos, simulacao de amplificador e FM/wave sao adaptados aos motores da Classic. Arpejador, morphs, EXTERN e crossfade de split nao sao reproduzidos integralmente. A janela mostra as adaptacoes depois do carregamento. Os presets antigos de NPNO, SF2 e demais motores continuam usando seus leitores habituais.

## Verificacao

O teste NordProgramRegression usa um arquivo sintetico, sem samples comerciais, para cobrir IDs de 32 bits, sinal, campos que atravessam bytes, tempo, selecao de cena e arquivos truncados/com numero incorreto de blocos. O workflow Validate Nord bundle reader executa no Linux, macOS e Windows.

## Revisão de 03/11/2026 (sem compilação)

NS4P Mod1 usa pares: 0/1 A-Pan, 2/3 Trem. Mod2: 0/1 Phaser,
2/3 Flanger, 4/5 Chorus, 6/7 Vibe, 8/9 Ensemble, 10/11 Spin.
A Classic aplica Trem/Flanger/Chorus e informa os demais como não suportados.
O EQ usa passos de 0,25 dB; valores -60..60 correspondem a -15..15 dB.
As variantes de chorus usam profundidades próprias da Classic (6/10 ms),
não medidas do DSP do WX. Efeitos e envelopes continuam aproximações.

O orçamento de PCM do novo preset passa a 25% da RAM física, com mínimo
256 MiB, máximo 3072 MiB e fallback 768 MiB quando a RAM é desconhecida.
Instrumentos iguais são contados uma vez por caminho dentro do preset.
A contagem usa a capacidade dos vetores PCM. Falhas na preparação mantêm
o programa atual. Um índice de weak_ptr permite reutilizar instrumentos
retidos pelas layers mesmo quando o LRU os remove, sem mantê-los vivos
indefinidamente. Não foi implementado streaming nesta revisão.
