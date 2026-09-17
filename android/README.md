# Classic Player Android

Base inicial do aplicativo Android, separada do projeto desktop. O mínimo é **Android 6.0 / API 23**; a primeira validação é pensada para tablet em orientação horizontal, incluindo o Galaxy Tab A7.

## O que já existe

- Tela Live Set responsiva em paisagem, com oito posições por banco.
- Navegação visual entre Live Set e Mixer, abrindo inicialmente no Mixer com seis layers e faders maiores.
- Descoberta de dispositivos MIDI USB usando a API MIDI nativa do Android 6.
- Tela cheia e tela mantida ligada durante o uso.

Foi adicionada uma base de áudio polifônica de baixa latência (`PolySynthEngine`) com 512 vozes, igual ao limite do motor SoundFont do Windows/Mac, para validar a saída e o ciclo de notas enquanto o carregamento do SF2 é integrado. Ela ainda não substitui o motor SoundFont final.

## Abrir e testar

1. Instale Android Studio com JDK 17 e Android SDK Platform 35.
2. Abra a pasta `android` como projeto no Android Studio e permita a sincronização do Gradle.
3. Conecte o tablet por USB, habilite a depuração USB e execute o módulo `app`.

O APK será instalado em Android 6 ou superior. Para MIDI USB, use um adaptador OTG quando o dispositivo exigir.

## Próxima etapa

O recebimento de notas MIDI USB já está conectado ao `PolySynthEngine` (primeiro dispositivo disponível, com Note On/Off). A próxima etapa é adicionar o carregador SoundFont nativo e conectar Mixer, programas e Live Set aos mesmos dados do motor.
A escolha de SF2 por layer usa o seletor de documentos do Android e é persistida entre aberturas do app.
O processamento MIDI também trata sustain (CC64) e mensagens All Notes Off (CC120/123) para reduzir notas presas.
Cada layer mantém URI, nome, preset e ganho em `SoundFontLayer`, fornecendo streams novos ao futuro renderizador nativo.
`LicenseManager` já mantém token, nome/e-mail da conta e uma janela offline de 30 dias; a tela de login e a validação HTTP serão conectadas ao serviço na próxima integração.
 A tela de login Android foi adicionada e envia as credenciais para `/v1/auth/login`; após sucesso, o Mixer é aberto e a sessão fica disponível offline.
 Ao voltar para o app, a sessão é validada em `/v1/license/validate`; falhas de rede preservam o modo offline, enquanto uma rejeição explícita exige novo login.
