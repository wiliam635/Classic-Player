# Investigação — chiado e NSMP4, 17/10/2026

Registro inicial abaixo é histórico; ver atualização de implementação ao final.

## Chiado: causa ainda não estabelecida

Relato do usuário: chiado junto ao som da layer Bancos Nord. Não foi fornecida
gravação nem identificado o programa ou se ocorre também sem notas.

Instrumentado `NordBankRegressionTest.cpp`, modo `--audio-metrics [44100|48000]`.
Teste local offline, sem passar pelo master, dispositivo de áudio ou DAW:
importação do bundle original, 2 s sem notas e acorde C4/E4/G4/C5 com velocity
0,8 por aproximadamente 4 s, volume da layer 0,8. Os 7 programas compatíveis
da cena I foram renderizados a 44,1 e 48 kHz, blocos de 256 frames.

Resultados: pico sem notas igual a zero em todos; nenhuma amostra acima de
±1 ou não-finita. Isso NÃO demonstra ausência de ruído dentro das notas nem
descarta problema de saída/dispositivo/master ou overload no computador do usuário.

| Programa | Pico a 48 kHz, antes do master | Carga média de render local |
| --- | ---: | ---: |
| Driv Organ FLP | 0,582 | 2,56% |
| Gordo Pno FLP | 0,665 | 1,10% |
| Hard Organ FLP | 0,471 | 2,63% |
| Pno Samba FLP | 0,125 | 0,84% |
| Pump Piano FLP | 0,907 | 2,97% |
| Radio Pno FLP | 0,892 | 0,51% |
| Soft Organ FLP | 0,154 | 2,52% |

Inspeção do código: partes inativas não são renderizadas. O Hammond nativo
tem key click transitório e leakage tonal por padrão; o banco atualmente
herda esses valores. Isso é hipótese para ruído de ataque em órgãos, não
explicação confirmada para chiado contínuo ou em piano. Não foram removidos
esses recursos nem introduzidos filtros/gates para mascarar o sintoma.

Próxima evidência necessária: programa/cena exatos, ruído só durante notas ou
também em repouso e gravação curta. Comparar o mesmo NPNO sozinho e no programa.
Nenhuma correção de áudio foi declarada ou nova compilação disparada nesta investigação.

## NSMP4: reconhecimento estrutural confirmado, reprodução pendente

Arquivo importado: `SymphStr XSoft Amb_ProjectSAM 4.1.nsmp4`, 13.183.920 bytes.
Exame direto dos bytes, não uma observação de execução do Nord:

- CBIN geração 1, tag `nsmp`, versão de conteúdo 410.
- CRC32 do corpo confere.
- Cadeia NSMP de seções big-endian termina exatamente no fim do arquivo.
- `hdr`, `cat`, `map` versão 21, 40 seções `stk`, `sty` e `meta`.
- Mapa declara 40 zonas; primeiras zonas: root/top/low 107/108/107,
  105/106/105, 103/104/103 e 101/102/101; velocities 0–127.
- O payload NSMP4 usa outro codec de strokes, não o CNSP dos NPNO. Não se
  deve aplicar o decoder de piano aos bytes nem renomear a extensão.

Referência primária permissiva encontrada: [nord-format 0.8.0 / drawbar](https://github.com/jmoo/drawbar/tree/master/crates/nord-format/src/formats/nsmp),
licença BSD-3-Clause (John Moore). Sua documentação informa constantes V4
inferidas de arquivos, não confirmadas no hardware. Código foi examinado
fora do checkout, em `/tmp/classic-nord-format-reference`; nenhum código
dessa referência ou do OpenNord foi incorporado ao aplicativo nesta etapa.

Próximas etapas: decoder limitado/bounded do codec V4, teste de PCM/loops/
ganhos/mapa/velocity, rompler sem decaimento de piano de 15 s e associação
às partes sample do programa. Síntese analógica/FM é distinta de NSMP4 e
exige mapear o motor correspondente; não substituir silenciosamente por saw.

## Limitação REA

REA abriu estaticamente o executável Nord Sound Manager 9.22, SHA-256
`47ffbea18bc69ebc2777b4b781fa167e4fe058d1fac930615959a2f7477b0034`.
`search_strings` retornou `provider_unavailable`: Hopper ausente, Ghidra
e IDA não configurados. Sessão fechada ao finalizar. A inspeção de bytes,
símbolos e os testes de áudio acima usaram ferramentas locais comuns;
não são resultados de descompilação REA. Não houve instalação nem mudança
de configuração do REA, nem execução do aplicativo Nord.

## Atualização — implementação e testes

Implementados `NordSampleDecoder` e `NordSampleEngine`, com o aviso BSD-3-Clause
incluído na distribuição. Decoder validado no NSMP4 do banco: 40 zonas estéreo,
nomes, mapas, PCM, escala, ganhos e pontos de loop. O teste sintético valida
mono e estéreo (palavras alternadas V4), canais separados, silêncio em repouso,
loop, note-off, sustain sem soltar tecla pressionada, panic, CRC e truncamento,
em 44,1/48 kHz. Não inclui samples comerciais no repositório.

Mapeadas as três partes synth: samples NSMP4 ou adaptação analógica ao motor
Classic Keys Analog. FM/WAVE, arpeggiador e órgão não-B3 continuam identificados
e bloqueados quando exigidos. Agora 24 programas da cena I mais o sample
NSMP4 individual renderizam áudio finito/não-silencioso; seis programas ainda
não têm reprodução completa. Síntese adaptada não é emulação idêntica Nord;
sync/shape/multi/super, modulações, efeitos e morphs não têm equivalência exata.

O teste comparativo de Gordo Pno (com os próprios Royal Grand 3D XL e Mellow
Upright Med do banco e as mesmas regulagens em slots NPNO independentes)
mediu erro máximo **zero** em cada amostra, em 44,1 e 48 kHz. A medição exclui
o master/dispositivo e não prova que os samples ou regulagens não tenham ruído;
apenas mostra que esse caminho de mixagem não acrescentou diferença ao sinal.
Key click e leakage herdados dos defaults Classic Player foram desligados
somente nos órgãos importados. Não é uma correção demonstrada do chiado no piano.
Não foi acrescentado denoiser, gate ou limiter automático.

Há picos acima de 0 dB em alguns synths e no sample quando somados em acorde,
registrados no teste. Não foram mascarados por normalização dinâmica; o master
existente e a layer NPNO original não foram alterados. Causa do chiado relatado
nos pianos ainda requer gravação/correlação com o caminho completo de saída.
