# Modelo de piano NPNO (dinâmica e decaimento)

Calibrado contra a saída de um piano de referência estilo Nord (WX Stage-IV), medida em áudio com o mesmo
arquivo `White Grand XL 6.3.npno`. Nenhum código, tabela ou amostra do player de referência foi usado; a
análise estática de um player de referência serviu apenas para saber que seleção de amostra, ganho por velocity,
decaimento do loop, ressonância e soltura são etapas separadas.

## O que foi medido (C3/C4/C5, velocity 20/64/127, 10 s)

| | velocity 20 | velocity 64 |
|---|---|---|
| Referência (dB abaixo da velocity 127) | −34 | −11 a −12 |
| Motor antigo (curva linear) | −13 a −14 | −4 a −5 |

Queda durante a nota (dB relativo ao 1º segundo, velocity 64, em 2/4/6/8/9,5 s):
referência C3 −15 −19 −25 −31 −36; C4 −14 −20 −28 −36 −42; C5 −22 −33 −43 −54 −61.
O motor antigo segurava o nível do loop e só aplicava uma queda quadrática igual para todas as teclas
(no C5, −41 dB aos 9,5 s contra −61 dB da referência).

## O que mudou

- `pianoDynamicsExponent` (2,2 em pianos carregados pelo usuário, em programas Nord importados e em
  performances antigas sem valor salvo; a estrutura interna tem padrão 1,0 = lei antiga):
  o volume da voz segue `velocity^expoente`. A camada de amostra (timbre) continua escolhida pela
  velocity já moldada. Com 2,2 a saída fica em −33 dB (v20) e −11,5 dB (v64), próximo da referência.
  Com o modelo ligado o nível sobe +4 dB (volume absoluto da referência: o forte e o mezzo-forte dela
  ficavam 4-5 dB acima do nosso).
- `pianoNaturalDecay` (ligado nos mesmos casos acima): quando a voz entra
  no loop de sustentação, passa a decair a uma taxa por tecla, ≈ 3,3 dB/s em C3 + 0,09 dB/s por semitom
  (limites 1,5 e 9 dB/s). O CAUDA NPNO passa a só encerrar a nota, com um fade final de até 2 s.
  Reproduz a referência dentro de ±3 dB nos pontos medidos.
- Performances antigas passam a usar o modelo novo (valor ausente = modelo novo). Valores já salvos por esta
  versão ou posterior são mantidos como estão.

Limites: a taxa de decaimento foi ajustada em um piano (White Grand XL); outros pianos têm amostras
com outros pontos de loop. A reflexão/ressonância e a amostra de soltura não foram alteradas.

## Segunda calibração: Nord de verdade (SF2 amostrado do hardware)

`Nord White Grand.sf2` e `Nord Royal 3D.sf2` (amostrados direto de um Nord) trazem o envelope real: release de
0,70 s, sem decay no envelope, amostras de 9 a 13 s sem loop e 7 camadas de velocity. Comparando com o nosso
motor tocando o NPNO do mesmo piano:

- Volume por velocity (rel. à velocity 127): dentro de 2,4 dB do Nord em v20 e v64 (nos dois pianos).
- Queda depois dos primeiros segundos: a inclinação do Nord (3,2 / 4,1 / 5,5 dB/s em C3/C4/C5) bate com a do
  modelo por tecla.
- Faltava queda nos primeiros 4 s: o Nord cai cerca de 10 dB a mais que a amostra NPNO crua. Foi adicionado
  `earlyDecayMul` (2,5 dB/s durante os primeiros 4 s, só com o modelo de piano ligado).
- Resultado: erro médio da queda de 2,7 dB (White Grand) e 2,6 dB (Royal Grand 3D) contra o Nord, ao longo de
  2/4/6/8/9,5 s em C3/C4/C5 (antes, uns 10 dB acima).

Não alterado: release. O Nord solta em 0,70 s (controle ≈ 55); o padrão dos pianos acústicos continua 30 (0,3 s).

## Carga do Live Set (relato de lentidão no Windows)

Causas encontradas no caminho de troca de performance:
- O piano NPNO e as amostras NSMP eram decodificados dentro do bloqueio do áudio (`restoreLayerPaths`), congelando
  áudio e tela durante toda a leitura. Agora são preparados antes do bloqueio (como o SF2) e já ao selecionar o
  slot do Live Set; a troca na primeira nota só anexa o instrumento.
- A "aquecida" do cache lia a cabeça (512 KiB) e a região de loop (até 2 MiB) de cada uma das ~1.200 amostras: quase
  o arquivo inteiro a cada carga, e o preload de um banco multiplicava isso. Agora só os primeiros 96 KiB do banco
  normal; o resto é lido à frente de cada nota pelo prefetcher.
- Um preload em andamento e uma carga do usuário podiam decodificar o mesmo arquivo duas vezes; `prepareSample`
  agora é serializado, então a segunda espera a primeira e encontra o resultado no cache.
- `liveset-diagnostico.txt` registra o tempo de preparo ao selecionar o slot e o do `loadProgram` na troca.

Release padrão dos pianos acústicos novos: 55 (≈0,68 s), na faixa dos SF2 do Nord (0,59 a 0,70 s).
