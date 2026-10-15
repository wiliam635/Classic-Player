# Importação de pianos Nord (.npno)

O Classic Player lê arquivos `.npno` por meio de um decoder independente em
`Source/NordPianoDecoder.cpp`; ele não depende do Nord Sound Manager e não
executa código do arquivo importado. O arquivo é validado antes de ser aceito,
incluindo assinatura/geração CBIN, checksum CRC, diretório, dimensões de áudio
e consistência das sobreposições entre blocos.

O decoder converte os strokes de áudio para PCM16 na memória. O `SampleEngine`
seleciona a nota-raiz mapeada, a camada de velocidade e o banco de ataque,
ressonância de sustain ou release, respeitando também o pedal MIDI CC64.
Arquivos maiores que 512 MB, fluxos diferentes de CNSP/CBIN geração 1 e versões
de stream diferentes de `0x0450` e `0x0464` são recusados explicitamente.

O arquivo `Royal Grand 3D Sml 6.1.npno` usa uma variante com camadas de
velocidade até 144 e ajustes de ganho com sinal em décimos de dB. O player
detecta essa variante pelo conteúdo do diretório. O perfil anterior usava
o ajuste como dB inteiro e limitava a seleção a 31, deixando esse piano
excessivamente baixo e sem acesso à maior parte das camadas dinâmicas.
Para camadas estendidas, o limite MIDI é escalado proporcionalmente ao maior
índice de ataque observado; essa correspondência ainda é uma aproximação.

Ao levantar o pedal após soltar teclas, as vozes sustentadas recebem um
abafamento curto. Não são iniciadas novas gravações de release nesse momento,
evitando que um acorde antigo reapareça como notas audíveis. A soltura de uma
tecla sem pedal usa o mesmo abafamento; as gravações de release do arquivo não
são disparadas, pois nesta versão produziam uma segunda nota audível.

Os ataques também trazem início e duração do loop de sustentação nos campos
`+0x1c` e `+0x20` de cada registro. O player usa esse trecho enquanto a nota
ou o pedal estão pressionados; ao liberar, deixa de repetir o trecho e aplica
o abafamento.

Este é um leitor inicial, não uma implementação certificada pela Nord. A
estrutura foi reconstruída a partir de inspeção estática do formato e de
descrições públicas de engenharia reversa, incluindo o layout CNSP e o codec
de áudio documentados em [nord-format](https://github.com/jmoo/drawbar/tree/master/crates/nord-format).
Nenhum código ou conteúdo de instrumento desse projeto foi incorporado, e o
arquivo `.npno` fornecido pelo usuário não faz parte do repositório.
