# Classic Player 2.0.5 teste: piano Nord

Esta edição de teste aceita `.npno` no botão de carregar instrumento de uma
layer SF2/Nord. O arquivo permanece na localização escolhida; a programação
salva guarda seu caminho, então ele deve continuar acessível ao reabrir.
Arquivos `.npno` não são enviados ao GitHub nem incluídos nos instaladores.

O importador independente verifica assinatura, versão, CRC, diretório e
blocos de áudio antes de decodificar. O player usa o ganho em décimos de dB,
as camadas de velocidade e os marcadores de loop observados no arquivo
`Royal Grand 3D Sml 6.1.npno`. Nota solta e pedal levantado abafam a voz
existente sem disparar outra amostra de piano. Esta é uma implementação
experimental e não é certificada pela Nord.

Os workflows `build-macos.yml` e `build-windows.yml` produzem artefatos com
`2.0.5-teste` no nome. O macOS universal inclui x86_64 com deployment target
10.13 (High Sierra) e arm64 com deployment target 11.0. O build Windows usa
o runner Windows 2022 x64. O teste em hardware/OS antigo ainda é necessário.
