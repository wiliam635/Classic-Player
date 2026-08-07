#!/bin/zsh

# Inicia o Classic Player localmente em um servidor privado e abre o navegador.
cd "$(dirname "$0")"
python3 -m http.server 8765 --bind 127.0.0.1 &
server_pid=$!
sleep 1
open "http://127.0.0.1:8765/index.html"
wait "$server_pid"
