# Backup Classic Player

O botão BACKUP no rodapé oferece Fazer Backup e Importar Backup. O ZIP64 é criado com compressão nível 1 em uma única thread de trabalho; não usa executáveis externos. O destino deve ficar fora da pasta de dados do aplicativo.

Conteúdo: dados do Classic Player, bibliotecas, performances/presets, configuração da interface, snapshot da sessão atual (Programs/Backup Session.ckprogram) e arquivos de timbres/áudio externos referenciados nesses estados. Plugins VST/AU instalados em diretórios do sistema não são redistribuídos. Referências de áudio ausentes são informadas e impedem concluir um backup incompleto.

A restauração exige confirmação, extrai para staging, verifica CRC e caminhos, rejeita links e entradas duplicadas, remapeia referências para o computador atual e preserva o diretório anterior. Depois carrega a sessão do backup. Preferências de áudio/MIDI do standalone são restauradas quando disponíveis; dispositivos de outro computador podem exigir nova seleção. Dentro da DAW não se altera o dispositivo de áudio do host.

Dependência vendorizada: miniz 3.1.0, commit 174573d60290f447c13a2b1b3405de2b96e27d6c, https://github.com/richgel999/miniz. Licença MIT em third_party/miniz/LICENSE. Arquivos de terceiros mantidos sem alterações, exceto miniz_export.h gerado localmente com exportação vazia para link estático.
