# Ajustes para performance — 28/08/2026

Base: `47202855b70ec02edfbe4213249d6e83462c88f0`.

## Alterações

- Volume DX7, Analog e master com suavização de 20 ms; sem alterar o ataque dos presets.
- DX7 e Analog respeitam a posição MIDI dentro do bloco, incluindo as entradas roteadas.
- DX7 preserva as amostras restantes do bloco MSFA de 64 amostras entre chamadas do host.
- Vozes DX7 livres são usadas antes de substituir vozes ainda em release.
- Transições/reutilização de vozes recebem compensação curta de descontinuidade (3 ms), sem acrescentar glide.
- Feedback MSFA inicializado e preservado no legato; mono DX7 retorna à última nota ainda pressionada.
- Standalone restaura a última programação **salva**, não simplesmente a última carregada ou os ajustes não salvos. Trocar dispositivo/sample rate não recarrega o programa.
- Primeira execução sem preferências novas procura o `.ckprogram` mais recentemente modificado. Arquivo marcado como último salvo ausente/corrompido não é substituído silenciosamente por outro programa.
- Preferências em `Classic Player/Startup.xml`; testes usam uma pasta temporária isolada, sem editar a biblioteca real.
- Plugins não carregam automaticamente programas do standalone.
- Master: clique em **LEARN**, mova o controlador; clique novamente para cancelar; **Shift+clique** apaga o mapeamento. CC e canal são memorizados; CC64 e mensagens de modo de canal ficam excluídos.
- Atualização de parâmetros MIDI e persistência de preferências fora do callback de áudio, mesmo com o editor fechado.

## Validação automatizada

Com `CLASSIC_PLAYER_BUILD_TESTS=ON`, há dois novos testes CTest:

- `audio-regression`: DX7 e Analog em mono/poli, notas com timestamps, equivalência entre blocos de 512 e 127 amostras, áudio finito/não silencioso, rampa de volume DX7 e compensação de transição.
- `master-midi`: CC/canal, extremos de volume, exclusão de sustain/modo de canal, persistência no estado, remoção de mapeamento e reinicialização de processadores com programas em diretório temporário. Inclui última programação salva versus carregada, nome restaurado, reinício do dispositivo, isolamento de plugins e arquivo ausente.

## Limites da validação

Os testes numéricos não substituem tocar os timbres reais com seu controlador/interface de áudio. Não afirmamos que todo estalo possível foi eliminado. Ainda é necessário validar auditivamente os bancos usados no domingo, o layout na janela real, e as compilações Windows e macOS Universal no GitHub Actions.

O SDK local deste Mac é antigo para uma constante usada pelo JUCE 9. Para validar localmente, foi passada somente ao CMake local a definição:
`-DkAudioAggregateDriftCompensationHighQuality=kAudioSubDeviceDriftCompensationHighQuality`.
Essa adaptação não está no código de produção nem nos workflows. Não trocar o SDK ou o JUCE faz parte destas alterações.

Nenhuma alteração foi enviada ao GitHub nesta etapa.
