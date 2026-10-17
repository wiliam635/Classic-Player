#include "NordBankEngine.h"
#include "NordBundleImporter.h"
#include "NordSampleDecoder.h"
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <cstdlib>

namespace {
void require(bool ok, const juce::String& reason) { if (!ok) throw std::runtime_error(reason.toStdString()); }
}
int runNordSampleRegressionTests();
int main(int argc, char** argv)
{
    try {
        require(argc >= 2, "Pass the local .ns4pbundle fixture (not shipped).");
        if(juce::String(argv[1])=="--self-test")return runNordSampleRegressionTests();
        if (juce::String(argv[1]).endsWithIgnoreCase(".nsmp4")) {
            NordSampleDecoder::Instrument instrument;
            const auto result=NordSampleDecoder::decode(juce::File(argv[1]),instrument);
            require(result.wasOk(),result.getErrorMessage());
            std::cout<<"NSMP4 "<<instrument.name<<" zones="<<instrument.zones.size()<<std::endl;
            for(const auto& z:instrument.zones)std::cout<<"root="<<z.root<<" frames="<<z.frames()<<" channels="<<z.channels<<" loop="<<z.loopStart<<":"<<z.loopEnd<<" gain="<<z.gain<<std::endl;
            return 0;
        }
        juce::TemporaryFile temporary;
        const auto root = temporary.getFile();
        require(root.createDirectory().wasOk(), "temporary directory");
        NordBundleImporter::ImportSummary summary;
        const auto imported = NordBundleImporter::importBundle(juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]), root, summary);
        require(imported.wasOk(), imported.getErrorMessage());
        require(summary.programCount == 30 && summary.pianoCount == 10 && summary.sampleLibraryCount == 1, "catalog counts");
        const auto programs = NordBankEngine::programs(summary.bankDirectory);
        require(programs.size() == 31, "manifest enumeration includes 30 programs and 1 NSMP4");
        auto engine = std::make_unique<NordBankEngine>();
        const auto rate = argc > 3 ? std::atof(argv[3]) : 44100.0;
        require(rate == 44100 || rate == 48000, "test sample rate");
        engine->prepare(rate, 256);
        int played = 0;
        juce::String previous;
        for (const auto& program : programs) {
            if (!program.playable[0]) continue;
            std::cout << "Load " << program.name << std::endl;
            auto result = engine->load(0, program.file, 0);
            require(result.wasOk(), result.getErrorMessage());
            previous = engine->path(0);
            Sf2Engine::LayerConfig config; config.release = 30;
            juce::AudioBuffer<float> output(2, 256); juce::MidiBuffer midi, empty;
            std::unique_ptr<SampleEngine> reference;
            std::array<Sf2Engine::LayerConfig,Sf2Engine::layerCount> referenceConfig {};
            std::array<juce::MidiBuffer,Sf2Engine::layerCount> referenceMidi;
            juce::AudioBuffer<float> referenceOutput(2,256);
            if(program.name=="Gordo Pno FLP" && argc>2) {
                juce::MemoryBlock bytes;program.file.loadFileAsData(bytes);
                const auto* first=static_cast<const std::uint8_t*>(bytes.getData());
                NordProgramDecoder::Program decoded;std::string error;
                require(NordProgramDecoder::decode({first,first+bytes.getSize()},decoded,error),juce::String(error));
                reference=std::make_unique<SampleEngine>();reference->prepare(rate,256);
                const auto files=summary.bankDirectory.findChildFiles(juce::File::findFiles,true,"*.npno");
                for(int part=0;part<2;++part) {
                    const auto& p=decoded.pianos[size_t(part)];juce::File file;
                    for(const auto& f:files)if(f.getFileNameWithoutExtension().startsWith(juce::String(p.pianoName)+" ")){file=f;break;}
                    require(file.existsAsFile(),"parity source missing");
                    require(reference->loadSample(part,file).wasOk(),"parity source decode");
                    auto c=config;c.gain*=p.gain;c.octave+=p.octave;c.sustainEnabled&=p.sustain;
                    c.pianoTimbre=p.timbre;c.eqLow=p.eqLow;c.eqMid=p.eqMid;c.eqHigh=p.eqHigh;c.reverb=p.reverb;
                    referenceConfig[size_t(part)]=c;
                }
            }
            if (argc > 2) {
                float idlePeak = 0;
                for (int block = 0; block < 350; ++block) {
                    output.clear(); engine->process(0, output, empty, empty, config);
                    if(reference){referenceOutput.clear();reference->process(referenceOutput,empty,referenceMidi,referenceConfig);}
                    idlePeak = juce::jmax(idlePeak, output.getMagnitude(0, 256));
                }
                std::cout << " idlePeak=" << idlePeak;
            }
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
            if (argc > 2) for (const auto note : {64,67,72})
                midi.addEvent(juce::MidiMessage::noteOn(1, note, 0.8f), 0);
            float peak = 0;
            double energy = 0, highEnergy = 0, low[2] {}, renderMs = 0; int clipped = 0, count = 0;float parityError=0;
            for (int block = 0; block < (argc > 2 ? 700 : 30); ++block) {
                const auto before = std::chrono::steady_clock::now();
                output.clear(); engine->process(0, output, midi, empty, config);
                if(reference) {
                    referenceOutput.clear();reference->process(referenceOutput,midi,referenceMidi,referenceConfig);
                    for(int ch=0;ch<2;++ch)for(int i=0;i<256;++i)parityError=juce::jmax(parityError,std::abs(referenceOutput.getSample(ch,i)-output.getSample(ch,i)));
                }
                midi.clear();
                renderMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count();
                const auto magnitude = output.getMagnitude(0, 256);
                require(std::isfinite(magnitude), "nonfinite audio"); peak = juce::jmax(peak, magnitude);
                if (argc > 2) for (int ch = 0; ch < 2; ++ch) for (int sample = 0; sample < 256; ++sample) {
                    const auto v = output.getSample(ch, sample);
                    low[ch] += 0.4 * (v - low[ch]);
                    energy += v * v; highEnergy += (v - low[ch]) * (v - low[ch]);
                    if (std::abs(v) > 1) ++clipped; ++count;
                }
            }
            if (argc > 2) std::cout << " heldPeak=" << peak << " RMS=" << std::sqrt(energy/count)
                << " highRatio=" << highEnergy/juce::jmax(energy, 1e-20) << " over0dB=" << clipped << '/' << count
                << " renderLoad=" << renderMs/(700*256/rate*1000)*100 << "% rate=" << rate << std::endl;
            require(peak > 0.00001f, "silent compatible program: " + program.name);
            if(reference) {std::cout<<"Gordo NPNO/reference max error="<<parityError<<std::endl;require(parityError<0.00001f,"bank changed piano samples versus independent NPNO sources");}
            ++played;
        }
        std::cout<<"Played scene I programs: "<<played<<std::endl;
        require(played > 7, "compatible scene I count");
        for (const auto& program : programs) if (!program.playable[0]) {
            require(engine->load(0, program.file, 0).failed(), "unsupported program accepted");
            require(engine->path(0) == previous, "failed selection replaced previous sound"); break;
        }
        engine->stopAllSounds(); engine->unload(0);
        require(!engine->isLoaded(0), "unload");
        require(NordBankEngine::programs(summary.bankDirectory).size() == 31, "persistent catalog");
        std::cout << "Nord bank import/playback regression passed" << std::endl;
        // Only our generated temporary test copy is deleted; original bundle is untouched.
        root.deleteRecursively(false);
    } catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 1; }
}
