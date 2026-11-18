# Modelo de piano NPNO (dinâmica e decaimento)

Calibrado contra a saída de um piano de referência estilo Nord (WX Stage-IV), medida em áudio com o mesmo
arquivo `White Grand XL 6.3.npno`. Nenhum código, tabela ou amostra do player de referência foi usado; a
análise estática do Sharp Stage Pianos serviu apenas para saber que seleção de amostra, ganho por velocity,
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
