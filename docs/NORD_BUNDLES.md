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
