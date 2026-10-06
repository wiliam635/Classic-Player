#include "PluginEditor.h"
#include "AnalogEditorPanel.h"
#include "AnalogBrowserPresets.h"
#include "HammondEditorPanel.h"
#include "LicenseVerifier.h"
#include "ClassicPlayerAssets.h"
#include "ChordDetector.h"
#if JucePlugin_Build_Standalone
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <initializer_list>
#include <limits>
#include <set>
#include <utility>

namespace
{
void setButtonTextIfChanged(juce::TextButton& button, const juce::String& value)
{
    if (button.getButtonText() != value)
        button.setButtonText(value);
}

void setButtonColourIfChanged(juce::TextButton& button, int colourId, juce::Colour value)
{
    if (button.findColour(colourId) != value)
        button.setColour(colourId, value);
}

struct UiPalette
{
    juce::uint32 background, panel, panelLight, line, accent, highlight, text, mutedText;
};

static constexpr std::array<UiPalette, 7> uiPalettes {{
    { 0xff091018, 0xff151f28, 0xff202c35, 0xff33414c, 0xff13b8ad, 0xffffd84a, 0xffedf4f7, 0xff9eabb5 },
    { 0xff050608, 0xff101216, 0xff1c2025, 0xff565e66, 0xffbec5cc, 0xfff3f5f6, 0xfff6f7f8, 0xffb1b7bd },
    { 0xff22040b, 0xff3b0912, 0xff60101e, 0xff8d7f82, 0xffd51b38, 0xffbec3c8, 0xfffaf7f8, 0xffc9bfc2 },
    { 0xff0d1027, 0xff151a35, 0xff202747, 0xff5d5b88, 0xff6549cf, 0xfff5f2ff, 0xfffbfaff, 0xffc8c4df },
    { 0xffe9eef4, 0xfff6f8fb, 0xffd6e0ec, 0xff6c849d, 0xff126dbe, 0xff3b92dd, 0xff14273b, 0xff4c6073 },
    { 0xff9fa5ad, 0xffc8cdd2, 0xffe0e3e7, 0xff717982, 0xff245e83, 0xffe9bc36, 0xff16222c, 0xff394853 },
    { 0xff24292e, 0xff30363d, 0xff424a53, 0xff74818c, 0xff9ac9df, 0xffffd84a, 0xfff1f4f6, 0xffbbc4cc }
}};
int activeUiPalette = 0;
juce::uint32 background = uiPalettes[0].background;
juce::uint32 panel = uiPalettes[0].panel;
juce::uint32 panelLight = uiPalettes[0].panelLight;
juce::uint32 paletteLine = uiPalettes[0].line;
juce::uint32 teal = uiPalettes[0].accent;
juce::uint32 yellow = uiPalettes[0].highlight;
juce::uint32 text = uiPalettes[0].text;
juce::uint32 mutedText = uiPalettes[0].mutedText;

void paintBrushedSteel(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    const bool dark = activeUiPalette == 6;
    juce::ColourGradient finish(juce::Colour(dark ? 0xff292e34 : 0xffa1a7ae), bounds.getX(), bounds.getY(),
                               juce::Colour(dark ? 0xff2b3036 : 0xffa4aab2), bounds.getRight(), bounds.getY(), false);
    finish.addColour(0.25, juce::Colour(dark ? 0xff414850 : 0xffdce0e4));
    finish.addColour(0.53, juce::Colour(dark ? 0xff20252a : 0xffb3bac1));
    finish.addColour(0.72, juce::Colour(dark ? 0xff555d65 : 0xffedf0f2));
    g.setGradientFill(finish); g.fillRect(bounds);
    static const auto grain = []
    {
        juce::Image image(juce::Image::ARGB, 512, 128, true);
        juce::Graphics texture(image);
        juce::Random random(1573);
        for (int row = 0; row < 128; ++row)
            for (int x = 0; x < 512;)
            {
                const auto width = 8 + random.nextInt(48);
                texture.setColour((random.nextBool() ? juce::Colours::white : juce::Colours::black)
                    .withAlpha(0.025f + random.nextFloat() * 0.035f));
                texture.fillRect(x, row, width, 1); x += width;
            }
        return image;
    }();
    g.setTiledImageFill(grain, 0, 0, dark ? 0.65f : 1.0f); g.fillRect(bounds);
}

void setUiPalette(int index)
{
    activeUiPalette = juce::jlimit(0, (int) uiPalettes.size() - 1, index);
    const auto& palette = uiPalettes[(size_t) activeUiPalette];
    background = palette.background;
    panel = palette.panel;
    panelLight = palette.panelLight;
    paletteLine = palette.line;
    teal = palette.accent;
    yellow = palette.highlight;
    text = palette.text;
    mutedText = palette.mutedText;
}

juce::Colour brandTextColour()
{
    return juce::Colour(activeUiPalette == 2 || activeUiPalette == 3 ? text : teal);
}

juce::Colour layerAccentColour(int layer)
{
    static const std::array<juce::Colour, 8> colours {
        juce::Colour(0xff138cff), juce::Colour(0xffd94cff),
        juce::Colour(0xff18d6a4), juce::Colour(0xffff9b42),
        juce::Colour(0xffff4f7b), juce::Colour(0xfff2d13d),
        juce::Colour(0xff27d8ed), juce::Colour(0xff8f72ff)
    };
    return colours[(size_t) juce::jlimit(0, 7, layer)];
}

juce::String layerOutlinePreferenceKey(int layer)
{
    return "layerOutline" + juce::String(layer + 1);
}

void drawCategoryArtwork(juce::Graphics& g, juce::Rectangle<int> bounds,
                         const juce::String& category, juce::Colour accent)
{
    if (bounds.isEmpty()) return;
    static constexpr std::array<const char*, 13> resourceNames {
        "pianoacustico_png", "pianoeletrico_png", "pianodx_jpeg",
        "strings_png", "pad_png", "synth_png", "brass_png", "organ_png",
        "guitar_png", "bass_png", "bells_png", "efeitos_png", "outros_png"
    };
    static std::array<std::unique_ptr<juce::Image>, resourceNames.size()> images;
    const auto categories = ClassicPlayerAudioProcessor::soundFontCategories();
    auto imageIndex = categories.indexOf(category);
    if (category == "DX7") imageIndex = 2;
    else if (category == "Hammond") imageIndex = 7;
    else if (category == "Moog Analog" || category == "Synth") imageIndex = 5;
    if (juce::isPositiveAndBelow(imageIndex, (int) images.size()))
    {
        auto& image = images[(size_t) imageIndex];
        if (image == nullptr)
        {
            int dataSize = 0;
            const auto* data = ClassicPlayerAssets::getNamedResource(resourceNames[(size_t) imageIndex], dataSize);
            const auto original = data != nullptr
                ? juce::ImageFileFormat::loadFrom(data, (size_t) dataSize) : juce::Image{};
            juce::Image thumbnail;
            if (original.isValid())
            {
                thumbnail = juce::Image(juce::Image::ARGB, 256, 120, true);
                juce::Graphics thumbnailGraphics(thumbnail);
                const auto sourceAspect = (float) original.getWidth() / (float) original.getHeight();
                constexpr auto targetAspect = 256.0f / 120.0f;
                const auto sourceWidth = sourceAspect > targetAspect
                    ? juce::roundToInt((float) original.getHeight() * targetAspect) : original.getWidth();
                const auto sourceHeight = sourceAspect > targetAspect
                    ? original.getHeight() : juce::roundToInt((float) original.getWidth() / targetAspect);
                thumbnailGraphics.drawImage(original, 0, 0, 256, 120,
                    (original.getWidth() - sourceWidth) / 2, (original.getHeight() - sourceHeight) / 2,
                    sourceWidth, sourceHeight);
            }
            image = std::make_unique<juce::Image>(std::move(thumbnail));
        }
        if (image->isValid())
        {
            // Keep the photo inside the frame, including its rounded corners.
            // The image used to reach the frame's outer edge and cover it at
            // the corners, especially on narrow mixer strips.
            {
                juce::Graphics::ScopedSaveState imageState(g);
                juce::Path imageClip;
                imageClip.addRoundedRectangle(bounds.toFloat().reduced(1.0f), 3.0f);
                g.reduceClipRegion(imageClip);
                const auto imageBounds = bounds.reduced(2);
                g.drawImageWithin(*image, imageBounds.getX(), imageBounds.getY(),
                                  imageBounds.getWidth(), imageBounds.getHeight(),
                                  juce::RectanglePlacement::fillDestination);
            }
            g.setColour(accent.withAlpha(0.75f));
            g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), 3.0f, 1.0f);
            return;
        }
    }
    auto area = bounds.toFloat();
    const auto centre = area.getCentre();
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff071525), area.getX(), area.getBottom(),
                                           accent.darker(0.45f), area.getRight(), area.getY(), false));
    g.fillRoundedRectangle(area, 3.0f);

    // Soft stage lights give every generated category banner the same visual
    // language as the supplied reference while keeping the asset resolution-independent.
    g.setGradientFill(juce::ColourGradient(accent.withAlpha(0.55f), centre.x, area.getBottom(),
                                           juce::Colours::transparentBlack, centre.x, area.getY(), true));
    g.fillEllipse(area.expanded(area.getWidth() * 0.12f, area.getHeight() * 0.28f));
    g.setColour(accent.withAlpha(0.65f));
    g.drawHorizontalLine(juce::roundToInt(area.getBottom() - 7.0f), area.getX() + 5.0f, area.getRight() - 5.0f);

    const auto key = category.toLowerCase();
    g.setColour(juce::Colours::white.withAlpha(0.9f));
    if (key.contains("dx7"))
    {
        // Six operator blocks and crossing modulation lines identify the FM engine.
        std::array<juce::Point<float>, 6> operators {
            juce::Point<float>(area.getX() + 22.0f, area.getY() + 16.0f),
            juce::Point<float>(centre.x, area.getY() + 12.0f),
            juce::Point<float>(area.getRight() - 22.0f, area.getY() + 16.0f),
            juce::Point<float>(area.getX() + 30.0f, area.getBottom() - 17.0f),
            juce::Point<float>(centre.x, area.getBottom() - 13.0f),
            juce::Point<float>(area.getRight() - 30.0f, area.getBottom() - 17.0f)
        };
        g.setColour(accent.brighter(0.35f).withAlpha(0.9f));
        for (size_t i = 1; i < operators.size(); ++i)
            g.drawLine(operators[i - 1].x, operators[i - 1].y,
                       operators[i].x, operators[i].y, 1.5f);
        g.drawLine(operators[0].x, operators[0].y, operators[4].x, operators[4].y, 1.2f);
        g.drawLine(operators[2].x, operators[2].y, operators[3].x, operators[3].y, 1.2f);
        g.setColour(juce::Colours::white.withAlpha(0.92f));
        for (const auto point : operators)
        {
            g.fillRoundedRectangle(point.x - 6.0f, point.y - 5.0f, 12.0f, 10.0f, 2.0f);
            g.setColour(juce::Colour(0xff14202b));
            g.fillEllipse(point.x - 2.0f, point.y - 2.0f, 4.0f, 4.0f);
            g.setColour(juce::Colours::white.withAlpha(0.92f));
        }
    }
    else if (key.contains("moog") || key.contains("analog"))
    {
        // Three oscillators, a filter sweep and a small keyboard form the analog banner.
        const auto topY = area.getY() + 13.0f;
        for (int oscillator = 0; oscillator < 3; ++oscillator)
        {
            const auto x = area.getX() + 25.0f + static_cast<float>(oscillator) * 25.0f;
            g.drawEllipse(x - 7.0f, topY - 7.0f, 14.0f, 14.0f, 2.0f);
            g.drawLine(x, topY, x + 4.0f, topY - 5.0f, 2.0f);
        }
        auto keyboard = area.reduced(13.0f, 7.0f).removeFromBottom(15.0f);
        g.fillRect(keyboard);
        g.setColour(juce::Colour(0xff101820));
        for (int i = 1; i < 9; ++i)
            g.drawVerticalLine(juce::roundToInt(keyboard.getX() + keyboard.getWidth() * static_cast<float>(i) / 9.0f),
                               keyboard.getY(), keyboard.getBottom());
        g.setColour(accent.brighter(0.35f));
        juce::Path filter;
        filter.startNewSubPath(area.getX() + 15.0f, centre.y + 7.0f);
        filter.cubicTo(centre.x - 8.0f, centre.y + 7.0f, centre.x + 2.0f, centre.y - 9.0f,
                       area.getRight() - 14.0f, centre.y - 9.0f);
        g.strokePath(filter, juce::PathStrokeType(2.2f));
    }
    else if (key.contains("hammond"))
    {
        // Drawbars are the Hammond's strongest visual signature.
        const auto baseY = area.getBottom() - 19.0f;
        for (int i = 0; i < 9; ++i)
        {
            const auto x = area.getX() + 10.0f + static_cast<float>(i) * (area.getWidth() - 20.0f) / 9.0f;
            const auto travel = 13.0f + static_cast<float>((i * 5) % 15);
            g.drawVerticalLine(juce::roundToInt(x), area.getY() + 7.0f, baseY);
            g.fillRoundedRectangle(x - 3.0f, baseY - travel, 6.0f, 9.0f, 1.5f);
        }
        g.setColour(accent.brighter(0.35f));
        g.drawHorizontalLine(juce::roundToInt(area.getBottom() - 10.0f),
                             area.getX() + 8.0f, area.getRight() - 8.0f);
    }
    else if (key.contains("piano") || key.contains("dx"))
    {
        auto keyboard = area.reduced(12.0f, 9.0f).removeFromBottom(17.0f);
        g.fillRect(keyboard);
        g.setColour(juce::Colour(0xff101820));
        for (int i = 1; i < 8; ++i)
            g.drawVerticalLine(juce::roundToInt(keyboard.getX() + keyboard.getWidth() * i / 8.0f),
                               keyboard.getY(), keyboard.getBottom());
        for (int i : { 1, 2, 4, 5, 6 })
            g.fillRect(keyboard.getX() + keyboard.getWidth() * i / 8.0f - 2.0f,
                       keyboard.getY(), 4.0f, keyboard.getHeight() * 0.58f);
        g.setColour(juce::Colours::white.withAlpha(0.9f));
        juce::Path piano;
        piano.startNewSubPath(area.getX() + 18.0f, keyboard.getY());
        piano.lineTo(area.getX() + 26.0f, area.getY() + 11.0f);
        piano.lineTo(area.getRight() - 17.0f, area.getY() + 7.0f);
        piano.lineTo(area.getRight() - 29.0f, keyboard.getY());
        piano.closeSubPath();
        g.fillPath(piano);
    }
    else if (key.contains("string"))
    {
        g.drawEllipse(centre.x - 13.0f, centre.y - 18.0f, 26.0f, 36.0f, 3.0f);
        g.drawLine(centre.x, area.getY() + 5.0f, centre.x, area.getBottom() - 5.0f, 2.0f);
        g.drawLine(centre.x, centre.y - 12.0f, area.getRight() - 18.0f, area.getY() + 8.0f, 2.0f);
    }
    else if (key.contains("organ"))
    {
        const auto baseY = area.getBottom() - 9.0f;
        for (int i = 0; i < 7; ++i)
        {
            const auto x = area.getX() + 16.0f + i * (area.getWidth() - 32.0f) / 7.0f;
            const auto height = 14.0f + static_cast<float>((i * 7) % 19);
            g.fillRoundedRectangle(x, baseY - height, 5.0f, height, 1.0f);
        }
    }
    else if (key.contains("guitar") || key.contains("bass"))
    {
        g.fillEllipse(centre.x - 20.0f, centre.y - 13.0f, 25.0f, 26.0f);
        g.fillEllipse(centre.x - 7.0f, centre.y - 10.0f, 21.0f, 20.0f);
        g.fillRect(centre.x + 7.0f, centre.y - 2.0f, area.getRight() - centre.x - 21.0f, 4.0f);
        g.setColour(juce::Colour(0xff101820));
        g.fillEllipse(centre.x - 3.0f, centre.y - 4.0f, 8.0f, 8.0f);
    }
    else if (key.contains("brass"))
    {
        g.drawLine(area.getX() + 18.0f, centre.y, area.getRight() - 28.0f, centre.y, 6.0f);
        juce::Path bell;
        bell.startNewSubPath(area.getRight() - 31.0f, centre.y - 5.0f);
        bell.lineTo(area.getRight() - 12.0f, centre.y - 15.0f);
        bell.lineTo(area.getRight() - 12.0f, centre.y + 15.0f);
        bell.lineTo(area.getRight() - 31.0f, centre.y + 5.0f);
        bell.closeSubPath();
        g.fillPath(bell);
    }
    else if (key.contains("bell"))
    {
        juce::Path bell;
        bell.addArc(centre.x - 17.0f, centre.y - 18.0f, 34.0f, 36.0f,
                    -juce::MathConstants<float>::pi, 0.0f, true);
        bell.lineTo(centre.x + 21.0f, centre.y + 13.0f);
        bell.lineTo(centre.x - 21.0f, centre.y + 13.0f);
        bell.closeSubPath();
        g.fillPath(bell);
        g.fillEllipse(centre.x - 4.0f, centre.y + 11.0f, 8.0f, 8.0f);
    }
    else
    {
        juce::Path wave;
        wave.startNewSubPath(area.getX() + 10.0f, centre.y);
        for (int x = 10; x <= bounds.getWidth() - 10; x += 3)
        {
            const auto phase = static_cast<float>(x) * 0.22f;
            wave.lineTo(area.getX() + static_cast<float>(x), centre.y + std::sin(phase) * 12.0f);
        }
        g.strokePath(wave, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));
    }

    g.setColour(accent.withAlpha(0.9f));
    g.drawRoundedRectangle(area.reduced(0.5f), 3.0f, 1.0f);
}

struct UiTranslation
{
    const char* portuguese;
    const char* english;
    const char* spanish;
};

// Portuguese is the source language for UI strings. Keep terminology stable
// across the standalone app and plug-in formats; audio terms such as MIDI,
// REVERB, CUTOFF and PANIC intentionally remain industry-standard labels.
static constexpr UiTranslation uiTranslations[] {
    { "PRATEADO", "SILVER", "PLATEADO" },
    { "AÇO ESCURO", "DARK STEEL", "ACERO OSCURO" },
    { "ATUALIZACOES", "UPDATES", "ACTUALIZACIONES" },
    { "VERIFICANDO...", "CHECKING...", "COMPROBANDO..." },
    { "ATUALIZACAO DISPONIVEL", "UPDATE AVAILABLE", "ACTUALIZACIÓN DISPONIBLE" },
    { "BAIXAR ATUALIZACAO", "DOWNLOAD UPDATE", "DESCARGAR ACTUALIZACIÓN" },
    { "MAIS TARDE", "LATER", "MÁS TARDE" },
    { "NOTAS DA VERSAO", "RELEASE NOTES", "NOTAS DE LA VERSIÓN" },
    { "Sua versão está atualizada.", "Your version is up to date.", "Tu versión está actualizada." },
    { "Não foi possível verificar as atualizações. Tente novamente mais tarde.", "Could not check for updates. Try again later.", "No se pudieron comprobar las actualizaciones. Inténtalo más tarde." },
    { "Carga do processamento de áudio do Classic Player. 100% indica que o tempo disponível para o buffer foi consumido.", "Classic Player audio processing load. 100% means the available buffer time was used.", "Carga de procesamiento de audio de Classic Player. 100% indica que se utilizó todo el tiempo disponible del búfer." },
    { "CONFIGURACOES", "SETTINGS", "CONFIGURACIÓN" },
    { "SONS QUE INSPIRAM", "SOUNDS THAT INSPIRE", "SONIDOS QUE INSPIRAN" },
    { "NOVO", "NEW", "NUEVO" }, { "SALVAR", "SAVE", "GUARDAR" },
    { "EXPORTAR", "EXPORT", "EXPORTAR" }, { "EXCLUIR", "DELETE", "ELIMINAR" },
    { "IMPORTAR", "IMPORT", "IMPORTAR" }, { "EDITAR", "EDIT", "EDITAR" },
    { "VOLTAR", "BACK", "VOLVER" }, { "ANTERIOR", "PREVIOUS", "ANTERIOR" },
    { "PROXIMO  >", "NEXT  >", "SIGUIENTE  >" },
    { "MISTO", "MIXED", "MIXTO" }, { "SUSTENIDO", "SHARP", "SOSTENIDO" },
    { "BEMOL", "FLAT", "BEMOL" },
    { "COR ACORDE", "CHORD COLOR", "COLOR DEL ACORDE" },
    { "COR TECLAS", "KEY COLOR", "COLOR DE TECLAS" },
    { "PADRÃO", "CLASSIC", "CLÁSICO" },
    { "PRETO", "BLACK", "NEGRO" },
    { "VERMELHO", "RED", "ROJO" },
    { "AZUL ROXO", "PURPLE BLUE", "AZUL VIOLETA" },
    { "BRANCO AZUL", "WHITE BLUE", "BLANCO AZUL" },
    { "TEMA", "THEME", "TEMA" },
    { "COR DO CONTORNO", "LAYER OUTLINE", "CONTORNO DE CAPA" },
    { "Automática", "Automatic", "Automático" },
    { "Vermelho", "Red", "Rojo" }, { "Laranja", "Orange", "Naranja" },
    { "Amarelo", "Yellow", "Amarillo" }, { "Verde", "Green", "Verde" },
    { "Ciano", "Cyan", "Cian" }, { "Azul", "Blue", "Azul" },
    { "Roxo", "Purple", "Morado" }, { "Rosa", "Pink", "Rosa" },
    { "Branco", "White", "Blanco" }, { "Cinza", "Gray", "Gris" },
    { "Escolher outra cor...", "Choose another color...", "Elegir otro color..." },
    { "Tema de cores do aplicativo", "Application color theme", "Tema de color de la aplicación" },
    { "OCULTAR TECLADO", "HIDE KEYBOARD", "OCULTAR TECLADO" },
    { "MOSTRAR TECLADO", "SHOW KEYBOARD", "MOSTRAR TECLADO" },
    { "GRAVAR WAV+MIDI", "RECORD WAV+MIDI", "GRABAR WAV+MIDI" },
    { "PARAR", "STOP", "DETENER" }, { "PANIC", "PANIC", "PANIC" },
    { "MASTER", "MASTER", "MASTER" }, { "VOLUME", "VOLUME", "VOLUMEN" },
    { "LIM", "LIM", "LIM" }, { "EQ", "EQ", "EQ" },
    { "NOVO PROGRAMA", "NEW PERFORMANCE", "NUEVA PERFORMANCE" },
    { "LIVE SET", "LIVE SET", "LIVE SET" },
    { "EDITAR LIVE SET", "EDIT LIVE SET", "EDITAR LIVE SET" },
    { "CONCLUIR EDICAO", "DONE EDITING", "FINALIZAR EDICIÓN" },
    { "BANCO", "BANK", "BANCO" },
    { "Piano Acustico", "Acoustic Piano", "Piano Acústico" },
    { "Piano Eletrico", "Electric Piano", "Piano Eléctrico" },
    { "Piano DX", "DX Piano", "Piano DX" },
    { "Strings", "Strings", "Cuerdas" },
    { "Synth", "Synth", "Sintetizador" },
    { "Brass", "Brass", "Metales" },
    { "Organ", "Organ", "Órgano" },
    { "Guitar", "Guitar", "Guitarra" },
    { "Bass", "Bass", "Bajo" },
    { "Bells", "Bells", "Campanas" },
    { "Efeitos", "Effects", "Efectos" },
    { "Outros", "Other", "Otros" },
    { "LAYER SF2", "SF2 LAYER", "CAPA SF2" },
    { "Layer SF2", "SF2 Layer", "Capa SF2" },
    { "Layer DX7 (.syx)", "DX7 Layer (.syx)", "Capa DX7 (.syx)" },
    { "Classic Keys Analog", "Classic Keys Analog", "Classic Keys Analog" },
    { "Layer Drum Pads (8)", "Drum Pads Layer (8)", "Capa Drum Pads (8)" },
    { "Hammond", "Hammond", "Hammond" },
    { "Pad Continuo (12)", "Continuous Pads (12)", "Pads Continuos (12)" },
    { "EDITAR LAYER", "EDIT LAYER", "EDITAR CAPA" },
    { "Excluir layer", "Delete layer", "Eliminar capa" },
    { "EDITAR REVERB", "EDIT REVERB", "EDITAR REVERB" },
    { "EDITAR COMP", "EDIT COMP", "EDITAR COMP" },
    { "EDITAR CHORUS", "EDIT CHORUS", "EDITAR CHORUS" },
    { "EDITAR EQ", "EDIT EQ", "EDITAR EQ" },
    { "EDITAR EQ / FILTROS", "EDIT EQ / FILTERS", "EDITAR EQ / FILTROS" },
    { "CATEGORIA", "CATEGORY", "CATEGORÍA" },
    { "BIBLIOTECA SF2", "SF2 LIBRARY", "BIBLIOTECA SF2" },
    { "PRESET", "PRESET", "PRESET" }, { "MODO", "MODE", "MODO" },
    { "SUSTAIN", "SUSTAIN", "SUSTAIN" }, { "CANAL MIDI", "MIDI CHANNEL", "CANAL MIDI" },
    { "ENTRADA MIDI", "MIDI INPUT", "ENTRADA MIDI" },
    { "OITAVA", "OCTAVE", "OCTAVA" }, { "FAIXA DE NOTAS", "NOTE RANGE", "RANGO DE NOTAS" },
    { "VELOCIDADE", "VELOCITY", "VELOCIDAD" },
    { "TODOS OS CONTROLADORES", "ALL CONTROLLERS", "TODOS LOS CONTROLADORES" },
    { "CONTROLADOR MIDI", "MIDI CONTROLLER", "CONTROLADOR MIDI" },
    { "POLI", "POLY", "POLI" }, { "MONO / LEGATO", "MONO / LEGATO", "MONO / LEGATO" },
    { "MONO LEGATO", "MONO LEGATO", "MONO LEGATO" },
    { "PORTAMENTO", "PORTAMENTO", "PORTAMENTO" },
    { "SUSTAIN ON", "SUSTAIN ON", "SUSTAIN ON" },
    { "SUSTAIN OFF", "SUSTAIN OFF", "SUSTAIN OFF" },
    { "MIDI OMNI", "MIDI OMNI", "MIDI OMNI" },
    { "VEL LINEAR", "LINEAR VEL", "VEL LINEAL" },
    { "VEL SOFT", "SOFT VEL", "VEL SUAVE" },
    { "VEL HARD", "HARD VEL", "VEL FUERTE" },
    { "ESCOLHA O SF2", "CHOOSE AN SF2", "ELIGE UN SF2" },
    { "CATEGORIA VAZIA", "EMPTY CATEGORY", "CATEGORÍA VACÍA" },
    { "BIBLIOTECA DX7 VAZIA", "EMPTY DX7 LIBRARY", "BIBLIOTECA DX7 VACÍA" },
    { "SELECIONE O TIMBRE DX7", "SELECT A DX7 PATCH", "SELECCIONA UN TIMBRE DX7" },
    { "VST INSTALADO", "INSTALLED VST", "VST INSTALADO" },
    { "IMPORTAR SF2", "IMPORT SF2", "IMPORTAR SF2" },
    { "EXCLUIR SF2", "DELETE SF2", "ELIMINAR SF2" },
    { "CARREGAR VST", "LOAD VST", "CARGAR VST" },
    { "ABRIR EDITOR", "OPEN EDITOR", "ABRIR EDITOR" },
    { "IMPORTAR DX7", "IMPORT DX7", "IMPORTAR DX7" },
    { "EXCLUIR DX7", "DELETE DX7", "ELIMINAR DX7" },
    { "BANCO DX7", "DX7 BANK", "BANCO DX7" },
    { "TIMBRE DX7", "DX7 PATCH", "TIMBRE DX7" },
    { "SEM SOUNDFONT", "NO SOUNDFONT", "SIN SOUNDFONT" },
    { "PAD CONTINUO", "CONTINUOUS PADS", "PADS CONTINUOS" },
    { "PADS CONTINUOS", "CONTINUOUS PADS", "PADS CONTINUOS" },
    { "DRUM PADS", "DRUM PADS", "DRUM PADS" },
    { "PAD", "PAD", "PAD" }, { "LOAD", "LOAD", "CARGAR" },
    { "LEARN", "LEARN", "APRENDER" }, { "LEARN CC", "LEARN CC", "APRENDER CC" },
    { "LEARN VOLUME", "LEARN VOLUME", "APRENDER VOLUMEN" },
    { "LEARN MUTE", "LEARN MUTE", "APRENDER MUTE" },
    { "LEARN STOP", "LEARN STOP", "APRENDER STOP" },
    { "LEARN M", "LEARN MUTE", "APRENDER MUTE" },
    { "VOLUME: MOVA O CC", "VOLUME: MOVE A CC", "VOLUMEN: MUEVE UN CC" },
    { "MUTE: MOVA O CC", "MUTE: MOVE A CC", "MUTE: MUEVE UN CC" },
    { "STOP: MOVA O CC", "STOP: MOVE A CC", "STOP: MUEVE UN CC" },
    { "MOVA O CC", "MOVE A CC", "MUEVE UN CC" },
    { "MOVA UM CC", "MOVE A CC", "MUEVE UN CC" },
    { "MOVE CC", "MOVE CC", "MUEVE UN CC" },
    { "AGUARDANDO...", "WAITING...", "ESPERANDO..." },
    { "RESET", "RESET", "RESTABLECER" }, { "RESET CC", "RESET CC", "RESTABLECER CC" },
    { "MUTE", "MUTE", "MUTE" }, { "COMP", "COMP", "COMP" },
    { "REVERB", "REVERB", "REVERB" }, { "CHORUS", "CHORUS", "CHORUS" },
    { "ATTACK", "ATTACK", "ATTACK" }, { "RELEASE", "RELEASE", "RELEASE" },
    { "ROTEAMENTO DA LAYER", "LAYER ROUTING", "RUTEO DE CAPA" },
    { "MODULATION DO TECLADO: ON", "KEYBOARD MODULATION: ON", "MODULACIÓN DEL TECLADO: ON" },
    { "MODULATION DO TECLADO: OFF", "KEYBOARD MODULATION: OFF", "MODULACIÓN DEL TECLADO: OFF" },
    { "EXPORTAR PRESET", "EXPORT PRESET", "EXPORTAR PRESET" },
    { "IMPORTAR PRESET", "IMPORT PRESET", "IMPORTAR PRESET" },
    { "Exportar Preset", "Export Preset", "Exportar Preset" },
    { "ESCOLHA UM PRESET", "CHOOSE A PRESET", "ELIGE UN PRESET" },
    { "Exporta um arquivo .ckprogram que pode ser importado em outro Classic Player.",
      "Exports a .ckprogram file that can be imported into another Classic Player.",
      "Exporta un archivo .ckprogram que se puede importar en otro Classic Player." },
    { "GRAVANDO 00:00", "RECORDING 00:00", "GRABANDO 00:00" },
    { "WAV + MIDI: Area de Trabalho", "WAV + MIDI: Desktop", "WAV + MIDI: Escritorio" },
    { "WAV + MIDI salvos na Area de Trabalho", "WAV + MIDI saved to Desktop", "WAV + MIDI guardados en el Escritorio" },
    { "LICENCA CLASSIC PLAYER", "CLASSIC PLAYER LICENSE", "LICENCIA CLASSIC PLAYER" },
    { "Entre com o e-mail e a senha da sua conta para liberar este computador.",
      "Sign in with your account email and password to activate this computer.",
      "Inicia sesión con el correo y la contraseña de tu cuenta para activar este equipo." },
    { "ENTRAR E ATIVAR", "SIGN IN & ACTIVATE", "INICIAR SESIÓN Y ACTIVAR" },
    { "IDIOMA / LANGUAGE / IDIOMA", "LANGUAGE / IDIOMA", "IDIOMA / LANGUAGE" },
    { "APLICAR", "APPLY", "APLICAR" }, { "CANCELAR", "CANCEL", "CANCELAR" },
    { "FECHAR", "CLOSE", "CERRAR" }, { "SUBSTITUIR", "REPLACE", "REEMPLAZAR" },
    { "Salvar", "Save", "Guardar" }, { "Importar", "Import", "Importar" },
    { "Selecionar", "Select", "Seleccionar" },
    { "CANCELAR MIDI LEARN DO VOLUME MASTER", "CANCEL MASTER VOLUME MIDI LEARN", "CANCELAR MIDI LEARN DEL VOLUMEN MASTER" },
    { "MIDI LEARN DO VOLUME MASTER", "MASTER VOLUME MIDI LEARN", "MIDI LEARN DEL VOLUMEN MASTER" },
    { "Equalizador master", "Master equalizer", "Ecualizador master" },
    { "Audio / MIDI", "Audio / MIDI", "Audio / MIDI" },
    { "Clique para atribuir uma programação salva", "Click to assign a saved performance", "Haz clic para asignar una performance guardada" },
    { "Posição vazia", "Empty slot", "Posición vacía" },
    { "Mova agora um controle MIDI CC", "Move a MIDI CC control now", "Mueve ahora un control MIDI CC" },
    { "Clique e mova um controle MIDI para carregar esta performance", "Click and move a MIDI control to load this performance", "Haz clic y mueve un control MIDI para cargar esta performance" },
    { "Clique para reaprender.", "Click to learn again.", "Haz clic para volver a aprender." },
    { "Clique para reaprender.", "Click to learn again.", "Haz clic para volver a aprender." },
    { "Escolha um SoundFont", "Choose a SoundFont", "Elige un SoundFont" },
    { "HAMMOND", "HAMMOND", "HAMMOND" },
    { "CLASSIC KEYS ANALOG", "CLASSIC KEYS ANALOG", "CLASSIC KEYS ANALOG" },
    { "TODOS OS CONTROLADORES", "ALL CONTROLLERS", "TODOS LOS CONTROLADORES" },
    { "PADS CONTINUOS", "CONTINUOUS PADS", "PADS CONTINUOS" },
    { "SUSTAIN ON", "SUSTAIN ON", "SUSTAIN ON" },
    { "AUDIO / MIDI", "AUDIO / MIDI", "AUDIO / MIDI" },
    { "MIDI OMNI", "MIDI OMNI", "MIDI OMNI" },
    { "EXPORTADO:", "EXPORTED:", "EXPORTADO:" },
    { "Carregando...", "Loading...", "Cargando..." },
    { "Carregando instrumento...", "Loading instrument...", "Cargando instrumento..." },
    { "Importando DX7...", "Importing DX7...", "Importando DX7..." },
    { "COMPLETO", "DONE", "COMPLETADO" },
    { "Copyright 2026 Willam Silva & Classic Keys. Todos os direitos reservados.",
      "Copyright 2026 Willam Silva & Classic Keys. All rights reserved.",
      "Copyright 2026 Willam Silva & Classic Keys. Todos los derechos reservados." },
    { "Gravar simultaneamente a saída em WAV e a performance em MIDI",
      "Record the audio output to WAV and the performance to MIDI at the same time",
      "Graba simultáneamente la salida de audio en WAV y la performance en MIDI" },
    { "Configurar dispositivo de audio, taxa de amostragem, buffer e MIDI",
      "Configure the audio device, sample rate, buffer and MIDI",
      "Configura el dispositivo de audio, la frecuencia de muestreo, el búfer y MIDI" },
    { "Ativa ou desativa a modulação recebida pelo teclado",
      "Enable or disable keyboard modulation",
      "Activar o desactivar la modulación recibida del teclado" },
    { "Ativa ou desativa a modulação recebida pelo teclado nesta layer",
      "Enable or disable keyboard modulation for this layer",
      "Activar o desactivar la modulación del teclado en esta capa" },
    { "Mostrar ou ocultar o teclado virtual para liberar espaço para as layers",
      "Show or hide the virtual keyboard to make room for layers",
      "Mostrar u ocultar el teclado virtual para dejar espacio a las capas" },
    { "Formato dos acidentes exibidos no visor de acordes",
      "Accidental style shown in the chord display",
      "Formato de las alteraciones mostrado en el visor de acordes" },
    { "Selecione uma performance para carregá-la imediatamente; digite um nome para salvar uma nova",
      "Select a performance to load it, or type a name to save a new one",
      "Selecciona una performance para cargarla o escribe un nombre para guardar una nueva" },
    { "Criar uma programação vazia do zero", "Create a blank performance", "Crear una performance vacía desde cero" },
    { "Salvar esta performance na biblioteca interna do Classic Player", "Save this performance in the Classic Player library", "Guardar esta performance en la biblioteca de Classic Player" },
    { "Exportar uma cópia portátil da performance para outro computador", "Export a portable copy of this performance for another computer", "Exportar una copia portátil de esta performance para otro equipo" },
    { "Importar uma performance portátil para a biblioteca deste computador e abri-la", "Import a portable performance into this computer's library and open it", "Importar una performance portátil a la biblioteca de este equipo y abrirla" },
    { "Envia All Notes Off/All Sound Off e solta qualquer nota presa", "Sends All Notes Off/All Sound Off and releases any stuck notes", "Envía All Notes Off/All Sound Off y libera cualquier nota atascada" },
    { "Aprender um MIDI CC para acionar o Panic", "Learn a MIDI CC to trigger Panic", "Aprender un MIDI CC para activar Panic" },
    { "Mova um controle MIDI CC; clique novamente para cancelar ou use o botão direito para excluir o mapeamento.",
      "Move a MIDI CC control; click again to cancel or right-click to delete the mapping.",
      "Mueve un control MIDI CC; haz clic de nuevo para cancelar o clic derecho para eliminar el mapeo." },
    { "Apagar todos os endereçamentos MIDI Learn desta layer", "Clear all MIDI Learn mappings for this layer", "Borrar todos los mapeos MIDI Learn de esta capa" },
    { "Excluir o SF2 selecionado da biblioteca", "Delete the selected SF2 from the library", "Eliminar el SF2 seleccionado de la biblioteca" },
    { "Master forte", "Powerful master", "Master potente" },
    { "Preset exportado", "Preset exported", "Preset exportado" },
    { "O arquivo está corrompido.", "The file is corrupted.", "El archivo está dañado." },
    { "Não foi possível exportar o preset.", "Could not export the preset.", "No se pudo exportar el preset." },
    { "Preset inválido", "Invalid preset", "Preset no válido" },
    { "Falha ao salvar", "Failed to save", "Error al guardar" },
    { "Idioma / Language / Idioma", "Language / Idioma", "Idioma / Language" },
    { "Selecionar idioma / Select language / Seleccionar idioma",
      "Select language / Idioma", "Seleccionar idioma / Language" },
    { "Exportar Preset Classic Player", "Export Classic Player Preset", "Exportar preset de Classic Player" },
    { "Importar SoundFont", "Import SoundFont", "Importar SoundFont" },
    { "Escolha um arquivo DX7 SysEx", "Choose a DX7 SysEx file", "Elige un archivo DX7 SysEx" },
    { "Carregar áudio do pad", "Load pad audio", "Cargar audio del pad" },
    { "Exportar preset da layer", "Export layer preset", "Exportar preset de la capa" },
    { "Importar preset da layer", "Import layer preset", "Importar preset de la capa" },
    { "Escolha um instrumento virtual", "Choose a virtual instrument", "Elige un instrumento virtual" },
    { "Exportar performance Classic Player", "Export Classic Player performance", "Exportar performance de Classic Player" },
    { "Importar performance Classic Player", "Import Classic Player performance", "Importar performance de Classic Player" },
    { "Falha ao exportar", "Export failed", "Error al exportar" },
    { "Falha ao exportar preset", "Failed to export preset", "Error al exportar el preset" },
    { "Falha ao importar preset", "Failed to import preset", "Error al importar el preset" },
    { "Preset da layer exportado", "Layer preset exported", "Preset de la capa exportado" },
    { "Arquivo exportado para:", "File exported to:", "Archivo exportado a:" },
    { "Exportação concluída", "Export complete", "Exportación completada" },
    { "Cópia portátil salva em:", "Portable copy saved to:", "Copia portátil guardada en:" },
    { "Falha ao Exportar Performance", "Failed to Export Performance", "Error al exportar la performance" },
    { "Falha ao importar performance", "Failed to import performance", "Error al importar la performance" },
    { "Falha ao carregar performance", "Failed to load performance", "Error al cargar la performance" },
    { "Falha ao substituir performance", "Failed to replace performance", "Error al reemplazar la performance" },
    { "Preset da layer importado", "Layer preset imported", "Preset de la capa importado" },
    { "Importação concluída", "Import complete", "Importación completada" },
    { "Digite o nome da performance", "Enter the performance name", "Escribe el nombre de la performance" },
    { "O carregamento de VST/AU está disponível apenas no aplicativo standalone.", "VST/AU loading is available only in the standalone app.", "La carga de VST/AU solo está disponible en la aplicación independiente." },
    { "Instrumento externo", "External instrument", "Instrumento externo" },
    { "Falha ao carregar instrumento", "Failed to load instrument", "Error al cargar el instrumento" },
    { "Falha ao carregar DX7", "Failed to load DX7", "Error al cargar DX7" },
    { "Falha ao importar DX7", "Failed to import DX7", "Error al importar DX7" },
    { "Limpar posição", "Clear slot", "Limpiar posición" },
    { "Excluir DX7", "Delete DX7", "Eliminar DX7" },
    { "Excluir SF2", "Delete SF2", "Eliminar SF2" },
    { "Falha ao excluir DX7", "Failed to delete DX7", "Error al eliminar DX7" },
    { "Falha ao Salvar Preset", "Failed to Save Preset", "Error al guardar el preset" },
    { "Falha ao Importar Performance", "Failed to Import Performance", "Error al importar la performance" },
    { "Falha ao Abrir Performance", "Failed to Open Performance", "Error al abrir la performance" },
    { "Live Set não atualizado", "Live Set not updated", "Live Set no actualizado" },
    { "Editor indisponível", "Editor unavailable", "Editor no disponible" },
    { "Drum pad", "Drum pad", "Drum pad" },
    { "Excluir programacao", "Delete performance", "Eliminar performance" },
    { "Carregar programação", "Load performance", "Cargar performance" },
    { "Nova programação", "New performance", "Nueva performance" },
    { "Substituir performance?", "Replace performance?", "¿Reemplazar la performance?" },
    { "Excluir", "Delete", "Eliminar" }, { "Cancelar", "Cancel", "Cancelar" },
    { "Começar uma programação vazia? As alterações não salvas e as layers atuais serão removidas desta sessão. Os arquivos salvos e os bancos do Live Set não serão apagados.",
      "Start a blank performance? Unsaved changes and the current layers will be removed from this session. Saved files and Live Set banks will not be deleted.",
      "¿Empezar una performance vacía? Los cambios sin guardar y las capas actuales se quitarán de esta sesión. Los archivos guardados y los bancos del Live Set no se eliminarán." },
    { "Já existe uma performance com esse nome na biblioteca deste computador. Deseja substituí-la?",
      "A performance with this name already exists in this computer's library. Replace it?",
      "Ya existe una performance con este nombre en la biblioteca de este equipo. ¿Quieres reemplazarla?" },
    { "Selecione uma programação salva na lista.", "Select a saved performance from the list.", "Selecciona una performance guardada de la lista." },
    { "Este instrumento virtual não possui uma janela de edição.",
      "This virtual instrument does not provide an editor window.",
      "Este instrumento virtual no tiene una ventana de edición." },
    { "INICIAL", "INITIAL", "INICIAL" },
    { "HIGH PASS: OFF", "HIGH PASS: OFF", "HIGH PASS: OFF" },
    { "LOW PASS: OFF", "LOW PASS: OFF", "LOW PASS: OFF" },
    { "EQ DA LAYER", "LAYER EQ", "EQ DE LA CAPA" },
    { "LOW FREQ Hz", "LOW FREQ Hz", "FREC. BAJA Hz" },
    { "LOW GAIN dB", "LOW GAIN dB", "GANANCIA BAJA dB" },
    { "LOW Q", "LOW Q", "Q BAJA" },
    { "MID FREQ Hz", "MID FREQ Hz", "FREC. MEDIA Hz" },
    { "MID GAIN dB", "MID GAIN dB", "GANANCIA MEDIA dB" },
    { "MID Q", "MID Q", "Q MEDIA" },
    { "HIGH FREQ Hz", "HIGH FREQ Hz", "FREC. ALTA Hz" },
    { "HIGH GAIN dB", "HIGH GAIN dB", "GANANCIA ALTA dB" },
    { "HIGH Q", "HIGH Q", "Q ALTA" },
    { "LOW CUT Hz", "LOW CUT Hz", "CORTE BAJO Hz" },
    { "HIGH CUT Hz", "HIGH CUT Hz", "CORTE ALTO Hz" },
    { "EQ LOW dB", "EQ LOW dB", "EQ BAJO dB" },
    { "EQ MID dB", "EQ MID dB", "EQ MEDIO dB" },
    { "EQ HIGH dB", "EQ HIGH dB", "EQ ALTO dB" },
    { "THRESHOLD dB", "THRESHOLD dB", "UMBRAL dB" },
    { "MAKEUP dB", "MAKEUP dB", "GANANCIA COMP. dB" },
    { "TAMANHO", "SIZE", "TAMAÑO" }, { "TEMPO", "SIZE", "TAMAÑO" },
    { "DIFUSAO", "DAMPING", "DIFUSIÓN" }, { "LARGURA", "WIDTH", "ANCHO" },
    { "Piano Intimo", "Intimate Piano", "Piano Íntimo" },
    { "Sala Clara", "Bright Room", "Sala Brillante" },
    { "Worship Hall", "Worship Hall", "Sala Worship" },
    { "Ambient Grande", "Large Ambient", "Ambiente Amplio" },
    { "Piano Natural", "Natural Piano", "Piano Natural" },
    { "Piano Presenca", "Piano Presence", "Presencia de Piano" },
    { "Worship Suave", "Soft Worship", "Worship Suave" },
    { "Worship Sustentado", "Sustained Worship", "Worship Sostenido" },
    { "INPUT dB", "INPUT dB", "ENTRADA dB" },
    { "OUTPUT dB", "OUTPUT dB", "SALIDA dB" },
    { "Equalizador parametrico de tres bandas: frequencia e ganho independentes.",
      "Three-band parametric equalizer: independent frequency and gain controls.",
      "Ecualizador paramétrico de tres bandas: frecuencia y ganancia independientes." },
    { "REVERB DA LAYER", "LAYER REVERB", "REVERB DE LA CAPA" },
    { "O knob REVERB controla a quantidade. Ajuste o carater da sala abaixo.",
      "The REVERB knob controls the amount. Adjust the room character below.",
      "El knob REVERB controla la cantidad. Ajusta el carácter de la sala abajo." },
    { "COMPRESSOR DA LAYER", "LAYER COMPRESSOR", "COMPRESOR DE LA CAPA" },
    { "O knob COMP controla a mistura. Ajuste a dinamica abaixo.",
      "The COMP knob controls the mix. Adjust the dynamics below.",
      "El knob COMP controla la mezcla. Ajusta la dinámica abajo." },
    { "CHORUS DA LAYER DX7", "DX7 LAYER CHORUS", "CHORUS DE LA CAPA DX7" },
    { "Ajuste o chorus em tempo real.", "Adjust chorus in real time.", "Ajusta el chorus en tiempo real." },
    { "Ajuste os controles desta layer sem expandir o canal.",
      "Adjust this layer's controls without expanding the channel.",
      "Ajusta los controles de esta capa sin expandir el canal." },
    { "Protecao da saida master. OUTPUT define o teto em dBFS.",
      "Protects the master output. OUTPUT sets the ceiling in dBFS.",
      "Protege la salida master. OUTPUT define el límite en dBFS." },
    { "EQ MASTER", "MASTER EQ", "EQ MASTER" },
    { "EQ de cinco estagios: corte baixo, tres bandas e corte alto.",
      "Five-stage EQ: low cut, three bands and high cut.",
      "EQ de cinco etapas: corte bajo, tres bandas y corte alto." },
    { "LIMITER MASTER", "MASTER LIMITER", "LIMITER MASTER" },
    { "Selecione o preset desta camada.", "Select this layer's preset.", "Selecciona el preset de esta capa." },
    { "Selecione o banco e o timbre desta camada.", "Select this layer's bank and patch.", "Selecciona el banco y el timbre de esta capa." },
    { "Exportar somente a configuração desta layer em um arquivo portátil", "Export only this layer's settings to a portable file", "Exportar solo la configuración de esta capa a un archivo portátil" },
    { "Importar uma configuração sem alterar as outras layers", "Import settings without changing other layers", "Importar una configuración sin cambiar las otras capas" },
    { "Exporta a programação completa para um arquivo portátil.", "Exports the complete performance to a portable file.", "Exporta la performance completa a un archivo portátil." },
    { "Clique novamente para cancelar; clique com o botão direito para excluir o mapeamento.", "Click again to cancel; right-click to delete the mapping.", "Haz clic de nuevo para cancelar; clic derecho para eliminar el mapeo." },
    { "Clique novamente para cancelar; botão direito para excluir o mapeamento do STOP.", "Click again to cancel; right-click to delete the STOP mapping.", "Haz clic de nuevo para cancelar; clic derecho para eliminar el mapeo de STOP." },
    { "Aprender um CC para alternar o mute desta layer", "Learn a CC to toggle this layer's mute", "Aprender un CC para alternar el mute de esta capa" },
    { "Aprender um CC de botão para alternar esta layer entre ativa e muda", "Learn a button CC to toggle this layer on and off", "Aprender un CC de botón para activar o silenciar esta capa" },
    { "Ativar ou silenciar esta layer; o volume do fader permanece salvo", "Enable or mute this layer; the fader volume is preserved", "Activar o silenciar esta capa; el volumen del fader se conserva" },
    { "Mostrar ou ocultar os controles desta layer", "Show or hide this layer's controls", "Mostrar u ocultar los controles de esta capa" },
    { "Excluir esta layer", "Delete this layer", "Eliminar esta capa" },
    { "Arraste o nome para mudar a ordem das layers", "Drag the name to reorder the layers", "Arrastra el nombre para cambiar el orden de las capas" },
    { "Escolher manualmente um instrumento VST3/AU", "Choose a VST3/AU instrument manually", "Elegir manualmente un instrumento VST3/AU" },
    { "Abrir a janela de configuração do instrumento virtual", "Open the virtual instrument settings window", "Abrir la ventana de configuración del instrumento virtual" },
    { "Importar banco ou voz DX7 em formato SysEx (.syx)", "Import a DX7 bank or patch in SysEx (.syx) format", "Importar un banco o timbre DX7 en formato SysEx (.syx)" },
    { "Excluir o banco DX7 selecionado da biblioteca", "Delete the selected DX7 bank from the library", "Eliminar el banco DX7 seleccionado de la biblioteca" },
    { "Excluir o SF2 selecionado da biblioteca", "Delete the selected SF2 from the library", "Eliminar el SF2 seleccionado de la biblioteca" },
    { "Clique para tocar este pad", "Click to play this pad", "Haz clic para tocar este pad" },
    { "Carregar MP3/WAV neste pad", "Load an MP3/WAV into this pad", "Cargar un MP3/WAV en este pad" },
    { "Clique para aprender; clique novamente para cancelar; botão direito para excluir o mapeamento.", "Click to learn; click again to cancel; right-click to delete the mapping.", "Haz clic para aprender; haz clic de nuevo para cancelar; clic derecho para eliminar el mapeo." },
    { "Volume da layer de drum pads", "Drum pads layer volume", "Volumen de la capa Drum Pads" },
    { "Volume individual deste pad", "Individual volume for this pad", "Volumen individual de este pad" },
    { "Ajustar com precisão a intensidade do efeito", "Fine-tune the effect amount", "Ajustar con precisión la intensidad del efecto" },
    { "Ajustar chorus da layer DX7", "Adjust the DX7 layer chorus", "Ajustar el chorus de la capa DX7" },
    { "Abrir o equalizador paramétrico da saída master", "Open the master output parametric equalizer", "Abrir el ecualizador paramétrico de la salida master" },
    { "Abrir limiter da saida master", "Open the master output limiter", "Abrir el limiter de la salida master" },
    { "Aprender CC e canal do volume master. Clique novamente para cancelar; Shift+clique apaga o mapeamento. CC64 reservado ao sustain.",
      "Learn the master volume CC and channel. Click again to cancel; Shift-click clears the mapping. CC64 is reserved for sustain.",
      "Aprender CC y canal del volumen master. Haz clic de nuevo para cancelar; Shift+clic borra el mapeo. CC64 reservado para sustain." },
    { "E-mail", "Email", "Correo electrónico" }, { "Senha", "Password", "Contraseña" },
    { "Informe e-mail e senha.", "Enter your email and password.", "Ingresa tu correo y contraseña." },
    { "Conectando ao servidor de licença...", "Connecting to the license server...", "Conectando con el servidor de licencias..." },
    { "Validando a licença deste computador...", "Validating this computer's license...", "Validando la licencia de este equipo..." },
    { "Não foi possível gravar", "Recording failed", "No se pudo grabar" },
    { "Falha ao carregar SF2", "Failed to load SF2", "Error al cargar SF2" },
    { "Falha ao importar SF2", "Failed to import SF2", "Error al importar SF2" },
    { "Falha ao excluir SF2", "Failed to delete SF2", "Error al eliminar SF2" },
    { "Falha ao excluir programacao", "Failed to delete performance", "Error al eliminar la performance" },
    { "Escolha um SoundFont", "Choose a SoundFont", "Elige un SoundFont" },
    { "Limpar posição", "Clear slot", "Limpiar posición" },
    { "EXPORTADO:", "EXPORTED:", "EXPORTADO:" },
    { "MOVA O CC", "MOVE A CC", "MUEVE UN CC" },
    { "MOVA UM CC", "MOVE A CC", "MUEVE UN CC" },
    { "Arraste os pontos para ajustar frequencia e ganho", "Drag the points to adjust frequency and gain", "Arrastra los puntos para ajustar frecuencia y ganancia" },
    { "ANALISADOR MASTER", "MASTER ANALYZER", "ANALIZADOR MASTER" },
    { "COMPRESSOR DA LAYER", "LAYER COMPRESSOR", "COMPRESOR DE LA CAPA" },
    { "CURVA", "CURVE", "CURVA" },
    { "OSCILLOSCOPE", "OSCILLOSCOPE", "OSCILOSCOPIO" },
    { "TODOS OS CONTROLADORES", "ALL CONTROLLERS", "TODOS LOS CONTROLADORES" },
    { "Exportar Preset Classic Player", "Export Classic Player Preset", "Exportar preset de Classic Player" },
    { "Importar SoundFont", "Import SoundFont", "Importar SoundFont" },
    { "Escolha um arquivo DX7 SysEx", "Choose a DX7 SysEx file", "Elige un archivo DX7 SysEx" },
    { "Carregar áudio do pad", "Load pad audio", "Cargar audio del pad" },
    { "Exportar preset da layer", "Export layer preset", "Exportar preset de la capa" },
    { "Importar preset da layer", "Import layer preset", "Importar preset de la capa" },
    { "Escolha um instrumento virtual", "Choose a virtual instrument", "Elige un instrumento virtual" },
    { "Exportar performance Classic Player", "Export Classic Player performance", "Exportar performance de Classic Player" },
    { "Importar performance Classic Player", "Import Classic Player performance", "Importar performance de Classic Player" },
    { "Falha ao exportar", "Export failed", "Error al exportar" },
    { "Falha ao exportar preset", "Failed to export preset", "Error al exportar el preset" },
    { "Falha ao importar preset", "Failed to import preset", "Error al importar el preset" },
    { "Preset da layer exportado", "Layer preset exported", "Preset de la capa exportado" },
    { "Arquivo exportado para:", "File exported to:", "Archivo exportado a:" },
    { "O carregamento de VST/AU está disponível apenas no aplicativo standalone.",
      "VST/AU loading is available only in the standalone app.",
      "La carga de VST/AU solo está disponible en la aplicación independiente." },
    { "Instrumento externo", "External instrument", "Instrumento externo" },
    { "Falha ao carregar instrumento", "Failed to load instrument", "Error al cargar el instrumento" },
    { "Falha ao carregar DX7", "Failed to load DX7", "Error al cargar DX7" },
    { "Falha ao importar DX7", "Failed to import DX7", "Error al importar DX7" },
    { "Falha ao exportar DX7", "Failed to export DX7", "Error al exportar DX7" },
    { "Exportação concluída", "Export complete", "Exportación completada" },
    { "Cópia portátil salva em:", "Portable copy saved to:", "Copia portátil guardada en:" },
    { "Falha ao Exportar Performance", "Failed to Export Performance", "Error al exportar la performance" },
    { "Falha ao importar performance", "Failed to import performance", "Error al importar la performance" },
    { "Falha ao carregar performance", "Failed to load performance", "Error al cargar la performance" },
    { "Falha ao substituir performance", "Failed to replace performance", "Error al reemplazar la performance" },
    { "Preset da layer importado", "Layer preset imported", "Preset de la capa importado" },
    { "Importação concluída", "Import complete", "Importación completada" },
    { "Digite o nome da performance", "Enter the performance name", "Escribe el nombre de la performance" },
    { "Não foi possível excluir", "Could not delete", "No se pudo eliminar" },
    { "INFORME O NOME", "ENTER A NAME", "INGRESA UN NOMBRE" },
    { "NOVO", "NEW", "NUEVO" },
    { "Escolha um SoundFont", "Choose a SoundFont", "Elige un SoundFont" },
    { "MIDI Learn do volume", "Volume MIDI Learn", "MIDI Learn del volumen" },
    { "Selecione o arquivo", "Select a file", "Selecciona un archivo" },
    { "Exportar", "Export", "Exportar" },
    { "Importar", "Import", "Importar" },
    { "Escolha um SoundFont", "Choose a SoundFont", "Elige un SoundFont" },
    { "Protecao transparente", "Transparent protection", "Protección transparente" },
    { "Piano suave", "Soft piano", "Piano suave" },
    { "Piano worship", "Worship piano", "Piano worship" },
    { "Piano presente", "Present piano", "Piano presente" },
    { "Master forte", "Powerful master", "Master potente" },
    { "CLASSIC KEYS SF2 WORKSTATION", "CLASSIC KEYS SF2 WORKSTATION", "ESTACIÓN DE TRABAJO SF2 CLASSIC KEYS" },
    { "Personalizado", "Custom", "Personalizado" },
    { "LIMPAR", "CLEAR", "LIMPIAR" },
    { "Sem CC", "No CC", "Sin CC" },
    { "Level", "Level", "Nivel" },
    { "Leslie / Mod wheel", "Leslie / Mod wheel", "Leslie / rueda de modulación" },
    { "Mod wheel (CC1): 0-63 lento, 64-127 rapido",
      "Mod wheel (CC1): 0-63 slow, 64-127 fast",
      "Rueda Mod (CC1): 0-63 lento, 64-127 rápido" },
    { "KEY CLICK", "KEY CLICK", "CLIC DE TECLA" },
    { "LEAKAGE", "LEAKAGE", "FUGA" },
    { "PINK NOISE", "PINK NOISE", "RUIDO ROSA" },
    { "OSCILLATOR - FILTER - MODULATION", "OSCILLATOR - FILTER - MODULATION", "OSCILADOR - FILTRO - MODULACIÓN" },
    { "BROWSER 12 dB - SOM APROVADO", "BROWSER 12 dB - APPROVED SOUND", "BROWSER 12 dB - SONIDO APROBADO" },
    { "PRESET ANALOG - BROWSER 12 dB", "ANALOG PRESET - BROWSER 12 dB", "PRESET ANALÓGICO - BROWSER 12 dB" },
    { "PRESET ANALOG - LEGADO", "ANALOG PRESET - LEGACY", "PRESET ANALÓGICO - LEGADO" },
    { "Drawbars, Leslie e MIDI. Salve a programação para guardar o timbre.",
      "Drawbars, Leslie and MIDI. Save the performance to store the sound.",
      "Drawbars, Leslie y MIDI. Guarda la performance para conservar el sonido." },
    { "OSC 1 LEVEL", "OSC 1 LEVEL", "NIVEL OSC 1" },
    { "OSC 2 LEVEL", "OSC 2 LEVEL", "NIVEL OSC 2" },
    { "OSC 3 LEVEL", "OSC 3 LEVEL", "NIVEL OSC 3" },
    { "OSC 2 TUNE", "OSC 2 TUNE", "AFINACIÓN OSC 2" },
    { "OSC 3 TUNE", "OSC 3 TUNE", "AFINACIÓN OSC 3" },
    { "NOISE", "NOISE", "RUIDO" },
    { "EMPHASIS", "EMPHASIS", "ÉNFASIS" },
    { "FILTER CONTOUR", "FILTER CONTOUR", "ENVOLVENTE DE FILTRO" },
    { "ATTACK ms", "ATTACK ms", "ATAQUE ms" },
    { "DECAY ms", "DECAY ms", "CAÍDA ms" },
    { "RELEASE ms", "RELEASE ms", "LIBERACIÓN ms" },
    { "LFO RATE Hz", "LFO RATE Hz", "VELOCIDAD LFO Hz" },
    { "LFO PITCH", "LFO PITCH", "TONO LFO" },
    { "LFO FILTER", "LFO FILTER", "FILTRO LFO" },
    { "KEY TRACK", "KEY TRACK", "SEGUIMIENTO DE TECLA" },
    { "MOD WHEEL", "MOD WHEEL", "RUEDA DE MODULACIÓN" },
    { "OSC 1 ON", "OSC 1 ON", "OSC 1 ACTIVADO" },
    { "OSC 2 ON", "OSC 2 ON", "OSC 2 ACTIVADO" },
    { "OSC 3 ON", "OSC 3 ON", "OSC 3 ACTIVADO" },
    { "Sem SoundFont", "No SoundFont", "Sin SoundFont" },
    { "Sem VST", "No VST", "Sin VST" },
    { "Sem DX7", "No DX7", "Sin DX7" },
    { "ABRIR CLASSIC KEYS ANALOG", "OPEN CLASSIC KEYS ANALOG", "ABRIR CLASSIC KEYS ANALOG" },
    { "ABRIR HAMMOND", "OPEN HAMMOND", "ABRIR HAMMOND" },
    { "EQ PARAMETRICO MASTER", "MASTER PARAMETRIC EQ", "EQ PARAMÉTRICO MASTER" },
    { "EQ PARAMETRICO DA LAYER", "LAYER PARAMETRIC EQ", "EQ PARAMÉTRICO DE LA CAPA" },
    { "MOVA PAD", "MOVE PAD", "MUEVE EL PAD" },
    { "Volume master", "Master volume", "Volumen master" },
    { "Volume master — use CONFIGURACOES para MIDI Learn",
      "Master volume — use SETTINGS for MIDI Learn",
      "Volumen master — usa CONFIGURACIÓN para MIDI Learn" },
};

std::atomic<int> activeUiLanguage { 0 };

juce::String localizedUiText(const juce::String& value, int language)
{
    const auto localeIndex = juce::jlimit(0, 2, language);
    for (const auto& entry : uiTranslations)
    {
        const auto pt = juce::String::fromUTF8(entry.portuguese);
        const auto en = juce::String::fromUTF8(entry.english);
        const auto es = juce::String::fromUTF8(entry.spanish);
        if (value == pt) return localeIndex == 0 ? pt : localeIndex == 1 ? en : es;
        if (value == en || value == es)
            return localeIndex == 0 ? pt : localeIndex == 1 ? en : es;
    }

    // Keep generated labels such as LAYER 3 and BANK 2 translatable without
    // treating performance names or sample paths as UI strings.
    const std::array<std::array<const char*, 3>, 4> prefixes {{
        {{ "CAMADA ", "LAYER ", "CAPA " }},
        {{ "BANCO ", "BANK ", "BANCO " }},
        {{ "CAMADAS", "LAYERS", "CAPAS" }},
        {{ "Drawbar ", "Drawbar ", "Tirador " }}
    }};
    for (const auto& prefix : prefixes)
    {
        for (int source = 0; source < 3; ++source)
        {
            const auto sourcePrefix = juce::String::fromUTF8(prefix[(size_t) source]);
            if (value.startsWith(sourcePrefix))
                return juce::String::fromUTF8(prefix[(size_t) localeIndex])
                     + value.substring(sourcePrefix.length());
        }
    }
    const std::array<std::array<const char*, 3>, 3> layerSuffixes {{
        {{ " CAMADA", " LAYER", " CAPA" }},
        {{ " CAMADAS", " LAYERS", " CAPAS" }},
        {{ " CAMADA(S)", " LAYER(S)", " CAPA(S)" }}
    }};
    for (const auto& suffix : layerSuffixes)
        for (int source = 0; source < 3; ++source)
        {
            const auto sourceSuffix = juce::String::fromUTF8(suffix[(size_t) source]);
            if (value.endsWith(sourceSuffix))
                return value.dropLastCharacters(sourceSuffix.length())
                     + juce::String::fromUTF8(suffix[(size_t) localeIndex]);
        }
    const std::array<std::array<const char*, 3>, 10> dynamicPrefixes {{
        {{ "EXPORTADO: ", "EXPORTED: ", "EXPORTADO: " }},
        {{ "Exportar preset de ", "Export effect preset: ", "Exportar preset de " }},
        {{ "GRAVANDO ", "RECORDING ", "GRABANDO " }},
        {{ "VOLUME / CC ", "VOLUME / CC ", "VOLUMEN / CC " }},
        {{ "VOLUME: CC ", "VOLUME: CC ", "VOLUMEN: CC " }},
        {{ "MUTE: CC ", "MUTE: CC ", "MUTE: CC " }},
        {{ "STOP: CC ", "STOP: CC ", "DETENER: CC " }},
        {{ "Carregar ", "Load ", "Cargar " }},
        {{ "HIGH PASS: ", "HIGH PASS: ", "PASA ALTOS: " }},
        {{ "LOW PASS: ", "LOW PASS: ", "PASA BAJOS: " }}
    }};
    for (const auto& prefix : dynamicPrefixes)
        for (int source = 0; source < 3; ++source)
        {
            const auto sourcePrefix = juce::String::fromUTF8(prefix[(size_t) source]);
            if (value.startsWith(sourcePrefix))
                return juce::String::fromUTF8(prefix[(size_t) localeIndex])
                     + value.substring(sourcePrefix.length());
        }
    const std::array<std::array<const char*, 3>, 2> pathPrefixes {{
        {{ "Cópia portátil salva em:\n", "Portable copy saved to:\n", "Copia portátil guardada en:\n" }},
        {{ "Arquivo exportado para:\n", "File exported to:\n", "Archivo exportado a:\n" }}
    }};
    for (const auto& prefix : pathPrefixes)
        for (int source = 0; source < 3; ++source)
        {
            const auto sourcePrefix = juce::String::fromUTF8(prefix[(size_t) source]);
            if (value.startsWith(sourcePrefix))
                return juce::String::fromUTF8(prefix[(size_t) localeIndex])
                     + value.substring(sourcePrefix.length());
        }
    const std::array<std::array<const char*, 3>, 2> deleteMessagePrefixes {{
        {{ "Excluir '", "Delete '", "¿Eliminar '" }},
        {{ "' da biblioteca?", "' from the library?", "' de la biblioteca?" }}
    }};
    for (int source = 0; source < 3; ++source)
    {
        const auto prefix = juce::String::fromUTF8(deleteMessagePrefixes[0][(size_t) source]);
        const auto suffix = juce::String::fromUTF8(deleteMessagePrefixes[1][(size_t) source]);
        if (value.startsWith(prefix) && value.endsWith(suffix)
            && value.length() >= prefix.length() + suffix.length())
        {
            const auto filename = value.substring(prefix.length(), value.length() - suffix.length());
            const auto targetPrefix = juce::String::fromUTF8(deleteMessagePrefixes[0][(size_t) localeIndex]);
            const auto targetSuffix = juce::String::fromUTF8(deleteMessagePrefixes[1][(size_t) localeIndex]);
            return targetPrefix + filename + targetSuffix;
        }
    }
    const std::array<std::array<const char*, 3>, 1> liveSetErrorPrefixes {{
        {{ "A performance foi salva, mas não foi possível atualizar a posição ativa do Live Set: ",
           "The performance was saved, but the active Live Set slot could not be updated: ",
           "La performance se guardó, pero no se pudo actualizar la posición activa del Live Set: " }}
    }};
    for (const auto& prefix : liveSetErrorPrefixes)
        for (int source = 0; source < 3; ++source)
        {
            const auto sourcePrefix = juce::String::fromUTF8(prefix[(size_t) source]);
            if (value.startsWith(sourcePrefix))
                return juce::String::fromUTF8(prefix[(size_t) localeIndex])
                + value.substring(sourcePrefix.length());
        }
    const std::array<std::array<const char*, 3>, 2> performanceCollision {{
        {{ "Já existe uma performance chamada \"", "A performance named \"", "Ya existe una performance llamada \"" }},
        {{ "\" na biblioteca do app. Deseja substituí-la?", "\" already exists in the app library. Replace it?", "\" en la biblioteca de la aplicación. ¿Quieres reemplazarla?" }}
    }};
    for (int source = 0; source < 3; ++source)
    {
        const auto prefix = juce::String::fromUTF8(performanceCollision[0][(size_t) source]);
        const auto suffix = juce::String::fromUTF8(performanceCollision[1][(size_t) source]);
        if (value.startsWith(prefix) && value.endsWith(suffix)
            && value.length() >= prefix.length() + suffix.length())
        {
            const auto name = value.substring(prefix.length(), value.length() - suffix.length());
            return juce::String::fromUTF8(performanceCollision[0][(size_t) localeIndex])
                 + name + juce::String::fromUTF8(performanceCollision[1][(size_t) localeIndex]);
        }
    }
    const std::array<std::array<const char*, 3>, 1> unitSuffixes {{
        {{ " OIT", " OCT", " OCT" }}
    }};
    for (const auto& suffix : unitSuffixes)
        for (int source = 0; source < 3; ++source)
        {
            const auto sourceSuffix = juce::String::fromUTF8(suffix[(size_t) source]);
            if (value.endsWith(sourceSuffix))
                return value.dropLastCharacters(sourceSuffix.length())
                     + juce::String::fromUTF8(suffix[(size_t) localeIndex]);
        }
    const std::array<std::array<const char*, 3>, 1> importPrefixes {{
        {{ "Importar preset de ", "Import effect preset: ", "Importar preset de " }}
    }};
    for (const auto& prefix : importPrefixes)
        for (int source = 0; source < 3; ++source)
        {
            const auto sourcePrefix = juce::String::fromUTF8(prefix[(size_t) source]);
            if (value.startsWith(sourcePrefix))
                return juce::String::fromUTF8(prefix[(size_t) localeIndex])
                     + value.substring(sourcePrefix.length());
        }
    return value;
}

// A UTF-8 literal passed through juce::String(const char*) may be decoded as
// single-byte text on some hosts. Decode literals before dictionary lookup.
juce::String localizedUiText(const char* value, int language)
{
    return localizedUiText(juce::String::fromUTF8(value), language);
}

juce::String selectedCategoryId(const juce::ComboBox& box)
{
    const auto categories = ClassicPlayerAudioProcessor::soundFontCategories();
    const auto index = box.getSelectedId() - 1;
    return juce::isPositiveAndBelow(index, categories.size()) ? categories[index] : categories[0];
}

juce::PropertiesFile::Options uiLanguageFileOptions()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "Classic Player";
    options.filenameSuffix = "ui-settings";
    options.osxLibrarySubFolder = "Application Support";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    return options;
}

int readUiLanguagePreference()
{
    juce::PropertiesFile settings(uiLanguageFileOptions());
    {
       #if JUCE_MAC
        const juce::File folder("/Library/Application Support/Classic Keys/Classic Player");
       #else
        const auto folder = juce::File::getSpecialLocation(juce::File::commonApplicationDataDirectory)
            .getChildFile("Classic Keys/Classic Player");
       #endif
        const auto marker = folder.getChildFile("installation-language.txt");
        const auto installed = marker.loadFileAsString().trim();
        if (installed == "brazilianportuguese" || installed == "english" || installed == "spanish")
        {
            const auto installation = juce::String(marker.getLastModificationTime().toMilliseconds()) + ":" + installed;
            if (settings.getValue("installationLanguageApplied") != installation)
            {
                settings.setValue("uiLanguage", installed == "english" ? 1 : installed == "spanish" ? 2 : 0);
                settings.setValue("installationLanguageApplied", installation);
                settings.saveIfNeeded();
            }
        }
    }
    return juce::jlimit(0, 2, settings.getIntValue("uiLanguage", 0));
}

void writeUiLanguagePreference(int language)
{
    juce::PropertiesFile settings(uiLanguageFileOptions());
    settings.setValue("uiLanguage", juce::jlimit(0, 2, language));
    settings.saveIfNeeded();
}

int readUiSkinPreference()
{
    juce::PropertiesFile settings(uiLanguageFileOptions());
    return juce::jlimit(0, (int) uiPalettes.size() - 1, settings.getIntValue("uiSkin", 0));
}

void writeUiSkinPreference(int skin)
{
    juce::PropertiesFile settings(uiLanguageFileOptions());
    settings.setValue("uiSkin", juce::jlimit(0, (int) uiPalettes.size() - 1, skin));
    settings.saveIfNeeded();
}

juce::Colour readLayerOutlinePreference(int layer, bool& customColour)
{
    juce::PropertiesFile settings(uiLanguageFileOptions());
    const auto savedColour = settings.getValue(layerOutlinePreferenceKey(layer));
    customColour = savedColour.isNotEmpty();
    return customColour ? juce::Colour::fromString(savedColour) : layerAccentColour(layer);
}

void writeLayerOutlinePreference(int layer, juce::Colour colour, bool useAutomatic)
{
    juce::PropertiesFile settings(uiLanguageFileOptions());
    const auto key = layerOutlinePreferenceKey(layer);
    if (useAutomatic)
        settings.removeValue(key);
    else
        settings.setValue(key, colour.toString());
    settings.saveIfNeeded();
}

void remapComponentColour(juce::Component& component, int colourId, const UiPalette& newPalette)
{
    const auto current = component.findColour(colourId, false);
    for (const auto& sourcePalette : uiPalettes)
    {
        const std::array<std::pair<juce::uint32, juce::uint32>, 8> replacements {{
            { sourcePalette.background, newPalette.background }, { sourcePalette.panel, newPalette.panel },
            { sourcePalette.panelLight, newPalette.panelLight }, { sourcePalette.line, newPalette.line },
            { sourcePalette.accent, newPalette.accent }, { sourcePalette.highlight, newPalette.highlight },
            { sourcePalette.text, newPalette.text }, { sourcePalette.mutedText, newPalette.mutedText }
        }};
        for (const auto& [sourceColour, targetColour] : replacements)
        {
            if (current == juce::Colour(sourceColour))
            {
                component.setColour(colourId, juce::Colour(targetColour));
                return;
            }
        }
    }
}

void applyUiSkinToComponentTree(juce::Component& component, const UiPalette& newPalette)
{
    // JUCE AlertWindows draw their shell from these three IDs rather than
    // ordinary child-component colours. Keep editor and effect dialogs in
    // sync with the active palette as well as their embedded controls.
    if (dynamic_cast<juce::AlertWindow*>(&component) != nullptr)
    {
        component.setColour(juce::AlertWindow::backgroundColourId,
            (newPalette.background == uiPalettes[5].background || newPalette.background == uiPalettes[6].background)
                                                             ? juce::Colours::transparentBlack
                                                             : juce::Colour(newPalette.panel));
        component.setColour(juce::AlertWindow::textColourId, juce::Colour(newPalette.text));
        component.setColour(juce::AlertWindow::outlineColourId, juce::Colour(newPalette.line));
    }

    if (dynamic_cast<juce::Label*>(&component) != nullptr)
    {
        for (const auto colourId : { juce::Label::textColourId, juce::Label::backgroundColourId,
                                     juce::Label::outlineColourId })
            remapComponentColour(component, colourId, newPalette);
    }
    if (dynamic_cast<juce::Button*>(&component) != nullptr)
    {
        const auto buttonOnColour = component.findColour(juce::TextButton::buttonOnColourId, false);
        const bool usesPaletteAccent = std::any_of(uiPalettes.begin(), uiPalettes.end(),
            [buttonOnColour](const UiPalette& palette)
            {
                return buttonOnColour == juce::Colour(palette.accent);
            });
        for (const auto colourId : { juce::TextButton::buttonColourId, juce::TextButton::buttonOnColourId,
                                     juce::TextButton::textColourOffId, juce::TextButton::textColourOnId })
            remapComponentColour(component, colourId, newPalette);
        if (usesPaletteAccent)
            component.setColour(juce::TextButton::textColourOnId,
                                juce::Colour(newPalette.accent == uiPalettes[2].accent
                                          || newPalette.accent == uiPalettes[3].accent
                                    ? newPalette.text : newPalette.background));
    }
    if (dynamic_cast<juce::ComboBox*>(&component) != nullptr)
    {
        for (const auto colourId : { juce::ComboBox::backgroundColourId, juce::ComboBox::textColourId,
                                     juce::ComboBox::outlineColourId, juce::ComboBox::arrowColourId,
                                     juce::ComboBox::focusedOutlineColourId })
            remapComponentColour(component, colourId, newPalette);
    }
    if (dynamic_cast<juce::Slider*>(&component) != nullptr)
    {
        for (const auto colourId : { juce::Slider::backgroundColourId, juce::Slider::thumbColourId,
                                     juce::Slider::trackColourId, juce::Slider::rotarySliderFillColourId,
                                     juce::Slider::rotarySliderOutlineColourId, juce::Slider::textBoxTextColourId,
                                     juce::Slider::textBoxBackgroundColourId, juce::Slider::textBoxOutlineColourId })
            remapComponentColour(component, colourId, newPalette);
    }
    if (dynamic_cast<juce::TextEditor*>(&component) != nullptr)
    {
        for (const auto colourId : { juce::TextEditor::backgroundColourId, juce::TextEditor::textColourId,
                                     juce::TextEditor::highlightColourId, juce::TextEditor::highlightedTextColourId,
                                     juce::TextEditor::outlineColourId, juce::TextEditor::focusedOutlineColourId,
                                     juce::TextEditor::shadowColourId })
            remapComponentColour(component, colourId, newPalette);
    }
    if (dynamic_cast<juce::ScrollBar*>(&component) != nullptr)
    {
        for (const auto colourId : { juce::ScrollBar::thumbColourId, juce::ScrollBar::trackColourId })
            remapComponentColour(component, colourId, newPalette);
    }
    if (dynamic_cast<juce::ColourSelector*>(&component) != nullptr)
    {
        for (const auto colourId : { juce::ColourSelector::backgroundColourId,
                                     juce::ColourSelector::labelTextColourId })
            remapComponentColour(component, colourId, newPalette);
    }

    for (int child = 0; child < component.getNumChildComponents(); ++child)
        if (auto* nested = component.getChildComponent(child))
            applyUiSkinToComponentTree(*nested, newPalette);
    component.repaint();
}

void applyUiLanguageToComponentTree(juce::Component& component, int language)
{
    const auto preserveText = static_cast<bool>(component.getProperties()["uiDataText"]);
    const auto translatedName = localizedUiText(component.getName(), language);
    if (translatedName != component.getName()) component.setName(translatedName);
    if (!static_cast<bool>(component.getProperties()["uiDataTooltip"]))
    {
        if (auto* tooltip = dynamic_cast<juce::SettableTooltipClient*>(&component))
        {
            const auto currentTooltip = tooltip->getTooltip();
            const auto translatedTooltip = localizedUiText(currentTooltip, language);
            if (translatedTooltip != currentTooltip) tooltip->setTooltip(translatedTooltip);
        }
    }

    if (auto* label = dynamic_cast<juce::Label*>(&component))
    {
        if (!preserveText)
        {
            const auto translated = localizedUiText(label->getText(), language);
            if (translated != label->getText()) label->setText(translated, juce::dontSendNotification);
        }
    }
    else if (auto* button = dynamic_cast<juce::TextButton*>(&component))
    {
        if (!preserveText)
        {
            const auto translated = localizedUiText(button->getButtonText(), language);
            if (translated != button->getButtonText()) button->setButtonText(translated);
        }
    }
    else if (auto* combo = dynamic_cast<juce::ComboBox*>(&component))
    {
        const auto selectionId = combo->getSelectedId();
        const auto typedText = combo->getText();
        const auto placeholder = combo->getTextWhenNothingSelected();
        const auto emptyMessage = combo->getTextWhenNoChoicesAvailable();
        const auto translatedPlaceholder = localizedUiText(placeholder, language);
        const auto translatedEmptyMessage = localizedUiText(emptyMessage, language);
        const auto preserveItemText = static_cast<bool>(component.getProperties()["uiDataItems"]);
        bool itemsChanged = false;
        std::vector<std::pair<int, juce::String>> items;
        items.reserve((size_t) combo->getNumItems());
        for (int index = 0; index < combo->getNumItems(); ++index)
        {
            const auto id = combo->getItemId(index);
            const auto oldText = combo->getItemText(index);
            const auto newText = preserveItemText ? oldText : localizedUiText(oldText, language);
            items.emplace_back(id, newText);
            itemsChanged = itemsChanged || oldText != newText;
        }

        if (itemsChanged)
        {
            combo->clear(juce::dontSendNotification);
            for (const auto& item : items)
            {
                if (item.first == 0) combo->addSeparator();
                else combo->addItem(item.second, item.first);
            }
            combo->setSelectedId(selectionId, juce::dontSendNotification);
        }
        if (translatedPlaceholder != placeholder)
            combo->setTextWhenNothingSelected(translatedPlaceholder);
        if (translatedEmptyMessage != emptyMessage)
            combo->setTextWhenNoChoicesAvailable(translatedEmptyMessage);
        if (!preserveItemText && combo->isTextEditable() && selectionId <= 0)
        {
            const auto translatedTypedText = localizedUiText(typedText, language);
            if (translatedTypedText != typedText)
                combo->setText(translatedTypedText, juce::dontSendNotification);
        }
    }
    else if (auto* editor = dynamic_cast<juce::TextEditor*>(&component))
    {
        const auto placeholder = editor->getTextToShowWhenEmpty();
        const auto translated = localizedUiText(placeholder, language);
        if (translated != placeholder) editor->setTextToShowWhenEmpty(translated, juce::Colours::grey);
    }

    for (int child = 0; child < component.getNumChildComponents(); ++child)
        if (auto* nested = component.getChildComponent(child))
            applyUiLanguageToComponentTree(*nested, language);
}

bool isClassicPlayerAlertTitle(const juce::String& title)
{
    static constexpr const char* ownedPrefixes[] {
        "Falha", "Não foi possível", "Instrumento externo", "Preset", "Exportação",
        "Importação", "Excluir", "Exclusão", "Confirmação", "LICENCA CLASSIC PLAYER",
        "Editor indisponível", "Drum pad", "Live Set não atualizado",
        "Carregar programação", "Nova programação", "Substituir performance?"
    };
    for (const auto* prefix : ownedPrefixes)
        if (title.startsWith(juce::String::fromUTF8(prefix))) return true;

    // After changing language, a Classic Player alert's title is translated
    // too. Recognize any translated dictionary entry so a second language
    // change can update that same window without claiming host-owned dialogs.
    for (const auto& entry : uiTranslations)
    {
        const auto pt = juce::String::fromUTF8(entry.portuguese);
        const auto en = juce::String::fromUTF8(entry.english);
        const auto es = juce::String::fromUTF8(entry.spanish);
        if (title == pt || title == en || title == es)
            if (pt != en || en != es) return true;
    }
    if (title == "Live Set") return true;
    return false;
}

juce::Colour drumPadColour(int pad)
{
    static const std::array<juce::Colour, 12> colours {
        juce::Colour(0xff168dff), juce::Colour(0xffb34cff),
        juce::Colour(0xffff526b), juce::Colour(0xffffa03a),
        juce::Colour(0xff19cf86), juce::Colour(0xffffd43a),
        juce::Colour(0xff17cedd), juce::Colour(0xfff05a9d),
        juce::Colour(0xff7f8cff), juce::Colour(0xffff7752),
        juce::Colour(0xff7dde55), juce::Colour(0xffaa75ed)
    };
    return colours[(size_t) juce::jlimit(0, 11, pad)];
}

void flatButton(juce::Button& button)
{
    button.setColour(juce::TextButton::buttonColourId, juce::Colour(panelLight));
    button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(teal));
    button.setColour(juce::TextButton::textColourOffId, juce::Colour(text));
    const auto onText = activeUiPalette == 2 || activeUiPalette == 3 ? text : background;
    button.setColour(juce::TextButton::textColourOnId, juce::Colour(onText));
}

juce::String midiNoteName(int note)
{
    static const char* names[] { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    return juce::String(names[note % 12]) + juce::String(note / 12 - 1);
}

juce::Image embeddedImage(const char* resourceName)
{
    int size = 0;
    if (const auto* data = ClassicPlayerAssets::getNamedResource(resourceName, size))
        return juce::ImageFileFormat::loadFrom(data, static_cast<size_t>(size));
    return {};
}

juce::String accountIdentityText()
{
    const auto name = LicenseVerifier::storedUserName().trim();
    const auto email = LicenseVerifier::storedUserEmail().trim();
    if (name.isEmpty()) return email;
    if (email.isEmpty() || name == email) return name;
    return name + "\n" + email;
}

class ClassicLookAndFeel : public juce::LookAndFeel_V4
{
public:
    int getAlertBoxWindowFlags() override
    {
        return juce::LookAndFeel_V4::getAlertBoxWindowFlags()
             | juce::ComponentPeer::windowHasTitleBar
             | juce::ComponentPeer::windowHasCloseButton;
    }

    ClassicLookAndFeel()
    {
        applyCurrentPalette();
    }

    void applyCurrentPalette()
    {
        setColour(juce::ComboBox::backgroundColourId, juce::Colour(panelLight));
        setColour(juce::ComboBox::outlineColourId, juce::Colour(paletteLine));
        setColour(juce::ComboBox::textColourId, juce::Colour(text));
        setColour(juce::ComboBox::arrowColourId, juce::Colour(teal));
        setColour(juce::PopupMenu::backgroundColourId, juce::Colour(panel));
        setColour(juce::PopupMenu::textColourId, juce::Colour(text));
        setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour(teal));
        setColour(juce::PopupMenu::highlightedTextColourId,
                  juce::Colour(activeUiPalette == 2 || activeUiPalette == 3 ? text : background));
        setColour(juce::Slider::textBoxTextColourId, juce::Colour(text));
        setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(background));
        setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(paletteLine));
    }

    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool over, bool down) override
    {
        if (!button.getName().startsWith("LIVE_SLOT_"))
            juce::LookAndFeel_V4::drawButtonText(g, button, over, down);
    }

    void drawAlertBox(juce::Graphics& g, juce::AlertWindow& alert,
                      const juce::Rectangle<int>& textArea, juce::TextLayout& layout) override
    {
        if (activeUiPalette == 5 || activeUiPalette == 6) paintBrushedSteel(g, alert.getLocalBounds().toFloat());
        juce::LookAndFeel_V4::drawAlertBox(g, alert, textArea, layout);
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override
    {
        if (button.getName().startsWith("LIVE_SLOT_"))
        {
            const auto bounds = button.getLocalBounds().toFloat().reduced(1.5f);
            const bool active = button.getProperties()["liveActive"];
            g.setColour(juce::Colour(background).brighter(shouldDrawButtonAsDown ? 0.18f : shouldDrawButtonAsHighlighted ? 0.10f : 0.0f));
            g.fillRoundedRectangle(bounds, 6.0f);
            g.setColour(juce::Colour(active ? teal : paletteLine));
            g.drawRoundedRectangle(bounds, 6.0f, active ? 2.0f : 1.0f);
            const float w = (float)button.getWidth(), h = (float)button.getHeight();
            g.setColour(juce::Colour(teal));
            g.setFont(juce::FontOptions(juce::jlimit(30.0f, 64.0f, h * 0.27f), juce::Font::bold));
            g.drawText(button.getProperties()["liveNumber"].toString(),
                       juce::Rectangle<float>(12, h * 0.06f, w - 24, h * 0.34f), juce::Justification::centred);
            g.setColour(juce::Colour(active ? yellow : teal).withAlpha(active ? 1.0f : 0.65f));
            g.fillRect(juce::Rectangle<float>(18, h * 0.43f, w - 36, active ? 3.0f : 1.0f));
            g.setColour(juce::Colour(text));
            g.setFont(juce::FontOptions(juce::jlimit(17.0f, 28.0f, w * 0.085f), juce::Font::bold));
            g.drawFittedText(button.getProperties()["liveTitle"].toString(),
                            14, (int)(h * 0.49f), (int)w - 28, (int)(h * 0.20f), juce::Justification::centred, 2);
            const auto summary = localizedUiText(button.getProperties()["liveSummary"].toString(),
                                                  activeUiLanguage.load());
            const auto volumes = button.getProperties()["liveVolumes"].toString();
            const auto movingLayers = static_cast<int>(button.getProperties()["liveMovingLayers"]);
            auto summaryBounds = juce::Rectangle<float>(14, volumes.isEmpty() ? h * 0.78f : h * 0.70f,
                                                        w - 28, volumes.isEmpty() ? h * 0.12f : h * 0.08f);
            if (summary.endsWith("CAMADA") || summary.endsWith("CAMADAS")
                || summary.endsWith("LAYER") || summary.endsWith("LAYERS")
                || summary.endsWith("CAPA") || summary.endsWith("CAPAS"))
            {
                const float iconX=w*0.5f-70.0f, iconY=summaryBounds.getCentreY()-7.0f;
                juce::Path layers;
                layers.startNewSubPath(iconX,iconY);
                layers.lineTo(iconX+10,iconY-5);
                layers.lineTo(iconX+20,iconY);
                layers.lineTo(iconX+10,iconY+5);
                layers.closeSubPath();
                for (float offset : { 5.0f,10.0f })
                {
                    layers.startNewSubPath(iconX,iconY+offset);
                    layers.lineTo(iconX+10,iconY+offset+5);
                    layers.lineTo(iconX+20,iconY+offset);
                }
                g.setColour(juce::Colour(teal));
                g.strokePath(layers,juce::PathStrokeType(1.3f));
                summaryBounds=juce::Rectangle<float>(w*0.5f-40.0f,summaryBounds.getY(),110.0f,summaryBounds.getHeight());
            }
            g.setColour(juce::Colour(mutedText));
            g.setFont(juce::FontOptions(juce::jlimit(11.0f, 15.0f, h * 0.065f)));
            g.drawText(summary, summaryBounds, juce::Justification::centred);
            if (volumes.isNotEmpty())
            {
                juce::StringArray tokens;
                tokens.addTokens(volumes, " ", "");
                std::vector<int> levels;
                for (const auto& token : tokens)
                    if (token.endsWithChar('%'))
                        levels.push_back(juce::jlimit(0, 100, token.dropLastCharacters(1).getIntValue()));
                const auto count = juce::jmin(8, static_cast<int>(levels.size()));
                if (count > 0)
                {
                    const auto bars = juce::Rectangle<float>(w * 0.18f, h * 0.84f,
                                                              w * 0.64f, h * 0.12f);
                    const auto cellWidth = bars.getWidth() / static_cast<float>(count);
                    for (int layer = 0; layer < count; ++layer)
                    {
                        const auto barWidth = juce::jmin(18.0f, cellWidth * 0.48f);
                        const auto x = bars.getX() + cellWidth * (layer + 0.5f) - barWidth * 0.5f;
                        const auto amount = static_cast<float>(levels[(size_t) layer]) / 100.0f;
                        g.setColour(juce::Colour(0xff253943));
                        g.fillRect(juce::Rectangle<float>(x, bars.getY(), barWidth, bars.getHeight()));
                        const auto isMoving = (movingLayers & (1 << layer)) != 0;
                        g.setColour(juce::Colour(isMoving ? yellow : teal));
                        g.fillRect(juce::Rectangle<float>(x, bars.getBottom() - bars.getHeight() * amount,
                                                          barWidth, bars.getHeight() * amount));
                        g.setColour(juce::Colour(text));
                        g.setFont(juce::FontOptions(juce::jlimit(8.0f, 11.0f, h * 0.045f), juce::Font::bold));
                        g.drawText("L" + juce::String(layer + 1),
                                   juce::Rectangle<float>(x - 5.0f, bars.getY() - 14.0f,
                                                          barWidth + 10.0f, 12.0f),
                                   juce::Justification::centred);
                    }
                }
            }
            if (button.hasKeyboardFocus(true))
            {
                g.setColour(juce::Colour(text).withAlpha(0.7f));
                g.drawRoundedRectangle(bounds.reduced(4),4,1);
            }
            return;
        }
        auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        auto colour = backgroundColour;
        if (shouldDrawButtonAsDown) colour = colour.darker(0.12f);
        else if (shouldDrawButtonAsHighlighted) colour = colour.brighter(0.08f);
        const auto isDrumPad = button.getName().startsWith("DRUM_PAD");
        if (isDrumPad)
        {
            // Give every pad its own colour while keeping the centre visibly
            // illuminated.  This makes the pad grid easier to scan and gives
            // a clear visual response when a pad is pressed.
            const auto centre = bounds.getCentre();
            const auto radiusPoint = juce::Point<float>(bounds.getRight(), bounds.getBottom());
            g.setGradientFill(juce::ColourGradient(colour.darker(0.32f), centre,
                                                   colour.brighter(0.22f), radiusPoint, true));
            g.fillRoundedRectangle(bounds, 8.0f);
        }
        else
        {
            g.setColour(colour);
            g.fillRoundedRectangle(bounds, 5.0f);
        }
        const auto outline = isDrumPad ? colour.brighter(0.35f)
                                       : juce::Colour(teal).interpolatedWith(backgroundColour, 0.35f);
        g.setColour(outline.withAlpha(0.28f));
        g.drawRoundedRectangle(bounds.reduced(1.0f), isDrumPad ? 8.0f : 5.0f, 3.0f);
        g.setColour(outline.withAlpha(0.92f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), isDrumPad ? 8.0f : 5.0f, 1.2f);
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float position, float startAngle, float endAngle,
                          juce::Slider&) override
    {
        const auto diameter = static_cast<float>(juce::jmin(width, height)) - 8.0f;
        const auto radius = diameter * 0.5f;
        const auto centre = juce::Point<float>(static_cast<float>(x) + static_cast<float>(width) * 0.5f,
                                               static_cast<float>(y) + static_cast<float>(height) * 0.5f);
        const auto bounds = juce::Rectangle<float>(diameter, diameter).withCentre(centre);
        const auto angle = startAngle + position * (endAngle - startAngle);

        g.setColour(juce::Colour(0xff080d11));
        g.fillEllipse(bounds.expanded(3.0f));
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff69747c), bounds.getX(), bounds.getY(),
                                               juce::Colour(0xff171e23), bounds.getRight(), bounds.getBottom(), false));
        g.fillEllipse(bounds);
        g.setColour(juce::Colour(0xff88939a));
        g.drawEllipse(bounds, 1.0f);

        juce::Path arc;
        arc.addCentredArc(centre.x, centre.y, radius + 3.0f, radius + 3.0f, 0.0f,
                          startAngle, angle, true);
        g.setColour(juce::Colour(teal));
        g.strokePath(arc, juce::PathStrokeType(2.6f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

        const auto pointer = juce::Point<float>(centre.x + std::sin(angle) * radius * 0.66f,
                                                centre.y - std::cos(angle) * radius * 0.66f);
        g.setColour(juce::Colour(teal));
        g.drawLine(centre.x, centre.y, pointer.x, pointer.y, 2.2f);
        g.fillEllipse(pointer.x - 2.0f, pointer.y - 2.0f, 4.0f, 4.0f);
    }

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearVertical)
        {
            LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos,
                                             minSliderPos, maxSliderPos, style, slider);
            return;
        }

        const auto centreX = static_cast<float>(x) + static_cast<float>(width) * 0.48f;
        g.setColour(juce::Colour(0xff080c0f));
        g.fillRoundedRectangle(centreX - 5.0f, static_cast<float>(y + 5), 10.0f,
                               static_cast<float>(height - 10), 2.0f);
        g.setColour(juce::Colour(0xff515b62));
        g.drawVerticalLine(static_cast<int>(centreX), static_cast<float>(y + 7),
                           static_cast<float>(y + height - 7));

        const auto compactThumb = static_cast<bool>(slider.getProperties()["compactLayerFader"]);
        const auto thumbWidth = compactThumb ? 23.0f : 30.0f;
        const auto thumbHeight = compactThumb ? 16.0f : 22.0f;
        const auto thumb = juce::Rectangle<float>(centreX - thumbWidth * 0.5f,
                                                  sliderPos - thumbHeight * 0.5f,
                                                  thumbWidth, thumbHeight);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xffeef1f2), thumb.getX(), thumb.getY(),
                                               juce::Colour(0xff929a9f), thumb.getRight(), thumb.getY(), false));
        g.fillRoundedRectangle(thumb, 1.5f);
        g.setColour(juce::Colour(0xff626b70));
        g.drawRoundedRectangle(thumb, 1.5f, 1.0f);
        g.setColour(juce::Colour(0xff555d62));
        const auto grooveExtent = compactThumb ? 4 : 6;
        for (int offset = -grooveExtent; offset <= grooveExtent; offset += 4)
            g.drawHorizontalLine(static_cast<int>(sliderPos) + offset, thumb.getX() + 3.0f,
                                 thumb.getRight() - 3.0f);
    }
};

ClassicLookAndFeel classicLookAndFeel;

class Dx7PatchLookAndFeel final : public ClassicLookAndFeel
{
public:
    juce::Font getPopupMenuFont() override
    {
        return juce::Font(juce::FontOptions(22.0f));
    }

    juce::PopupMenu::Options getOptionsForComboBoxPopupMenu(juce::ComboBox& box,
                                                            juce::Label& label) override
    {
        return ClassicLookAndFeel::getOptionsForComboBoxPopupMenu(box, label)
            .withStandardItemHeight(36)
            .withMaximumNumColumns(1);
    }

    juce::Component* getParentComponentForMenuOptions(
        const juce::PopupMenu::Options& options) override
    {
        if (auto* target = options.getTopLevelTargetComponent())
            return target->getTopLevelComponent();
        return ClassicLookAndFeel::getParentComponentForMenuOptions(options);
    }
};

Dx7PatchLookAndFeel dx7PatchLookAndFeel;

struct KnobEditorSpec
{
    const char* label;
    float value;
    float minimum;
    float maximum;
    float interval;
    int decimals;
};

// Let the OS provide the title bar and close control: traffic lights on macOS,
// standard caption buttons on Windows. Keep modal cleanup unchanged.
class LayerEditorWindow final : public juce::AlertWindow
{
public:
    LayerEditorWindow(const juce::String& title, const juce::String& message,
                      juce::MessageBoxIconType icon)
        : juce::AlertWindow(localizedUiText(title, activeUiLanguage.load()),
                            localizedUiText(message, activeUiLanguage.load()), icon),
          sourceTitle(title), sourceMessage(message)
    {
        setUsingNativeTitleBar(true);
        // New dialogs should start in the selected palette, including their
        // JUCE-drawn background and frame (not only the embedded controls).
        applyUiSkinToComponentTree(*this, uiPalettes[(size_t) activeUiPalette]);
    }

    void userTriedToCloseWindow() override { exitModalState(0); }

    void applyLanguage()
    {
        setName(localizedUiText(sourceTitle, activeUiLanguage.load()));
        setMessage(localizedUiText(sourceMessage, activeUiLanguage.load()));
        applyUiLanguageToComponentTree(*this, activeUiLanguage.load());
    }

    void visibilityChanged() override
    {
        if (isVisible()) applyLanguage();
    }

    void fitWithinApp(juce::Component* owner)
    {
        if (owner == nullptr) return;
        const auto* topLevel = owner->getTopLevelComponent();
        if (topLevel == nullptr) return;
        const auto appBounds = topLevel->getScreenBounds();
        if (appBounds.isEmpty()) return;
        const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(appBounds);
        const auto available = display != nullptr ? appBounds.getIntersection(display->userArea) : appBounds;
        const auto maxWidth = juce::jmax(320, available.getWidth() - 16);
        const auto maxHeight = juce::jmax(280, available.getHeight() - 16);
        const auto desiredWidth = getWidth();
        const auto desiredHeight = getHeight();
        // Editors are laid out to fit the 13-inch app window. Never replace
        // their controls with a scrollable viewport when space is tight.
        const auto width = juce::jmin(desiredWidth, maxWidth);
        const auto height = juce::jmin(desiredHeight, maxHeight);
        setSize(width, height);
        setTopLeftPosition(juce::jlimit(available.getX(), available.getRight() - width,
                                        appBounds.getCentreX() - width / 2),
                           juce::jlimit(available.getY(), available.getBottom() - height,
                                        appBounds.getCentreY() - height / 2));
        // AlertWindow may become visible before callers finish adding custom
        // controls. Translate again now, when the complete dialog tree exists.
        applyLanguage();
    }

    void fitCustomComponentsVertically()
    {
        const auto count = getNumCustomComponents();
        if (count < 2) return;

        const auto* first = getCustomComponent(0);
        if (first == nullptr) return;
        int contentHeight = 0;
        for (int i = 0; i < count; ++i)
            if (const auto* component = getCustomComponent(i))
                contentHeight += component->getHeight();

        // AlertWindow's default 10 px gaps can push the last row below a
        // window capped to the app height. Keep a bottom inset for the native
        // window frame while preserving every control's own height.
        const int availableForGaps = getHeight() - 10 - first->getY() - contentHeight;
        const int gap = juce::jlimit(2, 10, availableForGaps / (count - 1));
        int y = first->getY();
        for (int i = 0; i < count; ++i)
            if (auto* component = getCustomComponent(i))
            {
                component->setTopLeftPosition(component->getX(), y);
                y += component->getHeight() + gap;
            }
    }

    void resized() override
    {
        juce::AlertWindow::resized();
        // JUCE applies platform-dependent margins to AlertWindow content.
        // Re-centre every custom panel so macOS and Windows stay aligned.
        for (int i = 0; i < getNumCustomComponents(); ++i)
            if (auto* component = getCustomComponent(i))
            {
                auto bounds = component->getBounds();
                bounds.setX((getWidth() - bounds.getWidth()) / 2);
                component->setBounds(bounds);
            }
    }
private:
    juce::String sourceTitle, sourceMessage;
};

class KnobEditorPanel final : public juce::Component
{
public:
    KnobEditorPanel(std::initializer_list<KnobEditorSpec> specifications, int requestedColumns)
        : columns(juce::jmax(1, requestedColumns))
    {
        for (const auto& specification : specifications)
        {
            auto* label = labels.add(new juce::Label());
            label->setText(specification.label, juce::dontSendNotification);
            label->setJustificationType(juce::Justification::centred);
            label->setColour(juce::Label::textColourId, juce::Colour(text));
            label->setFont(juce::FontOptions(11.0f, juce::Font::bold));
            addAndMakeVisible(label);

            auto* knob = knobs.add(new juce::Slider());
            knob->setRange(specification.minimum, specification.maximum, specification.interval);
            knob->setValue(specification.value, juce::dontSendNotification);
            knob->setNumDecimalPlacesToDisplay(specification.decimals);
            knob->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            knob->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 20);
            knob->setLookAndFeel(&classicLookAndFeel);
            knob->setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(teal));
            addAndMakeVisible(knob);
        }

        const auto rows = juce::jmax(1, (knobs.size() + columns - 1) / columns);
        setSize(columns * 126, rows * 124);
    }

    void useAnalogHardwareLayout(bool shouldUse) { analogHardwareLayout = shouldUse; resized(); }
    void useCompactGrid(bool shouldUse) { compactGrid = shouldUse; resized(); }
    void useDenseGrid(bool shouldUse) { denseGrid = shouldUse; resized(); }

    float value(int item) const
    {
        return juce::isPositiveAndBelow(item, knobs.size()) ? (float) knobs[item]->getValue() : 0.0f;
    }

    void setValue(int item, float newValue)
    {
        if (juce::isPositiveAndBelow(item, knobs.size()))
            knobs[item]->setValue(newValue, juce::dontSendNotification);
    }

    void setOnValueChange(std::function<void()> callback)
    {
        onValueChange = std::move(callback);
        for (auto* knob : knobs)
            knob->onValueChange = [this] { if (onValueChange) onValueChange(); };
    }

    void bindParameter(int item, juce::AudioProcessorValueTreeState& state, const juce::String& id)
    {
        attachments.add(new juce::AudioProcessorValueTreeState::SliderAttachment(state, id, *knobs[item]));
    }

    void resized() override
    {
        if (denseGrid)
        {
            const auto cellWidth = getWidth() / columns;
            const auto rows = juce::jmax(1, (knobs.size() + columns - 1) / columns);
            const auto rowHeight = getHeight() / rows;
            for (int item = 0; item < knobs.size(); ++item)
            {
                auto cell = juce::Rectangle<int>((item % columns) * cellWidth,
                                                 (item / columns) * rowHeight,
                                                 cellWidth, rowHeight).reduced(3, 1);
                labels[item]->setBounds(cell.removeFromTop(14));
                knobs[item]->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 58, 17);
                knobs[item]->setBounds(cell);
            }
            return;
        }
        if (compactGrid)
        {
            const auto cellWidth = getWidth() / juce::jmax(1, columns);
            for (int item = 0; item < knobs.size(); ++item)
            {
                auto cell = juce::Rectangle<int>((item % columns) * cellWidth, 0,
                                                 cellWidth, getHeight()).reduced(2, 1);
                labels[item]->setBounds(cell.removeFromTop(13));
                knobs[item]->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 42, 16);
                knobs[item]->setBounds(cell.reduced(1, 0));
            }
            return;
        }

        if (analogHardwareLayout && knobs.size() == 19)
        {
            // Keep every label and knob inside a predictable three-row grid.
            // The old hand-written coordinates assumed a 1000px panel and
            // overlapped when the editor was resized by JUCE on smaller Macs.
            constexpr int gridColumns = 5;
            const auto cellWidth = getWidth() / gridColumns;
            // Compact four-row grid matching the reference editor.
            const auto rowHeight = juce::jmax(54, getHeight() / 4);
            for (int item = 0; item < knobs.size(); ++item)
            {
                const auto column = item % gridColumns;
                const auto row = item / gridColumns;
                auto cell = juce::Rectangle<int>(column * cellWidth, row * rowHeight,
                                                 cellWidth, rowHeight).reduced(4, 2);
                labels[item]->setBounds(cell.removeFromTop(17));
                knobs[item]->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 58, 18);
                knobs[item]->setBounds(cell.reduced(3, 1));
            }
            return;
        }

        const auto cellWidth = getWidth() / columns;
        for (int item = 0; item < knobs.size(); ++item)
        {
            const auto column = item % columns;
            const auto row = item / columns;
            auto cell = juce::Rectangle<int>(column * cellWidth, row * 124, cellWidth, 124).reduced(5, 2);
            labels[item]->setBounds(cell.removeFromTop(21));
            knobs[item]->setBounds(cell.reduced(2, 0));
        }
    }

private:
    int columns = 1;
    bool analogHardwareLayout = false;
    bool compactGrid = false;
    bool denseGrid = false;
    std::function<void()> onValueChange;
    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::Slider> knobs;
    // Disconnect listeners before their sliders are destroyed.
    juce::OwnedArray<juce::AudioProcessorValueTreeState::SliderAttachment> attachments;
};

// Keep response graphs and every numeric control visible together on short
// notebook displays instead of stacking them into a scrolling dialog.
class SideBySideEditorPanel final : public juce::Component
{
public:
    SideBySideEditorPanel(juce::Component* left, juce::Component* right,
                          int preferredWidth, int preferredHeight)
        : leftContent(left), rightContent(right)
    {
        owned.add(left); owned.add(right);
        addAndMakeVisible(left);
        addAndMakeVisible(right);
        setSize(preferredWidth, preferredHeight);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(3);
        const auto leftWidth = area.getWidth() / 2;
        leftContent->setBounds(area.removeFromLeft(leftWidth).reduced(3, 0));
        rightContent->setBounds(area.reduced(3, 0));
    }

private:
    juce::Component* leftContent;
    juce::Component* rightContent;
    juce::OwnedArray<juce::Component> owned;
};

static int editorContentWidth(const juce::Component* owner, int preferred)
{
    if (owner == nullptr || owner->getTopLevelComponent() == nullptr) return preferred;
    return juce::jmin(preferred, juce::jmax(620, owner->getTopLevelComponent()->getWidth() - 72));
}

class EffectPresetPanel final : public juce::Component
{
public:
    EffectPresetPanel(const juce::String& title, const juce::StringArray& names)
    {
        label.setText(title, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centredRight);
        label.setColour(juce::Label::textColourId, juce::Colour(text));
        label.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        addAndMakeVisible(label);

        for (int item = 0; item < names.size(); ++item)
            presets.addItem(names[item], item + 1);
        presets.setTextWhenNothingSelected("ESCOLHA UM PRESET");
        presets.setColour(juce::ComboBox::backgroundColourId, juce::Colour(panelLight));
        presets.setColour(juce::ComboBox::textColourId, juce::Colour(text));
        presets.setColour(juce::ComboBox::outlineColourId, juce::Colour(paletteLine));
        presets.onChange = [this]
        {
            if (onPresetSelected) onPresetSelected(presets.getSelectedItemIndex());
        };
        addAndMakeVisible(presets);
        setSize(620, 38);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4, 3);
        label.setBounds(area.removeFromLeft(120));
        presets.setBounds(area.removeFromLeft(330));
    }

    void setSelectedPreset(int index)
    {
        presets.setSelectedId(index + 1, juce::dontSendNotification);
    }

    std::function<void(int)> onPresetSelected;

private:
    juce::Label label;
    juce::ComboBox presets;
};

static constexpr std::array<std::array<float, 4>, 4> factoryReverbPresets {{
    {{ 32.0f, 55.0f, 70.0f, 14.0f }},
    {{ 45.0f, 75.0f, 85.0f, 18.0f }},
    {{ 72.0f, 68.0f, 100.0f, 28.0f }},
    {{ 92.0f, 82.0f, 100.0f, 42.0f }}
}};
static constexpr std::array<std::array<float, 6>, 4> factoryCompressorPresets {{
    {{ -9.0f,  2.5f, 17.0f, 238.0f, 1.0f, 45.0f }},
    {{ -18.0f, 3.5f,  7.0f, 180.0f, 4.0f, 55.0f }},
    {{ -14.0f, 2.0f, 25.0f, 260.0f, 2.0f, 50.0f }},
    {{ -22.0f, 4.0f, 35.0f, 360.0f, 4.5f, 65.0f }}
}};
static constexpr std::array<std::array<float, 3>, 5> factoryLimiterPresets {{
    {{ 0.0f, 80.0f, -0.3f }},
    {{ 1.0f, 180.0f, -1.0f }},
    {{ 2.0f, 250.0f, -1.0f }},
    {{ 3.0f, 120.0f, -0.7f }},
    {{ 5.0f, 80.0f, -0.5f }}
}};

class MasterLimiterMeters final : public juce::Component, private juce::Timer
{
public:
    explicit MasterLimiterMeters(ClassicPlayerAudioProcessor& p) : processor(p) { setSize(620, 190); startTimerHz(30); }
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff191f22));
        const std::array<juce::String, 3> names { "INPUT", "GR", "OUTPUT" };
        const std::array<float, 3> values {
            juce::Decibels::gainToDecibels(processor.limiterInputLevel(), -60.0f),
            processor.limiterGainReduction(),
            juce::Decibels::gainToDecibels(processor.limiterOutputLevel(), -60.0f)
        };
        for (int i = 0; i < 3; ++i)
        {
            const auto x = 115 + i * 150;
            g.setColour(juce::Colours::white);
            g.setFont(juce::FontOptions(12.0f));
            g.drawFittedText(names[(size_t)i], x - 30, 10, 95, 20, juce::Justification::centred, 1);
            const auto db = values[(size_t)i];
            g.drawFittedText(juce::String(db, 1) + " dB", x - 30, 30, 95, 20, juce::Justification::centred, 1);
            auto rail = juce::Rectangle<float>((float)x, 60.0f, 35.0f, 112.0f);
            g.setColour(juce::Colour(0xff0d1216)); g.fillRect(rail);
            g.setColour(juce::Colour(0xff7b8589)); g.drawRect(rail);
            const auto proportion = i == 1 ? juce::jlimit(0.0f, 1.0f, db / 24.0f)
                                           : juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
            g.setColour(i == 1 ? juce::Colour(0xffffd75c) : juce::Colour(teal));
            g.fillRect(rail.removeFromBottom(110.0f * proportion).reduced(2.0f, 0.0f));
        }
    }
private:
    void timerCallback() override { repaint(); }
    ClassicPlayerAudioProcessor& processor;
};

class LayerPresetFilePanel final : public juce::Component
{
public:
    LayerPresetFilePanel(std::function<void()> save, std::function<void()> load)
    {
        flatButton(saveButton);
        flatButton(loadButton);
        saveButton.setTooltip(juce::String::fromUTF8("Exportar somente a configuração desta layer em um arquivo portátil"));
        loadButton.setTooltip(juce::String::fromUTF8("Importar uma configuração sem alterar as outras layers"));
        saveButton.onClick = std::move(save);
        loadButton.onClick = std::move(load);
        addAndMakeVisible(saveButton);
        addAndMakeVisible(loadButton);
        setSize(430, 38);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(2);
        saveButton.setBounds(area.removeFromLeft(210).reduced(2, 0));
        loadButton.setBounds(area.removeFromLeft(210).reduced(2, 0));
    }

private:
    juce::TextButton saveButton { "EXPORTAR PRESET" };
    juce::TextButton loadButton { "IMPORTAR PRESET" };
};

class EffectPresetFilePanel final : public juce::Component
{
public:
    EffectPresetFilePanel(std::function<void()> save, std::function<void()> load)
    {
        flatButton(saveButton); flatButton(loadButton);
        saveButton.onClick = std::move(save); loadButton.onClick = std::move(load);
        addAndMakeVisible(saveButton); addAndMakeVisible(loadButton);
        setSize(430, 38);
    }
    void resized() override
    {
        auto area = getLocalBounds().reduced(2);
        saveButton.setBounds(area.removeFromLeft(210).reduced(2, 0));
        loadButton.setBounds(area.removeFromLeft(210).reduced(2, 0));
    }
private:
    juce::TextButton saveButton { "EXPORTAR PRESET" };
    juce::TextButton loadButton { "IMPORTAR PRESET" };
};

class LayerEffectButtons final : public juce::Component
{
public:
    LayerEffectButtons(std::function<void()> reverbCallback,
                       std::function<void()> compressorCallback,
                       std::function<void()> chorusCallback = {},
                       std::function<void()> eqCallback = {})
        : onReverb(std::move(reverbCallback)), onCompressor(std::move(compressorCallback)),
          onChorus(std::move(chorusCallback)), onEq(std::move(eqCallback))
    {
        reverb.setButtonText("EDITAR REVERB");
        compressor.setButtonText("EDITAR COMP");
        chorus.setButtonText("EDITAR CHORUS");
        eq.setButtonText("EDITAR EQ");
        for (auto* button : { &reverb, &compressor, &chorus, &eq })
        {
            flatButton(*button);
            addAndMakeVisible(*button);
        }
        reverb.onClick = [this] { if (onReverb) onReverb(); };
        compressor.onClick = [this] { if (onCompressor) onCompressor(); };
        chorus.onClick = [this] { if (onChorus) onChorus(); };
        eq.onClick = [this] { if (onEq) onEq(); };
        chorus.setVisible((bool) onChorus);
        eq.setVisible((bool) onEq);
        setSize(460, 38);
    }

    void resized() override
    {
        auto row = getLocalBounds();
        const bool hasChorus = static_cast<bool>(onChorus);
        const bool hasEq = static_cast<bool>(onEq);
        const auto columns = (hasChorus ? 1 : 0) + (hasEq ? 1 : 0) + 2;
        reverb.setBounds(row.removeFromLeft(row.getWidth() / columns).reduced(2, 1));
        compressor.setBounds(row.removeFromLeft(row.getWidth() / (columns - 1)).reduced(2, 1));
        if (hasChorus)
            chorus.setBounds(row.removeFromLeft(row.getWidth() / ((hasEq ? 1 : 0) + 1)).reduced(2, 1));
        if (hasEq)
            eq.setBounds(row.reduced(2, 1));
    }

private:
    std::function<void()> onReverb;
    std::function<void()> onCompressor;
    std::function<void()> onChorus;
    std::function<void()> onEq;
    juce::TextButton reverb, compressor, chorus, eq;
};

class CentredEditorPanel final : public juce::Component
{
public:
    CentredEditorPanel(juce::Component* childToOwn, int panelWidth)
        : child(childToOwn)
    {
        addAndMakeVisible(child.get());
        setSize(panelWidth, child->getHeight());
    }

    void resized() override
    {
        child->setTopLeftPosition((getWidth() - child->getWidth()) / 2, 0);
    }

private:
    std::unique_ptr<juce::Component> child;
};

class ParametricEqGraph final : public juce::Component, private juce::Timer
{
public:
    std::function<void(int, float, float)> onPointChanged;

    ParametricEqGraph(ClassicPlayerAudioProcessor& p, int layerIndex, bool masterEditor = false)
        : processor(p), layer(layerIndex), master(masterEditor)
    {
        setSize(700, 268);
        startTimerHz(30);
    }

    ~ParametricEqGraph() override { stopTimer(); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff0a1118));
        auto graph = getLocalBounds().toFloat().reduced(42.0f, 22.0f);
        graph.removeFromBottom(24.0f);
        graph.removeFromLeft(8.0f);
        g.setColour(juce::Colour(0xff25323c));
        g.fillRect(graph);

        const auto toX = [graph](float frequency)
        {
            const auto normalized = std::log10(juce::jlimit(20.0f, 20000.0f, frequency) / 20.0f)
                                  / std::log10(1000.0f);
            return graph.getX() + normalized * graph.getWidth();
        };
        const auto toY = [graph](float decibels)
        {
            const auto normalized = (juce::jlimit(-18.0f, 18.0f, decibels) + 18.0f) / 36.0f;
            return graph.getBottom() - normalized * graph.getHeight();
        };

        g.setFont(juce::FontOptions(10.0f));
        for (const auto db : { -18.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f, 18.0f })
        {
            const auto y = toY(db);
            g.setColour(db == 0.0f ? juce::Colour(0xff60727e) : juce::Colour(0xff33434e));
            g.drawHorizontalLine(juce::roundToInt(y), graph.getX(), graph.getRight());
            g.setColour(juce::Colour(mutedText));
            g.drawText(juce::String((int) db), 4, juce::roundToInt(y - 7.0f), 34, 14,
                       juce::Justification::centredRight);
        }

        for (const auto frequency : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f,
                                      2000.0f, 5000.0f, 10000.0f, 20000.0f })
        {
            const auto x = toX(frequency);
            g.setColour(juce::Colour(0xff33434e));
            g.drawVerticalLine(juce::roundToInt(x), graph.getY(), graph.getBottom());
            g.setColour(juce::Colour(mutedText));
            g.drawText(formatFrequency(frequency), juce::roundToInt(x - 25.0f),
                       juce::roundToInt(graph.getBottom() + 5.0f), 50, 14,
                       juce::Justification::centred);
        }

        // Live post-master FFT, drawn behind the EQ response so the user can
        // see both the source energy and the filter shape at once. The
        // analyser floor is lifted into the graph's +/-18 dB display range.
        juce::Path spectrum;
        for (int i = 0; i <= 240; ++i)
        {
            const auto normalized = (float) i / 240.0f;
            const auto frequency = 20.0f * std::pow(1000.0f, normalized);
            const auto level = juce::jlimit(-18.0f, 18.0f,
                spectrumDbAt(frequency) + 24.0f);
            const auto point = juce::Point<float>(toX(frequency), toY(level));
            if (i == 0) spectrum.startNewSubPath(point); else spectrum.lineTo(point);
        }
        auto spectrumArea = spectrum;
        spectrumArea.lineTo(graph.getRight(), toY(-18.0f));
        spectrumArea.lineTo(graph.getX(), toY(-18.0f));
        spectrumArea.closeSubPath();
        g.setColour(juce::Colour(0xff42a8d8).withAlpha(0.18f));
        g.fillPath(spectrumArea);
        g.setColour(juce::Colour(0xff4ba9d0).withAlpha(0.72f));
        g.strokePath(spectrum, juce::PathStrokeType(1.1f));

        const auto lowFrequency = parameter("EqLowFrequency", 220.0f);
        const auto midFrequency = parameter("EqMidFrequency", 1200.0f);
        const auto highFrequency = parameter("EqHighFrequency", 4200.0f);
        const auto lowGain = parameter("EqLow", 0.0f);
        const auto midGain = parameter("EqMid", 0.0f);
        const auto highGain = parameter("EqHigh", 0.0f);
        const auto lowQ = parameter("EqLowQ", 0.707f);
        const auto midQ = parameter("EqMidQ", 1.0f);
        const auto highQ = parameter("EqHighQ", 0.707f);
        float highPassHz = 20.0f;
        float lowPassHz = 20000.0f;
        if (master)
        {
            highPassHz = parameter("LowCut", 20.0f);
            lowPassHz = parameter("HighCut", 20000.0f);
        }
        else
        {
            const auto config = processor.layerConfig(layer);
            highPassHz = config.highPassHz;
            lowPassHz = config.lowPassHz;
        }

        juce::Path curve;
        for (int i = 0; i <= 240; ++i)
        {
            const auto normalized = (float) i / 240.0f;
            const auto frequency = 20.0f * std::pow(1000.0f, normalized);
            const auto response = responseAt(frequency, lowFrequency, lowGain,
                                              midFrequency, midGain, highFrequency, highGain,
                                              lowQ, midQ, highQ, highPassHz, lowPassHz);
            const auto point = juce::Point<float>(toX(frequency), toY(response));
            if (i == 0) curve.startNewSubPath(point);
            else curve.lineTo(point);
        }
        g.setColour(juce::Colour(teal).withAlpha(0.18f));
        juce::Path area = curve;
        area.lineTo(graph.getRight(), toY(0.0f));
        area.lineTo(graph.getX(), toY(0.0f));
        area.closeSubPath();
        g.fillPath(area);
        g.setColour(juce::Colour(teal));
        g.strokePath(curve, juce::PathStrokeType(2.2f));

        drawBandNode(g, graph, toX(lowFrequency), toY(lowGain), "1");
        drawBandNode(g, graph, toX(midFrequency), toY(midGain), "2");
        drawBandNode(g, graph, toX(highFrequency), toY(highGain), "3");
        g.setColour(juce::Colour(text));
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText(localizedUiText(master ? "EQ PARAMETRICO MASTER" : "EQ PARAMETRICO DA LAYER",
                                   activeUiLanguage.load()),
                   42, 3, getWidth() - 84, 18,
                   juce::Justification::centred);
        g.setFont(juce::FontOptions(10.0f));
        g.setColour(juce::Colour(mutedText));
        g.drawText(localizedUiText("Arraste os pontos para ajustar frequencia e ganho", activeUiLanguage.load()), 42, getHeight() - 18,
                   getWidth() - 84, 14, juce::Justification::centred);
        g.setColour(juce::Colour(0xff74c8e6).withAlpha(0.9f));
        g.drawText(localizedUiText("ANALISADOR MASTER", activeUiLanguage.load()), 10, 4, 120, 14, juce::Justification::left);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        selectedBand = nearestBand(event.position);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (selectedBand < 0) return;
        auto graph = graphBounds();
        const auto normalizedX = juce::jlimit(0.0f, 1.0f,
            (event.position.x - graph.getX()) / graph.getWidth());
        const auto frequency = 20.0f * std::pow(1000.0f, normalizedX);
        const auto normalizedY = juce::jlimit(0.0f, 1.0f,
            (graph.getBottom() - event.position.y) / graph.getHeight());
        const auto gain = juce::jmap(normalizedY, -18.0f, 18.0f);
        static constexpr std::array<const char*, 3> frequencyNames {
            "EqLowFrequency", "EqMidFrequency", "EqHighFrequency"
        };
        static constexpr std::array<const char*, 3> gainNames { "EqLow", "EqMid", "EqHigh" };
        const std::array<juce::String, 3> masterFrequencyNames {
            "masterEqLowFrequency", "masterEqFrequency", "masterEqHighFrequency"
        };
        const std::array<juce::String, 3> masterGainNames {
            "masterEqLow", "masterEqMid", "masterEqHigh"
        };
        const auto prefix = "layer" + juce::String(layer + 1);
        const auto frequencyId = master ? masterFrequencyNames[(size_t) selectedBand]
                                        : prefix + frequencyNames[(size_t) selectedBand];
        const auto gainId = master ? masterGainNames[(size_t) selectedBand]
                                   : prefix + gainNames[(size_t) selectedBand];
        if (auto* frequencyParameter = processor.parameters.getParameter(frequencyId))
            frequencyParameter->setValueNotifyingHost(frequencyParameter->convertTo0to1(frequency));
        if (auto* gainParameter = processor.parameters.getParameter(gainId))
            gainParameter->setValueNotifyingHost(gainParameter->convertTo0to1(gain));
        if (onPointChanged) onPointChanged(selectedBand, frequency, gain);
    }

private:
    juce::String parameterName(const char* suffix) const
    {
        if (master)
        {
            if (juce::String(suffix) == "EqLowFrequency") return "masterEqLowFrequency";
            if (juce::String(suffix) == "EqMidFrequency") return "masterEqFrequency";
            if (juce::String(suffix) == "EqHighFrequency") return "masterEqHighFrequency";
            if (juce::String(suffix) == "EqLow") return "masterEqLow";
            if (juce::String(suffix) == "EqMid") return "masterEqMid";
            if (juce::String(suffix) == "EqHigh") return "masterEqHigh";
            if (juce::String(suffix) == "LowCut") return "masterEqLowCut";
            if (juce::String(suffix) == "HighCut") return "masterEqHighCut";
        }
        return "layer" + juce::String(layer + 1) + suffix;
    }

    float parameter(const char* suffix, float fallback) const
    {
        if (master && juce::String(suffix).endsWithChar('Q'))
            return fallback;
        if (const auto* value = processor.parameters.getRawParameterValue(parameterName(suffix)))
            return value->load();
        return fallback;
    }

    juce::Rectangle<float> graphBounds() const
    {
        auto graph = getLocalBounds().toFloat().reduced(42.0f, 22.0f);
        graph.removeFromBottom(24.0f);
        return graph.withX(graph.getX() + 8.0f).withWidth(graph.getWidth() - 8.0f);
    }

    static juce::String formatFrequency(float frequency)
    {
        if (frequency >= 1000.0f)
            return juce::String(frequency / 1000.0f, frequency >= 10000.0f ? 0 : 1) + "k";
        return juce::String((int) std::round(frequency));
    }

    static float responseAt(float frequency, float lowFrequency, float lowGain,
                            float midFrequency, float midGain,
                            float highFrequency, float highGain,
                            float lowQ, float midQ, float highQ,
                            float highPassHz, float lowPassHz)
    {
        const auto peakingDb = [frequency](float centre, float gainDb, float q)
        {
            constexpr float sampleRate = 48000.0f;
            const auto safeCentre = juce::jlimit(20.0f, sampleRate * 0.49f, centre);
            const auto safeQ = juce::jlimit(0.1f, 20.0f, q);
            const auto amplitude = juce::Decibels::decibelsToGain(0.5f * gainDb);
            const auto omega = juce::MathConstants<float>::twoPi * safeCentre / sampleRate;
            const auto alpha = std::sin(omega) / (2.0f * safeQ);
            const auto cosine = std::cos(omega);
            const auto a0 = 1.0f + alpha / amplitude;
            const std::array<float, 3> b {
                (1.0f + alpha * amplitude) / a0,
                (-2.0f * cosine) / a0,
                (1.0f - alpha * amplitude) / a0
            };
            const std::array<float, 3> a {
                1.0f,
                (-2.0f * cosine) / a0,
                (1.0f - alpha / amplitude) / a0
            };
            const auto probe = juce::MathConstants<float>::twoPi
                             * juce::jlimit(20.0f, sampleRate * 0.49f, frequency) / sampleRate;
            const std::complex<float> z1 { std::cos(probe), -std::sin(probe) };
            const auto z2 = z1 * z1;
            const auto numerator = b[0] + b[1] * z1 + b[2] * z2;
            const auto denominator = a[0] + a[1] * z1 + a[2] * z2;
            return juce::Decibels::gainToDecibels(std::abs(numerator / denominator), -60.0f);
        };
        auto response =
            peakingDb(lowFrequency, lowGain, lowQ)
          + peakingDb(midFrequency, midGain, midQ)
          + peakingDb(highFrequency, highGain, highQ);

        // Match the gentle one-pole filters used by the layer audio path so
        // activating either cutoff is immediately visible in this graph.
        if (highPassHz > 20.5f)
        {
            const auto ratio = frequency / juce::jmax(20.0f, highPassHz);
            response += juce::Decibels::gainToDecibels(
                ratio / std::sqrt(1.0f + ratio * ratio), -60.0f);
        }
        if (lowPassHz < 19950.0f)
        {
            const auto ratio = frequency / juce::jmax(20.0f, lowPassHz);
            response += juce::Decibels::gainToDecibels(
                1.0f / std::sqrt(1.0f + ratio * ratio), -60.0f);
        }
        return juce::jlimit(-18.0f, 18.0f, response);
    }

    void drawBandNode(juce::Graphics& g, juce::Rectangle<float> graph,
                      float x, float y, const char* number) const
    {
        juce::ignoreUnused(graph);
        g.setColour(juce::Colour(0xffd9e4e8));
        g.fillEllipse(x - 7.0f, y - 7.0f, 14.0f, 14.0f);
        g.setColour(juce::Colour(background));
        g.drawEllipse(x - 7.0f, y - 7.0f, 14.0f, 14.0f, 1.0f);
        g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
        g.drawText(number, juce::Rectangle<float>(x - 6.0f, y - 6.0f, 12.0f, 12.0f),
                   juce::Justification::centred);
    }

    int nearestBand(juce::Point<float> position) const
    {
        const auto graph = graphBounds();
        const auto toX = [graph](float frequency)
        {
            return graph.getX() + std::log10(juce::jlimit(20.0f, 20000.0f, frequency) / 20.0f)
                 / std::log10(1000.0f) * graph.getWidth();
        };
        const auto toY = [graph](float gain)
        {
            return graph.getBottom() - (gain + 18.0f) / 36.0f * graph.getHeight();
        };
        const std::array<float, 3> x { toX(parameter("EqLowFrequency", 220.0f)),
                                       toX(parameter("EqMidFrequency", 1200.0f)),
                                       toX(parameter("EqHighFrequency", 4200.0f)) };
        const std::array<float, 3> y { toY(parameter("EqLow", 0.0f)),
                                       toY(parameter("EqMid", 0.0f)),
                                       toY(parameter("EqHigh", 0.0f)) };
        auto selected = -1;
        auto distance = 24.0f;
        for (int band = 0; band < 3; ++band)
        {
            const auto current = position.getDistanceFrom({ x[(size_t) band], y[(size_t) band] });
            if (current < distance) { distance = current; selected = band; }
        }
        return selected;
    }

    float spectrumDbAt(float frequency) const
    {
        const auto sampleRate = static_cast<float>(processor.spectrumSampleRate());
        const auto bin = juce::jlimit(0.0f, static_cast<float>(spectrumSize / 2),
            frequency / sampleRate * static_cast<float>(spectrumSize));
        const auto lower = static_cast<int>(std::floor(bin));
        const auto upper = juce::jmin(spectrumSize / 2, lower + 1);
        return juce::jmap(bin - static_cast<float>(lower),
                          spectrumBins[(size_t) lower], spectrumBins[(size_t) upper]);
    }

    void timerCallback() override
    {
        processor.copySpectrumSamples(spectrumData.data(), spectrumSize);
        spectrumWindow.multiplyWithWindowingTable(spectrumData.data(), spectrumSize);
        std::fill(spectrumData.begin() + spectrumSize, spectrumData.end(), 0.0f);
        spectrumFft.performFrequencyOnlyForwardTransform(spectrumData.data());
        const auto scale = 2.0f / static_cast<float>(spectrumSize);
        for (int bin = 0; bin <= spectrumSize / 2; ++bin)
            spectrumBins[(size_t) bin] = juce::jlimit(-100.0f, 6.0f,
                juce::Decibels::gainToDecibels(
                    juce::jmax(1.0e-7f, spectrumData[(size_t) bin] * scale)));
        repaint();
    }

    ClassicPlayerAudioProcessor& processor;
    static constexpr int spectrumOrder = 11;
    static constexpr int spectrumSize = 1 << spectrumOrder;
    juce::dsp::FFT spectrumFft { spectrumOrder };
    juce::dsp::WindowingFunction<float> spectrumWindow {
        spectrumSize, juce::dsp::WindowingFunction<float>::hann, true
    };
    std::array<float, spectrumSize * 2> spectrumData {};
    std::array<float, spectrumSize / 2 + 1> spectrumBins {};
    int layer = 0;
    bool master = false;
    int selectedBand = -1;
};

class CompressorResponseView final : public juce::Component, private juce::Timer
{
public:
    CompressorResponseView(ClassicPlayerAudioProcessor& p, int layerIndex)
        : processor(p), layer(layerIndex)
    {
        setSize(700, 284);
        startTimerHz(30);
    }

    ~CompressorResponseView() override { stopTimer(); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff101617));
        auto area = getLocalBounds().toFloat().reduced(18.0f, 24.0f);
        auto graph = area.withX(area.getX() + 74.0f).withWidth(area.getWidth() - 148.0f);
        graph.removeFromBottom(25.0f);
        graph.removeFromTop(4.0f);
        g.setColour(juce::Colour(0xff161d20));
        g.fillRoundedRectangle(area, 10.0f);
        g.setColour(juce::Colour(0xff303a3c));
        g.drawRoundedRectangle(area, 10.0f, 1.0f);

        const auto threshold = parameter("CompThreshold", -18.0f);
        const auto ratio = juce::jmax(1.0f, parameter("CompRatio", 4.0f));
        const auto makeup = parameter("CompMakeup", 0.0f);
        const auto mix = juce::jlimit(0.0f, 1.0f, parameter("Comp", 0.0f) / 100.0f);
        const auto outputDb = juce::jlimit(-60.0f, 6.0f,
            juce::Decibels::gainToDecibels(juce::jmax(1.0e-6f, processor.layerPeak(layer))));
        const auto reduction = outputDb > threshold
            ? (outputDb - threshold) * (1.0f - 1.0f / ratio) * mix : 0.0f;
        const auto inputDb = juce::jlimit(-60.0f, 6.0f, outputDb + reduction - makeup * mix);

        const auto toX = [graph](float db)
        {
            return graph.getX() + (juce::jlimit(-60.0f, 6.0f, db) + 60.0f) / 66.0f * graph.getWidth();
        };
        const auto toY = [graph](float db)
        {
            return graph.getBottom() - (juce::jlimit(-60.0f, 12.0f, db) + 60.0f) / 72.0f * graph.getHeight();
        };
        g.setFont(juce::FontOptions(9.0f));
        for (const auto db : { -60.0f, -45.0f, -30.0f, -15.0f, 0.0f })
        {
            const auto x = toX(db);
            const auto y = toY(db);
            g.setColour(juce::Colour(0xff354346));
            g.drawVerticalLine(juce::roundToInt(x), graph.getY(), graph.getBottom());
            g.drawHorizontalLine(juce::roundToInt(y), graph.getX(), graph.getRight());
            g.setColour(juce::Colour(mutedText));
            g.drawText(juce::String((int) db), juce::roundToInt(x - 14.0f),
                       juce::roundToInt(graph.getBottom() + 4.0f),
                       28, 14, juce::Justification::centred);
        }
        g.setColour(juce::Colour(0xff657578));
        g.drawLine(toX(-60.0f), toY(-60.0f), toX(6.0f), toY(6.0f), 1.0f);

        juce::Path curve;
        for (int i = 0; i <= 120; ++i)
        {
            const auto input = -60.0f + 66.0f * (float) i / 120.0f;
            const auto compressed = input <= threshold
                ? input : threshold + (input - threshold) / ratio + makeup;
            const auto point = juce::Point<float>(toX(input), toY(compressed));
            if (i == 0) curve.startNewSubPath(point); else curve.lineTo(point);
        }
        g.setColour(juce::Colour(teal).withAlpha(0.22f));
        juce::Path filled = curve;
        filled.lineTo(toX(6.0f), toY(-60.0f));
        filled.lineTo(toX(-60.0f), toY(-60.0f));
        filled.closeSubPath();
        g.fillPath(filled);
        g.setColour(juce::Colour(teal));
        g.strokePath(curve, juce::PathStrokeType(2.0f));
        g.setColour(juce::Colour(yellow));
        g.drawLine(toX(threshold), graph.getY(), toX(threshold), graph.getBottom(), 1.5f);
        const auto thresholdPoint = juce::Point<float>(toX(threshold), toY(threshold + makeup));
        const auto ratioPoint = juce::Point<float>(toX(6.0f),
            toY(threshold + (6.0f - threshold) / ratio + makeup));
        for (const auto& point : { thresholdPoint, ratioPoint })
        {
            g.setColour(juce::Colour(yellow));
            g.fillEllipse(point.x - 8.0f, point.y - 8.0f, 16.0f, 16.0f);
            g.setColour(juce::Colour(0xff101617));
            g.fillEllipse(point.x - 3.0f, point.y - 3.0f, 6.0f, 6.0f);
        }
        g.fillEllipse(toX(inputDb) - 4.5f, toY(outputDb) - 4.5f, 9.0f, 9.0f);

        drawMeter(g, area.getX() + 18.0f, area.getY() + 25.0f, area.getHeight() - 58.0f,
                  inputDb, -60.0f, 0.0f, "INPUT", juce::Colour(0xff4ac0aa));
        drawMeter(g, area.getRight() - 45.0f, area.getY() + 25.0f, area.getHeight() - 58.0f,
                  outputDb, -60.0f, 0.0f, "OUTPUT", juce::Colour(0xff4ac0aa));
        drawReductionMeter(g, area.getX() + 48.0f, area.getY() + 25.0f,
                           area.getHeight() - 58.0f, reduction);
        g.setColour(juce::Colour(text));
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText(localizedUiText("COMPRESSOR DA LAYER", activeUiLanguage.load()), juce::roundToInt(area.getX()), 4,
                   juce::roundToInt(area.getWidth()), 18,
                   juce::Justification::centred);
        g.setFont(juce::FontOptions(9.0f));
        g.setColour(juce::Colour(mutedText));
        g.drawText(localizedUiText("CURVA", activeUiLanguage.load()), juce::roundToInt(graph.getX()), juce::roundToInt(area.getBottom() - 17.0f),
                   juce::roundToInt(graph.getWidth()), 14,
                   juce::Justification::centred);
        g.setColour(juce::Colour(yellow));
        g.drawText(juce::String(reduction, 1) + " dB GR", juce::roundToInt(graph.getX()),
                   juce::roundToInt(area.getY() + 5.0f), juce::roundToInt(graph.getWidth()), 14,
                   juce::Justification::centred);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        const auto graph = graphBounds();
        const auto threshold = parameter("CompThreshold", -18.0f);
        const auto ratio = juce::jmax(1.0f, parameter("CompRatio", 4.0f));
        const auto makeup = parameter("CompMakeup", 0.0f);
        const auto thresholdPoint = juce::Point<float>(toX(graph, threshold), toY(graph, threshold + makeup));
        const auto ratioPoint = juce::Point<float>(toX(graph, 6.0f),
            toY(graph, threshold + (6.0f - threshold) / ratio + makeup));
        selectedHandle = event.position.getDistanceFrom(thresholdPoint)
                       <= event.position.getDistanceFrom(ratioPoint) ? 0 : 1;
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        const auto graph = graphBounds();
        if (selectedHandle == 0)
        {
            const auto threshold = juce::jmap(juce::jlimit(0.0f, 1.0f,
                (event.position.x - graph.getX()) / graph.getWidth()), -60.0f, 0.0f);
            setParameter("CompThreshold", threshold);
            if (onCurveChanged) onCurveChanged(0, threshold);
        }
        else
        {
            const auto ratio = juce::jmap(juce::jlimit(0.0f, 1.0f,
                (graph.getBottom() - event.position.y) / graph.getHeight()), 1.0f, 20.0f);
            setParameter("CompRatio", ratio);
            if (onCurveChanged) onCurveChanged(1, ratio);
        }
    }

    std::function<void(int, float)> onCurveChanged;

private:
    juce::Rectangle<float> graphBounds() const
    {
        auto area = getLocalBounds().toFloat().reduced(18.0f, 24.0f);
        auto graph = area.withX(area.getX() + 74.0f).withWidth(area.getWidth() - 148.0f);
        graph.removeFromBottom(25.0f);
        graph.removeFromTop(4.0f);
        return graph;
    }

    static float toX(juce::Rectangle<float> graph, float db)
    {
        return graph.getX() + (juce::jlimit(-60.0f, 6.0f, db) + 60.0f) / 66.0f * graph.getWidth();
    }

    static float toY(juce::Rectangle<float> graph, float db)
    {
        return graph.getBottom() - (juce::jlimit(-60.0f, 12.0f, db) + 60.0f) / 72.0f * graph.getHeight();
    }

    void setParameter(const char* suffix, float value)
    {
        if (auto* target = processor.parameters.getParameter(parameterName(suffix)))
            target->setValueNotifyingHost(target->convertTo0to1(value));
    }

    juce::String parameterName(const char* suffix) const
    {
        return "layer" + juce::String(layer + 1) + suffix;
    }

    float parameter(const char* suffix, float fallback) const
    {
        if (const auto* value = processor.parameters.getRawParameterValue(parameterName(suffix)))
            return value->load();
        return fallback;
    }

    static void drawMeter(juce::Graphics& g, float x, float y, float height,
                          float levelDb, float minimum, float maximum,
                          const char* label, juce::Colour colour)
    {
        auto meter = juce::Rectangle<float>(x, y, 18.0f, height);
        g.setColour(juce::Colour(0xff090d0f));
        g.fillRect(meter);
        const auto amount = juce::jlimit(0.0f, 1.0f, (levelDb - minimum) / (maximum - minimum));
        g.setColour(colour);
        g.fillRect(meter.withTop(meter.getBottom() - amount * meter.getHeight()));
        g.setColour(juce::Colour(mutedText));
        g.drawRect(meter, 1.0f);
        g.setFont(juce::FontOptions(8.0f, juce::Font::bold));
        g.drawText(label, juce::roundToInt(x - 18.0f), juce::roundToInt(meter.getBottom() + 4.0f), 54, 12,
                   juce::Justification::centred);
    }

    static void drawReductionMeter(juce::Graphics& g, float x, float y, float height, float reduction)
    {
        auto meter = juce::Rectangle<float>(x, y, 12.0f, height);
        g.setColour(juce::Colour(0xff090d0f));
        g.fillRect(meter);
        const auto amount = juce::jlimit(0.0f, 1.0f, reduction / 24.0f);
        g.setColour(juce::Colour(yellow));
        g.fillRect(meter.withTop(meter.getBottom() - amount * meter.getHeight()));
        g.setColour(juce::Colour(mutedText));
        g.drawRect(meter, 1.0f);
        g.setFont(juce::FontOptions(8.0f, juce::Font::bold));
        g.drawText("GR", juce::roundToInt(x - 9.0f), juce::roundToInt(meter.getBottom() + 4.0f), 30, 12,
                   juce::Justification::centred);
    }

    void timerCallback() override { repaint(); }

    ClassicPlayerAudioProcessor& processor;
    int layer = 0;
    int selectedHandle = -1;
};

class EqFilterButtonsPanel final : public juce::Component
{
public:
    EqFilterButtonsPanel(ClassicPlayerAudioProcessor& p, int layer)
        : processor(p), index(layer)
    {
        for (auto* button : { &highPass, &lowPass })
        {
            flatButton(*button);
            addAndMakeVisible(*button);
        }
        highPass.onClick = [this] { choose(true); };
        lowPass.onClick = [this] { choose(false); };
        refresh();
        setSize(700, 36);
    }

    void resized() override
    {
        auto row = getLocalBounds().reduced(90, 2);
        highPass.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(3, 0));
        lowPass.setBounds(row.reduced(3, 0));
    }

private:
    void refresh()
    {
        const auto config = processor.layerConfig(index);
        highPass.setButtonText(localizedUiText(config.highPassHz <= 20.5f ? "HIGH PASS: OFF"
            : "HIGH PASS: " + juce::String((int) config.highPassHz) + " Hz", activeUiLanguage.load()));
        lowPass.setButtonText(localizedUiText(config.lowPassHz >= 19950.0f ? "LOW PASS: OFF"
            : "LOW PASS: " + juce::String((int) (config.lowPassHz / 1000.0f)) + " kHz", activeUiLanguage.load()));
    }

    void choose(bool high)
    {
        const std::array<int, 7> highValues { 20, 60, 100, 160, 250, 400, 800 };
        const std::array<int, 7> lowValues { 20000, 16000, 12000, 8000, 6000, 4000, 2000 };
        juce::PopupMenu menu;
        for (int i = 0; i < 7; ++i)
        {
            const auto value = high ? highValues[(size_t) i] : lowValues[(size_t) i];
            menu.addItem(i + 1, (high ? value == 20 : value == 20000) ? "OFF"
                : high ? juce::String(value) + " Hz" : juce::String(value / 1000) + " kHz");
        }
        const juce::Component::SafePointer<EqFilterButtonsPanel> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(high ? highPass : lowPass),
            [safe, high, highValues, lowValues](int chosen)
            {
                if (safe == nullptr || chosen <= 0) return;
                auto config = safe->processor.layerConfig(safe->index);
                if (high) config.highPassHz = (float) highValues[(size_t) (chosen - 1)];
                else config.lowPassHz = (float) lowValues[(size_t) (chosen - 1)];
                safe->processor.setLayerConfig(safe->index, config);
                safe->refresh();
            });
    }

    ClassicPlayerAudioProcessor& processor;
    int index;
    juce::TextButton highPass, lowPass;
};

class ModulationTogglePanel final : public juce::Component
{
public:
    ModulationTogglePanel(ClassicPlayerAudioProcessor& p, int layer)
        : processor(p), index(layer)
    {
        flatButton(button);
        button.setClickingTogglesState(true);
        button.setTooltip(juce::String::fromUTF8("Ativa ou desativa a modulação recebida pelo teclado"));
        button.onClick = [this]
        {
            if (auto* parameter = processor.parameters.getParameter(
                    "layer" + juce::String(index + 1) + "ModulationEnabled"))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(
                    button.getToggleState() ? 1.0f : 0.0f));
            refresh();
        };
        addAndMakeVisible(button);
        refresh();
        setSize(700, 36);
    }

    void resized() override { button.setBounds(getLocalBounds().withSizeKeepingCentre(260, juce::jmin(30, getHeight()))); }

private:
    void refresh()
    {
        const auto* value = processor.parameters.getRawParameterValue(
            "layer" + juce::String(index + 1) + "ModulationEnabled");
        const auto enabled = value == nullptr || value->load() >= 0.5f;
        button.setToggleState(enabled, juce::dontSendNotification);
        button.setButtonText(localizedUiText(enabled ? "MODULATION DO TECLADO: ON"
                                                     : "MODULATION DO TECLADO: OFF",
                                             activeUiLanguage.load()));
        button.setColour(juce::TextButton::buttonColourId,
                         enabled ? juce::Colour(0xff1b554e) : juce::Colour(panelLight));
    }

    ClassicPlayerAudioProcessor& processor;
    int index;
    juce::TextButton button;
};

static void showParametricLayerEqEditor(
    ClassicPlayerAudioProcessor& processor, int layer, juce::Component* owner,
    std::function<void()> savePreset,
    std::function<void(std::function<void()>)> loadPreset)
{
    const auto prefix = "layer" + juce::String(layer + 1);
    const auto layerConfig = processor.layerConfig(layer);
    auto* dialog = new LayerEditorWindow(
        "EQ DA LAYER", "Equalizador parametrico de tres bandas: frequencia e ganho independentes.",
        juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    auto value = [&processor, prefix](const juce::String& suffix, float fallback)
    {
        if (auto* parameter = processor.parameters.getRawParameterValue(prefix + suffix))
            return parameter->load();
        return fallback;
    };
    auto* knobs = new KnobEditorPanel({
        { "LOW FREQ Hz", value("EqLowFrequency", 220.0f), 40.0f, 2000.0f, 1.0f, 0 },
        { "LOW GAIN dB", value("EqLow", 0.0f), -18.0f, 18.0f, 0.1f, 1 },
        { "LOW Q", value("EqLowQ", 0.707f), 0.1f, 4.0f, 0.01f, 2 },
        { "MID FREQ Hz", value("EqMidFrequency", 1200.0f), 60.0f, 12000.0f, 1.0f, 0 },
        { "MID GAIN dB", value("EqMid", 0.0f), -18.0f, 18.0f, 0.1f, 1 },
        { "MID Q", value("EqMidQ", 1.0f), 0.1f, 20.0f, 0.01f, 2 },
        { "HIGH FREQ Hz", value("EqHighFrequency", 4200.0f), 1000.0f, 20000.0f, 1.0f, 0 },
        { "HIGH GAIN dB", value("EqHigh", 0.0f), -18.0f, 18.0f, 0.1f, 1 },
        { "HIGH Q", value("EqHighQ", 0.707f), 0.1f, 4.0f, 0.01f, 2 },
        { "LOW CUT Hz", layerConfig.highPassHz, 20.0f, 250.0f, 1.0f, 0 },
        { "HIGH CUT Hz", layerConfig.lowPassHz, 2000.0f, 20000.0f, 1.0f, 0 }
    }, 4);
    auto* graph = new ParametricEqGraph(processor, layer);
    knobs->useDenseGrid(true);
    auto refreshControls = [&processor, layer, prefix, knobs, graph]
    {
        const std::array<const char*, 9> names {
            "EqLowFrequency", "EqLow", "EqLowQ", "EqMidFrequency", "EqMid", "EqMidQ",
            "EqHighFrequency", "EqHigh", "EqHighQ"
        };
        for (int i = 0; i < 9; ++i)
            if (auto* value = processor.parameters.getRawParameterValue(prefix + names[(size_t) i]))
                knobs->setValue(i, value->load());
        const auto config = processor.layerConfig(layer);
        knobs->setValue(9, config.highPassHz);
        knobs->setValue(10, config.lowPassHz);
        graph->repaint();
    };
    dialog->addCustomComponent(new CentredEditorPanel(new EffectPresetFilePanel(
        std::move(savePreset),
        [loadPreset = std::move(loadPreset), refreshControls]
        {
            loadPreset(refreshControls);
        }), 700));
    const auto contentWidth = editorContentWidth(owner, 1050);
    dialog->addCustomComponent(new SideBySideEditorPanel(graph, knobs, contentWidth, 244));
    dialog->setSize(contentWidth + 52, 448);
    graph->onPointChanged = [knobs](int band, float frequency, float gain)
    {
        const auto first = band * 3;
        knobs->setValue(first, frequency);
        knobs->setValue(first + 1, gain);
    };
    const auto apply = [&processor, layer, prefix, knobs]
    {
        const std::array<const char*, 9> names {
            "EqLowFrequency", "EqLow", "EqLowQ", "EqMidFrequency", "EqMid", "EqMidQ",
            "EqHighFrequency", "EqHigh", "EqHighQ"
        };
        for (int i = 0; i < 9; ++i)
            if (auto* parameter = processor.parameters.getParameter(prefix + names[(size_t) i]))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(knobs->value(i)));
        auto config = processor.layerConfig(layer);
        config.highPassHz = juce::jlimit(20.0f, 250.0f, knobs->value(9));
        config.lowPassHz = juce::jlimit(2000.0f, 20000.0f, knobs->value(10));
        processor.setLayerConfig(layer, config);
    };
    knobs->setOnValueChange(apply);
    dialog->fitWithinApp(owner);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [&processor](int) { juce::ignoreUnused(processor); }), true);
}

class AnalogSynthEditorPanel final : public juce::Component, private juce::Timer
{
public:
    explicit AnalogSynthEditorPanel(const AnalogSynthEngine::Config& source)
        : initial(source),
          sourceAtOpen(source),
          waves { source.oscillator1Wave, source.oscillator2Wave, source.oscillator3Wave },
          knobs({
              { "OSC 1 LEVEL", source.oscillator1Level * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "OSC 2 LEVEL", source.oscillator2Level * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "OSC 3 LEVEL", source.oscillator3Level * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "OSC 2 TUNE", source.oscillator2Semitones, -24.0f, 24.0f, 1.0f, 0 },
              { "OSC 3 TUNE", source.oscillator3Semitones, -24.0f, 24.0f, 1.0f, 0 },
              { "NOISE", source.noiseLevel * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "CUTOFF", source.cutoff, 0.0f, 100.0f, 0.0f, 2 },
              { "EMPHASIS", source.resonance * 100.0f, 0.0f, 100.0f, 0.0f, 2 },
              { "FILTER CONTOUR", source.filterEnvelopeAmount * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "ATTACK ms", source.ampAttackMs, 1.0f, 2000.0f, 1.0f, 0 },
              { "DECAY ms", source.ampDecayMs, 1.0f, 5000.0f, 1.0f, 0 },
              { "SUSTAIN", source.ampSustain * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "RELEASE ms", source.ampReleaseMs, 1.0f, 5000.0f, 1.0f, 0 },
              { "LFO RATE Hz", source.lfoRateHz, 0.05f, 20.0f, 0.01f, 2 },
              { "LFO PITCH", source.lfoToPitch, 0.0f, 12.0f, 0.1f, 1 },
              { "LFO FILTER", source.lfoToFilter, 0.0f, 100.0f, 1.0f, 0 },
              { "DRIVE", source.mixerDrive * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "KEY TRACK", source.filterKeyboardTracking * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "MOD WHEEL", source.modWheelToFilter * 100.0f, 0.0f, 100.0f, 1.0f, 0 }
          }, 6)
    {
        configureWaveButton(wave1, 0);
        configureWaveButton(wave2, 1);
        configureWaveButton(wave3, 2);
        configureSwitch(oscillator1On, "OSC 1 ON", source.oscillator1Enabled);
        configureSwitch(oscillator2On, "OSC 2 ON", source.oscillator2Enabled);
        configureSwitch(oscillator3On, "OSC 3 ON", source.oscillator3Enabled);
        configureSwitch(pinkNoise, "PINK NOISE", source.pinkNoise);
        configureMonoPoly(source.monophonic);
        presetBox.addItem("INICIAL", 1);
        for (size_t i = 0; i < AnalogBrowserPresets::bank.size(); ++i)
        {
            const auto& p = AnalogBrowserPresets::bank[i];
            presetBox.addItem("[" + juce::String(p.category) + "] " + p.name, static_cast<int>(i) + 2);
        }
        presetBox.setSelectedId(1, juce::dontSendNotification);
        presetBox.setColour(juce::ComboBox::backgroundColourId, juce::Colour(panelLight));
        presetBox.setColour(juce::ComboBox::textColourId, juce::Colour(text));
        presetBox.setColour(juce::ComboBox::outlineColourId, juce::Colour(paletteLine));
        presetBox.onChange = [this]
        {
            applyFactoryPreset(presetBox.getSelectedId());
            // Preset selection is an audible action: update the layer now so
            // changing the name never requires a second confirmation click.
            if (onPresetChanged)
                onPresetChanged(config());
            else if (onConfigChanged)
                onConfigChanged(config());
        };
        addAndMakeVisible(presetBox);
        addAndMakeVisible(wave1);
        addAndMakeVisible(wave2);
        addAndMakeVisible(wave3);
        addAndMakeVisible(oscillator1On);
        addAndMakeVisible(oscillator2On);
        addAndMakeVisible(oscillator3On);
        addAndMakeVisible(pinkNoise);
        addAndMakeVisible(monoPoly);
        knobs.useAnalogHardwareLayout(true);
        knobs.setOnValueChange([this] { if (onConfigChanged) onConfigChanged(config()); });
        for (auto* button : { &oscillator1On, &oscillator2On, &oscillator3On, &pinkNoise })
            button->onClick = [this] { if (onConfigChanged) onConfigChanged(config()); };
        addAndMakeVisible(knobs);
        // Four complete rows of hardware-style knobs need this height; a
        // shorter panel clipped the lower controls and made the footer appear
        // to overlap the editor content.
        // Keep the Analog editor compact enough for notebook displays while
        // preserving all four rows of controls and the oscilloscope.
        setSize(700, 320);
        startTimerHz(30);
    }

    void setPresetOnlyMode()
    {
        presetOnly = true;
        for (auto* component : { static_cast<juce::Component*>(&wave1),
                                 static_cast<juce::Component*>(&wave2),
                                 static_cast<juce::Component*>(&wave3),
                                 static_cast<juce::Component*>(&oscillator1On),
                                 static_cast<juce::Component*>(&oscillator2On),
                                 static_cast<juce::Component*>(&oscillator3On),
                                 static_cast<juce::Component*>(&pinkNoise),
                                 static_cast<juce::Component*>(&monoPoly),
                                 static_cast<juce::Component*>(&knobs) })
            component->setVisible(false);
        setSize(600, 86);
        resized();
        repaint();
    }

    AnalogSynthEngine::Config config() const
    {
        auto result = initial;
        result.oscillator1Wave = waves[0]; result.oscillator2Wave = waves[1]; result.oscillator3Wave = waves[2];
        result.oscillator1Level = knobs.value(0) / 100.0f; result.oscillator2Level = knobs.value(1) / 100.0f;
        result.oscillator3Level = knobs.value(2) / 100.0f; result.oscillator2Semitones = knobs.value(3);
        result.oscillator3Semitones = knobs.value(4); result.noiseLevel = knobs.value(5) / 100.0f;
        result.cutoff = knobs.value(6); result.resonance = knobs.value(7) / 100.0f;
        result.filterEnvelopeAmount = knobs.value(8) / 100.0f; result.ampAttackMs = knobs.value(9);
        result.ampDecayMs = knobs.value(10); result.ampSustain = knobs.value(11) / 100.0f;
        result.ampReleaseMs = knobs.value(12); result.lfoRateHz = knobs.value(13);
        result.lfoToPitch = knobs.value(14); result.lfoToFilter = knobs.value(15);
        result.glideMs = 0.0f;
        result.mixerDrive = knobs.value(16) / 100.0f;
        result.filterKeyboardTracking = knobs.value(17) / 100.0f;
        result.modWheelToFilter = knobs.value(18) / 100.0f;
        result.oscillator1Enabled = oscillator1On.getToggleState();
        result.oscillator2Enabled = oscillator2On.getToggleState();
        result.oscillator3Enabled = oscillator3On.getToggleState();
        result.pinkNoise = pinkNoise.getToggleState();
        result.monophonic = monoPoly.getToggleState();
        result.routing.mono = result.monophonic;
        // Keep the editor's filter value in the same signal path used by the
        // layer mixer. This makes the common Cutoff control and the Analog
        // panel operate on the same parameter instead of two disconnected
        // filters.
        result.routing.cutoff = result.cutoff;
        return result;
    }

    std::function<void(const AnalogSynthEngine::Config&)> onPresetChanged;
    std::function<void(const AnalogSynthEngine::Config&)> onConfigChanged;

    void paint(juce::Graphics& g) override
    {
        if (presetOnly)
        {
            g.fillAll(juce::Colour(0xff151f28));
            g.setColour(juce::Colour(mutedText));
            g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
            g.drawText(localizedUiText(initial.browserCompatible ? "PRESET ANALOG - BROWSER 12 dB"
                                                                 : "PRESET ANALOG - LEGADO",
                                        activeUiLanguage.load()),
                       12, 8, getWidth() - 24, 16, juce::Justification::left);
            return;
        }

        // Purpose-built Classic Keys Analog layout: clean functional sections,
        // no imitation of the reference hardware panel.
        g.fillAll(juce::Colour(0xff0b131b));

        g.setColour(juce::Colour(0xff16242f));
        g.fillRoundedRectangle(8.0f, 6.0f, (float) getWidth() - 16.0f, 48.0f, 7.0f);
        g.setColour(juce::Colour(teal));
        g.fillRoundedRectangle(18.0f, 44.0f, (float) getWidth() - 36.0f, 2.0f, 1.0f);

        g.setColour(juce::Colour(text));
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText(localizedUiText("CLASSIC KEYS ANALOG", activeUiLanguage.load()),
                   22, 13, 250, 20, juce::Justification::left);
        g.setColour(juce::Colour(mutedText));
        g.setFont(juce::FontOptions(9.0f));
        g.drawText(localizedUiText(initial.browserCompatible ? "BROWSER 12 dB - SOM APROVADO"
                                                             : "OSCILLATOR - FILTER - MODULATION",
                                   activeUiLanguage.load()),
                   22, 33, 250, 12, juce::Justification::left);

        const int left = 14, top = 62;
        const int moduleWidth = juce::jmax(420, getWidth() - 140);
        const int width = moduleWidth, height = getHeight() - 76;
        const int cardGap = 5;
        const int cardWidth = (width - cardGap * 4) / 5;
        for (int group = 0; group < 5; ++group)
        {
            const auto card = juce::Rectangle<float>(
                (float) left + (float) group * (cardWidth + cardGap),
                (float) top, (float) cardWidth, (float) height);
            g.setColour(juce::Colour(0xff131e27));
            g.fillRoundedRectangle(card, 5.0f);
            g.setColour(juce::Colour(0xff2d404c));
            g.drawRoundedRectangle(card, 5.0f, 1.0f);
        }

        // Dedicated scope card, separate from every control column.
        const auto scopeCard = juce::Rectangle<float>((float) left + moduleWidth + 10.0f,
                                                       (float) top, (float) getWidth() - left - moduleWidth - 20.0f,
                                                       (float) height);
        g.setColour(juce::Colour(0xff101a22));
        g.fillRoundedRectangle(scopeCard, 5.0f);
        g.setColour(juce::Colour(0xff2d404c));
        g.drawRoundedRectangle(scopeCard, 5.0f, 1.0f);

        const auto scope = scopeCard.reduced(8.0f).withHeight(118.0f).withY(scopeCard.getY() + 34.0f);
        g.setColour(juce::Colour(0xff05090d));
        g.fillRoundedRectangle(scope, 3.0f);
        g.setColour(juce::Colour(0xff3a5361));
        g.drawRoundedRectangle(scope, 3.0f, 1.0f);
        juce::Path waveform;
        for (int sample = 0; sample <= 48; ++sample)
        {
            const auto t = static_cast<float>(sample) / 48.0f;
            const auto y = scope.getCentreY() - std::sin(t * juce::MathConstants<float>::twoPi * 2.0f + scopePhase)
                                           * scope.getHeight() * 0.32f;
            const auto xPos = scope.getX() + 5.0f + t * (scope.getWidth() - 10.0f);
            if (sample == 0) waveform.startNewSubPath(xPos, y);
            else waveform.lineTo(xPos, y);
        }
        g.setColour(juce::Colour(0xfff0a52b));
        g.strokePath(waveform, juce::PathStrokeType(1.5f));
        g.setColour(juce::Colour(mutedText));
        g.setFont(juce::FontOptions(8.0f, juce::Font::bold));
        g.drawText(localizedUiText("OSCILLOSCOPE", activeUiLanguage.load()),
                   scopeCard.getX(), scope.getBottom() + 7.0f,
                   scopeCard.getWidth(), 12.0f, juce::Justification::centred);
    }

    void timerCallback() override
    {
        scopePhase = std::fmod(scopePhase + 0.16f, juce::MathConstants<float>::twoPi);
        repaint();
    }

    void resized() override
    {
        if (presetOnly)
        {
            presetBox.setBounds(12, 28, juce::jmax(120, getWidth() - 24), 28);
            return;
        }
        const auto w = getWidth();
        const int moduleWidth = juce::jmax(420, w - 140);
        presetBox.setBounds(25, 42, juce::jmin(270, moduleWidth - 30), 24);
        const auto waveY = 70;
        const auto waveWidth = juce::jmax(92, juce::jmin(132, (moduleWidth - 72) / 3));
        wave1.setBounds(juce::roundToInt(moduleWidth * 0.22f), waveY, waveWidth, 24);
        wave2.setBounds(juce::roundToInt(moduleWidth * 0.47f), waveY, waveWidth, 24);
        wave3.setBounds(juce::roundToInt(moduleWidth * 0.72f), waveY, waveWidth, 24);
        oscillator1On.setBounds(wave1.getX() + 8, 98, waveWidth - 16, 20);
        oscillator2On.setBounds(wave2.getX() + 8, 98, waveWidth - 16, 20);
        oscillator3On.setBounds(wave3.getX() + 8, 98, waveWidth - 16, 20);
        monoPoly.setBounds(18, 96, juce::jmin(112, moduleWidth / 5), 25);
        pinkNoise.setBounds(moduleWidth + 18, 98, juce::jmin(92, w - moduleWidth - 24), 20);
        // Start the knob grid below all oscillator controls.  This is the
        // single source of truth for the 19 controls, preventing overlap.
        knobs.setBounds(14, 126, juce::jmax(420, moduleWidth - 20),
                        juce::jmax(180, getHeight() - 132));
    }

private:
    void applyFactoryPreset(int preset)
    {
        if (preset <= 1)
        {
            setFromConfig(sourceAtOpen);
            return;
        }

        setFromConfig(AnalogBrowserPresets::config(
            static_cast<size_t>(juce::jlimit(2, 33, preset) - 2), sourceAtOpen.routing));
    }

    void setFromConfig(const AnalogSynthEngine::Config& value)
    {
        initial = value; waves = { value.oscillator1Wave, value.oscillator2Wave, value.oscillator3Wave };
        wave1.setButtonText("OSC 1 - " + waveformName(waves[0])); wave2.setButtonText("OSC 2 - " + waveformName(waves[1]));
        wave3.setButtonText("OSC 3 - " + waveformName(waves[2]));
        knobs.setValue(0, value.oscillator1Level * 100.0f); knobs.setValue(1, value.oscillator2Level * 100.0f);
        knobs.setValue(2, value.oscillator3Level * 100.0f); knobs.setValue(3, value.oscillator2Semitones);
        knobs.setValue(4, value.oscillator3Semitones); knobs.setValue(5, value.noiseLevel * 100.0f);
        knobs.setValue(6, value.cutoff); knobs.setValue(7, value.resonance * 100.0f);
        knobs.setValue(8, value.filterEnvelopeAmount * 100.0f); knobs.setValue(9, value.ampAttackMs);
        knobs.setValue(10, value.ampDecayMs); knobs.setValue(11, value.ampSustain * 100.0f);
        knobs.setValue(12, value.ampReleaseMs); knobs.setValue(13, value.lfoRateHz);
        knobs.setValue(14, value.lfoToPitch); knobs.setValue(15, value.lfoToFilter);
        knobs.setValue(16, value.mixerDrive * 100.0f);
        knobs.setValue(17, value.filterKeyboardTracking * 100.0f);
        knobs.setValue(18, value.modWheelToFilter * 100.0f);
        oscillator1On.setToggleState(value.oscillator1Enabled, juce::dontSendNotification);
        oscillator2On.setToggleState(value.oscillator2Enabled, juce::dontSendNotification);
        oscillator3On.setToggleState(value.oscillator3Enabled, juce::dontSendNotification);
        pinkNoise.setToggleState(value.pinkNoise, juce::dontSendNotification);
        monoPoly.setToggleState(value.monophonic, juce::dontSendNotification);
        updateMonoPolyText();
    }

    static juce::String waveformName(AnalogSynthEngine::Waveform waveform)
    {
        switch (waveform) { case AnalogSynthEngine::Waveform::triangle: return "TRIANGLE"; case AnalogSynthEngine::Waveform::saw: return "SAW"; case AnalogSynthEngine::Waveform::square: return "SQUARE"; case AnalogSynthEngine::Waveform::pulse: return "PULSE"; case AnalogSynthEngine::Waveform::sine: return "SINE"; }
        return "SAW";
    }

    void configureWaveButton(juce::TextButton& button, int oscillator)
    {
        flatButton(button); button.setClickingTogglesState(false); auto* buttonPtr = &button;
        button.onClick = [this, oscillator, buttonPtr]
        {
            auto value = static_cast<int>(waves[(size_t) oscillator]);
            waves[(size_t) oscillator] = static_cast<AnalogSynthEngine::Waveform>((value + 1) % 5);
            buttonPtr->setButtonText("OSC " + juce::String(oscillator + 1) + " - " + waveformName(waves[(size_t) oscillator]));
            if (onConfigChanged) onConfigChanged(config());
        };
        button.setButtonText("OSC " + juce::String(oscillator + 1) + " - " + waveformName(waves[(size_t) oscillator]));
    }

    void configureSwitch(juce::TextButton& button, const juce::String& label, bool enabled)
    {
        flatButton(button);
        button.setClickingTogglesState(true);
        button.setButtonText(label);
        button.setToggleState(enabled, juce::dontSendNotification);
        button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff1f8f89));
        button.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    }

    void configureMonoPoly(bool monophonic)
    {
        flatButton(monoPoly);
        monoPoly.setClickingTogglesState(true);
        monoPoly.setToggleState(monophonic, juce::dontSendNotification);
        monoPoly.onClick = [this] { updateMonoPolyText(); if (onConfigChanged) onConfigChanged(config()); };
        updateMonoPolyText();
    }

    void updateMonoPolyText()
    {
        monoPoly.setButtonText(monoPoly.getToggleState() ? "MONO / LEGATO" : "POLI");
    }

    AnalogSynthEngine::Config initial, sourceAtOpen;
    std::array<AnalogSynthEngine::Waveform, 3> waves;
    juce::ComboBox presetBox;
    juce::TextButton wave1, wave2, wave3;
    juce::TextButton oscillator1On, oscillator2On, oscillator3On, pinkNoise, monoPoly;
    KnobEditorPanel knobs;
    float scopePhase = 0.0f;
    bool presetOnly = false;
};

class EngineProgramSavePanel final : public juce::Component
{
public:
    EngineProgramSavePanel(ClassicPlayerAudioProcessor& p, int layer, juce::String engine)
        : processor(p), index(layer), engineName(std::move(engine))
    {
        flatButton(saveButton);
        saveButton.setButtonText("Exportar Preset");
        saveButton.setTooltip(juce::String::fromUTF8("Exporta a programação completa para um arquivo portátil."));
        saveButton.onClick = [this]
        {
            auto name = (engineName + " - Layer " + juce::String(index + 1))
                .retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_()");
            const auto destination = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile(name + ".ckprogram");
            chooser = std::make_unique<juce::FileChooser>(localizedUiText("Exportar Preset Classic Player", activeUiLanguage.load()),
                                                          destination, "*.ckprogram");
            const juce::Component::SafePointer<EngineProgramSavePanel> safe(this);
            chooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                [safe](const juce::FileChooser& selectedFile)
                {
                    if (safe == nullptr || selectedFile.getResult() == juce::File{}) return;
                    juce::File saved;
                    const auto result = safe->processor.saveProgramToFile(selectedFile.getResult(), saved);
                    if (result.failed())
                        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                               "Falha ao exportar", result.getErrorMessage());
                    else
                        safe->status.setText("EXPORTADO: " + saved.getFullPathName(), juce::dontSendNotification);
                });
        };
        status.setJustificationType(juce::Justification::centredLeft);
        status.setColour(juce::Label::textColourId,juce::Colour(mutedText));
        status.setText("Exporta um arquivo .ckprogram que pode ser importado em outro Classic Player.",juce::dontSendNotification);
        addAndMakeVisible(saveButton);addAndMakeVisible(status);setSize(600,36);
    }
    void resized() override {auto row=getLocalBounds().reduced(2);saveButton.setBounds(row.removeFromLeft(210));status.setBounds(row.reduced(8,0));}
private:
    ClassicPlayerAudioProcessor& processor;int index;juce::String engineName;juce::TextButton saveButton;juce::Label status;
    std::unique_ptr<juce::FileChooser> chooser;
};

class Sf2EditorPanel final : public juce::Component
{
public:
    Sf2EditorPanel(ClassicPlayerAudioProcessor& p, int layer) : processor(p), index(layer)
    {
        for (auto* box : { &libraryBox, &presetBox, &deviceBox })
            box->getProperties().set("uiDataItems", true);
        for (auto* label : { &categoryLabel, &libraryLabel, &presetLabel, &modeLabel,
                             &sustainLabel, &channelLabel, &deviceLabel, &octaveLabel,
                             &rangeLabel, &velocityLabel })
        {
            label->setColour(juce::Label::textColourId, juce::Colour(text));
            label->setFont(juce::FontOptions(11.0f, juce::Font::bold));
            addAndMakeVisible(*label);
        }
        categoryLabel.setText("CATEGORIA", juce::dontSendNotification);
        libraryLabel.setText("BIBLIOTECA SF2", juce::dontSendNotification);
        presetLabel.setText("PRESET", juce::dontSendNotification);
        modeLabel.setText("MODO", juce::dontSendNotification);
        sustainLabel.setText("SUSTAIN", juce::dontSendNotification);
        channelLabel.setText("CANAL MIDI", juce::dontSendNotification);
        deviceLabel.setText("ENTRADA MIDI", juce::dontSendNotification);
        octaveLabel.setText("OITAVA", juce::dontSendNotification);
        rangeLabel.setText("FAIXA DE NOTAS", juce::dontSendNotification);
        velocityLabel.setText("VELOCIDADE", juce::dontSendNotification);

        for (const auto& category : ClassicPlayerAudioProcessor::soundFontCategories())
            categoryBox.addItem(category, categoryBox.getNumItems() + 1);
        categoryBox.setSelectedId(1, juce::dontSendNotification);
        categoryBox.onChange = [this] { rebuildLibrary(); };
        libraryBox.onChange = [this]
        {
            const auto selected = libraryBox.getSelectedItemIndex();
            if (juce::isPositiveAndBelow(selected, libraryFiles.size()))
            {
                const auto result = processor.loadSoundFont(index, libraryFiles.getReference(selected));
                if (result.failed())
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                           "Falha ao carregar SF2", result.getErrorMessage());
                rebuildPresets();
            }
        };
        presetBox.onChange = [this]
        {
            const auto selected = presetBox.getSelectedItemIndex();
            if (juce::isPositiveAndBelow(selected, (int) presets.size()))
                processor.selectLayerPreset(index, presets[(size_t) selected].bank,
                                             presets[(size_t) selected].program);
        };
        importButton.setButtonText("IMPORTAR SF2");
        deleteButton.setButtonText("EXCLUIR SF2");
        flatButton(importButton); flatButton(deleteButton);
        importButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser>(localizedUiText("Importar SoundFont", activeUiLanguage.load()), juce::File{}, "*.sf2");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& c)
                {
                    const auto source = c.getResult();
                    if (!source.existsAsFile()) return;
                    juce::File imported;
                    const auto result = processor.importSoundFont(source, selectedCategoryId(categoryBox), imported);
                    if (result.failed())
                        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                               "Falha ao importar SF2", result.getErrorMessage());
                    else
                    {
                        processor.loadSoundFont(index, imported);
                        rebuildLibrary();
                    }
                });
        };
        deleteButton.onClick = [this]
        {
            const auto selected = libraryBox.getSelectedItemIndex();
            if (juce::isPositiveAndBelow(selected, libraryFiles.size()))
            {
                processor.deleteLibrarySoundFont(libraryFiles.getReference(selected));
                rebuildLibrary();
            }
        };
        for (auto* box : { &categoryBox, &libraryBox, &presetBox, &modeBox, &sustainBox,
                           &channelBox, &deviceBox, &octaveBox, &lowNoteBox, &highNoteBox,
                           &velocityBox })
        {
            addAndMakeVisible(*box);
            box->setColour(juce::ComboBox::backgroundColourId, juce::Colour(panelLight));
            box->setColour(juce::ComboBox::textColourId, juce::Colour(text));
        }
        addAndMakeVisible(importButton); addAndMakeVisible(deleteButton);
        modeBox.addItem("POLI", 1); modeBox.addItem("MONO / LEGATO", 2); modeBox.addItem("PORTAMENTO", 3);
        sustainBox.addItem("SUSTAIN ON", 1); sustainBox.addItem("SUSTAIN OFF", 2);
        channelBox.addItem("MIDI OMNI", 1);
        for (int channel = 1; channel <= 16; ++channel) channelBox.addItem("MIDI CH " + juce::String(channel), channel + 1);
        for (int value = -4; value <= 4; ++value) octaveBox.addItem((value > 0 ? "+" : "") + juce::String(value) + " OIT", value + 5);
        for (int note = 0; note < 128; ++note) { lowNoteBox.addItem(midiNoteName(note), note + 1); highNoteBox.addItem(midiNoteName(note), note + 1); }
        velocityBox.addItem("VEL LINEAR", 1); velocityBox.addItem("VEL SOFT", 2); velocityBox.addItem("VEL HARD", 3);
        for (auto* box : { &modeBox, &sustainBox, &channelBox, &octaveBox, &lowNoteBox, &highNoteBox, &velocityBox })
            box->onChange = [this] { applyRouting(); };
        deviceBox.addItem("TODOS OS CONTROLADORES", 1);
        for (const auto& device : processor.availableMidiDevices())
            deviceBox.addItem(device.name, deviceBox.getNumItems() + 1);
        deviceBox.onChange = [this]
        {
            const auto selected = deviceBox.getSelectedId() - 2;
            const auto devices = processor.availableMidiDevices();
            processor.setLayerMidiDevice(index, juce::isPositiveAndBelow(selected, devices.size())
                                                   ? devices.getReference(selected).identifier : juce::String{});
        };
        refreshFromProcessor();
        setSize(700, 430);
    }

    void useCompactLayout(bool shouldUse) { compactLayout = shouldUse; resized(); }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12);
        auto top = area.removeFromTop(compactLayout ? 24 : 26);
        categoryLabel.setBounds(top.removeFromLeft(105)); categoryBox.setBounds(top.removeFromLeft(250));
        importButton.setBounds(top.removeFromLeft(150).reduced(2)); deleteButton.setBounds(top.reduced(2));
        auto row = area.removeFromTop(compactLayout ? 22 : 25);
        libraryLabel.setBounds(row.removeFromLeft(105)); libraryBox.setBounds(row);
        row = area.removeFromTop(compactLayout ? 22 : 25);
        presetLabel.setBounds(row.removeFromLeft(105)); presetBox.setBounds(row);
        area.removeFromTop(compactLayout ? 3 : 10);
        const auto cell = area.getWidth() / 3;
        auto place = [compact = compactLayout](juce::Rectangle<int> r, juce::Label& l, juce::ComboBox& b)
        {
            l.setBounds(r.removeFromTop(compact ? 12 : 18));
            b.setBounds(r.reduced(2, compact ? 0 : 2));
        };
        for (int rowIndex = 0; rowIndex < 3; ++rowIndex)
        {
            auto line = area.removeFromTop(compactLayout ? 32 : 62);
            if (rowIndex == 0) { place(line.removeFromLeft(cell), modeLabel, modeBox); place(line.removeFromLeft(cell), sustainLabel, sustainBox); place(line, channelLabel, channelBox); }
            if (rowIndex == 1) { place(line.removeFromLeft(cell), deviceLabel, deviceBox); place(line.removeFromLeft(cell), octaveLabel, octaveBox); place(line, velocityLabel, velocityBox); }
            if (rowIndex == 2) { place(line.removeFromLeft(cell), rangeLabel, lowNoteBox); place(line.removeFromLeft(cell), rangeLabel, highNoteBox); }
        }
    }

private:
    void selectCurrentCategory()
    {
        const auto path = processor.soundFontPath(index);
        if (path.isEmpty()) return;

        const auto categories = ClassicPlayerAudioProcessor::soundFontCategories();
        const auto parentCategory = juce::File(path).getParentDirectory().getFileName();
        int selected = categories.indexOf(parentCategory);

        // Imported files from older versions may live directly in the library
        // root, so use the actual file membership as a reliable fallback.
        if (selected < 0)
        {
            for (int category = 0; category < categories.size(); ++category)
            {
                const auto files = processor.librarySoundFonts(categories[category]);
                for (const auto& file : files)
                    if (file.getFullPathName() == path)
                    {
                        selected = category;
                        break;
                    }
                if (selected >= 0) break;
            }
        }
        if (selected >= 0)
            categoryBox.setSelectedId(selected + 1, juce::dontSendNotification);
    }

    void rebuildLibrary()
    {
        libraryFiles = processor.librarySoundFonts(selectedCategoryId(categoryBox));
        libraryBox.clear(juce::dontSendNotification);
        const auto currentPath = processor.soundFontPath(index);
        int selectedId = 0;
        for (int i = 0; i < libraryFiles.size(); ++i)
        {
            const auto& file = libraryFiles.getReference(i);
            libraryBox.addItem(file.getFileNameWithoutExtension(), i + 1);
            if (file.getFullPathName() == currentPath) selectedId = i + 1;
        }
        libraryBox.setTextWhenNothingSelected("ESCOLHA O SF2");
        if (selectedId > 0)
            libraryBox.setSelectedId(selectedId, juce::dontSendNotification);
        rebuildPresets();
    }
    void rebuildPresets()
    {
        presets.clear(); presetBox.clear(juce::dontSendNotification);
        presets = processor.layerPresets(index);
        for (int i = 0; i < (int) presets.size(); ++i) presetBox.addItem(juce::String(presets[(size_t) i].bank) + ": " + presets[(size_t) i].name, i + 1);
        const auto selectedBank = processor.layerPresetBank(index);
        const auto selectedProgram = processor.layerPresetProgram(index);
        for (int i = 0; i < (int) presets.size(); ++i)
            if (presets[(size_t) i].bank == selectedBank && presets[(size_t) i].program == selectedProgram)
            {
                presetBox.setSelectedId(i + 1, juce::dontSendNotification);
                break;
            }
    }
    void refreshFromProcessor()
    {
        const auto config = processor.layerConfig(index);
        modeBox.setSelectedId(config.portamento ? 3 : config.mono ? 2 : 1, juce::dontSendNotification);
        sustainBox.setSelectedId(config.sustainEnabled ? 1 : 2, juce::dontSendNotification);
        channelBox.setSelectedId(config.midiChannel + 1, juce::dontSendNotification);
        octaveBox.setSelectedId(config.octave + 5, juce::dontSendNotification);
        lowNoteBox.setSelectedId(config.lowNote + 1, juce::dontSendNotification);
        highNoteBox.setSelectedId(config.highNote + 1, juce::dontSendNotification);
        velocityBox.setSelectedId(config.velocityCurve + 1, juce::dontSendNotification);
        const auto selectedDevice = processor.layerMidiDevice(index);
        int selectedDeviceId = 1;
        const auto devices = processor.availableMidiDevices();
        for (int device = 0; device < devices.size(); ++device)
            if (devices.getReference(device).identifier == selectedDevice)
            {
                selectedDeviceId = device + 2;
                break;
            }
        deviceBox.setSelectedId(selectedDeviceId, juce::dontSendNotification);
        selectCurrentCategory();
        rebuildLibrary();
    }
    void applyRouting()
    {
        auto config = processor.layerConfig(index);
        config.mono = modeBox.getSelectedId() == 2; config.portamento = modeBox.getSelectedId() == 3;
        config.sustainEnabled = sustainBox.getSelectedId() != 2; config.midiChannel = channelBox.getSelectedId() - 1;
        config.octave = octaveBox.getSelectedId() - 5; config.lowNote = lowNoteBox.getSelectedId() - 1; config.highNote = highNoteBox.getSelectedId() - 1;
        config.velocityCurve = velocityBox.getSelectedId() - 1; processor.setLayerConfig(index, config);
    }
    ClassicPlayerAudioProcessor& processor; int index;
    bool compactLayout = false;
    juce::Label categoryLabel, libraryLabel, presetLabel, modeLabel, sustainLabel, channelLabel, deviceLabel, octaveLabel, rangeLabel, velocityLabel;
    juce::ComboBox categoryBox, libraryBox, presetBox, modeBox, sustainBox, channelBox, deviceBox, octaveBox, lowNoteBox, highNoteBox, velocityBox;
    juce::TextButton importButton, deleteButton; juce::Array<juce::File> libraryFiles; std::vector<Sf2Engine::Preset> presets;
    std::unique_ptr<juce::FileChooser> chooser;
};

class LayerRoutingEditorPanel final : public juce::Component
{
public:
    LayerRoutingEditorPanel(ClassicPlayerAudioProcessor& p, int layer)
        : processor(p), index(layer)
    {
        deviceBox.getProperties().set("uiDataItems", true);
        const std::array<std::pair<juce::Label*, const char*>, 8> fields {{
            { &modeLabel, "MODO" }, { &sustainLabel, "SUSTAIN" },
            { &channelLabel, "CANAL MIDI" }, { &deviceLabel, "ENTRADA MIDI" },
            { &octaveLabel, "OITAVA" }, { &lowNoteLabel, "NOTA BAIXA" },
            { &highNoteLabel, "NOTA ALTA" }, { &velocityLabel, "VELOCIDADE" }
        }};
        for (const auto& field : fields)
        {
            field.first->setText(field.second, juce::dontSendNotification);
            field.first->setColour(juce::Label::textColourId, juce::Colour(text));
            field.first->setFont(juce::FontOptions(10.0f, juce::Font::bold));
            addAndMakeVisible(*field.first);
        }
        for (auto* box : { &modeBox, &sustainBox, &channelBox, &deviceBox,
                           &octaveBox, &lowNoteBox, &highNoteBox, &velocityBox })
        {
            box->setColour(juce::ComboBox::backgroundColourId, juce::Colour(panelLight));
            box->setColour(juce::ComboBox::textColourId, juce::Colour(text));
            addAndMakeVisible(*box);
        }
        modeBox.addItem("POLI", 1); modeBox.addItem("MONO / LEGATO", 2); modeBox.addItem("PORTAMENTO", 3);
        sustainBox.addItem("SUSTAIN ON", 1); sustainBox.addItem("SUSTAIN OFF", 2);
        channelBox.addItem("MIDI OMNI", 1);
        for (int channel = 1; channel <= 16; ++channel)
            channelBox.addItem("MIDI CH " + juce::String(channel), channel + 1);
        deviceBox.addItem("TODOS OS CONTROLADORES", 1);
        for (const auto& device : processor.availableMidiDevices())
            deviceBox.addItem(device.name, deviceBox.getNumItems() + 1);
        for (int value = -4; value <= 4; ++value)
            octaveBox.addItem((value > 0 ? "+" : "") + juce::String(value) + " OIT", value + 5);
        for (int note = 0; note < 128; ++note)
        {
            lowNoteBox.addItem(midiNoteName(note), note + 1);
            highNoteBox.addItem(midiNoteName(note), note + 1);
        }
        velocityBox.addItem("VEL LINEAR", 1); velocityBox.addItem("VEL SOFT", 2); velocityBox.addItem("VEL HARD", 3);
        for (auto* box : { &modeBox, &sustainBox, &channelBox, &octaveBox,
                           &lowNoteBox, &highNoteBox, &velocityBox })
            box->onChange = [this] { applyRouting(); };
        deviceBox.onChange = [this]
        {
            const auto selected = deviceBox.getSelectedId() - 2;
            const auto devices = processor.availableMidiDevices();
            processor.setLayerMidiDevice(index, juce::isPositiveAndBelow(selected, devices.size())
                                                   ? devices.getReference(selected).identifier : juce::String{});
        };
        refresh();
        if(processor.layerType(index)==ClassicPlayerAudioProcessor::LayerType::hammond){
            modeBox.setSelectedId(1,juce::dontSendNotification);modeBox.setEnabled(false);
            velocityBox.setEnabled(false);
        }
        setSize(700, 170);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        const auto cellWidth = area.getWidth() / 4;
        auto place = [](juce::Rectangle<int> cell, juce::Label& label, juce::ComboBox& box)
        {
            label.setBounds(cell.removeFromTop(18));
            box.setBounds(cell.reduced(2, 1));
        };
        const auto rowHeight = juce::jmax(1, area.getHeight() / 2);
        for (int row = 0; row < 2; ++row)
        {
            auto line = area.removeFromTop(rowHeight);
            if (row == 0)
            {
                place(line.removeFromLeft(cellWidth), modeLabel, modeBox);
                place(line.removeFromLeft(cellWidth), sustainLabel, sustainBox);
                place(line.removeFromLeft(cellWidth), channelLabel, channelBox);
                place(line, deviceLabel, deviceBox);
            }
            else
            {
                place(line.removeFromLeft(cellWidth), octaveLabel, octaveBox);
                place(line.removeFromLeft(cellWidth), lowNoteLabel, lowNoteBox);
                place(line.removeFromLeft(cellWidth), highNoteLabel, highNoteBox);
                place(line, velocityLabel, velocityBox);
            }
        }
    }

private:
    void refresh()
    {
        const auto config = processor.layerConfig(index);
        modeBox.setSelectedId(config.portamento ? 3 : config.mono ? 2 : 1, juce::dontSendNotification);
        sustainBox.setSelectedId(config.sustainEnabled ? 1 : 2, juce::dontSendNotification);
        channelBox.setSelectedId(config.midiChannel + 1, juce::dontSendNotification);
        octaveBox.setSelectedId(config.octave + 5, juce::dontSendNotification);
        lowNoteBox.setSelectedId(config.lowNote + 1, juce::dontSendNotification);
        highNoteBox.setSelectedId(config.highNote + 1, juce::dontSendNotification);
        velocityBox.setSelectedId(config.velocityCurve + 1, juce::dontSendNotification);
    }

    void applyRouting()
    {
        auto config = processor.layerConfig(index);
        config.mono = modeBox.getSelectedId() == 2;
        config.portamento = modeBox.getSelectedId() == 3;
        config.sustainEnabled = sustainBox.getSelectedId() != 2;
        config.midiChannel = channelBox.getSelectedId() - 1;
        config.octave = octaveBox.getSelectedId() - 5;
        config.lowNote = lowNoteBox.getSelectedId() - 1;
        config.highNote = highNoteBox.getSelectedId() - 1;
        config.velocityCurve = velocityBox.getSelectedId() - 1;
        processor.setLayerConfig(index, config);
    }

    ClassicPlayerAudioProcessor& processor;
    int index = 0;
    juce::Label modeLabel, sustainLabel, channelLabel, deviceLabel, octaveLabel,
                lowNoteLabel, highNoteLabel, velocityLabel;
    juce::ComboBox modeBox, sustainBox, channelBox, deviceBox, octaveBox,
                   lowNoteBox, highNoteBox, velocityBox;
};

class LayerMidiLearnPanel final : public juce::Component, private juce::Timer
{
public:
    LayerMidiLearnPanel(ClassicPlayerAudioProcessor& p, int layer)
        : processor(p), index(layer)
    {
        const std::array<const char*, 5> names {{ "VOLUME", "CUTOFF", "REVERB", "COMP", "MUTE" }};
        const std::array<ClassicPlayerAudioProcessor::LearnTarget, 5> targets {{
            ClassicPlayerAudioProcessor::LearnTarget::volume,
            ClassicPlayerAudioProcessor::LearnTarget::cutoff,
            ClassicPlayerAudioProcessor::LearnTarget::reverb,
            ClassicPlayerAudioProcessor::LearnTarget::compressor,
            ClassicPlayerAudioProcessor::LearnTarget::mute
        }};
        for (int i = 0; i < 5; ++i)
        {
            labels[(size_t) i].setText(names[(size_t) i], juce::dontSendNotification);
            labels[(size_t) i].setColour(juce::Label::textColourId, juce::Colour(text));
            labels[(size_t) i].setFont(juce::FontOptions(9.0f, juce::Font::bold));
            labels[(size_t) i].setJustificationType(juce::Justification::centred);
            addAndMakeVisible(labels[(size_t) i]);
            flatButton(buttons[(size_t) i]);
            buttons[(size_t) i].setButtonText("LEARN");
            buttons[(size_t) i].setTooltip(juce::String::fromUTF8("Clique novamente para cancelar; clique com o botão direito para excluir o mapeamento."));
            buttons[(size_t) i].onClick = [this, target = targets[(size_t) i]]
            {
                processor.beginMidiLearn(index, target);
                refresh();
            };
            buttons[(size_t) i].onClearMapping = [this, target = targets[(size_t) i]]
            {
                processor.clearMidiLearn(index, target);
                refresh();
            };
            addAndMakeVisible(buttons[(size_t) i]);
        }
        setSize(520, 52);
        refresh();
        startTimerHz(12);
    }

    ~LayerMidiLearnPanel() override { stopTimer(); }

    void timerCallback() override { refresh(); }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4, 2);
        const auto width = area.getWidth() / 5;
        for (int i = 0; i < 5; ++i)
        {
            auto cell = area.removeFromLeft(width);
            labels[(size_t) i].setBounds(cell.removeFromTop(18));
            buttons[(size_t) i].setBounds(cell.reduced(2, 1));
        }
    }

private:
    void refresh()
    {
        using Target = ClassicPlayerAudioProcessor::LearnTarget;
        const std::array<Target, 5> targets {{ Target::volume, Target::cutoff,
                                                Target::reverb, Target::compressor, Target::mute }};
        for (int i = 0; i < 5; ++i)
        {
            const auto cc = processor.midiLearnCC(index, targets[(size_t) i]);
            const auto channel = processor.midiLearnChannel(index, targets[(size_t) i]);
            const auto learning = processor.isMidiLearning(index, targets[(size_t) i]);
            buttons[(size_t) i].setButtonText(localizedUiText(learning ? "MOVA O CC"
                : cc < 0 ? "LEARN" : "CC " + juce::String(cc)
                    + (channel > 0 ? " C" + juce::String(channel) : juce::String{}),
                activeUiLanguage.load()));
        }
    }

    ClassicPlayerAudioProcessor& processor;
    int index = 0;
    std::array<juce::Label, 5> labels;
    std::array<MidiLearnButton, 5> buttons;
};

class Dx7EditorPanel final : public juce::Component
{
public:
    static constexpr int preferredHeight = 70; // Two full-height rows with compact insets.

    Dx7EditorPanel(ClassicPlayerAudioProcessor& p, int layer) : processor(p), index(layer)
    {
        bankBox.getProperties().set("uiDataItems", true);
        patchBox.getProperties().set("uiDataItems", true);
        patchBox.setLookAndFeel(&dx7PatchLookAndFeel);
        bankLabel.setText("BANCO DX7", juce::dontSendNotification);
        patchLabel.setText("TIMBRE DX7", juce::dontSendNotification);
        for (auto* label : { &bankLabel, &patchLabel })
        {
            label->setColour(juce::Label::textColourId, juce::Colour(text));
            label->setFont(juce::FontOptions(12.0f, juce::Font::bold));
            addAndMakeVisible(*label);
        }
        flatButton(importButton);
        flatButton(deleteButton);
        importButton.setButtonText("IMPORTAR DX7");
        deleteButton.setButtonText("EXCLUIR DX7");
        importButton.setTooltip("Importar banco ou voz DX7 em formato SysEx (.syx)");
        deleteButton.setTooltip("Excluir o banco DX7 selecionado da biblioteca");
        importButton.onClick = [this] { chooseDx7(); };
        deleteButton.onClick = [this] { deleteSelectedBank(); };
        addAndMakeVisible(importButton);
        addAndMakeVisible(deleteButton);
        rebuildBanks();
        bankBox.onChange = [this]
        {
            const auto selected = bankBox.getSelectedItemIndex();
            const auto banksNow = processor.libraryDx7Banks();
            deleteButton.setEnabled(juce::isPositiveAndBelow(selected, banksNow.size()));
            if (juce::isPositiveAndBelow(selected, banksNow.size()))
            {
                processor.loadDx7(index, banksNow.getReference(selected));
                rebuildPatches();
            }
        };
        patchBox.onChange = [this]
        {
            const auto selected = patchBox.getSelectedItemIndex();
            if (selected >= 0) processor.selectDx7Patch(index, selected);
        };
        addAndMakeVisible(bankBox);
        addAndMakeVisible(patchBox);
        rebuildPatches();
        setSize(560, preferredHeight);
    }

    ~Dx7EditorPanel() override { patchBox.setLookAndFeel(nullptr); }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12, 6);
        auto row = area.removeFromTop(22);
        bankLabel.setBounds(row.removeFromLeft(100));
        deleteButton.setBounds(row.removeFromRight(112).reduced(1, 0));
        importButton.setBounds(row.removeFromRight(112).reduced(1, 0));
        bankBox.setBounds(row);
        const auto bankFieldWidth = bankBox.getWidth();
        area.removeFromTop(8);
        row = area.removeFromTop(22);
        patchLabel.setBounds(row.removeFromLeft(100));
        patchBox.setBounds(row.removeFromLeft(bankFieldWidth));
    }

private:
    void deleteSelectedBank()
    {
        const auto selected = bankBox.getSelectedItemIndex();
        const auto banks = processor.libraryDx7Banks();
        if (!juce::isPositiveAndBelow(selected, banks.size())) return;
        const auto file = banks.getReference(selected);
        const juce::Component::SafePointer<Dx7EditorPanel> safe(this);
        juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::WarningIcon, "Excluir DX7",
            "Excluir '" + file.getFileName() + "' da biblioteca?",
            "Excluir", "Cancelar", this,
            juce::ModalCallbackFunction::create([safe, file](int answer)
            {
                if (safe == nullptr || answer == 0) return;
                const auto result = safe->processor.deleteLibraryDx7Bank(file);
                if (result.failed())
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon, "Falha ao excluir DX7",
                        result.getErrorMessage());
                safe->rebuildBanks();
                safe->rebuildPatches();
            }));
    }

    void rebuildBanks()
    {
        bankBox.clear(juce::dontSendNotification);
        const auto banks = processor.libraryDx7Banks();
        for (int i = 0; i < banks.size(); ++i)
            bankBox.addItem(banks.getReference(i).getFileNameWithoutExtension(), i + 1);
        bankBox.setTextWhenNothingSelected("BIBLIOTECA DX7 VAZIA");
        const auto selected = processor.dx7Path(index).isNotEmpty()
            ? banks.indexOf(juce::File(processor.dx7Path(index))) : -1;
        bankBox.setSelectedItemIndex(selected, juce::dontSendNotification);
        deleteButton.setEnabled(juce::isPositiveAndBelow(selected, banks.size()));
    }

    void chooseDx7()
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            localizedUiText("Escolha um arquivo DX7 SysEx", activeUiLanguage.load()), juce::File{}, "*.syx;*.SYX");
        fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();
                if (file == juce::File{}) return;
                importButton.setEnabled(false);
                juce::File importedFile;
                auto result = processor.importDx7Bank(file, importedFile);
                if (result.wasOk())
                    result = processor.loadDx7(index, importedFile);
                importButton.setEnabled(true);
                if (result.failed())
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        "Falha ao carregar DX7", result.getErrorMessage());
                rebuildBanks();
                rebuildPatches();
            });
    }

    void rebuildPatches()
    {
        patchBox.clear(juce::dontSendNotification);
        const auto count = processor.dx7PatchCount(index);
        for (int patch = 0; patch < count; ++patch)
            patchBox.addItem(juce::String(patch + 1) + ": " + processor.dx7PatchName(index, patch), patch + 1);
        if (count > 0)
            patchBox.setSelectedId(processor.dx7SelectedPatch(index) + 1, juce::dontSendNotification);
    }

    ClassicPlayerAudioProcessor& processor;
    int index;
    juce::Label bankLabel, patchLabel;
    juce::ComboBox bankBox, patchBox;
    juce::TextButton importButton;
    juce::TextButton deleteButton;
    std::unique_ptr<juce::FileChooser> fileChooser;
};

class ColourPicker final : public juce::Component, private juce::ChangeListener
{
public:
    ColourPicker(juce::Colour initial, std::function<void(juce::Colour)> changed,
                 std::function<void(juce::Colour)> committed = {})
        : callback(std::move(changed)), commitCallback(std::move(committed))
    {
        selector.setCurrentColour(initial);
        selector.setColour(juce::ColourSelector::backgroundColourId, juce::Colour(panel));
        selector.addChangeListener(this);
        addAndMakeVisible(selector);
        setSize(300, 300);
    }

    ~ColourPicker() override
    {
        selector.removeChangeListener(this);
        if (commitCallback && colourWasChanged) commitCallback(selector.getCurrentColour());
    }
    void resized() override { selector.setBounds(getLocalBounds()); }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        colourWasChanged = true;
        if (callback) callback(selector.getCurrentColour());
    }

    juce::ColourSelector selector { juce::ColourSelector::showColourAtTop |
                                    juce::ColourSelector::showSliders |
                                    juce::ColourSelector::showColourspace };
    std::function<void(juce::Colour)> callback;
    std::function<void(juce::Colour)> commitCallback;
    bool colourWasChanged = false;
};
}

ClassicPlayerAudioProcessorEditor::LanguageFlags::LanguageFlags()
{
    for (int i = 0; i < 3; ++i)
    {
        buttons[(size_t) i] = std::make_unique<Flag>(i);
        buttons[(size_t) i]->onClick = [this, i]
        {
            setSelectedId(i + 1, juce::dontSendNotification);
            if (onChange) onChange();
        };
        addAndMakeVisible(*buttons[(size_t) i]);
    }
}
void ClassicPlayerAudioProcessorEditor::LanguageFlags::addItem(const juce::String& name, int id)
{
    if (!juce::isPositiveAndBelow(id - 1, 3)) return;
    buttons[(size_t) (id - 1)]->setName(name);
    buttons[(size_t) (id - 1)]->setTooltip(name);
}
void ClassicPlayerAudioProcessorEditor::LanguageFlags::setSelectedId(int id, juce::NotificationType notification)
{
    if (id < 1 || id > 3) return;
    selected = id;
    for (int i = 0; i < 3; ++i)
        buttons[(size_t) i]->setToggleState(i + 1 == id, juce::dontSendNotification);
    if (notification != juce::dontSendNotification && onChange) onChange();
}
void ClassicPlayerAudioProcessorEditor::LanguageFlags::resized()
{
    for (int i = 0; i < 3; ++i)
        buttons[(size_t) i]->setBounds(i * getWidth() / 3, 0,
            (i + 1) * getWidth() / 3 - i * getWidth() / 3, getHeight());
}
void ClassicPlayerAudioProcessorEditor::LanguageFlags::Flag::paintButton(juce::Graphics& g, bool over, bool)
{
    auto area = getLocalBounds().toFloat().reduced(2.0f);
    g.setColour(juce::Colour(panelLight).brighter(over ? 0.12f : 0.0f));
    g.fillRoundedRectangle(area, 3.0f);
    g.setColour(juce::Colour(getToggleState() ? teal : paletteLine));
    g.drawRoundedRectangle(area, 3.0f, getToggleState() ? 2.0f : 1.0f);
    auto flag = area.reduced(4.0f, 3.0f);
    auto label = flag.removeFromBottom(9.0f);
    flag = flag.withSizeKeepingCentre(juce::jmin(27.0f, flag.getWidth()), juce::jmin(15.0f, flag.getHeight()));
    if (index == 0)
    {
        g.setColour(juce::Colour(0xff009b3a)); g.fillRect(flag);
        juce::Path diamond;
        diamond.startNewSubPath(flag.getCentreX(), flag.getY() + 1);
        diamond.lineTo(flag.getRight() - 1, flag.getCentreY());
        diamond.lineTo(flag.getCentreX(), flag.getBottom() - 1);
        diamond.lineTo(flag.getX() + 1, flag.getCentreY()); diamond.closeSubPath();
        g.setColour(juce::Colour(0xffffdf00)); g.fillPath(diamond);
        g.setColour(juce::Colour(0xff002776)); g.fillEllipse(flag.withSizeKeepingCentre(8.0f, 8.0f));
    }
    else if (index == 1)
    {
        g.setColour(juce::Colours::white); g.fillRect(flag);
        g.setColour(juce::Colour(0xffb22234));
        for (int stripe = 0; stripe < 13; stripe += 2)
            g.fillRect(flag.getX(), flag.getY() + flag.getHeight() * (float) stripe / 13.0f,
                       flag.getWidth(), flag.getHeight() / 13.0f);
        g.setColour(juce::Colour(0xff3c3b6e));
        g.fillRect(flag.withWidth(flag.getWidth() * 0.45f).withHeight(flag.getHeight() * 0.54f));
        g.setColour(juce::Colours::white);
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column)
                g.fillEllipse(flag.getX() + 1 + (float) column * 2.5f,
                              flag.getY() + 1 + (float) row * 2.0f, 1.0f, 1.0f);
    }
    else
    {
        g.setColour(juce::Colour(0xffaa151b)); g.fillRect(flag);
        g.setColour(juce::Colour(0xfff1bf00)); g.fillRect(flag.reduced(0.0f, flag.getHeight() * 0.25f));
    }
    g.setColour(juce::Colour(uiPalettes[(size_t) activeUiPalette].text));
    g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
    g.drawText(index == 0 ? "BR" : index == 1 ? "US" : "ES", label, juce::Justification::centred);
}

void ClassicPlayerAudioProcessorEditor::LevelMeter::setLevel(float newLevel)
{
    newLevel = juce::jlimit(0.0f, 1.0f, newLevel);
    if (std::abs(newLevel - level) > 0.006f)
    {
        level = newLevel;
        repaint();
    }
}

void ClassicPlayerAudioProcessorEditor::LevelMeter::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff080c10));
    g.fillRect(bounds);
    g.setColour(juce::Colour(paletteLine));
    g.drawRect(bounds, 1.0f);
    auto fill = bounds.reduced(2.0f);
    fill.removeFromTop(fill.getHeight() * (1.0f - level));
    g.setColour(level > 0.86f ? juce::Colour(0xffe14d45)
                             : level > 0.66f ? juce::Colour(yellow) : juce::Colour(teal));
    g.fillRect(fill);
}

void ClassicPlayerAudioProcessorEditor::CpuMeter::setUsage(double usage)
{
    const auto next = juce::jlimit(0, 100, juce::roundToInt(usage));
    if (percentage != next)
    {
        percentage = next;
        repaint();
    }
}

void ClassicPlayerAudioProcessorEditor::CpuMeter::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(panel));
    g.fillRoundedRectangle(bounds, 4.0f);
    g.setColour(juce::Colour(paletteLine));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.0f);
    auto bar = bounds.reduced(5.0f, 4.0f);
    auto label = bar.removeFromLeft(62.0f);
    g.setColour(juce::Colour(text));
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText("CPU " + juce::String(percentage) + "%", label, juce::Justification::centredLeft);
    g.setColour(juce::Colour(background));
    g.fillRoundedRectangle(bar, 2.0f);
    g.setColour(percentage >= 90 ? juce::Colour(0xffe14d45)
                               : percentage >= 70 ? juce::Colour(yellow) : juce::Colour(teal));
    g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * (float) percentage / 100.0f), 2.0f);
}

ClassicPlayerAudioProcessorEditor::NamedKeyboard::NamedKeyboard(juce::MidiKeyboardState& state)
    : MidiKeyboardComponent(state, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    setAvailableRange(21, 108);
    setLowestVisibleKey(21);
    setScrollButtonsVisible(false);
    setColour(keyDownOverlayColourId, juce::Colour(0xff1976d2));
    setColour(mouseOverKeyOverlayColourId, juce::Colour(0x331976d2));
    setColour(textLabelColourId, juce::Colour(0xff15212a));
}

juce::String ClassicPlayerAudioProcessorEditor::NamedKeyboard::noteLabel(int note)
{
    static const char* names[] { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    return names[note % 12];
}

void ClassicPlayerAudioProcessorEditor::NamedKeyboard::drawWhiteNote(
    int note, juce::Graphics& g, juce::Rectangle<float> area, bool isDown, bool isOver,
    juce::Colour lineColour, juce::Colour)
{
    auto fill = juce::Colour(0xfff4f4ef);
    if (isDown) fill = findColour(keyDownOverlayColourId);
    else if (isOver) fill = fill.interpolatedWith(findColour(mouseOverKeyOverlayColourId), 0.55f);
    g.setColour(fill);
    g.fillRect(area);
    g.setColour(lineColour);
    g.drawRect(area, 1.0f);
    g.setColour(isDown ? juce::Colours::white : juce::Colour(0xff16222c));
    g.setFont(juce::FontOptions(12.5f, juce::Font::bold));
    g.drawText(noteLabel(note), area.removeFromBottom(20.0f), juce::Justification::centred);
}

void ClassicPlayerAudioProcessorEditor::NamedKeyboard::drawBlackNote(
    int, juce::Graphics& g, juce::Rectangle<float> area, bool isDown, bool isOver,
    juce::Colour)
{
    auto fill = juce::Colour(0xff10151a);
    if (isDown) fill = findColour(keyDownOverlayColourId);
    else if (isOver) fill = fill.interpolatedWith(findColour(mouseOverKeyOverlayColourId), 0.7f);
    g.setColour(fill);
    g.fillRoundedRectangle(area, 0.0f);
    g.setColour(juce::Colour(0xff56616a));
    g.drawRect(area, 1.0f);
}

void ClassicPlayerAudioProcessorEditor::NamedKeyboard::setActiveColour(juce::Colour colour)
{
    setColour(keyDownOverlayColourId, colour);
    setColour(mouseOverKeyOverlayColourId, colour.withAlpha(0.22f));
    repaint();
}

ClassicPlayerAudioProcessorEditor::DrumPadPanel::DrumPadPanel(ClassicPlayerAudioProcessor& p, int layer)
    : processor(p), layerIndex(layer)
{
    for (int pad = 0; pad < 12; ++pad)
    {
        auto& trigger = pads[(size_t) pad];
        trigger.setName("DRUM_PAD_" + juce::String(pad + 1));
        trigger.getProperties().set("uiDataText", true);
        trigger.setButtonText("PAD " + juce::String(pad + 1));
        trigger.setTooltip("Clique para tocar este pad");
        trigger.setColour(juce::TextButton::buttonColourId, drumPadColour(pad));
        trigger.setColour(juce::TextButton::buttonOnColourId, juce::Colour(yellow));
        trigger.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff15191d));
        trigger.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff15191d));
        trigger.onClick = [this, pad] { if(continuous())processor.continuousPads(layerIndex).trigger(pad);else processor.triggerDrumPad(pad); };
        addAndMakeVisible(trigger);

        auto& load = loadButtons[(size_t) pad];
        load.setButtonText("LOAD");
        load.setTooltip("Carregar MP3/WAV neste pad");
        flatButton(load);
        load.onClick = [this, pad] { chooseSample(pad); };
        addAndMakeVisible(load);

        auto& learn = learnButtons[(size_t) pad];
        learn.setButtonText("LEARN");
        learn.setTooltip(juce::String::fromUTF8("Clique para aprender; clique novamente para cancelar; botão direito para excluir o mapeamento."));
        flatButton(learn);
        learn.onClick = [this, pad] { if(continuous())processor.continuousPads(layerIndex).learn(pad);else processor.beginDrumPadMidiLearn(pad); refresh(); };
        learn.onClearMapping = [this, pad]
        {
            if (continuous()) processor.continuousPads(layerIndex).clearMapping(pad);
            else processor.clearDrumPadMidiLearn(pad);
            refresh();
        };
        addAndMakeVisible(learn);

        auto& padVolume = padVolumes[(size_t) pad];
        padVolume.setSliderStyle(juce::Slider::LinearHorizontal);
        padVolume.setRange(0.0, 100.0, 1.0);
        padVolume.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 18);
        padVolume.setTextValueSuffix("%");
        padVolume.setTooltip("Volume individual deste pad");
        padVolume.onValueChange = [this, pad]
        {
            if (!continuous())
                processor.setDrumPadVolume(pad, static_cast<float>(padVolumes[(size_t) pad].getValue() / 100.0));
        };
        addAndMakeVisible(padVolume);
    }
    flatButton(stopButton);addAndMakeVisible(stopButton);
    for(auto* button:{&stopLearn,&volumeLearnButton,&muteLearnButton}){flatButton(*button);addAndMakeVisible(*button);}
    stopButton.onClick=[this]{processor.continuousPads(layerIndex).stop();};
    stopLearn.onClick=[this]{processor.continuousPads(layerIndex).learn(12);refresh();};
    stopLearn.setTooltip(juce::String::fromUTF8("Clique novamente para cancelar; botão direito para excluir o mapeamento do STOP."));
    stopLearn.onClearMapping=[this]{processor.continuousPads(layerIndex).clearMapping(12);refresh();};
    volumeLearnButton.onClick=[this]{processor.beginMidiLearn(layerIndex,ClassicPlayerAudioProcessor::LearnTarget::volume);refresh();};
    volumeLearnButton.onClearMapping=[this]{processor.clearMidiLearn(layerIndex,ClassicPlayerAudioProcessor::LearnTarget::volume);refresh();};
    muteLearnButton.setTooltip("Aprender um CC para alternar o mute desta layer");
    muteLearnButton.onClick=[this]{processor.beginMidiLearn(layerIndex,ClassicPlayerAudioProcessor::LearnTarget::mute);refresh();};
    muteLearnButton.onClearMapping=[this]{processor.clearMidiLearn(layerIndex,ClassicPlayerAudioProcessor::LearnTarget::mute);refresh();};
    fadeSlider.setRange(.02,10.0,.01);fadeSlider.setTextValueSuffix(" s crossfade");
    fadeSlider.setValue(processor.continuousPads(layerIndex).fadeSeconds(),juce::dontSendNotification);
    fadeSlider.onValueChange=[this]{processor.continuousPads(layerIndex).setFadeSeconds(fadeSlider.getValue());};
    addAndMakeVisible(fadeSlider);
    setControlsVisible(true);refresh();startTimerHz(15);
}

void ClassicPlayerAudioProcessorEditor::DrumPadPanel::setControlsVisible(bool shouldShow)
{
    controlsVisible = shouldShow;
    for (int pad = 0; pad < 12; ++pad)
    {
        pads[(size_t)pad].setVisible(pad<padCount());
        loadButtons[(size_t) pad].setVisible(controlsVisible && pad<padCount());
        learnButtons[(size_t) pad].setVisible(controlsVisible && pad<padCount());
        padVolumes[(size_t) pad].setVisible(controlsVisible && !continuous() && pad<padCount());
    }
    stopButton.setVisible(continuous());stopLearn.setVisible(controlsVisible&&continuous());
    fadeSlider.setVisible(controlsVisible&&continuous());volumeLearnButton.setVisible(controlsVisible);
    muteLearnButton.setVisible(controlsVisible);
    resized();
}

void ClassicPlayerAudioProcessorEditor::DrumPadPanel::chooseSample(int pad)
{
    fileChooser = std::make_unique<juce::FileChooser>(
        localizedUiText("Carregar áudio do pad", activeUiLanguage.load()), juce::File{}, "*.mp3;*.wav;*.aiff;*.flac");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles,
        [this, pad](const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();
            if (file.existsAsFile())
            {
                const auto result = continuous() ? processor.continuousPads(layerIndex).load(pad,file) : processor.loadDrumPad(pad, file);
                if (result.failed())
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon, "Drum pad", result.getErrorMessage());
                refresh();
            }
        });
}

void ClassicPlayerAudioProcessorEditor::DrumPadPanel::refresh()
{
    const auto volumeCC=processor.midiLearnCC(layerIndex,ClassicPlayerAudioProcessor::LearnTarget::volume);
    setButtonTextIfChanged(volumeLearnButton, localizedUiText(
        processor.isMidiLearning(layerIndex,ClassicPlayerAudioProcessor::LearnTarget::volume)
            ? "VOLUME: MOVA O CC" : volumeCC>=0 ? "VOLUME: CC "+juce::String(volumeCC) : "LEARN VOLUME",
        activeUiLanguage.load()));
    const auto muteTarget = ClassicPlayerAudioProcessor::LearnTarget::mute;
    const auto muteCC = processor.midiLearnCC(layerIndex, muteTarget);
    setButtonTextIfChanged(muteLearnButton, localizedUiText(
        processor.isMidiLearning(layerIndex, muteTarget) ? "MUTE: MOVA O CC"
            : muteCC >= 0 ? "MUTE: CC " + juce::String(muteCC) : "LEARN MUTE",
        activeUiLanguage.load()));
    if(continuous())
    {
        const int cc=processor.continuousPads(layerIndex).mapping(12);
        setButtonTextIfChanged(stopLearn, localizedUiText(
            processor.continuousPads(layerIndex).learningTarget()==12 ? "STOP: MOVA O CC"
                : cc>=0 ? "STOP: CC "+juce::String(cc) : "LEARN STOP",
            activeUiLanguage.load()));
    }
    for (int pad = 0; pad < padCount(); ++pad)
    {
        auto& trigger = pads[(size_t) pad];
        const auto active = continuous() ? processor.continuousPads(layerIndex).selected()==pad : processor.isDrumPadPlaying(pad);
        const auto samplePath = continuous() ? processor.continuousPads(layerIndex).path(pad) : processor.drumPadPath(pad);
        setButtonTextIfChanged(trigger, samplePath.isNotEmpty() ? juce::File(samplePath).getFileNameWithoutExtension()
                                                      : "PAD " + juce::String(pad + 1));
        setButtonColourIfChanged(trigger, juce::TextButton::buttonColourId,
                          active ? juce::Colour(yellow) : drumPadColour(pad));
        setButtonColourIfChanged(trigger, juce::TextButton::textColourOffId, juce::Colour(0xff101820));
        const int cc=continuous()?processor.continuousPads(layerIndex).mapping(pad):-1;
        const auto mapping = continuous() ? (cc>=0?"CC "+juce::String(cc):juce::String{}) : processor.drumPadMidiMapping(pad);
        setButtonTextIfChanged(learnButtons[(size_t) pad], localizedUiText(
            (continuous()?processor.continuousPads(layerIndex).learningTarget()==pad:processor.isDrumPadMidiLearning(pad)) ? "MOVA PAD"
            : mapping.isNotEmpty() ? mapping : "LEARN", activeUiLanguage.load()));
        if (!continuous() && !padVolumes[(size_t) pad].isMouseButtonDown()
            && std::abs(padVolumes[(size_t) pad].getValue() - processor.drumPadVolume(pad) * 100.0f) > 0.0001)
            padVolumes[(size_t) pad].setValue(processor.drumPadVolume(pad) * 100.0f,
                                               juce::dontSendNotification);
    }
}

void ClassicPlayerAudioProcessorEditor::DrumPadPanel::resized()
{
    auto available=getLocalBounds();
    if(controlsVisible)
    {
        muteLearnButton.setBounds(available.removeFromBottom(28).reduced(3));
        volumeLearnButton.setBounds(available.removeFromBottom(28).reduced(3));
        if(continuous())
        {fadeSlider.setBounds(available.removeFromBottom(32));stopLearn.setBounds(available.removeFromBottom(28).reduced(3));}
    }
    if(continuous())stopButton.setBounds(available.removeFromBottom(30).reduced(3));
    const int columns = continuous()?3:2;
    const auto cellWidth = juce::jmax(1, getWidth() / columns);
    // Derive the row height from the panel itself.  A fixed row size made the
    // fourth row extend below compact mixer layers and clip the pads.  The
    // pad body is then explicitly constrained to a square inside each cell.
    const auto rowHeight = juce::jmax(1, available.getHeight() / 4);
    for (int pad = 0; pad < padCount(); ++pad)
    {
        const auto column = pad % columns;
        const auto row = pad / columns;
        const auto cell = juce::Rectangle<int>(column * cellWidth, row * rowHeight,
                                               cellWidth, rowHeight);
        auto cellInner = cell.reduced(5, 3);
        // Reserve the control row before centering the pad.  Centering against
        // the full cell made the square extend into LOAD/LEARN and looked
        // vertically misaligned in the editor dialog.
        auto padAreaBounds = cellInner;
        juce::Rectangle<int> buttons;
        juce::Rectangle<int> volumeRow;
        if (controlsVisible)
        {
            buttons = padAreaBounds.removeFromBottom(25);
            if (!continuous())
                volumeRow = padAreaBounds.removeFromBottom(22);
        }
        // Wide, shallow pad bodies keep each LOAD/LEARN pair in its own column.
        const auto padHeight = juce::jmin(86, padAreaBounds.getHeight());
        auto padArea = padAreaBounds.withHeight(padHeight);
        padArea.setY(cellInner.getY());
        if (controlsVisible)
        {
            pads[(size_t) pad].setBounds(padArea);
            padVolumes[(size_t) pad].setBounds(volumeRow.reduced(1, 1));
            loadButtons[(size_t) pad].setBounds(buttons.removeFromLeft(buttons.getWidth() / 2).reduced(1, 0));
            learnButtons[(size_t) pad].setBounds(buttons.reduced(1, 0));
        }
        else
        {
            pads[(size_t) pad].setBounds(padArea);
            padVolumes[(size_t) pad].setBounds({});
            loadButtons[(size_t) pad].setBounds({});
            learnButtons[(size_t) pad].setBounds({});
        }
    }
}

ClassicPlayerAudioProcessorEditor::LayerStrip::LayerStrip(
    ClassicPlayerAudioProcessor& p, int layerIndex, std::function<void()> mixChanged)
    : processor(p), index(layerIndex), mixStateChanged(std::move(mixChanged)), drumPadPanel(p,layerIndex)
{
        outlineColour = readLayerOutlinePreference(index, hasCustomOutlineColour);
        for (auto* box : { &externalInstrumentBox, &dx7LibraryBox, &dx7PatchBox,
                       &libraryBox, &presetBox, &midiDevice })
        box->getProperties().set("uiDataItems", true);
    layerTitle.setText("LAYER " + juce::String(index + 1), juce::dontSendNotification);
    layerTitle.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    layerTitle.setColour(juce::Label::textColourId, juce::Colour(text));
    layerTitle.setTooltip("Arraste o nome para mudar a ordem das layers");
    layerTitle.setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    addAndMakeVisible(layerTitle);
    addMouseListener(this, true);
    addAndMakeVisible(drumPadPanel);
    drumPadPanel.setVisible(false);

    for (auto* button : { &muteButton, &soloButton, &resetButton, &editButton, &loadButton,
                          &externalInstrumentButton, &dx7Button, &deleteDx7LibraryButton, &openExternalEditorButton, &deleteLibraryButton })
    {
        flatButton(*button);
        addAndMakeVisible(*button);
    }
    muteButton.setClickingTogglesState(true);
    soloButton.setClickingTogglesState(true);
    muteButton.setTooltip("Ativar ou silenciar esta layer; o volume do fader permanece salvo");
    muteButton.onClick = [this]
    {
        muted = muteButton.getToggleState();
        processor.setLayerMuted(index, muted);
        mixStateChanged();
    };
    soloButton.onClick = [this] { solo = soloButton.getToggleState(); mixStateChanged(); };
    flatButton(modulationButton);
    addAndMakeVisible(modulationButton);
    modulationButton.setClickingTogglesState(true);
    modulationButton.setTooltip(juce::String::fromUTF8("Ativa ou desativa a modulação recebida pelo teclado nesta layer"));
    modulationButton.onClick = [this]
    {
        if (processor.layerType(index) != ClassicPlayerAudioProcessor::LayerType::sf2
            && processor.layerType(index) != ClassicPlayerAudioProcessor::LayerType::dx7) return;
        if (auto* parameter = processor.parameters.getParameter(
                "layer" + juce::String(index + 1) + "ModulationEnabled"))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(modulationButton.getToggleState() ? 1.0f : 0.0f));
        refresh();
    };
    resetButton.onClick = [this] { resetLayer(); };
    editButton.setTooltip("Mostrar ou ocultar os controles desta layer");
    editButton.onClick = [this]
    {
        if(processor.layerType(index)==ClassicPlayerAudioProcessor::LayerType::hammond){showHammondEditor();return;}
        if (processor.layerType(index) == ClassicPlayerAudioProcessor::LayerType::analog)
        {
            // Analog already has a dedicated, independent editor window.
            showAnalogSynthEditor();
            return;
        }
        if (processor.layerType(index) == ClassicPlayerAudioProcessor::LayerType::dx7)
        {
            showDx7Editor();
            return;
        }
        if (processor.layerType(index) == ClassicPlayerAudioProcessor::LayerType::drumPads || processor.layerType(index) == ClassicPlayerAudioProcessor::LayerType::continuousPads)
        {
            showDrumPadEditor();
            return;
        }
        showLayerEditor();
    };
    loadButton.onClick = [this] { chooseSoundFont(); };
    externalInstrumentButton.onClick = [this] { chooseExternalInstrument(); };
    dx7Button.onClick = [this]
    {
        if(processor.layerType(index)==ClassicPlayerAudioProcessor::LayerType::hammond){showHammondEditor();return;}
        if (processor.layerType(index) == ClassicPlayerAudioProcessor::LayerType::analog)
            showAnalogSynthEditor();
        else
            chooseDx7();
    };
    openExternalEditorButton.onClick = [this] { openExternalInstrumentEditor(); };
    externalInstrumentButton.setTooltip("Escolher manualmente um instrumento VST3/AU");
    openExternalEditorButton.setTooltip(juce::String::fromUTF8("Abrir a janela de configuração do instrumento virtual"));
    dx7Button.setTooltip("Importar banco ou voz DX7 em formato SysEx (.syx)");
    deleteDx7LibraryButton.setTooltip("Excluir o banco DX7 selecionado da biblioteca");
    deleteDx7LibraryButton.onClick = [this] { deleteSelectedDx7Bank(); };
    dx7LibraryBox.setTextWhenNothingSelected("BIBLIOTECA DX7 VAZIA");
    dx7LibraryBox.onChange = [this]
    {
        const auto selected = dx7LibraryBox.getSelectedItemIndex();
        deleteDx7LibraryButton.setEnabled(juce::isPositiveAndBelow(selected, dx7LibraryFiles.size()));
        if (!juce::isPositiveAndBelow(selected, dx7LibraryFiles.size())) return;
        const auto result = processor.loadDx7(index, dx7LibraryFiles.getReference(selected));
        if (result.failed())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "Falha ao carregar DX7", result.getErrorMessage());
        refresh();
    };
    dx7PatchBox.setTextWhenNothingSelected("SELECIONE O TIMBRE DX7");
    dx7PatchBox.onChange = [this]
    {
        const auto patch = dx7PatchBox.getSelectedItemIndex();
        if (processor.selectDx7Patch(index, patch)) refresh();
    };
    addAndMakeVisible(dx7LibraryBox);
    addAndMakeVisible(dx7PatchBox);
    addAndMakeVisible(deleteDx7LibraryButton);
    const auto canHost = processor.supportsExternalInstruments();
    externalInstrumentButton.setVisible(canHost);
    openExternalEditorButton.setVisible(canHost);
    externalInstrumentBox.setVisible(canHost);
    externalInstrumentBox.setTextWhenNothingSelected("VST INSTALADO");
    externalInstrumentBox.onChange = [this]
    {
        const auto selected = externalInstrumentBox.getSelectedItemIndex();
        if (!juce::isPositiveAndBelow(selected, externalInstrumentFiles.size())) return;
        externalEditorWindow.reset();
        const auto result = processor.loadExternalInstrument(index, externalInstrumentFiles.getReference(selected));
        if (result.failed())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "Falha ao carregar instrumento", result.getErrorMessage());
        refresh();
    };
    addAndMakeVisible(externalInstrumentBox);
    deleteLibraryButton.setTooltip("Excluir o SF2 selecionado da biblioteca");
    deleteLibraryButton.onClick = [this] { deleteSelectedSoundFont(); };
    deleteLibraryButton.setEnabled(false);

    fileLabel.setJustificationType(juce::Justification::centred);
    fileLabel.getProperties().set("uiDataText", true);
    fileLabel.setMinimumHorizontalScale(0.6f);
    fileLabel.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    addAndMakeVisible(fileLabel);
    sourceSummary.setJustificationType(juce::Justification::centred);
    sourceSummary.getProperties().set("uiDataText", true);
    sourceSummary.getProperties().set("uiDataTooltip", true);
    sourceSummary.setColour(juce::Label::textColourId, juce::Colour(text));
    sourceSummary.setFont(juce::FontOptions(17.0f, juce::Font::bold));
    sourceSummary.setMinimumHorizontalScale(0.45f);
    addAndMakeVisible(sourceSummary);
    for (const auto& category : ClassicPlayerAudioProcessor::soundFontCategories())
        categoryBox.addItem(category, categoryBox.getNumItems() + 1);
    categoryBox.setSelectedId(1, juce::dontSendNotification);
    categoryBox.onChange = [this] { rebuildLibrary(); };
    libraryBox.onChange = [this]
    {
        const auto selected = libraryBox.getSelectedItemIndex();
        deleteLibraryButton.setEnabled(juce::isPositiveAndBelow(selected, libraryFiles.size()));
        if (!juce::isPositiveAndBelow(selected, libraryFiles.size())) return;
        const auto result = processor.loadSoundFont(index, libraryFiles.getReference(selected));
        if (result.failed())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "Falha ao carregar SF2", result.getErrorMessage());
        refresh();
    };
    addAndMakeVisible(categoryBox);
    addAndMakeVisible(libraryBox);
    addAndMakeVisible(presetBox);

    gain.setSliderStyle(juce::Slider::LinearVertical);
    gain.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 58, 18);
    gain.getProperties().set("compactLayerFader", true);
    gain.setColour(juce::Slider::trackColourId, juce::Colour(teal));
    gain.setColour(juce::Slider::thumbColourId, juce::Colour(0xffd8dde0));
    addAndMakeVisible(gain);
    const auto parameterPrefix = "layer" + juce::String(index + 1);
    gainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "layer" + juce::String(index + 1) + "Gain", gain);

    for (auto* envelopeSlider : { &attack, &release })
    {
        envelopeSlider->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        envelopeSlider->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 54, 16);
        addAndMakeVisible(*envelopeSlider);
    }
    attack.setRange(0.0, 100.0, 0.1);
    release.setRange(0.0, 100.0, 1.0);
    attackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, parameterPrefix + "Attack", attack);
    releaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, parameterPrefix + "Release", release);

    cutoff.setRange(0.0, 100.0, 1.0);
    cutoff.setValue(100.0);
    cutoff.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    cutoff.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 54, 16);
    addAndMakeVisible(cutoff);
    reverb.setRange(0.0, 100.0, 1.0);
    reverb.setValue(0.0);
    reverb.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    reverb.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 54, 16);
    addAndMakeVisible(reverb);
    compressor.setRange(0.0, 100.0, 1.0);
    compressor.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    compressor.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 54, 16);
    addAndMakeVisible(compressor);
    cutoffAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, parameterPrefix + "Cutoff", cutoff);
    reverbAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, parameterPrefix + "Reverb", reverb);
    compressorAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, parameterPrefix + "Comp", compressor);
    chorus.setRange(0.0, 100.0, 1.0);
    chorus.setValue(20.0);
    chorus.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    chorus.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 54, 16);
    addAndMakeVisible(chorus);
    chorusAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, parameterPrefix + "Dx7Chorus", chorus);
    addAndMakeVisible(meter);

    for (auto* label : { &attackLabel, &releaseLabel, &cutoffLabel, &reverbLabel, &compressorLabel, &chorusLabel, &routingLabel })
    {
        label->setJustificationType(juce::Justification::centred);
        label->setColour(juce::Label::textColourId, juce::Colour(mutedText));
        label->setFont(juce::FontOptions(9.5f, juce::Font::bold));
        addAndMakeVisible(*label);
    }
    attackLabel.setText("ATTACK", juce::dontSendNotification);
    releaseLabel.setText("RELEASE", juce::dontSendNotification);
    cutoffLabel.setText("CUTOFF", juce::dontSendNotification);
    reverbLabel.setText("REVERB", juce::dontSendNotification);
    compressorLabel.setText("COMP", juce::dontSendNotification);
    chorusLabel.setText("CHORUS", juce::dontSendNotification);
    routingLabel.setText("ROTEAMENTO DA LAYER", juce::dontSendNotification);

    for (auto* button : { &volumeLearn, &cutoffLearn, &reverbLearn, &compressorLearn })
    {
        flatButton(*button);
        button->setTooltip(juce::String::fromUTF8("Mova um controle MIDI CC; clique novamente para cancelar ou use o botão direito para excluir o mapeamento."));
        addAndMakeVisible(*button);
    }
    volumeLearn.onClick = [this] { processor.beginMidiLearn(index, ClassicPlayerAudioProcessor::LearnTarget::volume); updateMidiLearnState(); };
    cutoffLearn.onClick = [this] { processor.beginMidiLearn(index, ClassicPlayerAudioProcessor::LearnTarget::cutoff); updateMidiLearnState(); };
    reverbLearn.onClick = [this] { processor.beginMidiLearn(index, ClassicPlayerAudioProcessor::LearnTarget::reverb); updateMidiLearnState(); };
    compressorLearn.onClick = [this] { processor.beginMidiLearn(index, ClassicPlayerAudioProcessor::LearnTarget::compressor); updateMidiLearnState(); };
    volumeLearn.onClearMapping = [this] { processor.clearMidiLearn(index, ClassicPlayerAudioProcessor::LearnTarget::volume); updateMidiLearnState(); };
    cutoffLearn.onClearMapping = [this] { processor.clearMidiLearn(index, ClassicPlayerAudioProcessor::LearnTarget::cutoff); updateMidiLearnState(); };
    reverbLearn.onClearMapping = [this] { processor.clearMidiLearn(index, ClassicPlayerAudioProcessor::LearnTarget::reverb); updateMidiLearnState(); };
    compressorLearn.onClearMapping = [this] { processor.clearMidiLearn(index, ClassicPlayerAudioProcessor::LearnTarget::compressor); updateMidiLearnState(); };
    for (auto* button : { &reverbEditButton, &compressorEditButton })
    {
        flatButton(*button);
        button->setTooltip(juce::String::fromUTF8("Ajustar com precisão a intensidade do efeito"));
        addAndMakeVisible(*button);
    }
    reverbEditButton.onClick = [this] { showReverbEditor(); };
    compressorEditButton.onClick = [this] { showCompressorEditor(); };
    flatButton(chorusEditButton);
    chorusEditButton.setTooltip("Ajustar chorus da layer DX7");
    chorusEditButton.onClick = [this] { showChorusEditor(); };
    addAndMakeVisible(chorusEditButton);
    flatButton(resetMidiLearnButton);
    resetMidiLearnButton.setTooltip("Apagar todos os endereçamentos MIDI Learn desta layer");
    resetMidiLearnButton.onClick = [this]
    {
        processor.resetMidiLearn(index);
        refresh();
    };
    addAndMakeVisible(resetMidiLearnButton);
    flatButton(muteLearn);
    muteLearn.setTooltip("Aprender um CC de botão para alternar esta layer entre ativa e muda");
    muteLearn.onClick = [this]
    {
        processor.beginMidiLearn(index, ClassicPlayerAudioProcessor::LearnTarget::mute);
        updateMidiLearnState();
    };
    muteLearn.onClearMapping = [this]
    {
        processor.clearMidiLearn(index, ClassicPlayerAudioProcessor::LearnTarget::mute);
        updateMidiLearnState();
    };
    addAndMakeVisible(muteLearn);

    initialiseComboBoxes();
    refresh();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::initialiseComboBoxes()
{
    mode.addItem("POLI", 1);
    mode.addItem("MONO LEGATO", 2);
    mode.addItem("PORTAMENTO", 3);
    sustain.addItem("SUSTAIN ON", 1); sustain.addItem("SUSTAIN OFF", 2);
    midiChannel.addItem("MIDI OMNI", 1);
    for (int channel = 1; channel <= 16; ++channel)
        midiChannel.addItem("MIDI CH " + juce::String(channel), channel + 1);
    midiDevice.onChange = [this]
    {
        const auto deviceIndex = midiDevice.getSelectedId() - 2;
        processor.setLayerMidiDevice(index,
            juce::isPositiveAndBelow(deviceIndex, midiDevices.size())
                ? midiDevices.getReference(deviceIndex).identifier : juce::String{});
    };
    addAndMakeVisible(midiDevice);
    for (int value = -4; value <= 4; ++value)
        octave.addItem((value > 0 ? "+" : "") + juce::String(value) + " OIT", value + 5);
    for (int note = 0; note < 128; ++note)
    {
        lowNote.addItem(midiNoteName(note), note + 1);
        highNote.addItem(midiNoteName(note), note + 1);
    }
    velocityCurve.addItem("VEL LINEAR", 1);
    velocityCurve.addItem("VEL SOFT", 2);
    velocityCurve.addItem("VEL HARD", 3);
    for (auto* box : { &mode, &sustain, &midiChannel, &octave, &lowNote, &highNote, &velocityCurve })
    {
        box->onChange = [this] { applyConfig(); };
        addAndMakeVisible(*box);
    }
    presetBox.onChange = [this]
    {
        const auto selected = presetBox.getSelectedItemIndex();
        if (juce::isPositiveAndBelow(selected, (int) presets.size()))
            processor.selectLayerPreset(index, presets[(size_t) selected].bank,
                                         presets[(size_t) selected].program);
    };
    refreshMidiDevices(processor.availableMidiDevices());
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::refreshMidiDevices(
    const juce::Array<juce::MidiDeviceInfo>& devices)
{
    juce::String fingerprint;
    for (const auto& device : devices) fingerprint << device.identifier << ";";
    if (fingerprint == midiDeviceFingerprint && midiDevice.getNumItems() > 0) return;
    midiDeviceFingerprint = fingerprint;
    midiDevices = devices;
    const auto selected = processor.layerMidiDevice(index);
    midiDevice.clear(juce::dontSendNotification);
    midiDevice.addItem("TODOS OS CONTROLADORES", 1);
    int selectedId = 1;
    for (int item = 0; item < devices.size(); ++item)
    {
        const auto& device = devices.getReference(item);
        // The item value stores the stable CoreMIDI identifier; its visible
        // label remains the human-readable device name.
        midiDevice.addItem(device.name, item + 2);
        if (device.identifier == selected) selectedId = item + 2;
    }
    midiDevice.setSelectedId(selectedId, juce::dontSendNotification);
    midiDevice.setTextWhenNothingSelected("CONTROLADOR MIDI");
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::showLayerEditor()
{
    const auto prefix = "layer" + juce::String(index + 1);
    auto* dialog = new LayerEditorWindow(
        "EDITAR LAYER", "Ajuste os controles desta layer sem expandir o canal.",
        juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    auto valueOf = [this](const juce::String& id, float fallback)
    {
        if (auto* value = processor.parameters.getRawParameterValue(id)) return value->load();
        return fallback;
    };
    auto* knobs = new KnobEditorPanel({
        { "VOLUME", valueOf(prefix + "Gain", 80.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "ATTACK ms", valueOf(prefix + "Attack", 5.0f), 0.0f, 100.0f, 0.1f, 1 },
        { "RELEASE", valueOf(prefix + "Release", 50.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "CUTOFF", valueOf(prefix + "Cutoff", 100.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "REVERB", valueOf(prefix + "Reverb", 0.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "COMP", valueOf(prefix + "Comp", 0.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "EQ LOW dB", valueOf(prefix + "EqLow", 0.0f), -18.0f, 18.0f, 0.1f, 1 },
        { "EQ MID dB", valueOf(prefix + "EqMid", 0.0f), -18.0f, 18.0f, 0.1f, 1 },
        { "EQ HIGH dB", valueOf(prefix + "EqHigh", 0.0f), -18.0f, 18.0f, 0.1f, 1 }
    }, 5);
    const std::array<const char*, 9> editorParameterSuffixes {
        "Gain", "Attack", "Release", "Cutoff", "Reverb", "Comp",
        "EqLow", "EqMid", "EqHigh"
    };
    for (int control = 0; control < (int) editorParameterSuffixes.size(); ++control)
        knobs->bindParameter(control, processor.parameters,
                             prefix + editorParameterSuffixes[(size_t) control]);
    auto* sf2Panel = new Sf2EditorPanel(processor, index);
    sf2Panel->useCompactLayout(true);
    sf2Panel->setSize(620, 190);
    knobs->useDenseGrid(true);
    knobs->setSize(620, 128);
    const juce::Component::SafePointer<LayerStrip> safe(this);
    auto* effectButtons = new LayerEffectButtons(
        [safe] { if (safe != nullptr) safe->showReverbEditor(); },
        [safe] { if (safe != nullptr) safe->showCompressorEditor(); }, {},
        [safe] { if (safe != nullptr) safe->showEqEditor(); });
    auto* midiPanel = new LayerMidiLearnPanel(processor, index);

    class CenteredPanel final : public juce::Component
    {
    public:
        CenteredPanel(juce::Component* child, int preferredWidth, int preferredHeight)
            : content(child), width(preferredWidth)
        {
            owned.add(child);
            addAndMakeVisible(child);
            setSize(preferredWidth, preferredHeight);
        }

        void resized() override
        {
            content->setBounds(getLocalBounds().withSizeKeepingCentre(
                juce::jmin(width, content->getWidth()),
                juce::jmin(getHeight(), content->getHeight())));
        }

    private:
        juce::Component* content;
        int width;
        juce::OwnedArray<juce::Component> owned;
    };

    // On a 13-inch display the complete editor, including numeric values and
    // footer, must fit without an inner scroll area.
    dialog->addCustomComponent(new CenteredPanel(sf2Panel, 620, 190));
    dialog->addCustomComponent(new CenteredPanel(new LayerPresetFilePanel(
        [safe] { if (safe != nullptr) safe->saveLayerPreset(); },
        [safe] { if (safe != nullptr) safe->loadLayerPreset(); }), 620, 28));
    dialog->addCustomComponent(new CenteredPanel(new ModulationTogglePanel(processor, index), 620, 28));
    dialog->addCustomComponent(new CenteredPanel(knobs, 620, 128));
    const auto actionWidth = editorContentWidth(this, 930);
    dialog->addCustomComponent(new SideBySideEditorPanel(effectButtons, midiPanel, actionWidth, 54));
    const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(getScreenBounds());
    const auto screenHeight = display != nullptr ? display->userArea.getHeight() : 700;
    knobs->setOnValueChange([safe = juce::Component::SafePointer<LayerStrip>(this), knobs, prefix]
    {
        if (safe == nullptr) return;
        const auto set = [safe, prefix](const juce::String& id, float value)
        {
            if (auto* parameter = safe->processor.parameters.getParameter(prefix + id))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        };
        set("Gain", knobs->value(0)); set("Attack", knobs->value(1));
        set("Release", knobs->value(2)); set("Cutoff", knobs->value(3));
        set("Reverb", knobs->value(4)); set("Comp", knobs->value(5));
        set("EqLow", knobs->value(6)); set("EqMid", knobs->value(7)); set("EqHigh", knobs->value(8));
    });
    dialog->setSize(actionWidth + 52, juce::jmin(570, screenHeight - 12));
    dialog->fitWithinApp(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe, dialog, knobs, prefix](int)
        {
            if (safe == nullptr) return;
            const auto set = [safe, prefix](const juce::String& id, float value)
            {
                if (auto* parameter = safe->processor.parameters.getParameter(prefix + id))
                    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
            };
            set("Gain", knobs->value(0));
            set("Attack", knobs->value(1));
            set("Release", knobs->value(2));
            set("Cutoff", knobs->value(3));
            set("Reverb", knobs->value(4));
            set("Comp", knobs->value(5));
            set("EqLow", knobs->value(6));
            set("EqMid", knobs->value(7));
            set("EqHigh", knobs->value(8));
            safe->refresh();
        }), true);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::showReverbEditor()
{
    const auto prefix = "layer" + juce::String(index + 1);
    auto* dialog = new LayerEditorWindow(
        "REVERB DA LAYER", "O knob REVERB controla a quantidade. Ajuste o carater da sala abaixo.",
        juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    auto* knobs = new KnobEditorPanel({
        { "TAMANHO", processor.parameters.getRawParameterValue(prefix + "ReverbSize")->load(), 0.0f, 100.0f, 1.0f, 0 },
        { "DIFUSAO", 100.0f - processor.parameters.getRawParameterValue(prefix + "ReverbDamping")->load(), 0.0f, 100.0f, 1.0f, 0 },
        { "LARGURA", processor.parameters.getRawParameterValue(prefix + "ReverbWidth")->load(), 0.0f, 100.0f, 1.0f, 0 },
        { "MIX", processor.parameters.getRawParameterValue(prefix + "Reverb")->load(), 0.0f, 100.0f, 1.0f, 0 }
    }, 2);
    // The mix knob is also the layer's learned REVERB target. Keep this
    // editor live when the value comes from a hardware controller.
    knobs->bindParameter(3, processor.parameters, prefix + "Reverb");
    auto* presets = new EffectPresetPanel("PRESET", {
        "Piano Intimo", "Sala Clara", "Worship Hall", "Ambient Grande"
    });
    const auto* savedPresetValue = processor.parameters.getRawParameterValue(prefix + "ReverbPreset");
    auto selectedPreset = savedPresetValue != nullptr ? juce::roundToInt(savedPresetValue->load()) : -1;
    const auto presetMatchesCurrentValues = [&] (int preset)
    {
        if (!juce::isPositiveAndBelow(preset, (int) factoryReverbPresets.size())) return false;
        const auto& values = factoryReverbPresets[(size_t) preset];
        const auto current = [&] (const char* suffix) { return processor.parameters.getRawParameterValue(prefix + suffix)->load(); };
        return std::abs(current("ReverbSize") - values[0]) < 0.1f
            && std::abs(current("ReverbDamping") - (100.0f - values[1])) < 0.1f
            && std::abs(current("ReverbWidth") - values[2]) < 0.1f
            && std::abs(current("Reverb") - values[3]) < 0.1f;
    };
    if (selectedPreset != -1 && !presetMatchesCurrentValues(selectedPreset))
    {
        // A manual adjustment (or an older/custom imported preset) no longer
        // represents the last factory preset selected for this layer.
        selectedPreset = -1;
        if (auto* parameter = processor.parameters.getParameter(prefix + "ReverbPreset"))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(-1.0f));
    }
    // Older performances predate the explicit preset marker. Preserve their
    // familiar selection by inferring it once from the stored reverb values.
    if (selectedPreset < 0)
    {
        for (int preset = 0; preset < (int) factoryReverbPresets.size(); ++preset)
        {
            if (presetMatchesCurrentValues(preset))
            {
                selectedPreset = preset;
                break;
            }
        }
    }
    if (selectedPreset >= 0) presets->setSelectedPreset(selectedPreset);
    const juce::Component::SafePointer<LayerStrip> safe(this);
    dialog->addCustomComponent(new CentredEditorPanel(presets, 700));
    dialog->addCustomComponent(new CentredEditorPanel(new EffectPresetFilePanel(
        [safe] { if (safe != nullptr) safe->saveEffectPreset("REVERB"); },
        [safe, knobs, prefix]
        {
            if (safe == nullptr) return;
            safe->loadEffectPreset("REVERB", [safe, knobs, prefix]
            {
                if (safe == nullptr) return;
                knobs->setValue(0, safe->processor.parameters.getRawParameterValue(prefix + "ReverbSize")->load());
                knobs->setValue(1, 100.0f - safe->processor.parameters.getRawParameterValue(prefix + "ReverbDamping")->load());
                knobs->setValue(2, safe->processor.parameters.getRawParameterValue(prefix + "ReverbWidth")->load());
                knobs->setValue(3, safe->processor.parameters.getRawParameterValue(prefix + "Reverb")->load());
            });
        }), 700));
    dialog->addCustomComponent(new CentredEditorPanel(knobs, 700));
    dialog->setSize(760, 580);
    dialog->fitWithinApp(this);
    const auto setParameter = [safe, prefix](const juce::String& id, float value)
    {
        if (safe == nullptr) return;
        if (auto* parameter = safe->processor.parameters.getParameter(prefix + id))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };
    presets->onPresetSelected = [safe, knobs, setParameter](int preset)
    {
        if (safe == nullptr || preset < 0) return;
        // Reverb size, damping, width and mix. The four voicings move from a
        // close piano room to the long, wide tail commonly used for worship.
        const auto& selected = factoryReverbPresets[(size_t) juce::jlimit(0, 3, preset)];
        for (int control = 0; control < 4; ++control)
            knobs->setValue(control, selected[(size_t) control]);
        setParameter("ReverbSize", selected[0]);
        setParameter("ReverbDamping", 100.0f - selected[1]);
        setParameter("ReverbWidth", selected[2]);
        setParameter("Reverb", selected[3]);
        setParameter("ReverbPreset", static_cast<float>(preset));
    };
    knobs->setOnValueChange([safe, knobs, prefix]
    {
        if (safe == nullptr) return;
        const auto set = [safe, prefix](const juce::String& id, float value)
        {
            if (auto* parameter = safe->processor.parameters.getParameter(prefix + id))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        };
        set("ReverbPreset", -1.0f);
        set("ReverbSize", juce::jlimit(0.0f, 100.0f, knobs->value(0)));
        set("ReverbDamping", 100.0f - juce::jlimit(0.0f, 100.0f, knobs->value(1)));
        set("ReverbWidth", juce::jlimit(0.0f, 100.0f, knobs->value(2)));
        set("Reverb", juce::jlimit(0.0f, 100.0f, knobs->value(3)));
    });
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe](int) { if (safe != nullptr) safe->refresh(); }), true);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::saveLayerPreset()
{
    juce::String typeName = "Layer";
    switch (processor.layerType(index))
    {
        case ClassicPlayerAudioProcessor::LayerType::sf2: typeName = "SF2"; break;
        case ClassicPlayerAudioProcessor::LayerType::dx7: typeName = "DX7"; break;
        case ClassicPlayerAudioProcessor::LayerType::analog: typeName = "Moog"; break;
        case ClassicPlayerAudioProcessor::LayerType::hammond: typeName = "Hammond"; break;
        default: break;
    }
    const auto defaultFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Classic Player " + typeName + ".cklayer");
    fileChooser = std::make_unique<juce::FileChooser>(
        localizedUiText("Exportar preset da layer", activeUiLanguage.load()), defaultFile, "*.cklayer");
    const juce::Component::SafePointer<LayerStrip> safe(this);
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                           | juce::FileBrowserComponent::canSelectFiles
                           | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe](const juce::FileChooser& chooser)
        {
            if (safe == nullptr || chooser.getResult() == juce::File{}) return;
            juce::File saved;
            const auto result = safe->processor.saveLayerPreset(safe->index, chooser.getResult(), saved);
            juce::AlertWindow::showMessageBoxAsync(
                result.wasOk() ? juce::MessageBoxIconType::InfoIcon
                               : juce::MessageBoxIconType::WarningIcon,
                result.wasOk() ? "Preset da layer exportado" : "Falha ao exportar preset",
                result.wasOk() ? "Arquivo exportado para:\n" + saved.getFullPathName()
                               : result.getErrorMessage());
        });
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::loadLayerPreset()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        localizedUiText("Importar preset da layer", activeUiLanguage.load()), juce::File{}, "*.cklayer");
    const juce::Component::SafePointer<LayerStrip> safe(this);
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                           | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& chooser)
        {
            if (safe == nullptr || chooser.getResult() == juce::File{}) return;
            const auto result = safe->processor.loadLayerPreset(safe->index, chooser.getResult());
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Falha ao importar preset",
                                                       result.getErrorMessage());
            else
                safe->refresh();
        });
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::saveEffectPreset(const juce::String& effect)
{
    const juce::String extension = effect == "EQ" ? ".ckeq"
                                 : effect == "COMP" ? ".ckcomp" : ".ckreverb";
    const auto defaultFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Classic Player " + effect + extension);
    fileChooser = std::make_unique<juce::FileChooser>(
        localizedUiText("Exportar preset de " + effect, activeUiLanguage.load()), defaultFile, "*" + extension);
    const juce::Component::SafePointer<LayerStrip> safe(this);
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                           | juce::FileBrowserComponent::canSelectFiles
                           | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe, effect, extension](const juce::FileChooser& chooser)
        {
            if (safe == nullptr || chooser.getResult() == juce::File{}) return;
            auto destination = chooser.getResult();
            if (destination.getFileExtension().toLowerCase() != extension)
                destination = destination.withFileExtension(extension);
            juce::ValueTree preset("ClassicPlayerEffectPreset");
            preset.setProperty("format", 1, nullptr);
            preset.setProperty("effect", effect, nullptr);
            const auto prefix = "layer" + juce::String(safe->index + 1);
            juce::StringArray suffixes;
            if (effect == "EQ")
                suffixes = { "EqLow", "EqMid", "EqHigh", "EqLowFrequency", "EqMidFrequency",
                             "EqHighFrequency", "EqLowQ", "EqMidQ", "EqHighQ" };
            else if (effect == "COMP")
                suffixes = { "Comp", "CompThreshold", "CompRatio", "CompAttack",
                             "CompRelease", "CompMakeup" };
            else
                suffixes = { "Reverb", "ReverbSize", "ReverbDamping", "ReverbWidth" };
            for (const auto& suffix : suffixes)
                if (auto* value = safe->processor.parameters.getRawParameterValue(prefix + suffix))
                    preset.setProperty(suffix, value->load(), nullptr);
            if (effect == "EQ")
            {
                const auto config = safe->processor.layerConfig(safe->index);
                preset.setProperty("LowCut", config.highPassHz, nullptr);
                preset.setProperty("HighCut", config.lowPassHz, nullptr);
            }
            auto xml = preset.createXml();
            const auto ok = xml != nullptr && destination.replaceWithText(xml->toString());
            juce::AlertWindow::showMessageBoxAsync(
                ok ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                localizedUiText(ok ? "Preset exportado" : "Falha ao exportar preset", activeUiLanguage.load()),
                ok ? localizedUiText(juce::String::fromUTF8("Arquivo exportado para:\n") + destination.getFullPathName(), activeUiLanguage.load())
                   : localizedUiText("Não foi possível exportar o preset.", activeUiLanguage.load()));
        });
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::loadEffectPreset(
    const juce::String& effect, std::function<void()> onLoaded)
{
    const juce::String extension = effect == "EQ" ? ".ckeq"
                                 : effect == "COMP" ? ".ckcomp" : ".ckreverb";
    fileChooser = std::make_unique<juce::FileChooser>(
        localizedUiText(juce::String::fromUTF8("Importar preset de ") + effect, activeUiLanguage.load()), juce::File{}, "*" + extension);
    const juce::Component::SafePointer<LayerStrip> safe(this);
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                           | juce::FileBrowserComponent::canSelectFiles,
        [safe, effect, onLoaded = std::move(onLoaded)](const juce::FileChooser& chooser)
        {
            if (safe == nullptr || chooser.getResult() == juce::File{}) return;
            auto xml = juce::XmlDocument::parse(chooser.getResult());
            if (xml == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       localizedUiText("Preset inválido", activeUiLanguage.load()),
                                                       localizedUiText("O arquivo está corrompido.", activeUiLanguage.load()));
                return;
            }
            const auto preset = juce::ValueTree::fromXml(*xml);
            if (!preset.hasType("ClassicPlayerEffectPreset")
                || preset.getProperty("effect").toString() != effect)
            {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       localizedUiText("Preset incompatível", activeUiLanguage.load()),
                                                       localizedUiText(juce::String::fromUTF8("Escolha um preset de ") + effect + ".", activeUiLanguage.load()));
                return;
            }
            const auto prefix = "layer" + juce::String(safe->index + 1);
            for (int property = 0; property < preset.getNumProperties(); ++property)
            {
                const auto suffix = preset.getPropertyName(property).toString();
                if (suffix == "format" || suffix == "effect" || suffix == "LowCut" || suffix == "HighCut")
                    continue;
                if (auto* parameter = safe->processor.parameters.getParameter(prefix + suffix))
                {
                    const auto value = static_cast<float>(preset.getProperty(suffix));
                    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
                }
            }
            if (effect == "REVERB")
            {
                // Imported effect files can contain arbitrary settings; they
                // are not one of the built-in choices shown in the selector.
                if (auto* parameter = safe->processor.parameters.getParameter(prefix + "ReverbPreset"))
                    parameter->setValueNotifyingHost(parameter->convertTo0to1(-1.0f));
            }
            if (effect == "EQ")
            {
                auto config = safe->processor.layerConfig(safe->index);
                config.highPassHz = static_cast<float>(preset.getProperty("LowCut", config.highPassHz));
                config.lowPassHz = static_cast<float>(preset.getProperty("HighCut", config.lowPassHz));
                safe->processor.setLayerConfig(safe->index, config);
            }
            if (onLoaded) onLoaded();
            safe->refresh();
        });
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::showEqEditor()
{
    const juce::Component::SafePointer<LayerStrip> safe(this);
    showParametricLayerEqEditor(processor, index, this,
        [safe] { if (safe != nullptr) safe->saveEffectPreset("EQ"); },
        [safe](std::function<void()> loaded)
        {
            if (safe != nullptr) safe->loadEffectPreset("EQ", std::move(loaded));
        });
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::showCompressorEditor()
{
    const auto prefix = "layer" + juce::String(index + 1);
    auto* dialog = new LayerEditorWindow(
        "COMPRESSOR DA LAYER", "O knob COMP controla a mistura. Ajuste a dinamica abaixo.",
        juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    auto* knobs = new KnobEditorPanel({
        { "THRESHOLD dB", processor.parameters.getRawParameterValue(prefix + "CompThreshold")->load(), -60.0f, 0.0f, 0.1f, 1 },
        { "RATIO", processor.parameters.getRawParameterValue(prefix + "CompRatio")->load(), 1.0f, 20.0f, 0.1f, 1 },
        { "ATTACK ms", processor.parameters.getRawParameterValue(prefix + "CompAttack")->load(), 0.1f, 100.0f, 0.1f, 1 },
        { "RELEASE ms", processor.parameters.getRawParameterValue(prefix + "CompRelease")->load(), 5.0f, 1000.0f, 1.0f, 0 },
        { "MAKEUP dB", processor.parameters.getRawParameterValue(prefix + "CompMakeup")->load(), 0.0f, 24.0f, 0.1f, 1 }
    }, 3);
    auto* graph = new CompressorResponseView(processor, index);
    auto* presets = new EffectPresetPanel("PRESET", {
        "Piano Natural", "Piano Presenca", "Worship Suave", "Worship Sustentado"
    });
    for (int preset = 0; preset < (int) factoryCompressorPresets.size(); ++preset)
    {
        const auto& values = factoryCompressorPresets[(size_t) preset];
        const auto current = [&] (const char* suffix) { return processor.parameters.getRawParameterValue(prefix + suffix)->load(); };
        if (std::abs(current("CompThreshold") - values[0]) < 0.1f
            && std::abs(current("CompRatio") - values[1]) < 0.1f
            && std::abs(current("CompAttack") - values[2]) < 0.1f
            && std::abs(current("CompRelease") - values[3]) < 0.1f
            && std::abs(current("CompMakeup") - values[4]) < 0.1f
            && std::abs(current("Comp") - values[5]) < 0.1f)
        {
            presets->setSelectedPreset(preset);
            break;
        }
    }
    const juce::Component::SafePointer<LayerStrip> safe(this);
    dialog->addCustomComponent(new CentredEditorPanel(presets, 700));
    dialog->addCustomComponent(new CentredEditorPanel(new EffectPresetFilePanel(
        [safe] { if (safe != nullptr) safe->saveEffectPreset("COMP"); },
        [safe, knobs, prefix]
        {
            if (safe == nullptr) return;
            safe->loadEffectPreset("COMP", [safe, knobs, prefix]
            {
                if (safe == nullptr) return;
                const std::array<const char*, 5> names {
                    "CompThreshold", "CompRatio", "CompAttack", "CompRelease", "CompMakeup"
                };
                for (int i = 0; i < 5; ++i)
                    knobs->setValue(i, safe->processor.parameters.getRawParameterValue(prefix + names[(size_t) i])->load());
            });
        }), 700));
    knobs->useDenseGrid(true);
    const auto contentWidth = editorContentWidth(this, 1000);
    dialog->addCustomComponent(new SideBySideEditorPanel(graph, knobs, contentWidth, 240));
    dialog->setSize(contentWidth + 52, 448);
    dialog->fitWithinApp(this);
    const auto setParameter = [safe, prefix](const juce::String& id, float value)
    {
        if (safe == nullptr) return;
        if (auto* parameter = safe->processor.parameters.getParameter(prefix + id))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };
    presets->onPresetSelected = [safe, knobs, setParameter](int preset)
    {
        if (safe == nullptr || preset < 0) return;
        // Threshold, ratio, attack, release, makeup and parallel mix. The
        // first two start from Yamaha's documented piano compressor programs;
        // the worship variants remain gentle enough to preserve piano attack.
        const auto& selected = factoryCompressorPresets[(size_t) juce::jlimit(0, 3, preset)];
        for (int control = 0; control < 5; ++control)
            knobs->setValue(control, selected[(size_t) control]);
        setParameter("CompThreshold", selected[0]);
        setParameter("CompRatio", selected[1]);
        setParameter("CompAttack", selected[2]);
        setParameter("CompRelease", selected[3]);
        setParameter("CompMakeup", selected[4]);
        setParameter("Comp", selected[5]);
    };
    knobs->setOnValueChange([safe, knobs, prefix]
    {
        if (safe == nullptr) return;
        const auto set = [safe, prefix](const juce::String& id, float value)
        {
            if (auto* parameter = safe->processor.parameters.getParameter(prefix + id))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        };
        set("CompThreshold", juce::jlimit(-60.0f, 0.0f, knobs->value(0)));
        set("CompRatio", juce::jlimit(1.0f, 20.0f, knobs->value(1)));
        set("CompAttack", juce::jlimit(0.1f, 100.0f, knobs->value(2)));
        set("CompRelease", juce::jlimit(5.0f, 1000.0f, knobs->value(3)));
        set("CompMakeup", juce::jlimit(0.0f, 24.0f, knobs->value(4)));
    });
    graph->onCurveChanged = [knobs](int handle, float value)
    {
        knobs->setValue(handle, value);
    };
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe](int) { if (safe != nullptr) safe->refresh(); }), true);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::showDrumPadEditor()
{
    if (processor.layerType(index) != ClassicPlayerAudioProcessor::LayerType::drumPads && processor.layerType(index) != ClassicPlayerAudioProcessor::LayerType::continuousPads) return;
    auto* dialog = new LayerEditorWindow(processor.layerType(index)==ClassicPlayerAudioProcessor::LayerType::continuousPads ? "PADS CONTINUOS" : "DRUM PADS", {}, juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    auto* pads = new DrumPadPanel(processor,index);
    pads->setControlsVisible(true);
    // Leave enough room for both complete columns and their LOAD/LEARN rows;
    // the previous width let the right column run underneath the dialog edge.
    // Keep both pad columns and their LOAD/LEARN rows inside the 678 px
    // dialog shown by the reference layout.
    pads->setSize(560, 430);
    dialog->addCustomComponent(pads);
    const juce::Component::SafePointer<LayerStrip> safe(this);
    if (processor.layerType(index) == ClassicPlayerAudioProcessor::LayerType::continuousPads)
    {
        auto* eqButton = new juce::TextButton("EDITAR EQ / FILTROS");
        flatButton(*eqButton);
        eqButton->setSize(440, 34);
        eqButton->onClick = [safe] { if (safe != nullptr) safe->showEqEditor(); };
        dialog->addCustomComponent(eqButton);
    }
    dialog->setSize(678, processor.layerType(index) == ClassicPlayerAudioProcessor::LayerType::continuousPads ? 580 : 560);
    dialog->fitWithinApp(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe](int) { if (safe != nullptr) safe->drumPadPanel.refresh(); }), true);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::showChorusEditor()
{
    if (processor.layerType(index) != ClassicPlayerAudioProcessor::LayerType::dx7) return;
    const auto prefix = "layer" + juce::String(index + 1);
    auto* dialog = new LayerEditorWindow(
        "CHORUS DA LAYER DX7", "Ajuste o chorus em tempo real.", juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    auto* knobs = new KnobEditorPanel({
        { "MIX", processor.parameters.getRawParameterValue(prefix + "Dx7Chorus")->load(),
          0.0f, 100.0f, 1.0f, 0 }
    }, 1);
    dialog->addCustomComponent(knobs);
    const juce::Component::SafePointer<LayerStrip> safe(this);
    knobs->setOnValueChange([safe, knobs, prefix]
    {
        if (safe == nullptr) return;
        if (auto* parameter = safe->processor.parameters.getParameter(prefix + "Dx7Chorus"))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(knobs->value(0)));
    });
    dialog->fitWithinApp(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe](int) { if (safe != nullptr) safe->refresh(); }), true);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::mouseDown(const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu())
    {
        // This strip listens to descendants for layer dragging. LEARN owns
        // its context menu, so the recursive listener must not open another.
        for (auto* source = event.originalComponent; source != nullptr && source != this;
             source = source->getParentComponent())
            if (dynamic_cast<MidiLearnButton*>(source) != nullptr)
                return;
        juce::PopupMenu menu;
        juce::PopupMenu outlineMenu;
        const auto language = activeUiLanguage.load();
        outlineMenu.addItem(10, localizedUiText(juce::String::fromUTF8("Automática"), language),
                            true, !hasCustomOutlineColour);
        const auto addOutlineOption = [&outlineMenu, language, this](int id, const char* label,
                                                                    juce::Colour colour)
        {
            outlineMenu.addColouredItem(id, localizedUiText(juce::String::fromUTF8(label), language),
                                        colour, true, hasCustomOutlineColour && outlineColour == colour);
        };
        addOutlineOption(11, "Vermelho", juce::Colour(0xffe5394b));
        addOutlineOption(12, "Laranja", juce::Colour(0xffff9138));
        addOutlineOption(13, "Amarelo", juce::Colour(0xffffd23f));
        addOutlineOption(14, "Verde", juce::Colour(0xff22c58b));
        addOutlineOption(15, "Ciano", juce::Colour(0xff18c7d9));
        addOutlineOption(16, "Azul", juce::Colour(0xff138cff));
        addOutlineOption(17, "Roxo", juce::Colour(0xff9857ff));
        addOutlineOption(18, "Rosa", juce::Colour(0xffed5aa1));
        addOutlineOption(19, "Branco", juce::Colour(0xfff3f5f7));
        addOutlineOption(20, "Cinza", juce::Colour(0xff9aa3ad));
        outlineMenu.addSeparator();
        outlineMenu.addItem(21, localizedUiText(juce::String::fromUTF8("Escolher outra cor..."), language));
        menu.addSubMenu(localizedUiText(juce::String::fromUTF8("COR DO CONTORNO"), language), outlineMenu);
        menu.addSeparator();
        menu.addItem(1, localizedUiText("Excluir layer", language));
        const juce::Component::SafePointer<LayerStrip> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                           [safe](int result)
        {
            if (safe == nullptr || result == 0) return;

            const auto setColourFromMenu = [safe](juce::Colour colour)
            {
                if (safe != nullptr) safe->setOutlineColour(colour);
            };
            switch (result)
            {
                case 10: safe->setOutlineColour(layerAccentColour(safe->index), true); return;
                case 11: setColourFromMenu(juce::Colour(0xffe5394b)); return;
                case 12: setColourFromMenu(juce::Colour(0xffff9138)); return;
                case 13: setColourFromMenu(juce::Colour(0xffffd23f)); return;
                case 14: setColourFromMenu(juce::Colour(0xff22c58b)); return;
                case 15: setColourFromMenu(juce::Colour(0xff18c7d9)); return;
                case 16: setColourFromMenu(juce::Colour(0xff138cff)); return;
                case 17: setColourFromMenu(juce::Colour(0xff9857ff)); return;
                case 18: setColourFromMenu(juce::Colour(0xffed5aa1)); return;
                case 19: setColourFromMenu(juce::Colour(0xfff3f5f7)); return;
                case 20: setColourFromMenu(juce::Colour(0xff9aa3ad)); return;
                case 21:
                {
                    auto picker = std::make_unique<ColourPicker>(
                        safe->outlineColour,
                        [safe](juce::Colour colour)
                        {
                            if (safe != nullptr) safe->setOutlineColour(colour, false, false);
                        },
                        [safe](juce::Colour colour)
                        {
                            if (safe != nullptr) safe->setOutlineColour(colour);
                        });
                    juce::CallOutBox::launchAsynchronously(std::move(picker), safe->getScreenBounds(), nullptr);
                    return;
                }
                case 1: break;
                default: return;
            }

            // A hosted instrument editor must close before its layer is
            // released, just as it did with the old X button.
            safe->externalEditorWindow.reset();
            if (safe->removeLayerCallback) safe->removeLayerCallback();
        });
        draggingLayerTitle = false;
        return;
    }

    if (event.mods.isLeftButtonDown()
        && categoryArtworkBounds.contains(event.getEventRelativeTo(this).position.toInt()))
    {
        showQuickPresetMenu();
        return;
    }
    draggingLayerTitle = event.originalComponent == &layerTitle && event.mods.isLeftButtonDown();
    if (draggingLayerTitle)
        layerDragger.startDraggingComponent(this, event.getEventRelativeTo(this));
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::showQuickPresetMenu()
{
    if (quickPresetMenuOpen) return;
    const auto type = processor.layerType(index);
    const auto sourcePath = type == ClassicPlayerAudioProcessor::LayerType::sf2 ? processor.soundFontPath(index)
        : type == ClassicPlayerAudioProcessor::LayerType::dx7 ? processor.dx7Path(index) : juce::String{};
    if (type == ClassicPlayerAudioProcessor::LayerType::vst)
    {
        openExternalInstrumentEditor();
        return;
    }
    juce::PopupMenu menu;
    juce::Array<juce::File> banks;
    std::vector<Sf2Engine::Preset> sf2Presets;
    if (type == ClassicPlayerAudioProcessor::LayerType::sf2)
    {
        menu.addSectionHeader(categoryBox.getText());
        sf2Presets = processor.layerPresets(index);
        for (int i = 0; i < (int) sf2Presets.size(); ++i)
            menu.addItem(i + 1, sf2Presets[(size_t) i].name, true,
                sf2Presets[(size_t) i].bank == processor.layerPresetBank(index)
                && sf2Presets[(size_t) i].program == processor.layerPresetProgram(index));
        banks = processor.librarySoundFonts(selectedCategoryId(categoryBox));
    }
    else if (type == ClassicPlayerAudioProcessor::LayerType::dx7)
    {
        for (int i = 0; i < processor.dx7PatchCount(index); ++i)
            menu.addItem(i + 1, processor.dx7PatchName(index, i), true, i == processor.dx7SelectedPatch(index));
        banks = processor.libraryDx7Banks();
    }
    else if (type == ClassicPlayerAudioProcessor::LayerType::hammond)
    {
        const auto names = HammondEngine::presetNames();
        for (int i = 0; i < names.size(); ++i)
            menu.addItem(i + 1, names[i], true, i == processor.hammondConfig(index).preset);
    }
    else if (type == ClassicPlayerAudioProcessor::LayerType::analog)
    {
        for (int i = 0; i < (int) AnalogBrowserPresets::bank.size(); ++i)
            menu.addItem(i + 1, AnalogBrowserPresets::bank[(size_t) i].name);
    }
    if (!banks.isEmpty())
    {
        juce::PopupMenu bankMenu;
        for (int i = 0; i < banks.size(); ++i)
            bankMenu.addItem(100000 + i, banks[i].getFileNameWithoutExtension());
        menu.addSeparator();
        menu.addSubMenu(localizedUiText(type == ClassicPlayerAudioProcessor::LayerType::sf2
            ? "SF2 LIBRARY" : "DX7", activeUiLanguage.load()), bankMenu);
    }
    if (menu.getNumItems() == 0)
        menu.addItem(999999, localizedUiText("CATEGORIA VAZIA", activeUiLanguage.load()), false);
    quickPresetMenuOpen = true;
    const juce::Component::SafePointer<LayerStrip> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(
        localAreaToGlobal(categoryArtworkBounds)), [safe, type, sourcePath, banks, sf2Presets](int selected)
    {
        if (safe == nullptr) return;
        safe->quickPresetMenuOpen = false;
        if (selected == 0 || safe->processor.layerType(safe->index) != type) return;
        auto& p = safe->processor;
        const int layer = safe->index;
        if (selected >= 100000 && juce::isPositiveAndBelow(selected - 100000, banks.size()))
        {
            const auto result = type == ClassicPlayerAudioProcessor::LayerType::sf2
                ? p.loadSoundFont(layer, banks[selected - 100000]) : p.loadDx7(layer, banks[selected - 100000]);
            safe->refresh();
            if (result.wasOk()) safe->showQuickPresetMenu();
            else juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                localizedUiText(type == ClassicPlayerAudioProcessor::LayerType::sf2
                    ? "Falha ao carregar SF2" : "Falha ao carregar DX7", activeUiLanguage.load()), result.getErrorMessage());
            return;
        }
        const int preset = selected - 1;
        if ((type == ClassicPlayerAudioProcessor::LayerType::sf2 && p.soundFontPath(layer) != sourcePath)
            || (type == ClassicPlayerAudioProcessor::LayerType::dx7 && p.dx7Path(layer) != sourcePath)) return;
        if (type == ClassicPlayerAudioProcessor::LayerType::sf2
            && juce::isPositiveAndBelow(preset, (int) sf2Presets.size()))
            p.selectLayerPreset(layer, sf2Presets[(size_t) preset].bank, sf2Presets[(size_t) preset].program);
        else if (type == ClassicPlayerAudioProcessor::LayerType::dx7)
            p.selectDx7Patch(layer, preset);
        else if (type == ClassicPlayerAudioProcessor::LayerType::hammond)
        {
            const auto previous = p.hammondConfig(layer);
            auto config = HammondEngine::preset(preset);
            config.routing = previous.routing; config.cc = previous.cc;
            config.channel = previous.channel; config.level = previous.level;
            p.setHammondConfig(layer, config);
        }
        else if (type == ClassicPlayerAudioProcessor::LayerType::analog
            && juce::isPositiveAndBelow(preset, (int) AnalogBrowserPresets::bank.size()))
        {
            p.resetAnalogSynthVoices(layer);
            p.setAnalogSynthConfig(layer, AnalogBrowserPresets::config((size_t) preset, p.layerConfig(layer)));
        }
        safe->refresh();
    });
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::setOutlineColour(juce::Colour colour,
                                                                      bool useAutomatic,
                                                                      bool savePreference)
{
    hasCustomOutlineColour = !useAutomatic;
    outlineColour = useAutomatic ? layerAccentColour(index) : colour.withAlpha(1.0f);
    if (savePreference)
        writeLayerOutlinePreference(index, outlineColour, useAutomatic);
    repaint();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::mouseMove(const juce::MouseEvent& event)
{
    setMouseCursor(categoryArtworkBounds.contains(event.getEventRelativeTo(this).position.toInt())
        ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::mouseExit(const juce::MouseEvent&)
{
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::mouseDrag(const juce::MouseEvent& event)
{
    if (draggingLayerTitle)
        layerDragger.dragComponent(this, event.getEventRelativeTo(this), nullptr);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::mouseUp(const juce::MouseEvent& event)
{
    if (!draggingLayerTitle) return;
    draggingLayerTitle = false;
    if (event.mouseWasDraggedSinceMouseDown() && reorderCallback)
        reorderCallback(index, event.getScreenPosition());
    else
        if (auto* parent = getParentComponent()) parent->resized();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    const auto accent = outlineColour;
    const auto drumLayer = processor.layerType(index) == ClassicPlayerAudioProcessor::LayerType::drumPads || processor.layerType(index)==ClassicPlayerAudioProcessor::LayerType::continuousPads;
    const auto paintPanel = [&g, bounds]
    {
        g.setColour(juce::Colour(panel)); g.fillRoundedRectangle(bounds, 7.0f);
        if (activeUiPalette == 5 || activeUiPalette == 6)
        {
            const juce::Graphics::ScopedSaveState state(g);
            juce::Path clip; clip.addRoundedRectangle(bounds.reduced(1.0f), 7.0f);
            g.reduceClipRegion(clip);
            paintBrushedSteel(g, bounds);
        }
    };
    if (drumLayer)
    {
        paintPanel();
        g.setColour(accent);
        g.drawRoundedRectangle(bounds.reduced(1.0f), 7.0f, 2.0f);
        return;
    }
    paintPanel();
    g.setColour(accent);
    g.drawRoundedRectangle(bounds.reduced(1.0f), 7.0f, 2.0f);
    const auto type = processor.layerType(index);
    auto category = type == ClassicPlayerAudioProcessor::LayerType::sf2 ? selectedCategoryId(categoryBox)
                  : type == ClassicPlayerAudioProcessor::LayerType::dx7 ? "DX7"
                  : type == ClassicPlayerAudioProcessor::LayerType::hammond ? "Hammond"
                  : type == ClassicPlayerAudioProcessor::LayerType::analog ? "Moog Analog"
                  : type == ClassicPlayerAudioProcessor::LayerType::vst ? "Synth"
                  : "Efeitos";
    drawCategoryArtwork(g, categoryArtworkBounds, category, accent);
    g.setColour(juce::Colour(mutedText));
    g.setFont(9.0f);
    const auto scaleX = gain.getRight() - 22;
    const auto scaleTop = gain.getY() + 8;
    const auto scaleHeight = juce::jmax(80, gain.getHeight() - 42);
    static const std::array<std::pair<float, const char*>, 8> marks {{
        { 0.0f, "-inf" }, { 0.18f, "-40" }, { 0.36f, "-20" }, { 0.52f, "-10" },
        { 0.65f, "-5" }, { 0.77f, "0" }, { 0.88f, "+3" }, { 1.0f, "+6" }
    }};
    for (size_t markIndex = 0; markIndex < marks.size(); ++markIndex)
    {
        const auto [amount, label] = marks[markIndex];
        const auto y = scaleTop + static_cast<int>((1.0f - amount) * static_cast<float>(scaleHeight));
        g.setColour(juce::Colour(markIndex == 5 ? text : mutedText));
        g.drawHorizontalLine(y, static_cast<float>(scaleX - 5), static_cast<float>(scaleX + 2));
        g.drawText(label, scaleX + 4, y - 7, 27, 14, juce::Justification::left);
    }

    g.setColour(juce::Colour(paletteLine));
    g.drawHorizontalLine(routingLabel.getY() - 4, 108.0f, static_cast<float>(getWidth() - 12));
    g.drawHorizontalLine(lowNote.getY() - 9, 108.0f, static_cast<float>(getWidth() - 12));
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::resized()
{
    auto area = getLocalBounds().reduced(9);
    auto top = area.removeFromTop(25);
    layerTitle.setBounds(top.removeFromLeft(juce::jmin(68, juce::jmax(54, top.getWidth() - 54))));
    muteButton.setBounds(top.removeFromLeft(27).reduced(1));
    soloButton.setBounds(top.removeFromLeft(27).reduced(1));
    auto layerActions = area.removeFromTop(25);
    editButton.setBounds(layerActions.removeFromLeft(70).reduced(1));
    resetButton.setBounds(layerActions.removeFromRight(52).reduced(1));
    const auto type = processor.layerType(index);
    const bool isPadLayer = type == ClassicPlayerAudioProcessor::LayerType::drumPads
                         || type == ClassicPlayerAudioProcessor::LayerType::continuousPads;
    area.removeFromTop(5);
    if (isPadLayer)
        categoryArtworkBounds = {};
    else
    {
        categoryArtworkBounds = area.removeFromTop(60).reduced(1, 0);
        area.removeFromTop(5);
    }
    auto summaryRow = area.removeFromTop(32);
    sourceSummary.setBounds(summaryRow.reduced(2, 0));
    area.removeFromTop(4);
    modulationButton.setVisible(false);
    if (isPadLayer)
    {
        gain.setSliderStyle(juce::Slider::LinearVertical);
        gain.setTextBoxStyle(juce::Slider::TextBoxBelow,false,58,18);
        gain.setTooltip("Volume da layer de drum pads");
        auto faderArea = area.removeFromRight(100).reduced(8, 4);
        volumeLearn.setBounds(faderArea.removeFromBottom(24).withWidth(juce::jmin(86, faderArea.getWidth())));
        faderArea.removeFromBottom(6);
        meter.setBounds(faderArea.removeFromLeft(18).reduced(1, 2));
        const auto faderWidth = juce::jmin(82, juce::jmax(54, faderArea.getWidth() / 2));
        gain.setBounds(faderArea.removeFromLeft(faderWidth).reduced(4, 2));
        drumPadPanel.setBounds(area);
        return;
    }
    gain.setSliderStyle(juce::Slider::LinearVertical);
    gain.setTextBoxStyle(juce::Slider::TextBoxBelow,false,58,18);
    if (! expanded)
    {
        auto faderArea = area.reduced(8, 4);
        volumeLearn.setBounds(faderArea.removeFromBottom(24).withWidth(juce::jmin(86, faderArea.getWidth())));
        faderArea.removeFromBottom(6);
        meter.setBounds(faderArea.removeFromLeft(18).reduced(1, 2));
        const auto faderWidth = juce::jmin(82, juce::jmax(54, faderArea.getWidth() / 2));
        gain.setBounds(faderArea.removeFromLeft(faderWidth).reduced(4, 2));
        return;
    }
    if (type == ClassicPlayerAudioProcessor::LayerType::sf2)
    {
        loadButton.setBounds(area.removeFromTop(28));
        area.removeFromTop(4);
        categoryBox.setBounds(area.removeFromTop(28));
        area.removeFromTop(4);
        auto libraryRow = area.removeFromTop(28);
        deleteLibraryButton.setBounds(libraryRow.removeFromRight(76).reduced(1, 0));
        libraryBox.setBounds(libraryRow);
        area.removeFromTop(4);
        fileLabel.setBounds(area.removeFromTop(27));
        area.removeFromTop(4);
        presetBox.setBounds(area.removeFromTop(28));
    }
    else if (type == ClassicPlayerAudioProcessor::LayerType::vst)
    {
        externalInstrumentBox.setBounds(area.removeFromTop(28));
        area.removeFromTop(4);
        auto externalRow = area.removeFromTop(28);
        externalInstrumentButton.setBounds(externalRow.removeFromLeft(externalRow.getWidth() / 2).reduced(1, 0));
        openExternalEditorButton.setBounds(externalRow.reduced(1, 0));
        area.removeFromTop(4);
        fileLabel.setBounds(area.removeFromTop(27));
    }
    else if (type == ClassicPlayerAudioProcessor::LayerType::dx7)
    {
        dx7Button.setBounds(area.removeFromTop(28));
        area.removeFromTop(4);
        auto bankRow = area.removeFromTop(28);
        deleteDx7LibraryButton.setBounds(bankRow.removeFromRight(86).reduced(1, 0));
        dx7LibraryBox.setBounds(bankRow);
        area.removeFromTop(4);
        fileLabel.setBounds(area.removeFromTop(27));
        area.removeFromTop(4);
        dx7PatchBox.setBounds(area.removeFromTop(28));
    }
    else
    {
        dx7Button.setBounds(area.removeFromTop(30));
        area.removeFromTop(5);
        fileLabel.setBounds(area.removeFromTop(28));
    }
    area.removeFromTop(10);

    auto controls = area;
    auto faderColumn = controls.removeFromLeft(96);
    meter.setBounds(faderColumn.removeFromLeft(16).reduced(1, 7));
    volumeLearn.setBounds(faderColumn.removeFromBottom(22).reduced(1, 0));
    gain.setBounds(faderColumn.reduced(1, 0));
    controls.removeFromLeft(4);

    auto knobs = controls.removeFromTop(126);
    const auto knobCount = type == ClassicPlayerAudioProcessor::LayerType::dx7 ? 7 : 6;
    const auto knobWidth = knobs.getWidth() / knobCount;
    auto placeKnob = [knobWidth](juce::Rectangle<int>& row, juce::Label& label,
                                 juce::Slider& slider, juce::TextButton* learn,
                                 juce::TextButton* edit)
    {
        auto cell = row.removeFromLeft(knobWidth).reduced(2, 0);
        auto titleRow = cell.removeFromTop(16);
        if (edit != nullptr)
        {
            edit->setBounds(titleRow.removeFromRight(28).reduced(1, 0));
            label.setBounds(titleRow);
        }
        else
        {
            label.setBounds(titleRow);
        }
        if (learn != nullptr) learn->setBounds(cell.removeFromBottom(20).reduced(1));
        slider.setBounds(cell);
    };
    placeKnob(knobs, attackLabel, attack, nullptr, nullptr);
    placeKnob(knobs, releaseLabel, release, nullptr, nullptr);
    placeKnob(knobs, cutoffLabel, cutoff, &cutoffLearn, nullptr);
    placeKnob(knobs, reverbLabel, reverb, &reverbLearn, &reverbEditButton);
    placeKnob(knobs, compressorLabel, compressor, &compressorLearn, &compressorEditButton);
    if (type == ClassicPlayerAudioProcessor::LayerType::dx7)
        placeKnob(knobs, chorusLabel, chorus, nullptr, &chorusEditButton);
    else
    {
        chorus.setVisible(false);
        chorusEditButton.setVisible(false);
        chorusLabel.setVisible(false);
    }

    controls.removeFromTop(5);
    auto routingRow = controls.removeFromTop(22);
    resetMidiLearnButton.setBounds(routingRow.removeFromRight(76).reduced(1, 1));
    muteLearn.setBounds(routingRow.removeFromRight(74).reduced(1, 1));
    routingLabel.setBounds(routingRow);
    controls.removeFromTop(6);
    auto row = controls.removeFromTop(31);
    mode.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(2, 1));
    sustain.setBounds(row.reduced(2, 1));
    controls.removeFromTop(7);
    midiDevice.setBounds(controls.removeFromTop(31).reduced(2, 1));
    controls.removeFromTop(7);
    row = controls.removeFromTop(31);
    midiChannel.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(2, 1));
    octave.setBounds(row.reduced(2, 1));
    controls.removeFromTop(18);
    row = controls.removeFromTop(31);
    lowNote.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(2, 1));
    highNote.setBounds(row.reduced(2, 1));
    controls.removeFromTop(7);
    velocityCurve.setBounds(controls.removeFromTop(31).reduced(2, 1));
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::chooseSoundFont()
{
    fileChooser = std::make_unique<juce::FileChooser>(localizedUiText("Escolha um SoundFont", activeUiLanguage.load()), juce::File{}, "*.sf2;*.SF2");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode |
                             juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();
            if (file == juce::File{}) return;
            externalEditorWindow.reset();
            loadButton.setEnabled(false);
            fileLabel.setText(localizedUiText("Carregando...", activeUiLanguage.load()), juce::dontSendNotification);
            juce::File importedFile;
            auto result = processor.importSoundFont(file, selectedCategoryId(categoryBox), importedFile);
            if (result.wasOk()) result = processor.loadSoundFont(index, importedFile);
            loadButton.setEnabled(true);
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Falha ao carregar SF2", result.getErrorMessage());
            refresh();
        });
}

namespace
{
class HostedInstrumentEditorWindow final : public juce::DocumentWindow
{
public:
    HostedInstrumentEditorWindow(const juce::String& title, juce::AudioProcessorEditor* editor)
        : DocumentWindow(title, juce::Colour(0xff111820), DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(editor, true);
        centreWithSize(juce::jmax(420, getWidth()), juce::jmax(300, getHeight()));
        setVisible(true);
    }

    void closeButtonPressed() override { setVisible(false); }
};
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::chooseExternalInstrument()
{
    if (!processor.supportsExternalInstruments())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               "Instrumento externo",
                                               "O carregamento de VST/AU está disponível apenas no aplicativo standalone.");
        return;
    }

   #if JUCE_MAC
    const auto filters = "*.vst3;*.component";
   #else
    const auto filters = "*.vst3";
   #endif
    fileChooser = std::make_unique<juce::FileChooser>(localizedUiText("Escolha um instrumento virtual", activeUiLanguage.load()),
                                                      juce::File{}, filters);
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();
            if (file == juce::File{}) return;
            externalEditorWindow.reset();
            externalInstrumentButton.setEnabled(false);
            fileLabel.setText(localizedUiText("Carregando instrumento...", activeUiLanguage.load()), juce::dontSendNotification);
            const auto result = processor.loadExternalInstrument(index, file);
            externalInstrumentButton.setEnabled(true);
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Falha ao carregar instrumento", result.getErrorMessage());
            refresh();
        });
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::chooseDx7()
{
    fileChooser = std::make_unique<juce::FileChooser>(localizedUiText("Escolha um arquivo DX7 SysEx", activeUiLanguage.load()),
                                                      juce::File{}, "*.syx;*.SYX");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();
            if (file == juce::File{}) return;
            dx7Button.setEnabled(false);
            fileLabel.setText(localizedUiText("Importando DX7...", activeUiLanguage.load()), juce::dontSendNotification);
            juce::File importedFile;
            auto result = processor.importDx7Bank(file, importedFile);
            if (result.wasOk()) result = processor.loadDx7(index, importedFile);
            dx7Button.setEnabled(true);
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                    "Falha ao carregar DX7", result.getErrorMessage());
            refresh();
        });
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::updateSourceTypeVisibility()
{
    const auto type = processor.layerType(index);
    const auto isSf2 = type == ClassicPlayerAudioProcessor::LayerType::sf2;
    const auto isVst = type == ClassicPlayerAudioProcessor::LayerType::vst;
    const auto isDx7 = type == ClassicPlayerAudioProcessor::LayerType::dx7;
    const auto isAnalog = type == ClassicPlayerAudioProcessor::LayerType::analog;
    const auto isHammond = type == ClassicPlayerAudioProcessor::LayerType::hammond;
    const auto isDrumPads = type == ClassicPlayerAudioProcessor::LayerType::drumPads || type == ClassicPlayerAudioProcessor::LayerType::continuousPads;
    // Drum samples are edited in their dedicated window. The mixer strip
    // mirrors the other layers: vertical fader, meter and volume CC Learn.
    drumPadPanel.setVisible(isDrumPads);
    drumPadPanel.setControlsVisible(false);

    // Restore the shared layer controls on every non-drum refresh. Without
    // this explicit reset, switching from Drum Pads left the meter, knobs and
    // routing controls hidden in the next SF2/DX7/Analog layer.
    const std::initializer_list<juce::Component*> sharedControls {
        &fileLabel, &gain, &attack, &release, &cutoff, &reverb, &compressor, &mode, &sustain,
        &midiChannel, &octave, &lowNote, &highNote, &velocityCurve, &midiDevice,
        &volumeLearn, &resetMidiLearnButton, &muteLearn, &cutoffLearn, &reverbLearn,
        &compressorLearn, &reverbEditButton, &compressorEditButton, &meter,
        &chorus, &chorusEditButton, &attackLabel, &releaseLabel, &cutoffLabel, &reverbLabel, &compressorLabel,
        &chorusLabel, &routingLabel,
        &modulationButton
    };
    for (auto* control : sharedControls)
        control->setVisible(!isDrumPads);

    loadButton.setVisible(isSf2);
    categoryBox.setVisible(isSf2);
    libraryBox.setVisible(isSf2);
    deleteLibraryButton.setVisible(isSf2);
    presetBox.setVisible(isSf2);
    externalInstrumentBox.setVisible(isVst && processor.supportsExternalInstruments());
    externalInstrumentButton.setVisible(isVst && processor.supportsExternalInstruments());
    openExternalEditorButton.setVisible(isVst && processor.supportsExternalInstruments());
    dx7Button.setVisible(isDx7 || isAnalog || isHammond);
    modulationButton.setVisible(false);
    mode.setEnabled(!isHammond); velocityCurve.setEnabled(!isHammond);
    dx7LibraryBox.setVisible(isDx7);
    dx7PatchBox.setVisible(isDx7);
    deleteDx7LibraryButton.setVisible(isDx7);
    editButton.setVisible(true);
    chorus.setVisible(isDx7 && expanded);
    chorusEditButton.setVisible(isDx7 && expanded);
    chorusLabel.setVisible(isDx7 && expanded);
    sourceSummary.setText(fileLabel.getText().isNotEmpty()
                              ? fileLabel.getText()
                              : (isSf2 ? "SF2" : isDx7 ? "DX7" : isAnalog ? "CLASSIC KEYS ANALOG"
                                                   : isHammond ? "HAMMOND" : isVst ? "VST" : "DRUM PADS"),
                          juce::dontSendNotification);
    if (isDrumPads)
    {
        sourceSummary.setText(localizedUiText(
            type==ClassicPlayerAudioProcessor::LayerType::continuousPads ? "PAD CONTINUO" : "DRUM PADS",
            activeUiLanguage.load()), juce::dontSendNotification);
        const std::initializer_list<juce::Component*> controls {
            &loadButton, &externalInstrumentButton, &dx7Button, &deleteDx7LibraryButton,
            &openExternalEditorButton, &deleteLibraryButton, &categoryBox, &libraryBox,
            &presetBox, &externalInstrumentBox, &dx7LibraryBox, &dx7PatchBox, &fileLabel,
            &gain, &attack, &release, &cutoff, &reverb, &compressor, &mode, &sustain, &midiChannel,
            &octave, &lowNote, &highNote, &velocityCurve, &midiDevice,
            &volumeLearn, &resetMidiLearnButton, &muteLearn, &cutoffLearn, &reverbLearn,
            &compressorLearn, &reverbEditButton, &compressorEditButton, &meter,
            &chorus, &chorusEditButton, &attackLabel, &releaseLabel, &cutoffLabel, &reverbLabel, &compressorLabel,
            &chorusLabel, &routingLabel
        };
        for (auto* control : controls)
            control->setVisible(false);
        gain.setVisible(true);
        meter.setVisible(true);
        volumeLearn.setVisible(false);
        sourceSummary.setVisible(true);
        resized();
        return;
    }
    if (! expanded)
    {
        const std::initializer_list<juce::Component*> detailedControls {
            &loadButton, &externalInstrumentButton, &dx7Button, &deleteDx7LibraryButton,
            &openExternalEditorButton, &deleteLibraryButton, &categoryBox, &libraryBox,
            &presetBox, &externalInstrumentBox, &dx7LibraryBox, &dx7PatchBox, &fileLabel,
            &attack, &release, &cutoff, &reverb, &compressor, &mode, &sustain, &midiChannel, &octave,
            &lowNote, &highNote, &velocityCurve, &midiDevice,
            &resetMidiLearnButton, &muteLearn, &cutoffLearn, &reverbLearn, &compressorLearn,
            &reverbEditButton, &compressorEditButton, &chorus, &chorusEditButton,
            &attackLabel, &releaseLabel, &cutoffLabel, &reverbLabel, &compressorLabel, &chorusLabel, &routingLabel
        };
        for (auto* control : detailedControls)
            control->setVisible(false);
        sourceSummary.setVisible(true);
        gain.setVisible(true);
        meter.setVisible(true);
        volumeLearn.setVisible(true);
        resized();
        return;
    }
    sourceSummary.setVisible(false);
    fileLabel.setVisible(true);
    if (isHammond) fileLabel.setText(localizedUiText("HAMMOND", activeUiLanguage.load()), juce::dontSendNotification);
    if (isAnalog)
        fileLabel.setText(localizedUiText("CLASSIC KEYS ANALOG", activeUiLanguage.load()), juce::dontSendNotification);
    resized();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::closeExternalInstrumentEditor()
{
    // The plug-in owns this native editor. It must be gone before a saved
    // program can unload, replace, or restore the hosted instrument.
    externalEditorWindow.reset();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::openExternalInstrumentEditor()
{
    // Closing a hosted editor only hides its DocumentWindow. Reuse that same
    // window/editor on the next click: asking JUCE for another editor before
    // destroying the hidden window can hand out the existing editor pointer.
    if (externalEditorWindow != nullptr)
    {
        externalEditorWindow->setVisible(true);
        externalEditorWindow->toFront(true);
        return;
    }

    if (auto* editor = processor.createExternalInstrumentEditor(index))
    {
        externalEditorWindow = std::make_unique<HostedInstrumentEditorWindow>(
            processor.externalInstrumentName(index), editor);
        return;
    }

    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                           "Editor indisponível",
                                           "Este instrumento virtual não possui uma janela de edição.");
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::deleteSelectedSoundFont()
{
    const auto selected = libraryBox.getSelectedItemIndex();
    if (!juce::isPositiveAndBelow(selected, libraryFiles.size())) return;
    const auto file = libraryFiles.getReference(selected);
    const juce::Component::SafePointer<LayerStrip> safe(this);
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon,
                                       "Excluir SF2",
                                       "Excluir '" + file.getFileName() + "' da biblioteca?",
                                       "Excluir", "Cancelar", this,
                                       juce::ModalCallbackFunction::create(
                                           [safe, file](int answer)
                                           {
                                               if (safe == nullptr || answer == 0) return;
                                               const auto result = safe->processor.deleteLibrarySoundFont(file);
                                               if (result.failed())
                                                   juce::AlertWindow::showMessageBoxAsync(
                                                       juce::MessageBoxIconType::WarningIcon,
                                                       "Falha ao excluir SF2", result.getErrorMessage());
                                               safe->refresh();
                                           }));
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::resetLayer()
{
    externalEditorWindow.reset();
    processor.unloadSoundFont(index);
    processor.unloadExternalInstrument(index);
    processor.unloadDx7(index);
    processor.setLayerType(index, ClassicPlayerAudioProcessor::LayerType::sf2);
    muted = solo = false;
    processor.setLayerMuted(index, false);
    muteButton.setToggleState(false, juce::dontSendNotification);
    soloButton.setToggleState(false, juce::dontSendNotification);
    cutoff.setValue(100.0);
    reverb.setValue(0.0);
    compressor.setValue(0.0);
    gain.setValue(80.0);
    mode.setSelectedId(1);
    sustain.setSelectedId(1);
    midiChannel.setSelectedId(2);
    octave.setSelectedId(5);
    lowNote.setSelectedId(1);
    highNote.setSelectedId(128);
    velocityCurve.setSelectedId(1);
    if (auto* parameter = processor.parameters.getParameter(
            "layer" + juce::String(index + 1) + "ModulationEnabled"))
        parameter->setValueNotifyingHost(parameter->getDefaultValue());
    refresh();
    mixStateChanged();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::rebuildPresets()
{
    presets = processor.layerPresets(index);
    presetBox.clear(juce::dontSendNotification);
    for (int i = 0; i < (int) presets.size(); ++i)
    {
        const auto& preset = presets[(size_t) i];
        presetBox.addItem(juce::String(preset.bank) + ":" + juce::String(preset.program)
                          + "  " + preset.name, i + 1);
    }
    const auto selectedBank = processor.layerPresetBank(index);
    const auto selectedProgram = processor.layerPresetProgram(index);
    for (int i = 0; i < (int) presets.size(); ++i)
        if (presets[(size_t) i].bank == selectedBank && presets[(size_t) i].program == selectedProgram)
        {
            presetBox.setSelectedId(i + 1, juce::dontSendNotification);
            break;
        }
    presetBox.setEnabled(!presets.empty());
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::rebuildExternalInstrumentLibrary()
{
    externalInstrumentFiles = processor.availableExternalInstruments();
    externalInstrumentBox.clear(juce::dontSendNotification);
    for (int item = 0; item < externalInstrumentFiles.size(); ++item)
        externalInstrumentBox.addItem(externalInstrumentFiles.getReference(item).getFileNameWithoutExtension(), item + 1);
    externalInstrumentBox.setTextWhenNothingSelected(
        externalInstrumentFiles.isEmpty() ? "NENHUM VST ENCONTRADO" : "VST INSTALADO");
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::refreshExternalInstrumentLibrary()
{
    rebuildExternalInstrumentLibrary();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::rebuildLibrary()
{
    const auto currentPath = processor.soundFontPath(index);
    libraryFiles = processor.librarySoundFonts(selectedCategoryId(categoryBox));
    libraryBox.clear(juce::dontSendNotification);
    auto selectedId = 0;
    for (int item = 0; item < libraryFiles.size(); ++item)
    {
        const auto& file = libraryFiles.getReference(item);
        libraryBox.addItem(file.getFileNameWithoutExtension(), item + 1);
        if (file.getFullPathName() == currentPath) selectedId = item + 1;
    }
    libraryBox.setTextWhenNothingSelected(libraryFiles.isEmpty() ? "CATEGORIA VAZIA" : "ESCOLHA O SF2");
    if (selectedId > 0) libraryBox.setSelectedId(selectedId, juce::dontSendNotification);
    deleteLibraryButton.setEnabled(libraryBox.getSelectedItemIndex() >= 0);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::selectCurrentCategory()
{
    const auto path = processor.soundFontPath(index);
    if (path.isEmpty()) return;

    const auto categories = ClassicPlayerAudioProcessor::soundFontCategories();
    const auto parentCategory = juce::File(path).getParentDirectory().getFileName();
    int selected = categories.indexOf(parentCategory);
    if (selected < 0)
    {
        for (int category = 0; category < categories.size(); ++category)
        {
            const auto files = processor.librarySoundFonts(categories[category]);
            for (const auto& file : files)
                if (file.getFullPathName() == path)
                {
                    selected = category;
                    break;
                }
            if (selected >= 0) break;
        }
    }
    if (selected >= 0)
        categoryBox.setSelectedId(selected + 1, juce::dontSendNotification);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::rebuildDx7Library()
{
    const auto currentPath = processor.dx7Path(index);
    dx7LibraryFiles = processor.libraryDx7Banks();
    dx7LibraryBox.clear(juce::dontSendNotification);
    int selectedId = 0;
    for (int item = 0; item < dx7LibraryFiles.size(); ++item)
    {
        const auto& file = dx7LibraryFiles.getReference(item);
        dx7LibraryBox.addItem(file.getFileNameWithoutExtension(), item + 1);
        if (file.getFullPathName() == currentPath) selectedId = item + 1;
    }
    dx7LibraryBox.setTextWhenNothingSelected(dx7LibraryFiles.isEmpty()
        ? "BIBLIOTECA DX7 VAZIA" : "ESCOLHA O BANCO DX7");
    if (selectedId > 0) dx7LibraryBox.setSelectedId(selectedId, juce::dontSendNotification);
    deleteDx7LibraryButton.setEnabled(dx7LibraryBox.getSelectedItemIndex() >= 0);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::rebuildDx7Patches()
{
    dx7PatchBox.clear(juce::dontSendNotification);
    const auto count = processor.dx7PatchCount(index);
    for (int patch = 0; patch < count; ++patch)
        dx7PatchBox.addItem(juce::String(patch + 1) + ": " + processor.dx7PatchName(index, patch), patch + 1);
    if (count > 0)
        dx7PatchBox.setSelectedId(processor.dx7SelectedPatch(index) + 1, juce::dontSendNotification);
    dx7PatchBox.setEnabled(count > 0);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::deleteSelectedDx7Bank()
{
    const auto selected = dx7LibraryBox.getSelectedItemIndex();
    if (!juce::isPositiveAndBelow(selected, dx7LibraryFiles.size())) return;
    const auto file = dx7LibraryFiles.getReference(selected);
    const juce::Component::SafePointer<LayerStrip> safe(this);
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon,
        "Excluir DX7", "Excluir '" + file.getFileName() + "' da biblioteca?",
        "Excluir", "Cancelar", this, juce::ModalCallbackFunction::create(
        [safe, file](int answer)
        {
            if (safe == nullptr || answer == 0) return;
            const auto result = safe->processor.deleteLibraryDx7Bank(file);
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                    "Falha ao excluir DX7", result.getErrorMessage());
            safe->refresh();
        }));
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::refresh()
{
    const auto type = processor.layerType(index);
    muted = processor.isLayerMuted(index);
    muteButton.setToggleState(muted, juce::dontSendNotification);
    if (type == ClassicPlayerAudioProcessor::LayerType::drumPads || type == ClassicPlayerAudioProcessor::LayerType::continuousPads)
    {
        updateSourceTypeVisibility();
        drumPadPanel.refresh();
        // Drum-pad layers do not use a SoundFont.  Keep the source label
        // explicit so the mixer never presents them as an empty SF2 layer.
        fileLabel.setText(localizedUiText(
            type==ClassicPlayerAudioProcessor::LayerType::continuousPads ? "PAD CONTINUO" : "DRUM PADS",
            activeUiLanguage.load()), juce::dontSendNotification);
        fileLabel.setColour(juce::Label::backgroundColourId, juce::Colour(yellow));
        fileLabel.setColour(juce::Label::textColourId, juce::Colours::black);
        sourceSummary.setTooltip(fileLabel.getText());
        return;
    }
    const auto path = processor.soundFontPath(index);
    const auto externalName = processor.externalInstrumentName(index);
    const auto dx7Name = processor.dx7PatchName(index);
    dx7Button.setButtonText(type == ClassicPlayerAudioProcessor::LayerType::analog
                                 ? "ABRIR CLASSIC KEYS ANALOG" : type == ClassicPlayerAudioProcessor::LayerType::hammond ? "ABRIR HAMMOND" : "IMPORTAR DX7");
    if (type == ClassicPlayerAudioProcessor::LayerType::sf2)
    {
        selectCurrentCategory();
        rebuildLibrary();
        rebuildPresets();
    }
    else if (type == ClassicPlayerAudioProcessor::LayerType::vst)
        rebuildExternalInstrumentLibrary();
    else if (type == ClassicPlayerAudioProcessor::LayerType::dx7)
    {
        rebuildDx7Library();
        rebuildDx7Patches();
    }

    const auto hasSource = type == ClassicPlayerAudioProcessor::LayerType::sf2 ? path.isNotEmpty()
        : type == ClassicPlayerAudioProcessor::LayerType::vst ? externalName.isNotEmpty()
        : type == ClassicPlayerAudioProcessor::LayerType::dx7 ? processor.hasDx7(index)
        : type == ClassicPlayerAudioProcessor::LayerType::hammond || processor.hasAnalogSynth(index);
    const auto sourceLabel = type == ClassicPlayerAudioProcessor::LayerType::sf2
        ? (path.isNotEmpty() ? juce::File(path).getFileNameWithoutExtension()
                             : localizedUiText("Sem SoundFont", activeUiLanguage.load()))
        : type == ClassicPlayerAudioProcessor::LayerType::vst
            ? (externalName.isNotEmpty() ? externalName
                                         : localizedUiText("Sem VST", activeUiLanguage.load()))
            : type == ClassicPlayerAudioProcessor::LayerType::dx7
                ? (dx7Name.isNotEmpty() ? dx7Name
                                        : localizedUiText("Sem DX7", activeUiLanguage.load()))
                : type == ClassicPlayerAudioProcessor::LayerType::hammond
                    ? "HAMMOND" : "CLASSIC KEYS ANALOG";
    fileLabel.setText(sourceLabel, juce::dontSendNotification);
    fileLabel.setColour(juce::Label::backgroundColourId,
                        hasSource ? juce::Colour(yellow) : juce::Colour(0xff0b1218));
    fileLabel.setColour(juce::Label::textColourId,
                        hasSource ? juce::Colours::black : juce::Colour(mutedText));
    sourceSummary.setText(sourceLabel, juce::dontSendNotification);
    sourceSummary.setTooltip(fileLabel.getText());
    openExternalEditorButton.setEnabled(processor.supportsExternalInstruments()
                                        && processor.hasExternalInstrument(index));
    const auto config = processor.layerConfig(index);
    const auto modulationParameter = processor.parameters.getRawParameterValue(
        "layer" + juce::String(index + 1) + "ModulationEnabled");
    const auto modulationEnabled = modulationParameter == nullptr || modulationParameter->load() >= 0.5f;
    modulationButton.setToggleState(modulationEnabled, juce::dontSendNotification);
    modulationButton.setButtonText(modulationEnabled ? "MOD: ON" : "MOD: OFF");
    modulationButton.setColour(juce::TextButton::buttonColourId,
                               modulationEnabled ? juce::Colour(0xff1b554e)
                                                  : juce::Colour(panelLight));
    mode.setSelectedId(config.portamento ? 3 : (config.mono ? 2 : 1),
                       juce::dontSendNotification);
    sustain.setSelectedId(config.sustainEnabled ? 1 : 2, juce::dontSendNotification);
    midiChannel.setSelectedId(config.midiChannel + 1, juce::dontSendNotification);
    octave.setSelectedId(config.octave + 5, juce::dontSendNotification);
    lowNote.setSelectedId(config.lowNote + 1, juce::dontSendNotification);
    highNote.setSelectedId(config.highNote + 1, juce::dontSendNotification);
    velocityCurve.setSelectedId(config.velocityCurve + 1, juce::dontSendNotification);
    updateSourceTypeVisibility();
    repaint();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::applyConfig()
{
    auto config = processor.layerConfig(index);
    config.mono = mode.getSelectedId() >= 2;
    config.portamento = mode.getSelectedId() == 3;
    config.sustainEnabled = sustain.getSelectedId() != 2;
    config.midiChannel = juce::jlimit(0, 16, midiChannel.getSelectedId() - 1);
    config.octave = octave.getSelectedId() - 5;
    config.lowNote = juce::jlimit(0, 127, lowNote.getSelectedId() - 1);
    config.highNote = juce::jlimit(0, 127, highNote.getSelectedId() - 1);
    config.velocityCurve = juce::jlimit(0, 2, velocityCurve.getSelectedId() - 1);
    if (config.lowNote > config.highNote)
    {
        config.highNote = config.lowNote;
        highNote.setSelectedId(config.highNote + 1, juce::dontSendNotification);
    }
    processor.setLayerConfig(index, config);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::setEngineEnabled(bool enabled)
{
    auto config = processor.layerConfig(index);
    config.enabled = enabled;
    processor.setLayerConfig(index, config);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::updateMeter()
{
    meter.setLevel(juce::jlimit(0.0f, 1.0f, processor.layerPeak(index)));
    const auto processorMuted = processor.isLayerMuted(index);
    if (muted != processorMuted)
    {
        muted = processorMuted;
        muteButton.setToggleState(muted, juce::dontSendNotification);
        mixStateChanged();
    }
    // MIDI Learn updates the processor from the message-thread bridge. Mirror
    // the current parameter values explicitly so the on-screen knobs follow
    // the physical controller even when a host delays attachment callbacks.
    const auto prefix = "layer" + juce::String(index + 1);
    const auto sync = [this, prefix](juce::Slider& slider, const char* suffix)
    {
        if (const auto* parameter = processor.parameters.getParameter(prefix + suffix))
        {
            // MIDI processing updates the parameter object first. Reading its
            // authoritative value prevents an audible CC change from leaving
            // the on-screen knob stationary while an APVTS callback is late.
            const auto value = parameter->convertFrom0to1(parameter->getValue());
            if (std::abs(slider.getValue() - value) > 0.0001)
            {
                slider.setValue(value, juce::dontSendNotification);
            }
        }
    };
    sync(gain, "Gain");
    sync(cutoff, "Cutoff");
    sync(reverb, "Reverb");
    sync(compressor, "Comp");
    sync(release, "Release");
    if (processor.layerType(index)==ClassicPlayerAudioProcessor::LayerType::drumPads || processor.layerType(index)==ClassicPlayerAudioProcessor::LayerType::continuousPads)
        drumPadPanel.refresh();
    updateMidiLearnState();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::updateMidiLearnState()
{
    using Target = ClassicPlayerAudioProcessor::LearnTarget;
    const std::array<std::pair<Target, juce::TextButton*>, 5> controls {{
        { Target::volume, &volumeLearn }, { Target::cutoff, &cutoffLearn },
        { Target::reverb, &reverbLearn }, { Target::compressor, &compressorLearn },
        { Target::mute, &muteLearn }
    }};
    for (const auto& [target, button] : controls)
    {
        const auto learning = processor.isMidiLearning(index, target);
        const auto cc = processor.midiLearnCC(index, target);
        const auto channel = processor.midiLearnChannel(index, target);
        const auto mappingText = cc < 0 ? juce::String("LEARN")
            : "CC " + juce::String(cc) + (channel > 0 ? " C" + juce::String(channel) : juce::String{});
        setButtonTextIfChanged(*button, localizedUiText(learning ? "MOVA O CC" : mappingText,
                                              activeUiLanguage.load()));
        setButtonColourIfChanged(*button, juce::TextButton::buttonColourId,
                          learning ? juce::Colour(yellow)
                                   : cc >= 0 ? juce::Colour(0xff1b554e) : juce::Colour(panelLight));
        setButtonColourIfChanged(*button, juce::TextButton::textColourOffId,
                          learning ? juce::Colour(background) : juce::Colour(text));
    }
}

ClassicPlayerAudioProcessorEditor::ClassicPlayerAudioProcessorEditor(
    ClassicPlayerAudioProcessor& p, bool shouldValidateOnlineSession)
    : AudioProcessorEditor(&p), classicProcessor(p), keyboard(p.keyboardState)
{
    juce::Logger::writeToLog("Editor Classic Player inicializado");
    uiLanguage = readUiLanguagePreference();
    activeUiLanguage.store(uiLanguage);
    uiSkin = readUiSkinPreference();
    setUiPalette(uiSkin);
    classicLookAndFeel.applyCurrentPalette();
    setLookAndFeel(&classicLookAndFeel);
    setOpaque(true);
#if JucePlugin_Build_Standalone
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
        classicProcessor.attachStandaloneMidiRouting(holder->deviceManager, holder->player);
#endif
    // Scan the standard VST3/AU locations once when the standalone editor opens.
    if (classicProcessor.supportsExternalInstruments())
        classicProcessor.refreshExternalInstrumentLibrary();

    // UI preferences live in the processor state so JUCE's standalone holder
    // persists them together with the current performance when the app exits.
    // Keeping them outside the editor also means reopening the editor in the
    // same session restores the user's choices immediately.
    const auto uiState = classicProcessor.parameters.state;
    chordColour = juce::Colour::fromString(
        uiState.getProperty("uiChordColour", chordColour.toString()).toString());
    const auto savedKeyColour = juce::Colour::fromString(
        uiState.getProperty("uiKeyColour",
                            keyboard.findColour(juce::MidiKeyboardComponent::keyDownOverlayColourId)
                                .toString()).toString());
    keyboard.setActiveColour(savedKeyColour);
    virtualKeyboardVisible = static_cast<bool>(
        uiState.getProperty("uiVirtualKeyboardVisible", true));

    appIcon.setImage(embeddedImage("classicplayerappicon_png"), juce::RectanglePlacement::centred);
    appIcon.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(appIcon);


    title.setText("CLASSIC PLAYER", juce::dontSendNotification);
    versionLabel.setText("v" JucePlugin_VersionString, juce::dontSendNotification);
    versionLabel.getProperties().set("uiDataText", true);
    versionLabel.setFont(juce::FontOptions(11.0f));
    versionLabel.setColour(juce::Label::textColourId, juce::Colour(text));
    addAndMakeVisible(versionLabel);
    title.setFont(juce::FontOptions(25.0f, juce::Font::bold));
    title.setColour(juce::Label::textColourId, brandTextColour());
    addAndMakeVisible(title);
    subtitle.setText("CLASSIC KEYS SF2 WORKSTATION", juce::dontSendNotification);
    subtitle.setColour(juce::Label::textColourId, juce::Colour(mutedText));
    addAndMakeVisible(subtitle); userLabel.getProperties().set("uiDataText", true); userLabel.setColour(juce::Label::textColourId, brandTextColour()); userLabel.setFont(juce::FontOptions(11.0f, juce::Font::bold)); userLabel.setJustificationType(juce::Justification::topLeft); userLabel.setText(accountIdentityText(), juce::dontSendNotification); addAndMakeVisible(userLabel);
    chordLabel.setText("-", juce::dontSendNotification);
    chordLabel.setFont(juce::FontOptions(36.0f, juce::Font::bold));
    chordLabel.setJustificationType(juce::Justification::centred);
    chordLabel.setColour(juce::Label::backgroundColourId, juce::Colours::black);
    chordLabel.setColour(juce::Label::textColourId, chordColour);
    addAndMakeVisible(chordLabel);
    // O visor já identifica visualmente a cifra; não exibir um rótulo adicional
    // acima do nome do acorde.
    chordCaption.setVisible(false);
    flatButton(chordColourButton);
    chordColourButton.onClick = [this]
    {
        const juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor> safe(this);
        auto picker = std::make_unique<ColourPicker>(chordColour, [safe](juce::Colour colour)
        {
            if (safe == nullptr) return;
            safe->chordColour = colour;
            safe->chordLabel.setColour(juce::Label::textColourId, colour);
            safe->chordLabel.repaint();
            safe->classicProcessor.parameters.state.setProperty(
                "uiChordColour", colour.toString(), nullptr);
        });
        juce::CallOutBox::launchAsynchronously(std::move(picker), chordColourButton.getScreenBounds(), nullptr);
    };
    addAndMakeVisible(chordColourButton);

    flatButton(keyColourButton);
    keyColourButton.onClick = [this]
    {
        const auto current = keyboard.findColour(juce::MidiKeyboardComponent::keyDownOverlayColourId);
        const juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor> safe(this);
        auto picker = std::make_unique<ColourPicker>(current, [safe](juce::Colour colour)
        {
            if (safe == nullptr) return;
            safe->keyboard.setActiveColour(colour);
            safe->classicProcessor.parameters.state.setProperty(
                "uiKeyColour", colour.toString(), nullptr);
        });
        juce::CallOutBox::launchAsynchronously(std::move(picker), keyColourButton.getScreenBounds(), nullptr);
    };
    addAndMakeVisible(keyColourButton);


    accidentalStyleBox.addItem("MISTO", 1);
    accidentalStyleBox.addItem("SUSTENIDO", 2);
    accidentalStyleBox.addItem("BEMOL", 3);
    accidentalStyleBox.setSelectedId(1, juce::dontSendNotification);
    accidentalStyleBox.setTooltip("Formato dos acidentes exibidos no visor de acordes");
    accidentalStyleBox.onChange = [this]
    {
        switch (accidentalStyleBox.getSelectedId())
        {
            case 2: accidentalStyle = ClassicChordDetector::AccidentalStyle::sharp; break;
            case 3: accidentalStyle = ClassicChordDetector::AccidentalStyle::flat; break;
            default: accidentalStyle = ClassicChordDetector::AccidentalStyle::mixed; break;
        }
        triggerAsyncUpdate();
    };
    addAndMakeVisible(accidentalStyleBox);

    programBox.getProperties().set("uiDataItems", true);
    programBox.setEditableText(true);
    programBox.setTextWhenNothingSelected("NOVO PROGRAMA");
    programBox.setTooltip(juce::String::fromUTF8("Selecione uma performance para carregá-la imediatamente; digite um nome para salvar uma nova"));
    programBox.onChange = [this]
    {
        // Text entry is used to name a new performance. Only a real list item
        // selection has a positive ID and should replace the current state.
        if (programBox.getSelectedId() > 0)
            loadSelectedProgram();
    };
    addAndMakeVisible(programBox);
    flatButton(newProgramButton);
    flatButton(saveProgramButton);
    flatButton(exportProgramButton);
    flatButton(deleteProgramButton);
    flatButton(importProgramButton);
    newProgramButton.setTooltip(juce::String::fromUTF8("Criar uma programação vazia do zero"));
    saveProgramButton.setTooltip("Salvar esta performance na biblioteca interna do Classic Player");
    exportProgramButton.setTooltip(juce::String::fromUTF8("Exportar uma cópia portátil da performance para outro computador"));
    importProgramButton.setTooltip(juce::String::fromUTF8("Importar uma performance portátil para a biblioteca deste computador e abri-la"));
    newProgramButton.onClick = [this] { createNewProgram(); };
    saveProgramButton.onClick = [this] { saveProgram(); };
    exportProgramButton.onClick = [this] { exportProgram(); };
    deleteProgramButton.onClick = [this] { deleteSelectedProgram(); };
    importProgramButton.onClick = [this] { importProgramFile(); };
    addAndMakeVisible(newProgramButton);
    addAndMakeVisible(saveProgramButton);
    addAndMakeVisible(exportProgramButton);
    addAndMakeVisible(deleteProgramButton);
    addAndMakeVisible(importProgramButton);
    refreshProgramLibrary();

    flatButton(addLayerButton);
    addLayerButton.onClick = [this]
    {
        juce::PopupMenu menu;
        menu.addItem(1, localizedUiText("Layer SF2", uiLanguage));
        menu.addItem(2, localizedUiText("Layer DX7 (.syx)", uiLanguage));
        menu.addItem(3, localizedUiText("Classic Keys Analog", uiLanguage));
        menu.addItem(4, localizedUiText("Layer Drum Pads (8)", uiLanguage));
        menu.addItem(5, localizedUiText("Hammond", uiLanguage));
        menu.addItem(6, localizedUiText("Pad Continuo (12)", uiLanguage));
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addLayerButton),
            [safeThis = juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor>(this)](int choice)
            {
                if (safeThis == nullptr || choice == 0) return;
                const auto type = choice == 1 ? ClassicPlayerAudioProcessor::LayerType::sf2
                                : choice == 2 ? ClassicPlayerAudioProcessor::LayerType::dx7
                                : choice == 3 ? ClassicPlayerAudioProcessor::LayerType::analog
                                : choice == 5 ? ClassicPlayerAudioProcessor::LayerType::hammond
                                : choice == 6 ? ClassicPlayerAudioProcessor::LayerType::continuousPads
                                              : ClassicPlayerAudioProcessor::LayerType::drumPads;
                safeThis->addLayer(type);
            });
    };
    addAndMakeVisible(addLayerButton);

    flatButton(recordingButton);
    recordingButton.setTooltip(juce::String::fromUTF8("Gravar simultaneamente a saída em WAV e a performance em MIDI"));
    recordingButton.onClick = [this]
    {
        if (classicProcessor.isAudioRecording())
        {
            classicProcessor.stopAudioRecording();
            recordingStatus.setText(localizedUiText("WAV + MIDI salvos na Area de Trabalho", activeUiLanguage.load()),
                                     juce::dontSendNotification);
            recordingButton.setButtonText(localizedUiText("GRAVAR WAV+MIDI", activeUiLanguage.load()));
            return;
        }

        const auto result = classicProcessor.startAudioRecording();
        if (result.failed())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "Não foi possível gravar", result.getErrorMessage());
            return;
        }
        recordingStartedAtMs = juce::Time::currentTimeMillis();
        recordingStatus.setText(localizedUiText("GRAVANDO 00:00", activeUiLanguage.load()), juce::dontSendNotification);
        recordingButton.setButtonText(localizedUiText("PARAR", activeUiLanguage.load()));
    };
    addAndMakeVisible(recordingButton);
    recordingStatus.setJustificationType(juce::Justification::centredLeft);
    recordingStatus.setColour(juce::Label::textColourId, juce::Colour(mutedText));
    recordingStatus.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    recordingStatus.setText("WAV + MIDI: Area de Trabalho", juce::dontSendNotification);
    addAndMakeVisible(recordingStatus);

    flatButton(panicButton);
    panicButton.setTooltip("Envia All Notes Off/All Sound Off e solta qualquer nota presa");
    panicButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff7d3540));
    panicButton.onClick = [this] { classicProcessor.panic(); };
    addAndMakeVisible(panicButton);
    flatButton(panicLearnButton);
    panicLearnButton.setTooltip("Aprender um MIDI CC para acionar o Panic");
    panicLearnButton.onClick = [this] { classicProcessor.beginPanicMidiLearn(); };
    panicLearnButton.onClearMapping = [this] { classicProcessor.resetPanicMidiLearn(); };
    addAndMakeVisible(panicLearnButton);

    flatButton(keyboardVisibilityButton);
    keyboardVisibilityButton.setTooltip(juce::String::fromUTF8("Mostrar ou ocultar o teclado virtual para liberar espaço para as layers"));
        keyboardVisibilityButton.setButtonText(localizedUiText(virtualKeyboardVisible ? "OCULTAR TECLADO"
                                                                                      : "MOSTRAR TECLADO",
                                                               activeUiLanguage.load()));
    keyboardVisibilityButton.onClick = [this]
    {
        virtualKeyboardVisible = !virtualKeyboardVisible;
        keyboardVisibilityButton.setButtonText(localizedUiText(virtualKeyboardVisible ? "OCULTAR TECLADO"
                                                                                      : "MOSTRAR TECLADO",
                                                               activeUiLanguage.load()));
        keyboard.setVisible(virtualKeyboardVisible && !showingLiveSet);
        classicProcessor.parameters.state.setProperty(
            "uiVirtualKeyboardVisible", virtualKeyboardVisible, nullptr);
        resized();
    };
    addAndMakeVisible(keyboardVisibilityButton);

    flatButton(audioMidiSettingsButton);
    audioMidiSettingsButton.setTooltip("Configurar dispositivo de audio, taxa de amostragem, buffer e MIDI");
    audioMidiSettingsButton.onClick = [this] { showAudioMidiSettings(); };
    addChildComponent(audioMidiSettingsButton);

    languageSelector.addItem(juce::String::fromUTF8("PT - Português"), 1);
    languageSelector.addItem("EN - English", 2);
    languageSelector.addItem(juce::String::fromUTF8("ES - Español"), 3);
    languageSelector.setSelectedId(uiLanguage + 1, juce::dontSendNotification);
    languageSelector.setTooltip(juce::String::fromUTF8("Selecionar idioma / Select language / Seleccionar idioma"));
    languageSelector.setColour(juce::ComboBox::backgroundColourId, juce::Colour(panelLight));
    languageSelector.setColour(juce::ComboBox::textColourId, juce::Colour(text));
    languageSelector.setColour(juce::ComboBox::outlineColourId, juce::Colour(paletteLine));
    languageSelector.onChange = [this]
    {
        const auto selected = languageSelector.getSelectedId();
        if (selected >= 1 && selected <= 3) setUiLanguage(selected - 1);
    };
    addAndMakeVisible(languageSelector);

    skinSelector.addItem(juce::String::fromUTF8("PADRÃO"), 1);
    skinSelector.addItem("PRETO", 2);
    skinSelector.addItem("VERMELHO", 3);
    skinSelector.addItem("AZUL ROXO", 4);
    skinSelector.addItem("BRANCO AZUL", 5);
    skinSelector.addItem("PRATEADO", 6);
    skinSelector.addItem(juce::String::fromUTF8("AÇO ESCURO"), 7);
    skinSelector.setSelectedId(uiSkin + 1, juce::dontSendNotification);
    skinSelector.setName(juce::String::fromUTF8("Tema de cores do aplicativo"));
    skinSelector.setTooltip(juce::String::fromUTF8("Tema de cores do aplicativo"));
    skinSelector.setColour(juce::ComboBox::backgroundColourId, juce::Colour(panelLight));
    skinSelector.setColour(juce::ComboBox::textColourId, juce::Colour(text));
    skinSelector.setColour(juce::ComboBox::outlineColourId, juce::Colour(paletteLine));
    skinSelector.onChange = [this]
    {
        const auto selected = skinSelector.getSelectedId();
        if (selected >= 1 && selected <= (int) uiPalettes.size()) setUiSkin(selected - 1);
    };
    addAndMakeVisible(skinSelector);
    skinCaption.setText("TEMA", juce::dontSendNotification);
    skinCaption.setName("TEMA");
    skinCaption.setColour(juce::Label::textColourId, juce::Colour(mutedText));
    skinCaption.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    skinCaption.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(skinCaption);

    flatButton(liveSetButton);
    liveSetButton.onClick = [this] { showLiveSet(!showingLiveSet); };
    addAndMakeVisible(liveSetButton);

    flatButton(editLiveSetButton);
    editLiveSetButton.onClick = [this]
    {
        editingLiveSet = !editingLiveSet;
        editLiveSetButton.setButtonText(localizedUiText(editingLiveSet ? "CONCLUIR EDICAO" : "EDITAR LIVE SET",
                                                        activeUiLanguage.load()));
        refreshLiveSet();
        resized();
    };
    addAndMakeVisible(editLiveSetButton);
    for (auto* button : { &livePreviousButton, &liveNextButton, &liveSettingsButton })
    {
        flatButton(*button);
        addAndMakeVisible(button);
    }
    livePreviousButton.onClick = [this] {
        activeLiveSetBank = juce::jmax(0, activeLiveSetBank - 1); refreshLiveSet();
    };
    liveNextButton.onClick = [this] {
        activeLiveSetBank = juce::jmin(ClassicPlayerAudioProcessor::liveSetBankCount - 1, activeLiveSetBank + 1); refreshLiveSet();
    };
    liveSettingsButton.onClick = [this] {
        juce::PopupMenu menu;
        juce::PopupMenu languageMenu;
        languageMenu.addItem(10, juce::String::fromUTF8("Português"), uiLanguage == 0);
        languageMenu.addItem(11, "English", uiLanguage == 1);
        languageMenu.addItem(12, juce::String::fromUTF8("Español"), uiLanguage == 2);
        menu.addSubMenu(localizedUiText("IDIOMA / LANGUAGE / IDIOMA", uiLanguage), languageMenu);
        menu.addSeparator();
        if (classicProcessor.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
        {
            menu.addItem(3, localizedUiText("Audio / MIDI", uiLanguage));
            menu.addSeparator();
        }
        menu.addItem(1, localizedUiText("Equalizador master", uiLanguage));
        menu.addItem(2, localizedUiText(classicProcessor.isMasterMidiLearning()
                                             ? "CANCELAR MIDI LEARN DO VOLUME MASTER"
                                             : "MIDI LEARN DO VOLUME MASTER", uiLanguage));
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(liveSettingsButton),
            [safe = juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor>(this)](int id) {
                if (safe == nullptr) return;
                if (id == 1) safe->masterEqButton.triggerClick();
                if (id == 2) safe->masterLearnButton.triggerClick();
                if (id == 3) safe->showAudioMidiSettings();
                if (id >= 10 && id <= 12) safe->setUiLanguage(id - 10);
            });
    };

    for (int bank = 0; bank < ClassicPlayerAudioProcessor::liveSetBankCount; ++bank)
    {
        auto& button = liveSetBankButtons[(size_t) bank];
        flatButton(button);
        button.setButtonText("BANCO " + juce::String(bank + 1));
        button.onClick = [this, bank]
        {
            activeLiveSetBank = bank;
            refreshLiveSet();
        };
        addAndMakeVisible(button);
    }
    for (int slot = 0; slot < ClassicPlayerAudioProcessor::liveSetSlotsPerBank; ++slot)
    {
        auto& button = liveSetSlotButtons[(size_t) slot];
        button.setName("LIVE_SLOT_" + juce::String(slot));
        button.getProperties().set("uiDataText", true);
        flatButton(button);
        button.onClick = [this, slot]
        {
            if (editingLiveSet) chooseLiveSetSlot(slot);
            else loadLiveSetSlot(slot);
        };
        addAndMakeVisible(button);

        auto& learnButton = liveSetSlotLearnButtons[(size_t) slot];
        flatButton(learnButton);
        learnButton.setButtonText("LEARN CC");
        learnButton.setTooltip("Clique e mova um controle MIDI para carregar esta performance");
        learnButton.onClick = [this, slot]
        {
            classicProcessor.beginLiveSetSlotMidiLearn(activeLiveSetBank, slot);
            refreshLiveSet();
        };
        learnButton.onClearMapping = [this, slot]
        {
            classicProcessor.resetLiveSetSlotMidiLearn(activeLiveSetBank, slot);
            refreshLiveSet();
        };
        addAndMakeVisible(learnButton);
    }
    showLiveSet(false);

    flatButton(masterEqButton);
    masterEqButton.setTooltip(juce::String::fromUTF8("Abrir o equalizador paramétrico da saída master"));
    masterEqButton.onClick = [this] { showMasterEqEditor(); };
    addAndMakeVisible(masterEqButton);
    flatButton(masterLimiterButton);
    masterLimiterButton.setTooltip("Abrir limiter da saida master");
    masterLimiterButton.onClick = [this] { showMasterLimiterEditor(); };
    addAndMakeVisible(masterLimiterButton);

    master.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    master.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 18);
    master.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(teal));
    addAndMakeVisible(master);
    masterLabel.setText("MASTER", juce::dontSendNotification);
    masterLabel.setJustificationType(juce::Justification::centred);
    masterLabel.setColour(juce::Label::textColourId, juce::Colour(text));
    addAndMakeVisible(masterLabel);
    flatButton(masterLearnButton);
    masterLearnButton.setTooltip("Aprender CC e canal do volume master. Clique novamente para cancelar; Shift+clique apaga o mapeamento. CC64 reservado ao sustain.");
    masterLearnButton.onClick = [this]
    {
        if (juce::ModifierKeys::getCurrentModifiers().isShiftDown())
            classicProcessor.resetMasterMidiLearn();
        else
            classicProcessor.beginMasterMidiLearn();
    };
    masterLearnButton.onClearMapping = [this] { classicProcessor.resetMasterMidiLearn(); };
    addAndMakeVisible(masterLearnButton);
    addAndMakeVisible(masterMeter);
    cpuMeter.setTooltip(juce::String::fromUTF8("Carga do processamento de áudio do Classic Player. 100% indica que o tempo disponível para o buffer foi consumido."));
    addAndMakeVisible(cpuMeter);
    flatButton(updateButton);
    updateButton.onClick = [this]
    {
        if (latestUpdate.available) showUpdateDetails();
        else checkForUpdates(true);
    };
    addAndMakeVisible(updateButton);
    nextUpdateCheckMs = juce::Time::getMillisecondCounterHiRes() + 10000.0;
    masterAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        classicProcessor.parameters, "master", master);

    layerViewport.setViewedComponent(&layerContent, false);
    // Keep all layer controls accessible on compact notebook displays.
    layerViewport.setScrollBarsShown(false, true);
    layerViewport.setScrollBarThickness(9);
    layerViewport.setWantsKeyboardFocus(false);
    addAndMakeVisible(layerViewport);

    const auto visibleLayerCount = classicProcessor.activeLayerCount();
    displayedLayerCount = visibleLayerCount;
    for (int i = 0; i < Sf2Engine::layerCount; ++i)
    {
        strips[(size_t) i] = std::make_unique<LayerStrip>(classicProcessor, i,
                                                          [this] { applyMixerStates(); layoutLayerStrips(); });
        strips[(size_t) i]->setRemoveCallback([this, i] { removeLayer(i); });
        strips[(size_t) i]->setReorderCallback([this](int layer, juce::Point<int> position)
        { reorderLayerFromDrag(layer, position); });
        layerContent.addAndMakeVisible(*strips[(size_t) i]);
        strips[(size_t) i]->setVisible(i < visibleLayerCount);
    }
    addLayerButton.setEnabled(visibleLayerCount < Sf2Engine::layerCount);
    addAndMakeVisible(keyboard);
    classicProcessor.keyboardState.addListener(this);

    addAndMakeVisible(activationPanel);
    activationBackdrop.setColour(juce::Label::backgroundColourId, juce::Colour(0xff07131c));
    activationPanel.addAndMakeVisible(activationBackdrop);
    activationTitle.setText("LICENCA CLASSIC PLAYER", juce::dontSendNotification);
    activationTitle.setFont(juce::FontOptions(24.0f, juce::Font::bold));
    activationTitle.setJustificationType(juce::Justification::centred);
    activationPanel.addAndMakeVisible(activationTitle);
    activationHelp.setText("Entre com o e-mail e a senha da sua conta para liberar este computador.", juce::dontSendNotification);
    activationHelp.setJustificationType(juce::Justification::centred);
    activationPanel.addAndMakeVisible(activationHelp);
    activationEmail.setMultiLine(false);
    activationEmail.setTextToShowWhenEmpty("E-mail", juce::Colours::grey);
    activationEmail.setInputRestrictions(190, {});
    activationPanel.addAndMakeVisible(activationEmail);
    activationPassword.setMultiLine(false);
    activationPassword.setPasswordCharacter('*');
    activationPassword.setTextToShowWhenEmpty("Senha", juce::Colours::grey);
    activationPanel.addAndMakeVisible(activationPassword);
    activationButton.onClick = [this] { activate(); };
    flatButton(activationButton);
    activationPanel.addAndMakeVisible(activationButton);
    activationStatus.setJustificationType(juce::Justification::centred);
    activationStatus.setColour(juce::Label::textColourId, juce::Colours::salmon);
    activationPanel.addAndMakeVisible(activationStatus);
    activationPanel.setVisible(!classicProcessor.isActivated());
    activationPanel.toFront(false);
    if (activationPanel.isVisible()) languageSelector.toFront(false);
    
        if (shouldValidateOnlineSession)
            validateStoredOnlineSession();

    // setSize() invokes resized() immediately. All layer strips must exist
    // before that callback can lay them out.
    setResizable(true, true);
    // Fit inside the usable desktop area (menu bar and Dock excluded).  The
    // standalone window adds its own title bar around this component, so keep
    // a small vertical allowance as well.
    setResizeLimits(900, 600, 1920, 1080);
    const auto display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    const auto available = display != nullptr ? display->userBounds.toNearestInt()
                                               : juce::Rectangle<int>(0, 0, 1366, 768);
    const auto initialWidth = juce::roundToInt(static_cast<float>(available.getWidth()) * 0.96f);
    const auto initialHeight = juce::roundToInt(static_cast<float>(available.getHeight()) * 0.88f);
    setSize(juce::jmin(1600, juce::jmax(900, initialWidth)),
            juce::jmin(900, juce::jmax(600, initialHeight)));
    applyUiLanguage();
    startTimerHz(20);
}

ClassicPlayerAudioProcessorEditor::~ClassicPlayerAudioProcessorEditor()
{
    stopTimer();
    cancelPendingUpdate();
    classicProcessor.keyboardState.removeListener(this);
    setLookAndFeel(nullptr);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::showAnalogSynthEditor()
{
    if (processor.layerType(index) != ClassicPlayerAudioProcessor::LayerType::analog) return;

    auto* dialog = new LayerEditorWindow(
        "Classic Keys Analog", "Selecione o preset desta camada.",
        juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);

    // The Analog layer intentionally follows the compact DX7 editor: one
    // preset selector, routing, shared layer controls and MIDI learn.  The
    // oscillator controls remain implemented in the engine but are not exposed
    // in this window, avoiding a second, oversized editor layout.
    auto* controls = new AnalogSynthEditorPanel(processor.analogSynthConfig(index));
    controls->setPresetOnlyMode();
    auto* routing = new LayerRoutingEditorPanel(processor, index);
    auto* common = createAnalogCommonControls(processor, index).release();
    const juce::Component::SafePointer<LayerStrip> safe(this);
    auto* effectButtons = new LayerEffectButtons(
        [safe] { if (safe != nullptr) safe->showReverbEditor(); },
        [safe] { if (safe != nullptr) safe->showCompressorEditor(); }, {},
        [safe] { if (safe != nullptr) safe->showEqEditor(); });
    auto* midiPanel = new LayerMidiLearnPanel(processor, index);

    class CenteredPanel final : public juce::Component
    {
    public:
        CenteredPanel(juce::Component* child, int preferredWidth, int preferredHeight)
            : content(child), width(preferredWidth)
        {
            owned.add(child);
            addAndMakeVisible(child);
            setSize(preferredWidth, preferredHeight);
        }
        void resized() override
        {
            content->setBounds(getLocalBounds().withSizeKeepingCentre(
                juce::jmin(width, content->getWidth()),
                juce::jmin(getHeight(), content->getHeight())));
        }
    private:
        juce::Component* content;
        int width;
        juce::OwnedArray<juce::Component> owned;
    };

    dialog->addCustomComponent(new CenteredPanel(controls, 600, 86));
    dialog->addCustomComponent(new CenteredPanel(new LayerPresetFilePanel(
        [safe] { if (safe != nullptr) safe->saveLayerPreset(); },
        [safe] { if (safe != nullptr) safe->loadLayerPreset(); }), 600, 38));
    dialog->addCustomComponent(new CenteredPanel(routing, 600, 122));
    dialog->addCustomComponent(new CenteredPanel(common, 600, 100));
    const auto actionWidth = editorContentWidth(this, 930);
    dialog->addCustomComponent(new SideBySideEditorPanel(effectButtons, midiPanel, actionWidth, 54));
    controls->onConfigChanged = [safe](const AnalogSynthEngine::Config& config)
    {
        if (safe != nullptr) safe->processor.setAnalogSynthConfig(safe->index, config);
    };
    controls->onPresetChanged = [safe](const AnalogSynthEngine::Config& config)
    {
        if (safe != nullptr)
        {
            safe->processor.resetAnalogSynthVoices(safe->index);
            safe->processor.setAnalogSynthConfig(safe->index, config);
        }
    };
    dialog->setSize(actionWidth + 52, 570);
    dialog->fitWithinApp(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe](int)
        {
            if (safe == nullptr) return;
            // Edits are already live. Closing must never replay stale UI values.
            safe->refresh();
        }), true);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::showDx7Editor()
{
    if (processor.layerType(index) != ClassicPlayerAudioProcessor::LayerType::dx7) return;
    const auto prefix = "layer" + juce::String(index + 1);
    auto* dialog = new LayerEditorWindow(
        "DX7", "Selecione o banco e o timbre desta camada.", juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    auto* dx7Panel = new Dx7EditorPanel(processor, index);
    auto* routingPanel = new LayerRoutingEditorPanel(processor, index);
    dx7Panel->setSize(560, Dx7EditorPanel::preferredHeight);
    routingPanel->setSize(600, 92);
    auto valueOf = [this, prefix](const juce::String& suffix, float fallback)
    {
        if (auto* value = processor.parameters.getRawParameterValue(prefix + suffix)) return value->load();
        return fallback;
    };
    auto* common = new KnobEditorPanel({
        { "VOLUME", valueOf("Gain", 80.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "ATTACK ms", valueOf("Attack", 5.0f), 0.0f, 100.0f, 0.1f, 1 },
        { "RELEASE ms", valueOf("Release", 50.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "CUTOFF", valueOf("Cutoff", 100.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "REVERB", valueOf("Reverb", 0.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "COMP", valueOf("Comp", 0.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "CHORUS", valueOf("Dx7Chorus", 20.0f), 0.0f, 100.0f, 1.0f, 0 }
    }, 4);
    const std::array<const char*, 7> commonParameterSuffixes {
        "Gain", "Attack", "Release", "Cutoff", "Reverb", "Comp", "Dx7Chorus"
    };
    for (int control = 0; control < (int) commonParameterSuffixes.size(); ++control)
        common->bindParameter(control, processor.parameters,
                              prefix + commonParameterSuffixes[(size_t) control]);
    common->useDenseGrid(true);
    common->setSize(600, 128);
    const juce::Component::SafePointer<LayerStrip> safe(this);
    auto* effectButtons = new LayerEffectButtons(
        [safe] { if (safe != nullptr) safe->showReverbEditor(); },
        [safe] { if (safe != nullptr) safe->showCompressorEditor(); },
        [safe] { if (safe != nullptr) safe->showChorusEditor(); },
        [safe] { if (safe != nullptr) safe->showEqEditor(); });
    auto* midiPanel = new LayerMidiLearnPanel(processor, index);

    class CenteredPanel final : public juce::Component
    {
    public:
        CenteredPanel(juce::Component* child, int preferredWidth, int preferredHeight)
            : content(child), width(preferredWidth)
        {
            owned.add(child);
            addAndMakeVisible(child);
            setSize(preferredWidth, preferredHeight);
        }

        void resized() override
        {
            content->setBounds(getLocalBounds().withSizeKeepingCentre(
                juce::jmin(width, content->getWidth()),
                juce::jmin(getHeight(), content->getHeight())));
        }

    private:
        juce::Component* content;
        int width;
        juce::OwnedArray<juce::Component> owned;
    };

    dialog->addCustomComponent(new CenteredPanel(dx7Panel, 600, Dx7EditorPanel::preferredHeight));
    dialog->addCustomComponent(new CenteredPanel(new EngineProgramSavePanel(processor, index, "DX7"), 600, 30));
    dialog->addCustomComponent(new CenteredPanel(new LayerPresetFilePanel(
        [safe] { if (safe != nullptr) safe->saveLayerPreset(); },
        [safe] { if (safe != nullptr) safe->loadLayerPreset(); }), 600, 30));
    dialog->addCustomComponent(new CenteredPanel(routingPanel, 600, 92));
    dialog->addCustomComponent(new CenteredPanel(common, 600, 128));
    const auto actionWidth = editorContentWidth(this, 930);
    common->setOnValueChange([safe = juce::Component::SafePointer<LayerStrip>(this), common, prefix]
    {
        if (safe == nullptr) return;
        const auto set = [safe, prefix](const juce::String& suffix, float value)
        {
            if (auto* parameter = safe->processor.parameters.getParameter(prefix + suffix))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        };
        set("Gain", common->value(0)); set("Attack", common->value(1));
        set("Release", common->value(2)); set("Cutoff", common->value(3));
        set("Reverb", common->value(4)); set("Comp", common->value(5));
        set("Dx7Chorus", common->value(6));
    });
    dialog->addCustomComponent(new CenteredPanel(new ModulationTogglePanel(processor, index), 600, 28));
    dialog->addCustomComponent(new SideBySideEditorPanel(effectButtons, midiPanel, actionWidth, 54));
    // Reserve a full row for effect controls and MIDI Learn before the
    // footer so FECHAR cannot cover the reverb Learn button.
    dialog->setSize(actionWidth + 52, 580);
    dialog->fitWithinApp(this);
    dialog->fitCustomComponentsVertically();
    // Use AlertWindow's footer button so JUCE reserves a dedicated row below
    // the MIDI Learn panel instead of treating FECHAR as another component.
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe, common, prefix](int)
        {
            if (safe == nullptr) return;
            const auto set = [safe, prefix](const juce::String& suffix, float value)
            {
                if (auto* parameter = safe->processor.parameters.getParameter(prefix + suffix))
                    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
            };
            set("Gain", common->value(0));
            set("Attack", common->value(1));
            set("Release", common->value(2));
            set("Cutoff", common->value(3));
            set("Reverb", common->value(4));
            set("Comp", common->value(5));
            set("Dx7Chorus", common->value(6));
            safe->refresh();
        }), true);
}

void ClassicPlayerAudioProcessorEditor::showMasterEqEditor()
{
    auto* dialog = new LayerEditorWindow(
        "EQ MASTER", "EQ de cinco estagios: corte baixo, tres bandas e corte alto.",
        juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    auto* knobs = new KnobEditorPanel({
        { "LOW CUT Hz", classicProcessor.masterEqValue("masterEqLowCut"), 20.0f, 250.0f, 1.0f, 0 },
        { "LOW GAIN dB", classicProcessor.masterEqValue("masterEqLow"), -12.0f, 12.0f, 0.1f, 1 },
        { "LOW FREQ Hz", classicProcessor.masterEqValue("masterEqLowFrequency"), 40.0f, 400.0f, 1.0f, 0 },
        { "MID GAIN dB", classicProcessor.masterEqValue("masterEqMid"), -12.0f, 12.0f, 0.1f, 1 },
        { "MID FREQ Hz", classicProcessor.masterEqValue("masterEqFrequency"), 200.0f, 6000.0f, 1.0f, 0 },
        { "HIGH GAIN dB", classicProcessor.masterEqValue("masterEqHigh"), -12.0f, 12.0f, 0.1f, 1 },
        { "HIGH FREQ Hz", classicProcessor.masterEqValue("masterEqHighFrequency"), 2000.0f, 16000.0f, 1.0f, 0 },
        { "HIGH CUT Hz", classicProcessor.masterEqValue("masterEqHighCut"), 2000.0f, 20000.0f, 1.0f, 0 }
    }, 4);
    auto* graph = new ParametricEqGraph(classicProcessor, 0, true);
    knobs->useDenseGrid(true);
    const auto contentWidth = editorContentWidth(this, 1000);
    dialog->addCustomComponent(new SideBySideEditorPanel(graph, knobs, contentWidth, 240));
    dialog->setSize(contentWidth + 52, 380);
    dialog->fitWithinApp(this);
    graph->onPointChanged = [knobs](int band, float frequency, float gain)
    {
        static constexpr std::array<int, 3> frequencyKnobs { 2, 4, 6 };
        static constexpr std::array<int, 3> gainKnobs { 1, 3, 5 };
        knobs->setValue(frequencyKnobs[(size_t) band], frequency);
        knobs->setValue(gainKnobs[(size_t) band], gain);
    };
    const juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor> safe(this);
    knobs->setOnValueChange([safe, knobs]
    {
        if (safe == nullptr) return;
        safe->classicProcessor.setMasterEqValue("masterEqLowCut", juce::jlimit(20.0f, 250.0f, knobs->value(0)));
        safe->classicProcessor.setMasterEqValue("masterEqLow", juce::jlimit(-12.0f, 12.0f, knobs->value(1)));
        safe->classicProcessor.setMasterEqValue("masterEqLowFrequency", juce::jlimit(40.0f, 400.0f, knobs->value(2)));
        safe->classicProcessor.setMasterEqValue("masterEqMid", juce::jlimit(-12.0f, 12.0f, knobs->value(3)));
        safe->classicProcessor.setMasterEqValue("masterEqFrequency", juce::jlimit(200.0f, 6000.0f, knobs->value(4)));
        safe->classicProcessor.setMasterEqValue("masterEqHigh", juce::jlimit(-12.0f, 12.0f, knobs->value(5)));
        safe->classicProcessor.setMasterEqValue("masterEqHighFrequency", juce::jlimit(2000.0f, 16000.0f, knobs->value(6)));
        safe->classicProcessor.setMasterEqValue("masterEqHighCut", juce::jlimit(2000.0f, 20000.0f, knobs->value(7)));
    });
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe](int) { if (safe != nullptr) safe->repaint(); }), true);
}
void ClassicPlayerAudioProcessorEditor::showMasterLimiterEditor()
{
    auto* dialog = new LayerEditorWindow("LIMITER MASTER", "Protecao da saida master. OUTPUT define o teto em dBFS.",
                                        juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    auto& state = classicProcessor.parameters;
    const auto current = [&state](const char* id) { return state.getRawParameterValue(id)->load(); };
    auto* meters = new MasterLimiterMeters(classicProcessor);
    auto* knobs = new KnobEditorPanel({
        { "INPUT dB", current("limiterInput"), -12.0f, 12.0f, 0.1f, 1 },
        { "RELEASE ms", current("limiterRelease"), 10.0f, 500.0f, 1.0f, 0 },
        { "OUTPUT dB", current("limiterCeiling"), -12.0f, 0.0f, 0.1f, 1 }
    }, 3);
    knobs->bindParameter(0, state, "limiterInput");
    knobs->bindParameter(1, state, "limiterRelease");
    knobs->bindParameter(2, state, "limiterCeiling");
    auto* presets = new EffectPresetPanel("PRESET", {
        "Protecao transparente", "Piano suave", "Piano worship", "Piano presente", "Master forte"
    });
    for (int preset = 0; preset < (int) factoryLimiterPresets.size(); ++preset)
    {
        const auto& values = factoryLimiterPresets[(size_t) preset];
        if (std::abs(current("limiterInput") - values[0]) < 0.1f
            && std::abs(current("limiterRelease") - values[1]) < 0.1f
            && std::abs(current("limiterCeiling") - values[2]) < 0.1f)
        {
            presets->setSelectedPreset(preset);
            break;
        }
    }
    const juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor> safe(this);
    presets->onPresetSelected = [safe](int index)
    {
        if (safe == nullptr) return;
        if (!juce::isPositiveAndBelow(index, 5)) return;
        const char* ids[] = { "limiterInput", "limiterRelease", "limiterCeiling" };
        for (int i = 0; i < 3; ++i)
            if (auto* parameter = safe->classicProcessor.parameters.getParameter(ids[i]))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(factoryLimiterPresets[(size_t) index][(size_t) i]));
    };
    dialog->addCustomComponent(new CentredEditorPanel(meters, 650));
    dialog->addCustomComponent(new CentredEditorPanel(knobs, 650));
    dialog->addCustomComponent(new CentredEditorPanel(presets, 650));
    dialog->setSize(720, 490);
    dialog->fitWithinApp(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([](int) {}), true);
}
void ClassicPlayerAudioProcessorEditor::paint(juce::Graphics& g)
{
    if (activeUiPalette == 5 || activeUiPalette == 6)
        paintBrushedSteel(g, getLocalBounds().toFloat());
    else if (activeUiPalette == 2)
    {
        juce::ColourGradient nordGradient(juce::Colour(0xff7a1127), 0.0f, 0.0f,
                                          juce::Colour(background), (float) getWidth(), (float) getHeight(), false);
        nordGradient.addColour(0.46, juce::Colour(0xff3d0914));
        g.setGradientFill(nordGradient);
        g.fillAll();
    }
    else if (activeUiPalette == 3)
    {
        juce::ColourGradient violetGradient(juce::Colour(0xff351083), 0.0f, 0.0f,
                                            juce::Colour(background), (float) getWidth(), (float) getHeight(), false);
        violetGradient.addColour(0.52, juce::Colour(0xff1c1757));
        g.setGradientFill(violetGradient);
        g.fillAll();
    }
    else
        g.fillAll(juce::Colour(background));
    if (showingLiveSet)
    {
        g.setColour(juce::Colour(paletteLine));
        g.drawHorizontalLine(81, 14.0f, (float)getWidth()-14.0f);
        g.drawRoundedRectangle(juce::Rectangle<float>(14, (float)getHeight()-76, (float)getWidth()-28, 62), 5, 1);
        g.setColour(juce::Colour(text));
        g.setFont(juce::FontOptions(34.0f, juce::Font::bold));
        g.drawText(localizedUiText("LIVE SET", activeUiLanguage.load()), getWidth()/2-130, 12, 260, 56, juce::Justification::centred);
        const int start = getWidth()/2-100;
        for (int i=0; i<ClassicPlayerAudioProcessor::liveSetBankCount; ++i)
        {
            g.setColour(juce::Colour(i == activeLiveSetBank ? teal : paletteLine));
            g.fillEllipse((float)(start+i*26), (float)getHeight()-49, 8, 8);
        }
        return;
    }
    g.setGradientFill(juce::ColourGradient(juce::Colour(panelLight), 0.0f, 0.0f,
                                           juce::Colour(background), (float) getWidth(), 220.0f, false));
    if (activeUiPalette == 5 || activeUiPalette == 6)
        paintBrushedSteel(g, juce::Rectangle<float>(0, 0, (float) getWidth(), 142));
    else
        g.fillRect(0, 0, getWidth(), 142);
    g.setColour(juce::Colour(paletteLine));
    g.drawHorizontalLine(141, 18.0f, (float) getWidth() - 18.0f);
    g.drawHorizontalLine(getHeight() - 66, 18.0f, (float) getWidth() - 18.0f);
    g.setColour(juce::Colour(mutedText));
    g.setFont(10.5f);
        g.drawText(localizedUiText("Copyright 2026 Willam Silva & Classic Keys. Todos os direitos reservados.",
                                   activeUiLanguage.load()),
               305, getHeight() - 49, getWidth() - 610, 28, juce::Justification::centred);
}

void ClassicPlayerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(14);
    auto header = area.removeFromTop(120);
    const bool compactHeader = getWidth() < 1100;
    appIcon.setBounds(header.removeFromLeft(compactHeader ? 58 : 70).reduced(4));
    header.removeFromLeft(5);
    const auto brandWidth = compactHeader ? 164 : juce::jlimit(205, 255, getWidth() / 6);
    auto brand = header.removeFromLeft(brandWidth);
    brand.removeFromTop(16);
    title.setFont(juce::FontOptions(compactHeader ? 18.0f : 25.0f, juce::Font::bold));
    auto brandTitle = brand.removeFromTop(38);
    title.setBounds(brandTitle.removeFromTop(26));
    versionLabel.setBounds(brandTitle);
    subtitle.setBounds(brand.removeFromTop(25)); userLabel.setBounds(brand.removeFromTop(34)); userLabel.setVisible(userLabel.getText().isNotEmpty());

    auto masterArea = header.removeFromRight(compactHeader ? 112 : 136);
    masterMeter.setBounds(masterArea.removeFromRight(13).reduced(0, 6));
    masterLabel.setBounds(masterArea.removeFromTop(17));
    auto masterKnobArea = masterArea.removeFromTop(57);
    master.setBounds(masterKnobArea.reduced(3, 0));
    auto masterActions = masterArea.removeFromTop(20);
    masterLearnButton.setBounds(masterActions.removeFromRight(52).reduced(1, 0));
    masterLimiterButton.setBounds(masterActions.removeFromRight(42).reduced(1, 0));
    masterEqButton.setButtonText("EQ");
    masterEqButton.setBounds(masterActions.reduced(1, 0));
    header.removeFromRight(5);
    const auto rightActionsWidth = compactHeader ? 95
        : juce::jlimit(95, 190, juce::roundToInt(95.0f
            + static_cast<float>(getWidth() - 1100) * 0.23f));
    auto rightActions = header.removeFromRight(rightActionsWidth);
    const auto rightActionHeight = 28;
    liveSetButton.setBounds(rightActions.removeFromTop(rightActionHeight).reduced(1));
    addLayerButton.setBounds(rightActions.removeFromTop(rightActionHeight).reduced(1));
    chordColourButton.setBounds(rightActions.removeFromTop(rightActionHeight).reduced(1));
    keyColourButton.setBounds(rightActions.removeFromTop(rightActionHeight).reduced(1));
    header.removeFromRight(5);

    const auto programActionsWidth = compactHeader ? 124
        : juce::jlimit(124, 260, juce::roundToInt(124.0f
            + static_cast<float>(getWidth() - 1100) * 0.43f));
    auto programActions = header.removeFromRight(programActionsWidth);
    const auto actionWidth = programActions.getWidth() / 2;
    auto actionRow = programActions.removeFromTop(27);
    importProgramButton.setBounds(actionRow.removeFromLeft(actionWidth).reduced(1));
    newProgramButton.setBounds(actionRow.reduced(1));
    actionRow = programActions.removeFromTop(27);
    saveProgramButton.setBounds(actionRow.removeFromLeft(actionWidth).reduced(1));
    deleteProgramButton.setBounds(actionRow.reduced(1));
    actionRow = programActions.removeFromTop(27);
    exportProgramButton.setBounds(actionRow.removeFromLeft(actionWidth).reduced(1));
    accidentalStyleBox.setBounds(actionRow.reduced(1));
    languageSelector.setBounds(programActions.removeFromTop(32).reduced(1));
    languageSelector.setVisible(true);
    header.removeFromRight(5);

    auto displayArea = header.reduced(2, 1);
    programBox.setBounds(displayArea.removeFromBottom(31).reduced(1));
    chordCaption.setBounds({});
    chordLabel.setBounds(displayArea.reduced(1));

    area.removeFromTop(12);
    auto footer = area.removeFromBottom(54);
    cpuMeter.setBounds(footer.getX(), footer.getY() + 29, 138, 22);
    updateButton.setBounds(footer.getX() + 148, footer.getY() + 29, 140, 22);
    auto panicArea = footer.removeFromRight(166).removeFromBottom(28);
    panicLearnButton.setBounds(panicArea.removeFromRight(78).reduced(1, 0));
    panicButton.setBounds(panicArea.reduced(1, 0));
    auto recordingArea = footer.removeFromTop(27);
    recordingButton.setBounds(recordingArea.removeFromLeft(156).reduced(1, 0));
    recordingStatus.setBounds(recordingArea.removeFromLeft(230).reduced(6, 0));
    keyboardVisibilityButton.setBounds(recordingArea.removeFromLeft(156).reduced(2, 0));
    audioMidiSettingsButton.setBounds(recordingArea.removeFromLeft(140).reduced(2, 0));
    skinCaption.setBounds(recordingArea.removeFromLeft(48).reduced(2, 0));
    skinSelector.setBounds(recordingArea.removeFromLeft(160).reduced(2, 0));
    skinSelector.setVisible(true);
    audioMidiSettingsButton.setVisible(!showingLiveSet
        && classicProcessor.wrapperType == juce::AudioProcessor::wrapperType_Standalone);
    recordingButton.setVisible(!showingLiveSet);
    recordingStatus.setVisible(!showingLiveSet);
    keyboardVisibilityButton.setVisible(!showingLiveSet);

    if (showingLiveSet)
    {
        languageSelector.setVisible(false);
        auto liveArea = getLocalBounds().reduced(14);
        auto liveHeader = liveArea.removeFromTop(84);
        cpuMeter.setBounds(350, 20, 140, 22);
        updateButton.setBounds(350, 48, 140, 22);
        appIcon.setBounds(18, 14, 54, 54);
        title.setBounds(82, 20, 260, 28);
        versionLabel.setBounds(18, 70, 64, 14);
        subtitle.setBounds(82, 47, 260, 20); userLabel.setBounds(82, 66, 260, 32); userLabel.setVisible(userLabel.getText().isNotEmpty());
        auto controls = liveHeader.removeFromRight(370);
        liveSetButton.setBounds(controls.removeFromRight(72).reduced(2,16));
        auto volumeArea = controls.removeFromRight(140).reduced(8,8);
        masterLabel.setBounds(volumeArea.removeFromTop(19));
        master.setBounds(volumeArea);
        liveSettingsButton.setBounds(controls.reduced(4,16));
        liveArea.removeFromTop(8);
        auto banks = liveArea.removeFromTop(46);
        const auto bankWidth = banks.getWidth() / ClassicPlayerAudioProcessor::liveSetBankCount;
        for (int bank = 0; bank < ClassicPlayerAudioProcessor::liveSetBankCount; ++bank)
            liveSetBankButtons[(size_t) bank].setBounds(
                banks.removeFromLeft(bank == ClassicPlayerAudioProcessor::liveSetBankCount - 1
                                     ? banks.getWidth() : bankWidth).reduced(2, 2));

        auto editArea = liveArea.removeFromBottom(68).reduced(6, 10);
        editLiveSetButton.setBounds(editArea.removeFromRight(224).reduced(4, 3));
        livePreviousButton.setBounds(editArea.removeFromLeft(155).reduced(2,3));
        liveNextButton.setBounds(editArea.removeFromRight(155).reduced(2,3));
        const auto tileHeight = liveArea.getHeight() / 2;
        const auto tileWidth = liveArea.getWidth() / 4;
        for (int slot = 0; slot < ClassicPlayerAudioProcessor::liveSetSlotsPerBank; ++slot)
        {
            const auto column = slot % 4;
            const auto row = slot / 4;
            auto tile = juce::Rectangle<int>(
                liveArea.getX() + column * tileWidth + 3,
                liveArea.getY() + row * tileHeight + 3,
                tileWidth - 6, tileHeight - 6);
            auto learnArea = editingLiveSet ? tile.removeFromBottom(28) : juce::Rectangle<int>{};
            liveSetSlotButtons[(size_t) slot].setBounds(tile);
            liveSetSlotLearnButtons[(size_t) slot].setBounds(learnArea.reduced(0, 2));
        }
    }
    else
    {
        if (virtualKeyboardVisible)
        {
            auto keyboardArea = area.removeFromBottom(112);
            keyboard.setBounds(keyboardArea.reduced(0, 4));
            area.removeFromBottom(8);
        }
        else
        {
            keyboard.setBounds({});
        }
        layerViewport.setBounds(area);
        layoutLayerStrips();
    }

    activationPanel.setBounds(getLocalBounds());
    activationBackdrop.setBounds(activationPanel.getLocalBounds());
    if (activationPanel.isVisible())
    {
        activationPanel.toFront(false);
        languageSelector.toFront(false);
    }
    auto activation = activationPanel.getLocalBounds().withSizeKeepingCentre(
        juce::jmin(650, getWidth() - 60), 360);
    activationTitle.setBounds(activation.removeFromTop(58));
    activationHelp.setBounds(activation.removeFromTop(38));
    activation.removeFromTop(12);
    activationEmail.setBounds(activation.removeFromTop(44));
    activation.removeFromTop(10);
    activationPassword.setBounds(activation.removeFromTop(44));
    activation.removeFromTop(14);
    activationButton.setBounds(activation.removeFromTop(44).withSizeKeepingCentre(180, 44));
    activationStatus.setBounds(activation.removeFromTop(38));

    keyboard.setKeyWidth(juce::jmax(11.0f, static_cast<float>(keyboard.getWidth()) / 52.0f));
}

void ClassicPlayerAudioProcessorEditor::checkForUpdates(bool manual)
{
    nextUpdateCheckMs = juce::Time::getMillisecondCounterHiRes() + 6.0 * 60.0 * 60.0 * 1000.0;
    if (updateChecker.start())
    {
        manualUpdateCheck = manual;
        refreshUpdateNotice();
    }
}

void ClassicPlayerAudioProcessorEditor::refreshUpdateNotice()
{
    const bool announce = latestUpdate.available && !updateDismissed;
    setButtonTextIfChanged(updateButton, localizedUiText(updateChecker.isChecking() ? "VERIFICANDO..."
        : announce ? "ATUALIZACAO DISPONIVEL" : "ATUALIZACOES", uiLanguage));
    updateButton.setEnabled(!updateChecker.isChecking());
    setButtonColourIfChanged(updateButton, juce::TextButton::buttonColourId,
                             juce::Colour(announce ? teal : panelLight));
    setButtonColourIfChanged(updateButton, juce::TextButton::textColourOffId,
                             juce::Colour(announce ? background : text));
    updateButton.setTooltip(latestUpdate.available ? "Classic Player " + latestUpdate.version
        : localizedUiText("ATUALIZACOES", uiLanguage) + " — " JucePlugin_VersionString);
    cpuMeter.setTooltip(localizedUiText("Carga do processamento de áudio do Classic Player. 100% indica que o tempo disponível para o buffer foi consumido.", uiLanguage));
}

void ClassicPlayerAudioProcessorEditor::showUpdateDetails()
{
    auto* window = new juce::AlertWindow(localizedUiText("ATUALIZACAO DISPONIVEL", uiLanguage),
        "Classic Player " + latestUpdate.version + "\n\n" + latestUpdate.notes,
        juce::MessageBoxIconType::InfoIcon);
    window->setLookAndFeel(&classicLookAndFeel);
    applyUiSkinToComponentTree(*window, uiPalettes[(size_t) activeUiPalette]);
    window->addButton(localizedUiText("BAIXAR ATUALIZACAO", uiLanguage), 1);
    if (latestUpdate.notesUrl.isNotEmpty())
        window->addButton(localizedUiText("NOTAS DA VERSAO", uiLanguage), 2);
    window->addButton(localizedUiText("MAIS TARDE", uiLanguage), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    const auto download = latestUpdate.downloadUrl;
    const auto notes = latestUpdate.notesUrl;
    const juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor> safe(this);
    window->enterModalState(true, juce::ModalCallbackFunction::create([safe, download, notes](int selected)
    {
        if (selected == 1) juce::URL(download).launchInDefaultBrowser();
        if (selected == 2) juce::URL(notes).launchInDefaultBrowser();
        if (safe != nullptr && selected == 0)
        {
            safe->updateDismissed = true;
            safe->refreshUpdateNotice();
        }
    }), true);
}

void ClassicPlayerAudioProcessorEditor::timerCallback()
{
    cpuMeter.setUsage(classicProcessor.audioCpuUsagePercent());
    ClassicPlayerUpdateChecker::Result updateResult;
    if (updateChecker.takeResult(updateResult))
    {
        latestUpdate = std::move(updateResult);
        updateDismissed = false;
        refreshUpdateNotice();
        if (manualUpdateCheck && latestUpdate.available)
            showUpdateDetails();
        else if (manualUpdateCheck)
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                localizedUiText("ATUALIZACOES", uiLanguage),
                localizedUiText(latestUpdate.succeeded ? "Sua versão está atualizada."
                    : "Não foi possível verificar as atualizações. Tente novamente mais tarde.", uiLanguage));
        manualUpdateCheck = false;
    }
    if (juce::Time::getMillisecondCounterHiRes() >= nextUpdateCheckMs)
        checkForUpdates(false);
    const auto panicCC = classicProcessor.panicMidiLearnCC();
    setButtonTextIfChanged(panicLearnButton, localizedUiText(classicProcessor.isPanicMidiLearning() ? "MOVA O CC"
        : panicCC < 0 ? "LEARN" : "CC " + juce::String(panicCC), activeUiLanguage.load()));
    setButtonColourIfChanged(panicLearnButton, juce::TextButton::buttonColourId,
        classicProcessor.isPanicMidiLearning() ? juce::Colour(yellow)
                                               : panicCC >= 0 ? juce::Colour(0xff1b554e)
                                                              : juce::Colour(panelLight));
    const auto masterCC = classicProcessor.masterMidiLearnCC();
    setButtonTextIfChanged(masterLearnButton, localizedUiText(classicProcessor.isMasterMidiLearning() ? "MOVE CC"
        : masterCC < 0 ? "LEARN" : "CC " + juce::String(masterCC), activeUiLanguage.load()));
    if (showingLiveSet)
        masterLabel.setText(localizedUiText(classicProcessor.isMasterMidiLearning() ? "MOVA UM CC"
            : masterCC < 0 ? "VOLUME" : "VOLUME / CC " + juce::String(masterCC), activeUiLanguage.load()),
            juce::dontSendNotification);
    classicProcessor.consumeMidiControlUpdates();
    if (showingLiveSet)
        refreshLiveSetVolumeIndicators();
    if (classicProcessor.consumeLiveSetSlotMidiLearnChanged())
    {
        classicProcessor.saveLiveSetSlotMidiLearnState();
        refreshLiveSet();
    }
    if (const auto requested = classicProcessor.consumeRequestedLiveSetSlot();
        juce::isPositiveAndBelow(requested,
                                 ClassicPlayerAudioProcessor::liveSetBankCount
                                 * ClassicPlayerAudioProcessor::liveSetSlotsPerBank))
    {
        activeLiveSetBank = requested / ClassicPlayerAudioProcessor::liveSetSlotsPerBank;
        activeLiveSetSlot = -1;
        if (!showingLiveSet) showLiveSet(true);
        loadLiveSetSlot(requested % ClassicPlayerAudioProcessor::liveSetSlotsPerBank);
    }
    for (int i = 0; i < classicProcessor.activeLayerCount(); ++i)
    {
        if (strips[(size_t) i] != nullptr)
            strips[(size_t) i]->updateMeter();
    }
    if (++timerTicks >= 20)
    {
        timerTicks = 0;
        const auto activeCount = classicProcessor.activeLayerCount();
        if (displayedLayerCount != activeCount)
        {
            displayedLayerCount = activeCount;
            addLayerButton.setEnabled(activeCount < Sf2Engine::layerCount);
            layoutLayerStrips();
        }
        classicProcessor.refreshStandaloneMidiInputs();
        const auto devices = classicProcessor.availableMidiDevices();
        for (auto& strip : strips)
            if (strip != nullptr) strip->refreshMidiDevices(devices);
    }
    masterMeter.setLevel(juce::jlimit(0.0f, 1.0f, classicProcessor.limiterOutputLevel()));

    if (classicProcessor.isAudioRecording())
    {
        const auto elapsed = juce::jmax<juce::int64>(0, juce::Time::currentTimeMillis() - recordingStartedAtMs) / 1000;
        recordingStatus.setText(localizedUiText("GRAVANDO " + juce::String(elapsed / 60).paddedLeft('0', 2)
                                + ":" + juce::String(elapsed % 60).paddedLeft('0', 2), activeUiLanguage.load()),
                                juce::dontSendNotification);
        setButtonTextIfChanged(recordingButton, localizedUiText("PARAR", activeUiLanguage.load()));
    }
}

void ClassicPlayerAudioProcessorEditor::handleNoteOn(juce::MidiKeyboardState*, int, int note, float)
{
    if (juce::isPositiveAndBelow(note, 128)) heldNotes[(size_t) note].store(true);
    triggerAsyncUpdate();
}

void ClassicPlayerAudioProcessorEditor::handleNoteOff(juce::MidiKeyboardState*, int, int note, float)
{
    if (juce::isPositiveAndBelow(note, 128)) heldNotes[(size_t) note].store(false);
    triggerAsyncUpdate();
}

void ClassicPlayerAudioProcessorEditor::handleAsyncUpdate()
{
    const auto chord = ClassicChordDetector::formatAccidentals(detectedChord(), accidentalStyle);
    const auto fontSize = chord.length() <= 5 ? 56.0f
                         : chord.length() <= 9 ? 46.0f
                         : chord.length() <= 13 ? 36.0f
                         : chord.length() <= 18 ? 28.0f : 21.0f;
    chordLabel.setFont(juce::FontOptions(fontSize, juce::Font::bold));
    chordLabel.setText(chord, juce::dontSendNotification);
}

juce::String ClassicPlayerAudioProcessorEditor::detectedChord() const
{
    std::vector<int> notes;
    for (int note = 0; note < 128; ++note)
        if (heldNotes[(size_t) note].load()) notes.push_back(note);
    return ClassicChordDetector::detect(notes);
}

void ClassicPlayerAudioProcessorEditor::refreshExternalInstrumentLibrary()
{
    if (!classicProcessor.supportsExternalInstruments()) return;

    classicProcessor.refreshExternalInstrumentLibrary();
    for (auto& strip : strips)
        if (strip != nullptr)
            strip->refreshExternalInstrumentLibrary();
}

void ClassicPlayerAudioProcessorEditor::refreshProgramLibrary()
{
    programFiles = classicProcessor.savedPrograms();
    const auto currentName = programBox.getText().isEmpty()
        ? classicProcessor.currentSavedProgramName() : programBox.getText();
    programBox.clear(juce::dontSendNotification);
    int selectedId = 0;
    for (int item = 0; item < programFiles.size(); ++item)
    {
        const auto name = programFiles.getReference(item).getFileNameWithoutExtension();
        programBox.addItem(name, item + 1);
        if (name == currentName) selectedId = item + 1;
    }
    programBox.setTextWhenNothingSelected("NOVO PROGRAMA");
    if (selectedId > 0)
        programBox.setSelectedId(selectedId, juce::dontSendNotification);
    else if (currentName.isNotEmpty())
        programBox.setText(currentName, juce::dontSendNotification);
}

void ClassicPlayerAudioProcessorEditor::saveProgram()
{
    auto name = programBox.getText().trim();
    if (name.isEmpty()) name = "Classic Player Preset";
    name = name.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_()");
    if (name.isEmpty()) name = "Classic Player Preset";

    bool replacing = false;
    for (const auto& file : classicProcessor.savedPrograms())
        if (file.getFileNameWithoutExtension().compareIgnoreCase(name) == 0)
        {
            replacing = true;
            break;
        }

    const juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor> safe(this);
    const auto saveToLibrary = [safe, name]
    {
        if (safe == nullptr) return;
        juce::File savedFile;
        const auto result = safe->classicProcessor.saveProgram(name, savedFile);
        if (result.failed())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "Falha ao Salvar Preset", result.getErrorMessage());
            return;
        }

        // Saving a performance loaded from the Live Set updates its reference
        // to the newly written library file, so reselecting the tile restores
        // the latest settings.
        if (juce::isPositiveAndBelow(safe->activeLiveSetSlot,
                                     ClassicPlayerAudioProcessor::liveSetSlotsPerBank)
            && safe->activeLiveSetBank == safe->loadedLiveSetBank)
        {
            const auto assigned = safe->classicProcessor.assignLiveSetSlot(
                safe->activeLiveSetBank, safe->activeLiveSetSlot, savedFile);
            if (assigned.failed())
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon,
                    localizedUiText("Live Set não atualizado", activeUiLanguage.load()),
                    localizedUiText(juce::String::fromUTF8("A performance foi salva, mas não foi possível atualizar a posição ativa do Live Set: ")
                        + assigned.getErrorMessage(), activeUiLanguage.load()));
        }

        safe->programBox.setText(savedFile.getFileNameWithoutExtension(), juce::dontSendNotification);
        safe->refreshProgramLibrary();
        safe->refreshLiveSet();
    };

    if (replacing)
        juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::QuestionIcon, "Substituir performance?",
            localizedUiText(juce::String::fromUTF8("Já existe uma performance chamada \"") + name
                + juce::String::fromUTF8("\" na biblioteca do app. Deseja substituí-la?"), activeUiLanguage.load()),
            "SUBSTITUIR", "CANCELAR", this,
            juce::ModalCallbackFunction::create([saveToLibrary](int answer)
            {
                if (answer != 0) saveToLibrary();
            }));
    else
        saveToLibrary();
}

void ClassicPlayerAudioProcessorEditor::exportProgram()
{
    auto name = programBox.getText().trim();
    if (name.isEmpty()) name = classicProcessor.currentSavedProgramName();
    if (name.isEmpty()) name = "Classic Player Preset";
    name = name.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_()");
    if (name.isEmpty()) name = "Classic Player Preset";

    const auto defaultFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile(name + ".ckprogram");
    programFileChooser = std::make_unique<juce::FileChooser>(
        localizedUiText("Exportar performance Classic Player", uiLanguage), defaultFile, "*.ckprogram");
    programFileChooser->launchAsync(
        juce::FileBrowserComponent::saveMode
        | juce::FileBrowserComponent::canSelectFiles
        | juce::FileBrowserComponent::warnAboutOverwriting,
        [this](const juce::FileChooser& chooser)
        {
            const auto destination = chooser.getResult();
            if (destination == juce::File{}) return;

            juce::File exportedFile;
            const auto result = classicProcessor.exportProgramToFile(destination, exportedFile);
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Falha ao Exportar Performance", result.getErrorMessage());
            else
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::NoIcon,
                    localizedUiText("Exportação concluída", activeUiLanguage.load()),
                    localizedUiText(juce::String::fromUTF8("Cópia portátil salva em:\n")
                        + exportedFile.getFullPathName(), activeUiLanguage.load()));
        });
}

void ClassicPlayerAudioProcessorEditor::importProgramFile()
{
    programFileChooser = std::make_unique<juce::FileChooser>(
        localizedUiText("Importar performance Classic Player", uiLanguage), juce::File{}, "*.ckprogram");
    programFileChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& chooser)
        {
            const auto source = chooser.getResult();
            if (!source.existsAsFile()) return;

            bool alreadyInLibrary = false;
            bool nameCollision = false;
            for (const auto& file : classicProcessor.savedPrograms())
            {
                alreadyInLibrary = alreadyInLibrary || file == source;
                nameCollision = nameCollision || file.getFileName().compareIgnoreCase(source.getFileName()) == 0;
            }

            const juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor> safe(this);
            const auto importAndOpen = [safe, source](bool replaceExisting)
            {
                if (safe == nullptr) return;
                juce::File importedFile;
                const auto imported = safe->classicProcessor.importProgramFromFile(
                    source, importedFile, replaceExisting);
                if (imported.failed())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon, "Falha ao Importar Performance",
                        imported.getErrorMessage());
                    return;
                }

                const auto loaded = safe->classicProcessor.loadProgram(importedFile);
                if (loaded.failed())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon, "Falha ao Abrir Performance",
                        loaded.getErrorMessage());
                    return;
                }
                safe->activeLiveSetSlot = -1;
                safe->loadedLiveSetBank = -1;
                safe->programBox.setText(importedFile.getFileNameWithoutExtension(),
                                          juce::dontSendNotification);
                safe->refreshProgramLibrary();
                safe->refreshAfterProgramLoad();
            };

            if (nameCollision && !alreadyInLibrary)
                juce::AlertWindow::showOkCancelBox(
                    juce::MessageBoxIconType::QuestionIcon, "Substituir performance?",
                    localizedUiText("Já existe uma performance com esse nome na biblioteca deste computador. Deseja substituí-la?",
                                    activeUiLanguage.load()),
                    "SUBSTITUIR", "CANCELAR", this,
                    juce::ModalCallbackFunction::create([importAndOpen](int answer)
                    {
                        if (answer != 0) importAndOpen(true);
                    }));
            else
                importAndOpen(false);
        });
}

void ClassicPlayerAudioProcessorEditor::createNewProgram()
{
    const juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor> safe(this);
    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::WarningIcon,
        juce::String::fromUTF8("Nova programação"),
        juce::String::fromUTF8(
            "Começar uma programação vazia? As alterações não salvas e as layers atuais serão removidas desta sessão. "
            "Os arquivos salvos e os bancos do Live Set não serão apagados."),
        "NOVO", "Cancelar", this,
        juce::ModalCallbackFunction::create([safe](int answer)
        {
            if (safe == nullptr || answer == 0) return;

            for (auto& strip : safe->strips)
                if (strip != nullptr)
                    strip->closeExternalInstrumentEditor();

            safe->classicProcessor.resetToNewProgram();
            safe->activeLiveSetSlot = -1;
            safe->loadedLiveSetBank = -1;
            safe->programBox.setText(juce::String{}, juce::dontSendNotification);
            safe->refreshProgramLibrary();
            safe->refreshAfterProgramLoad();
        }));
}

void ClassicPlayerAudioProcessorEditor::deleteSelectedProgram()
{
    const auto selected = programBox.getSelectedItemIndex();
    if (!juce::isPositiveAndBelow(selected, programFiles.size()))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                               "Excluir programacao",
                                               "Seleciona uma programacao salva na lista.");
        return;
    }

    const auto result = classicProcessor.deleteProgram(programFiles.getReference(selected));
    if (result.failed())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               "Falha ao excluir programacao", result.getErrorMessage());
        return;
    }

    programBox.clear(juce::dontSendNotification);
    refreshProgramLibrary();
    refreshLiveSet();
}

void ClassicPlayerAudioProcessorEditor::showAudioMidiSettings()
{
   #if JucePlugin_Build_Standalone
    // Use the running standalone device manager, not a second audio device.
    if (classicProcessor.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
            holder->showAudioSettingsDialog();
   #endif
}

void ClassicPlayerAudioProcessorEditor::setUiLanguage(int language)
{
    uiLanguage = juce::jlimit(0, 2, language);
    activeUiLanguage.store(uiLanguage);
    languageSelector.setSelectedId(uiLanguage + 1, juce::dontSendNotification);
    writeUiLanguagePreference(uiLanguage);
    applyUiLanguage();
    repaint();
}

void ClassicPlayerAudioProcessorEditor::setUiSkin(int skin, bool savePreference)
{
    const auto nextIndex = juce::jlimit(0, (int) uiPalettes.size() - 1, skin);
    if (nextIndex == activeUiPalette && nextIndex == uiSkin) return;

    const auto& nextPalette = uiPalettes[(size_t) nextIndex];
    uiSkin = nextIndex;
    skinSelector.setSelectedId(uiSkin + 1, juce::dontSendNotification);

    // Remap JUCE component colours against every known palette; custom
    // layer/pad/key colours are not palette tokens and remain untouched.
    setUiPalette(nextIndex);
    classicLookAndFeel.applyCurrentPalette();
    applyUiSkinToComponentTree(*this, nextPalette);
    title.setColour(juce::Label::textColourId, brandTextColour());
    userLabel.setColour(juce::Label::textColourId, brandTextColour());

    auto& desktop = juce::Desktop::getInstance();
    for (int index = 0; index < desktop.getNumComponents(); ++index)
    {
        if (auto* window = desktop.getComponent(index))
        {
            if (auto* layerEditor = dynamic_cast<LayerEditorWindow*>(window))
            {
                applyUiSkinToComponentTree(*layerEditor, nextPalette);
                continue;
            }
            const bool isOurAlert = dynamic_cast<juce::AlertWindow*>(window) != nullptr
                                 && isClassicPlayerAlertTitle(window->getName());
            if (isOurAlert || window->getName() == "Hammond")
            {
                if (auto* documentWindow = dynamic_cast<juce::DocumentWindow*>(window))
                    documentWindow->setBackgroundColour(juce::Colour(panel));
                applyUiSkinToComponentTree(*window, nextPalette);
            }
        }
    }

    if (savePreference) writeUiSkinPreference(uiSkin);
    resized();
    repaint();
}

void ClassicPlayerAudioProcessorEditor::applyUiLanguage()
{
    uiLanguage = juce::jlimit(0, 2, activeUiLanguage.load());
    languageSelector.setSelectedId(uiLanguage + 1, juce::dontSendNotification);
    applyUiLanguageToComponentTree(*this, uiLanguage);
    refreshUpdateNotice();

    // Layer editors and Classic Player's own error/confirmation alerts are
    // separate desktop windows rather than children of the plug-in component.
    // A host shares JUCE's Desktop, so leave unrelated host dialogs alone.
    auto& desktop = juce::Desktop::getInstance();
    for (int index = 0; index < desktop.getNumComponents(); ++index)
    {
        if (auto* window = desktop.getComponent(index))
        {
            if (auto* layerEditor = dynamic_cast<LayerEditorWindow*>(window))
            {
                layerEditor->applyLanguage();
                continue;
            }
            const bool isOurAlert = dynamic_cast<juce::AlertWindow*>(window) != nullptr
                                 && isClassicPlayerAlertTitle(window->getName());
            if (isOurAlert || window->getName() == "Hammond")
                applyUiLanguageToComponentTree(*window, uiLanguage);
        }
    }
}

void ClassicPlayerAudioProcessorEditor::showLiveSet(bool show)
{
    showingLiveSet = show;
    title.setColour(juce::Label::textColourId, brandTextColour());
    subtitle.setText(localizedUiText(show ? "SONS QUE INSPIRAM" : "CLASSIC KEYS SF2 WORKSTATION",
                                     activeUiLanguage.load()), juce::dontSendNotification);
    subtitle.setColour(juce::Label::textColourId, juce::Colour(show ? teal : mutedText));
    liveSetButton.setButtonText(localizedUiText(show ? "VOLTAR" : "LIVE SET", activeUiLanguage.load()));
    layerViewport.setVisible(!show);
    keyboard.setVisible(!show && virtualKeyboardVisible);
    keyboardVisibilityButton.setVisible(!show);
    programBox.setVisible(!show);
    newProgramButton.setVisible(!show);
    saveProgramButton.setVisible(!show);
    exportProgramButton.setVisible(!show);
    deleteProgramButton.setVisible(!show);
    importProgramButton.setVisible(!show);
    addLayerButton.setVisible(!show);
    for (juce::Component* component : std::initializer_list<juce::Component*>{
             &chordLabel, &chordCaption, &chordColourButton, &keyColourButton,
             &accidentalStyleBox, &masterMeter, &masterEqButton, &masterLimiterButton, &masterLearnButton })
        component->setVisible(!show);
    masterLabel.setText(show ? "VOLUME" : "MASTER", juce::dontSendNotification);
    masterLabel.setVisible(true);
    master.setSliderStyle(show ? juce::Slider::LinearHorizontal : juce::Slider::RotaryHorizontalVerticalDrag);
    master.setTextBoxStyle(show ? juce::Slider::NoTextBox : juce::Slider::TextBoxBelow, false, 64, 18);
    master.setTooltip(localizedUiText(show ? "Volume master — use CONFIGURACOES para MIDI Learn"
                                           : "Volume master", activeUiLanguage.load()));
    livePreviousButton.setVisible(show);
    liveNextButton.setVisible(show);
    liveSettingsButton.setVisible(show);

    editLiveSetButton.setVisible(show);
    for (auto& button : liveSetBankButtons) button.setVisible(show);
    for (auto& button : liveSetSlotButtons) button.setVisible(show);
    for (auto& button : liveSetSlotLearnButtons) button.setVisible(show);
    if (show) refreshLiveSet();
    resized();
    repaint();
}

void ClassicPlayerAudioProcessorEditor::refreshLiveSet()
{
    livePreviousButton.setEnabled(activeLiveSetBank > 0);
    liveNextButton.setEnabled(activeLiveSetBank + 1 < ClassicPlayerAudioProcessor::liveSetBankCount);
    repaint();
    for (int bank = 0; bank < ClassicPlayerAudioProcessor::liveSetBankCount; ++bank)
    {
        auto& button = liveSetBankButtons[(size_t) bank];
        button.setToggleState(bank == activeLiveSetBank, juce::dontSendNotification);
        button.setColour(juce::TextButton::buttonColourId,
                         bank == activeLiveSetBank ? juce::Colour(teal) : juce::Colour(panelLight));
        button.setColour(juce::TextButton::buttonOnColourId,
                         bank == activeLiveSetBank ? juce::Colour(teal) : juce::Colour(teal));
        button.setColour(juce::TextButton::textColourOffId,
                         bank == activeLiveSetBank ? juce::Colour(background) : juce::Colour(text));
    }

    for (int slot = 0; slot < ClassicPlayerAudioProcessor::liveSetSlotsPerBank; ++slot)
    {
        auto& button = liveSetSlotButtons[(size_t) slot];
        const auto name = classicProcessor.liveSetSlotName(activeLiveSetBank, slot);
        const auto layers = classicProcessor.liveSetSlotLayerSummary(activeLiveSetBank, slot);
        const auto volumes = classicProcessor.liveSetSlotLayerVolumes(activeLiveSetBank, slot);
        const auto displayName = name.isNotEmpty() ? name : "-";
        button.setButtonText(juce::String(slot + 1).paddedLeft('0', 2)
                             + "\n" + displayName
                             + (name.isNotEmpty() && layers.isNotEmpty() ? "\n" + layers : juce::String{})
                             + (name.isNotEmpty() && volumes.isNotEmpty() ? "\n" + volumes : juce::String{}));
        const auto active = slot == activeLiveSetSlot && activeLiveSetBank == loadedLiveSetBank;
        button.getProperties().set("liveNumber", juce::String(slot+1).paddedLeft('0',2));
        button.getProperties().set("liveTitle", displayName);
        button.getProperties().set("liveSummary", name.isEmpty() ? juce::String{} : layers);
        button.getProperties().set("liveVolumes", name.isEmpty() ? juce::String{} : volumes);
        button.getProperties().set("liveMovingLayers", 0);
        button.getProperties().set("liveActive", active);
        button.repaint();
        button.setColour(juce::TextButton::buttonColourId,
                         active ? juce::Colour(yellow) : juce::Colour(panel));
        button.setColour(juce::TextButton::buttonOnColourId,
                         active ? juce::Colour(yellow) : juce::Colour(teal));
        button.setColour(juce::TextButton::textColourOffId,
                         active ? juce::Colour(background) : juce::Colour(text));
        button.setTooltip(localizedUiText(editingLiveSet ? "Clique para atribuir uma programação salva"
            : (name.isEmpty() ? "Posição vazia" : "Carregar " + name), activeUiLanguage.load()));

        auto& learnButton = liveSetSlotLearnButtons[(size_t) slot];
        learnButton.setVisible(showingLiveSet && editingLiveSet);
        const auto cc = classicProcessor.liveSetSlotMidiLearnCC(activeLiveSetBank, slot);
        const auto learning = classicProcessor.isLiveSetSlotMidiLearning(activeLiveSetBank, slot);
        learnButton.setButtonText(localizedUiText(
            learning ? "AGUARDANDO..." : (cc >= 0 ? "CC " + juce::String(cc) : "LEARN CC"),
            activeUiLanguage.load()));
        learnButton.setColour(juce::TextButton::buttonColourId,
                              learning ? juce::Colour(teal) : juce::Colour(panelLight));
        learnButton.setColour(juce::TextButton::textColourOffId,
                              learning ? juce::Colour(background) : juce::Colour(text));
        learnButton.setTooltip(learning ? "Mova agora um controle MIDI CC"
                                        : (cc >= 0 ? "CC " + juce::String(cc)
                                                   + " no canal "
                                                   + juce::String(classicProcessor.liveSetSlotMidiLearnChannel(activeLiveSetBank, slot))
                                                   + ". Clique para reaprender."
                                                   : "Clique e mova um controle MIDI para carregar esta performance"));
    }

    refreshLiveSetVolumeIndicators();
}

void ClassicPlayerAudioProcessorEditor::refreshLiveSetVolumeIndicators()
{
    if (!showingLiveSet
        || !juce::isPositiveAndBelow(activeLiveSetSlot,
                                     ClassicPlayerAudioProcessor::liveSetSlotsPerBank)
        || activeLiveSetBank != loadedLiveSetBank)
        return;

    const auto isNewSlot = liveVolumeTrackedBank != activeLiveSetBank
                        || liveVolumeTrackedSlot != activeLiveSetSlot;
    liveVolumeTrackedBank = activeLiveSetBank;
    liveVolumeTrackedSlot = activeLiveSetSlot;

    const auto now = juce::Time::currentTimeMillis();
    if (isNewSlot)
        liveSetVolumeHighlightUntil.fill(0);
    const auto layerCount = juce::jlimit(0, Sf2Engine::layerCount,
                                         classicProcessor.activeLayerCount());
    juce::String volumes;
    int movingLayers = 0;
    for (int position = 0; position < layerCount; ++position)
    {
        const auto layer = classicProcessor.visualLayerAt(position);
        if (!juce::isPositiveAndBelow(layer, layerCount)) continue;
        const auto* value = classicProcessor.parameters.getRawParameterValue(
            "layer" + juce::String(layer + 1) + "Gain");
        const auto current = value != nullptr ? value->load() : 0.0f;
        if (!isNewSlot && std::abs(current - liveSetLastVolumes[(size_t) layer]) > 0.01f)
            liveSetVolumeHighlightUntil[(size_t) layer] = now + 450;
        liveSetLastVolumes[(size_t) layer] = current;
        if (liveSetVolumeHighlightUntil[(size_t) layer] > now)
            movingLayers |= 1 << position;
        if (volumes.isNotEmpty()) volumes << " ";
        volumes << "L" << juce::String(position + 1) << " "
                << juce::String(juce::roundToInt(current)) << "%";
    }

    auto& button = liveSetSlotButtons[(size_t) activeLiveSetSlot];
    const auto volumeChanged = button.getProperties()["liveVolumes"].toString() != volumes;
    const auto movementChanged = static_cast<int>(button.getProperties()["liveMovingLayers"])
                              != movingLayers;
    if (volumeChanged || movementChanged)
    {
        button.getProperties().set("liveVolumes", volumes);
        button.getProperties().set("liveMovingLayers", movingLayers);
        button.repaint();
    }
}

void ClassicPlayerAudioProcessorEditor::chooseLiveSetSlot(int slot)
{
    refreshProgramLibrary();
    juce::PopupMenu menu;
    menu.addItem(1, localizedUiText("Limpar posição", uiLanguage));
    menu.addSeparator();
    for (int item = 0; item < programFiles.size(); ++item)
        menu.addItem(item + 2, programFiles.getReference(item).getFileNameWithoutExtension());

    const juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor> safe(this);
    const auto bank = activeLiveSetBank;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(liveSetSlotButtons[(size_t) slot]),
        [safe, bank, slot](int selected)
        {
            if (safe == nullptr || selected == 0) return;
            if (selected == 1)
                safe->classicProcessor.clearLiveSetSlot(bank, slot);
            else
            {
                const auto item = selected - 2;
                if (juce::isPositiveAndBelow(item, safe->programFiles.size()))
                {
                    const auto result = safe->classicProcessor.assignLiveSetSlot(
                        bank, slot, safe->programFiles.getReference(item));
                    if (result.failed())
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::MessageBoxIconType::WarningIcon,
                            "Live Set", result.getErrorMessage());
                }
            }
            safe->refreshLiveSet();
        });
}

void ClassicPlayerAudioProcessorEditor::refreshAfterProgramLoad()
{
    displayedLayerCount = classicProcessor.activeLayerCount();
    for (int i = 0; i < Sf2Engine::layerCount; ++i)
    {
        if (strips[(size_t) i] == nullptr) continue;
        strips[(size_t) i]->setVisible(i < displayedLayerCount);
        strips[(size_t) i]->refresh();
    }
    addLayerButton.setEnabled(displayedLayerCount < Sf2Engine::layerCount);
    layoutLayerStrips();
    applyMixerStates();
    refreshLiveSet();
}

void ClassicPlayerAudioProcessorEditor::loadLiveSetSlot(int slot)
{
    for (auto& strip : strips)
        if (strip != nullptr)
            strip->closeExternalInstrumentEditor();

    const auto result = classicProcessor.loadLiveSetSlot(activeLiveSetBank, slot);
    if (result.failed())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                               "Live Set", result.getErrorMessage());
        return;
    }
    activeLiveSetSlot = slot;
    loadedLiveSetBank = activeLiveSetBank;
    programBox.setText(classicProcessor.currentSavedProgramName(), juce::dontSendNotification);
    refreshProgramLibrary();
    refreshAfterProgramLoad();
}

void ClassicPlayerAudioProcessorEditor::loadSelectedProgram()
{
    const auto selected = programBox.getSelectedItemIndex();
    if (!juce::isPositiveAndBelow(selected, programFiles.size()))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                               "Carregar programação",
                                               "Selecione uma programação salva na lista.");
        return;
    }

    // Close every hosted native editor before the processor restores the
    // program. This prevents a VST editor from retaining pointers to an
    // instrument instance that may be replaced by the selected program.
    for (auto& strip : strips)
        if (strip != nullptr)
            strip->closeExternalInstrumentEditor();

    const auto result = classicProcessor.loadProgram(programFiles.getReference(selected));
    if (result.failed())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               "Falha ao carregar programação", result.getErrorMessage());
        return;
    }

    activeLiveSetSlot = -1;
    loadedLiveSetBank = -1;
    displayedLayerCount = classicProcessor.activeLayerCount();
    for (int i = 0; i < Sf2Engine::layerCount; ++i)
    {
        if (strips[(size_t) i] == nullptr) continue;
        strips[(size_t) i]->setVisible(i < displayedLayerCount);
        strips[(size_t) i]->refresh();
    }
    addLayerButton.setEnabled(displayedLayerCount < Sf2Engine::layerCount);
    layoutLayerStrips();
    applyMixerStates();
    refreshLiveSet();
}

void ClassicPlayerAudioProcessorEditor::applyMixerStates()
{
    bool anySolo = false;
    const auto count = classicProcessor.activeLayerCount();
    for (int i = 0; i < count; ++i)
        anySolo = anySolo || strips[(size_t) i]->isSolo();
    for (int i = 0; i < count; ++i)
    {
        auto& strip = strips[(size_t) i];
        // Mute is a non-destructive output gate driven by its saved APVTS
        // state; keep the layer rendered so a learned controller works even
        // with the editor hidden. Solo still controls engine note routing.
        strip->setEngineEnabled(anySolo ? strip->isSolo() : true);
    }
}

void ClassicPlayerAudioProcessorEditor::addLayer(ClassicPlayerAudioProcessor::LayerType type)
{
    const auto newLayerIndex = classicProcessor.activeLayerCount();
    if (!classicProcessor.addLayer(type)) return;
    displayedLayerCount = classicProcessor.activeLayerCount();
    if (strips[(size_t) newLayerIndex] != nullptr)
    {
        strips[(size_t) newLayerIndex]->setVisible(true);
        strips[(size_t) newLayerIndex]->refresh();
    }
    addLayerButton.setEnabled(classicProcessor.activeLayerCount() < Sf2Engine::layerCount);
    layoutLayerStrips();
    layerViewport.setViewPosition(juce::jmax(0, layerContent.getWidth() - layerViewport.getWidth()), 0);
    applyMixerStates();
}

void ClassicPlayerAudioProcessorEditor::removeLayer(int layer)
{
    if (!classicProcessor.removeLayer(layer)) return;
    displayedLayerCount = classicProcessor.activeLayerCount();
    for (int i = 0; i < Sf2Engine::layerCount; ++i)
    {
        if (strips[(size_t) i] != nullptr)
        {
            strips[(size_t) i]->setVisible(i < displayedLayerCount);
            if (i < displayedLayerCount)
                strips[(size_t) i]->refresh();
        }
    }
    addLayerButton.setEnabled(displayedLayerCount < Sf2Engine::layerCount);
    layoutLayerStrips();
    applyMixerStates();
}

void ClassicPlayerAudioProcessorEditor::reorderLayerFromDrag(int layer, juce::Point<int> screenPosition)
{
    const auto count = classicProcessor.activeLayerCount();
    if (juce::isPositiveAndBelow(layer, count)
        && layerViewport.getScreenBounds().contains(screenPosition))
    {
        int closestLayer = -1;
        int closestDistance = std::numeric_limits<int>::max();
        for (int position = 0; position < count; ++position)
        {
            const auto candidate = classicProcessor.visualLayerAt(position);
            if (candidate == layer || !juce::isPositiveAndBelow(candidate, count)) continue;
            const auto distance = strips[(size_t) candidate]->getScreenBounds()
                .getCentre().getDistanceSquaredFrom(screenPosition);
            if (distance < closestDistance)
            {
                closestDistance = distance;
                closestLayer = candidate;
            }
        }
        if (closestLayer >= 0)
            classicProcessor.moveLayerVisually(layer, closestLayer);
    }
    layoutLayerStrips();
    refreshLiveSetVolumeIndicators();
}

void ClassicPlayerAudioProcessorEditor::layoutLayerStrips()
{
    const auto count = classicProcessor.activeLayerCount();
    if (count <= 0 || layerViewport.getWidth() <= 0) return;
    constexpr int gap = 8;
    // Keep each layer as a narrow mixer strip.  Empty space to the right is
    // intentional when fewer than eight layers are active; do not stretch the
    // channels merely to fill the viewport.
    const auto availableWidth = juce::jmax(1, layerViewport.getWidth());
    constexpr int expandedHeight = 590;
    const auto viewportHeight = layerViewport.getHeight()
        - layerViewport.getScrollBarThickness();
    // Mixer-style channels fill the complete area above the keyboard instead
    // of collapsing into short horizontal cards at the top.
    const int compactHeight = juce::jmax(148, viewportHeight - gap * 2);
    const int maxStripHeight = juce::jmax(1, viewportHeight - gap * 2);
    // Match the new slim-channel design. Fixed-width compact strips preserve
    // the proportions of the artwork and fader instead of stretching to fill a row.
    constexpr int compactStripWidth = 142;
    const auto stripWidth = juce::jmin(availableWidth, compactStripWidth);
    // Pack variable-width pad strips without stretching ordinary instruments.
    std::vector<juce::Rectangle<int>> bounds;
    int x = 0;
    int rowHeight = 0;
    for (int position=0;position<count;++position)
    {
        const auto i=classicProcessor.visualLayerAt(position);
        if (!juce::isPositiveAndBelow(i, count)) continue;
        const bool pads=classicProcessor.layerType(i)==ClassicPlayerAudioProcessor::LayerType::drumPads || classicProcessor.layerType(i)==ClassicPlayerAudioProcessor::LayerType::continuousPads;
        const int width=pads ? juce::jmin(availableWidth,juce::jmax(420,stripWidth*2)) : stripWidth;
        const int requestedHeight = strips[(size_t)i] && strips[(size_t)i]->isExpanded()
            ? expandedHeight : juce::jmax(pads ? 322 : 148, compactHeight);
        const int height = juce::jmin(maxStripHeight, requestedHeight);
        bounds.emplace_back(x, gap, width, height);
        x+=width+gap;rowHeight=juce::jmax(rowHeight,height);
    }
    const auto contentWidth = juce::jmax(availableWidth, x > 0 ? x - gap : availableWidth);
    const auto contentHeight = juce::jmax(layerViewport.getHeight()
        - layerViewport.getScrollBarThickness(), rowHeight + gap * 2);
    layerContent.setSize(contentWidth, contentHeight);
    for (int position = 0; position < Sf2Engine::layerCount; ++position)
    {
        const auto i = classicProcessor.visualLayerAt(position);
        if (!juce::isPositiveAndBelow(i, Sf2Engine::layerCount)) continue;
        if (strips[(size_t) i] == nullptr) continue;
        strips[(size_t) i]->setVisible(position < count);
        if (position >= count) continue;
        strips[(size_t) i]->setDisplayPosition(position);
        strips[(size_t) i]->setBounds(bounds[(size_t)position]);
    }
}

void ClassicPlayerAudioProcessorEditor::activate()
{
    const auto email = activationEmail.getText().trim();
    const auto password = activationPassword.getText();
    if (email.isEmpty() || password.isEmpty())
    { activationStatus.setText(localizedUiText("Informe e-mail e senha.", activeUiLanguage.load()), juce::dontSendNotification); return; }
    activationButton.setEnabled(false);
    activationStatus.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    activationStatus.setText(localizedUiText("Conectando ao servidor de licença...", activeUiLanguage.load()), juce::dontSendNotification);
    const juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor> safe(this);
    juce::Thread::launch([safe, email, password]
    {
        juce::String error;
        const auto ok = LicenseVerifier::loginOnline(email, password, error);
        juce::MessageManager::callAsync([safe, ok, error]
        {
            if (safe == nullptr) return;
            safe->activationButton.setEnabled(true);
            if (ok)
            { safe->classicProcessor.refreshActivation(); safe->userLabel.setText(accountIdentityText(), juce::dontSendNotification); safe->userLabel.setVisible(safe->userLabel.getText().isNotEmpty()); safe->activationPanel.setVisible(false); }
            else
            { safe->activationStatus.setColour(juce::Label::textColourId, juce::Colours::salmon); safe->activationStatus.setText(error, juce::dontSendNotification); }
        });
    });
}

void ClassicPlayerAudioProcessorEditor::validateStoredOnlineSession()
{
    activationButton.setEnabled(false);
    activationStatus.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    activationStatus.setText(localizedUiText("Validando a licença deste computador...", activeUiLanguage.load()), juce::dontSendNotification);
    const juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor> safe(this);
    juce::Thread::launch([safe]
    {
        juce::String error;
        const auto ok = LicenseVerifier::validateOnlineSession(error);
        juce::MessageManager::callAsync([safe, ok, error]
        {
            if (safe == nullptr) return;
            safe->activationButton.setEnabled(true);
            if (ok)
            { safe->classicProcessor.refreshActivation(); safe->userLabel.setText(accountIdentityText(), juce::dontSendNotification); safe->userLabel.setVisible(safe->userLabel.getText().isNotEmpty()); safe->activationPanel.setVisible(false); }
            else
            {
                // A timeout/DNS failure must not turn a valid cached session
                // into a login prompt.  validateOnlineSession() only removes
                // the session after an explicit server rejection, so this
                // branch cleanly separates offline use from revocation.
                if (LicenseVerifier::hasOnlineSession())
                {
                    safe->classicProcessor.refreshActivation();
                    safe->userLabel.setText(accountIdentityText(), juce::dontSendNotification);
                    safe->userLabel.setVisible(safe->userLabel.getText().isNotEmpty());
                    safe->activationPanel.setVisible(false);
                }
                else
                {
                    safe->classicProcessor.refreshActivation();
                    safe->activationPanel.setVisible(true);
                    safe->activationPanel.toFront(false);
                    safe->languageSelector.toFront(false);
                    safe->activationStatus.setColour(juce::Label::textColourId, juce::Colours::salmon);
                    safe->activationStatus.setText(error.isNotEmpty() ? error
                        : juce::String::fromUTF8("Faça login para ativar este computador."),
                        juce::dontSendNotification);
                }
            }
        });
    });
}

std::unique_ptr<juce::Component> createHammondEditorContent(ClassicPlayerAudioProcessor& processor,
                                                             int index,
                                                             std::function<void()> reverbCallback,
                                                             std::function<void()> compressorCallback,
                                                             std::function<void()> eqCallback,
                                                             std::function<void()> saveLayerCallback,
                                                             std::function<void()> loadLayerCallback)
{
    class Content final : public juce::Component
    {
    public:
        Content()
        {
            heading.setText(juce::String::fromUTF8("Drawbars, Leslie e MIDI. Salve a programação para guardar o timbre."),
                            juce::dontSendNotification);
            heading.setJustificationType(juce::Justification::centred);
            heading.setColour(juce::Label::textColourId,juce::Colour(text));
            addAndMakeVisible(heading);
        }
        void add(juce::Component* child,int height)
        {
            children.add(child);heights.push_back(height);addAndMakeVisible(child);
        }
        void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(panel)); }
        void resized() override
        {
            heading.setBounds(12,3,getWidth()-24,26);
            int y=32;
            for(int i=0;i<children.size();++i){
                children[i]->setBounds(12,y,getWidth()-24,heights[(size_t)i]);
                y+=heights[(size_t)i]+4;
            }
        }
        juce::OwnedArray<juce::Component> children;
        std::vector<int> heights;
        juce::Label heading;
    };
    auto content=std::make_unique<Content>();
    auto* hammondPanel = new HammondEditorPanel(processor,index);
    hammondPanel->setUiTextTranslator([](const juce::String& value)
    {
        return localizedUiText(value, activeUiLanguage.load());
    });
    content->add(hammondPanel,220);
    content->add(new EngineProgramSavePanel(processor,index,"Hammond"),28);
    content->add(new LayerPresetFilePanel(std::move(saveLayerCallback),
                                          std::move(loadLayerCallback)),28);
    content->add(new LayerRoutingEditorPanel(processor,index),92);
    const auto prefix="layer"+juce::String(index+1);
    const auto value=[&processor,prefix](const char* name){return processor.parameters.getRawParameterValue(prefix+name)->load();};
    auto* common=new KnobEditorPanel({
        {"VOLUME",value("Gain"),0,100,1,0},{"ATTACK ms",value("Attack"),0,100,0.1f,1},
        {"RELEASE ms",value("Release"),0,100,1,0},{"CUTOFF",value("Cutoff"),0,100,1,0},
        {"REVERB",value("Reverb"),0,100,1,0},{"COMP",value("Comp"),0,100,1,0}},6);
    const std::array<const char*,6> commonParameterSuffixes {
        "Gain","Attack","Release","Cutoff","Reverb","Comp"
    };
    for(int control=0;control<(int)commonParameterSuffixes.size();++control)
        common->bindParameter(control,processor.parameters,
                              prefix+commonParameterSuffixes[(size_t)control]);
    // This layout follows the available height, including each numeric field.
    common->useCompactGrid(true);
    content->add(common,70);
    common->setOnValueChange([&processor,common,prefix]{
        const std::array<const char*,6> names {"Gain","Attack","Release","Cutoff","Reverb","Comp"};
        for(int i=0;i<6;++i)if(auto* p=processor.parameters.getParameter(prefix+names[(size_t)i]))
            p->setValueNotifyingHost(p->convertTo0to1(common->value(i)));
    });
    content->add(new SideBySideEditorPanel(
        new LayerEffectButtons(std::move(reverbCallback), std::move(compressorCallback), {}, std::move(eqCallback)),
        new LayerMidiLearnPanel(processor,index), 680, 54), 54);
    content->setSize(704,548);
    return content;
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::showHammondEditor()
{
    if(processor.layerType(index)!=ClassicPlayerAudioProcessor::LayerType::hammond)return;
    class Window final : public juce::DocumentWindow
    {
    public:
        Window(std::unique_ptr<juce::Component> content, juce::Component* owner)
            :DocumentWindow("Hammond",juce::Colour(panel),juce::DocumentWindow::closeButton)
        {
            setLookAndFeel(&classicLookAndFeel);
            setUsingNativeTitleBar(true);
            setContentOwned(content.release(),false);
            auto& displays=juce::Desktop::getInstance().getDisplays();
            const auto appBounds=owner!=nullptr&&owner->getTopLevelComponent()!=nullptr
                ? owner->getTopLevelComponent()->getScreenBounds():juce::Rectangle<int>{};
            const auto* display=appBounds.isEmpty()?displays.getPrimaryDisplay()
                                                    :displays.getDisplayForRect(appBounds);
            const auto screen=display!=nullptr?display->userArea.toNearestInt()
                                               :juce::Rectangle<int>(0,0,1280,800);
            const auto usable=screen.reduced(8);
            const auto visibleAppBounds=appBounds.getIntersection(screen);
            const auto centre=visibleAppBounds.isEmpty()?screen.getCentre()
                                                        :visibleAppBounds.getCentre();
            const auto width=juce::jmax(1,juce::jmin(740,usable.getWidth()));
            const auto height=juce::jmax(1,juce::jmin(580,usable.getHeight()));
            const auto x=juce::jlimit(usable.getX(),usable.getRight()-width,centre.x-width/2);
            const auto y=juce::jlimit(usable.getY(),usable.getBottom()-height,centre.y-height/2);
            setBounds(x,y,width,height);
        }
        ~Window() override { clearContentComponent();setLookAndFeel(nullptr); }
        void resized() override
        {
            DocumentWindow::resized();
            if(auto* content=getContentComponent())
                content->setSize(juce::jmax(620,getWidth()-12),548);
        }
        void closeButtonPressed() override { exitModalState(0); }
    };
    const juce::Component::SafePointer<LayerStrip> safe(this);
    auto* window=new Window(createHammondEditorContent(processor, index,
        [safe] { if (safe != nullptr) safe->showReverbEditor(); },
        [safe] { if (safe != nullptr) safe->showCompressorEditor(); },
        [safe] { if (safe != nullptr) safe->showEqEditor(); },
        [safe] { if (safe != nullptr) safe->saveLayerPreset(); },
        [safe] { if (safe != nullptr) safe->loadLayerPreset(); }), this);
    window->enterModalState(true,juce::ModalCallbackFunction::create([safe](int){if(safe!=nullptr)safe->refresh();}),true);
}

std::unique_ptr<juce::Component> createAnalogCommonControls(ClassicPlayerAudioProcessor& processor, int layer)
{
    auto controls = std::make_unique<KnobEditorPanel>(std::initializer_list<KnobEditorSpec>{
        { "VOLUME", 80, 0, 100, 1, 0 },
        { "ATTACK ms", 5, 0, 100, 0.1f, 1 },
        { "RELEASE ms", 50, 0, 100, 1, 0 },
        { "CUTOFF", 100, 0, 100, 0, 2 },
        { "REVERB", 0, 0, 100, 1, 0 },
        { "COMP", 0, 0, 100, 1, 0 }
    }, 6);
    const auto prefix = "layer" + juce::String(layer + 1);
    const std::array<const char*, 6> suffixes { "Gain", "Attack", "Release", "Cutoff", "Reverb", "Comp" };
    for (int i = 0; i < 6; ++i)
        controls->bindParameter(i, processor.parameters, prefix + suffixes[(size_t) i]);
    controls->useCompactGrid(false);
    return controls;
}
