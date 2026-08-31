# Classic Player Android

Base inicial do aplicativo Android, separada do projeto desktop. O mínimo é **Android 6.0 / API 23**; a primeira validação é pensada para tablet em orientação horizontal, incluindo o Galaxy Tab A7.

## O que já existe

- Tela Live Set responsiva em paisagem, com oito posições por banco.
- Navegação visual entre Live Set e Mixer provisório.
- Descoberta de dispositivos MIDI USB usando a API MIDI nativa do Android 6.
- Tela cheia e tela mantida ligada durante o uso.

Ainda não há reprodução de SF2, DX7, Hammond ou áudio de pads nesta base. Isso evita prometer funcionamento sonoro antes de definir e testar o motor nativo Android de baixa latência.

## Abrir e testar

1. Instale Android Studio com JDK 17 e Android SDK Platform 35.
2. Abra a pasta `android` como projeto no Android Studio e permita a sincronização do Gradle.
3. Conecte o tablet por USB, habilite a depuração USB e execute o módulo `app`.

O APK será instalado em Android 6 ou superior. Para MIDI USB, use um adaptador OTG quando o dispositivo exigir.

## Próxima etapa

Criar o motor de áudio Android: saída de baixa latência, recebimento de notas MIDI e uma primeira layer de piano/pad. Depois disso, conectar Mixer, programas e Live Set aos mesmos dados do motor.
