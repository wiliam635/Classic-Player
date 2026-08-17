# Dependências e distribuição

Antes de publicar comercialmente, revise e cumpra as licenças destas dependências:

- **JUCE 9** — licenciamento dual/comercial conforme os termos oficiais da JUCE.
- **FluidSynth** — LGPL-2.1-or-later; preserve os avisos e as condições aplicáveis à distribuição dinâmica.
- **OpenSSL** — Apache License 2.0.
- **Steinberg ASIO SDK** — necessário somente para ASIO direto no aplicativo Standalone para Windows; não é necessário para o plugin VST3, pois a DAW administra o driver.
- **MSFA/Dexed DX7 algorithm representation** — a tabela de roteamento DX7 usada pelo motor é derivada da representação MSFA do projeto Dexed, licenciada sob Apache License 2.0. O Classic Player não incorpora a interface ou o wrapper GPL do Dexed.

O código e a chave pública de verificação podem ser distribuídos. A chave privada ECDSA usada para emitir ativações deve permanecer fora do repositório, do aplicativo, do VST3/AU e de todos os instaladores.
