# Dependências e distribuição

Antes de publicar comercialmente, revise e cumpra as licenças destas dependências:

- **JUCE 9** — licenciamento dual/comercial conforme os termos oficiais da JUCE.
- **FluidSynth** — LGPL-2.1-or-later; preserve os avisos e as condições aplicáveis à distribuição dinâmica.
- **OpenSSL** — Apache License 2.0.
- **Steinberg ASIO SDK** — necessário somente para ASIO direto no aplicativo Standalone para Windows; não é necessário para o plugin VST3, pois a DAW administra o driver.
- **MSFA DX7 synthesis core** — o núcleo de síntese FM em `third_party/msfa` é derivado do MSFA usado pelo Dexed e está sob Apache License 2.0. Os cabeçalhos de copyright e licença foram preservados. O Classic Player não incorpora a interface nem o wrapper GPL do Dexed.

O código e a chave pública de verificação podem ser distribuídos. A chave privada ECDSA usada para emitir ativações deve permanecer fora do repositório, do aplicativo, do VST3/AU e de todos os instaladores.

- **Classic Keys Analog factory preset mappings** — the initial “Solo Lead”, “Warm Pad”, “Atmospheric Pad”, “Modern Lead”, and “Dub Bass” settings are adapted from the open-source preset data in [stevebarakat/Minimoog](https://github.com/stevebarakat/Minimoog), licensed under MIT. The Classic Keys Analog engine and its physical-style interface are original work; only settings that map to its implemented controls are used.
