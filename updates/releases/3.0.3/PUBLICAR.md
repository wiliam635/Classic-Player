# Classic Player 3.0.3 — pacote para publicação

Não publicar o manifesto antes de disponibilizar e testar os instaladores.

1. Usar os instaladores do commit 44ee450f570953842a7e57f1b36e6446e1258aab. Os hashes serão registrados após baixar os artefatos aprovados pelo CI.
2. Substituir os downloads macOS/Windows na área de membros pelos novos arquivos. Os links do aplicativo continuam apontando para a área de membros existente; não usar URLs temporárias do Actions para clientes.
3. Enviar site-upload/updates/classic-player-3.0.3.html para /updates/classic-player-3.0.3.html.
4. Após o teste e aprovação do instalador, enviar site-upload/updates/classic-player.json para /updates/classic-player.json por último. Esse passo anuncia a atualização aos clientes.
5. Conferir HTTP 200/JSON com o User-Agent Classic-Player/3.0.3 e parâmetro _cp_check; verificar downloads nos dois sistemas.

O feed público foi verificado e permanece em 3.0.2. Não houve upload para o servidor.
