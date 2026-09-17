# Classic Player Android

Base inicial do aplicativo Android, separada do projeto desktop. O mínimo é **Android 6.0 / API 23**; a primeira validação é pensada para tablet em orientação horizontal, incluindo o Galaxy Tab A7.

## O que já existe

- Tela Live Set responsiva em paisagem, com oito posições por banco.
- Navegação visual entre Live Set e Mixer, abrindo inicialmente no Mixer com seis layers e faders maiores.
- Descoberta de dispositivos MIDI USB usando a API MIDI nativa do Android 6.
- Tela cheia e tela mantida ligada durante o uso.

O áudio é renderizado por um motor SoundFont nativo (`TinySoundFont`) dentro do APK. Cada uma das seis layers pode carregar um arquivo `.sf2`; o arquivo é copiado para o armazenamento privado do aplicativo e continua disponível offline. As notas MIDI USB, sustain (CC64), All Notes Off (CC120/123), volumes individuais e master são enviados ao mesmo motor.

## Abrir e testar

1. Instale Android Studio com JDK 17 e Android SDK Platform 35.
2. Abra a pasta `android` como projeto no Android Studio e permita a sincronização do Gradle.
3. Conecte o tablet por USB, habilite a depuração USB e execute o módulo `app`.

O APK será instalado em Android 6 ou superior. Para MIDI USB, use um adaptador OTG quando o dispositivo exigir.

## Limitações atuais

- O carregamento e a reprodução de SF2 são reais, mas a escolha de preset por layer ainda usa o primeiro preset de cada banco.
- Live Set, pads, presets completos e edição avançada das layers ainda serão conectados ao mesmo motor nas próximas etapas.
`LicenseManager` já mantém token, nome/e-mail da conta e uma janela offline de 30 dias; a tela de login e a validação HTTP serão conectadas ao serviço na próxima integração.
 A tela de login Android foi adicionada e envia as credenciais para `/v1/auth/login`; após sucesso, o Mixer é aberto e a sessão fica disponível offline.
 Ao voltar para o app, a sessão é validada em `/v1/license/validate`; falhas de rede preservam o modo offline, enquanto uma rejeição explícita exige novo login.
