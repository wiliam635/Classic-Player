#include "NordPianoEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace nord_android {
namespace {
constexpr size_t headerBytes=44, minimumBody=0x732, directory=0x732;
constexpr size_t recordBytes=118, wordsPerBlock=511, overlap=64;
uint16_t be16(const uint8_t* p){return (uint16_t)((p[0]<<8)|p[1]);}
uint32_t be32(const uint8_t* p){return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
uint32_t le32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
std::string ascii(const uint8_t* p,size_t n){size_t end=0;while(end<n&&p[end])++end;std::string s((const char*)p,end);while(!s.empty()&&s.back()==' ')s.pop_back();return s;}
uint32_t crc32(const uint8_t* p,size_t n){uint32_t c=0xffffffffu;for(size_t i=0;i<n;++i){c^=p[i];for(int b=0;b<8;++b)c=(c>>1)^((c&1)?0xedb88320u:0u);}return c^0xffffffffu;}
struct Bits {
    const uint8_t* data;size_t length,next=0;uint64_t reservoir=0;unsigned held=0;
    bool take(unsigned width,int32_t& result){
        while(held<width){size_t at=next*2;if(at+2>length)return false;reservoir|=uint64_t(be16(data+at))<<held;held+=16;++next;}
        uint64_t mask=(uint64_t(1)<<width)-1;int64_t v=int64_t(reservoir&mask);reservoir>>=width;held-=width;
        if(v&(int64_t(1)<<(width-1)))v-=int64_t(1)<<width;result=(int32_t)v;return true;
    }
};
bool decode(const uint8_t* audio,size_t bytes,int channels,uint32_t expected,uint16_t blocks,
            const uint8_t* record,std::vector<int16_t>& pcm,std::string& error){
    const size_t blockBytes=wordsPerBlock*2*size_t(channels);
    if(!blocks||!expected||bytes!=size_t(blocks)*blockBytes){error="Gravação Nord vazia ou tamanho inválido";return false;}
    const uint64_t longest=(8*(blockBytes-2))/channels;
    if(longest<=overlap||uint64_t(expected)>uint64_t(blocks)*(longest-overlap)){
        error="Duração Nord inválida";return false;
    }
    std::array<std::array<int64_t,4>,2> history{};
    for(int ch=0;ch<channels;++ch)for(int seed=0;seed<4;++seed)
        history[(size_t)ch][(size_t)(3-seed)]=(int16_t)be16(record+12+(ch*4+seed)*2);
    pcm.clear();pcm.reserve(size_t(expected)*size_t(channels));
    std::vector<int64_t> tail;size_t produced=0;
    static constexpr int coefficients[5][4]={{0,0,0,0},{-1,0,0,0},{-2,1,0,0},{-3,3,-1,0},{-4,6,-4,1}};
    for(size_t block=0;block<blocks;++block){
        const uint8_t* raw=audio+block*blockBytes;uint16_t h=be16(raw);
        unsigned width=h&31u,order=(h>>5)&7u;
        if(width<1||width>16||order>4){error="Bloco Nord não suportado";return false;}
        size_t frames=(8*(blockBytes-2))/(width*size_t(channels));
        if(frames<overlap+4){error="Bloco Nord truncado";return false;}
        Bits bits{raw+2,blockBytes-2};std::vector<int64_t> decoded(frames*size_t(channels));
        for(size_t frame=0;frame<frames;++frame)for(int ch=0;ch<channels;++ch){
            int32_t residual=0;if(!bits.take(width,residual)){error="Áudio Nord incompleto";return false;}
            auto& hist=history[(size_t)ch];int64_t predicted=0;
            for(unsigned i=0;i<order;++i)predicted+=int64_t(coefficients[order][i])*hist[i];
            int64_t value=int64_t(residual)-predicted;
            for(int i=3;i>0;--i)hist[(size_t)i]=hist[(size_t)i-1];hist[0]=value;
            decoded[frame*size_t(channels)+size_t(ch)]=value;
        }
        const size_t owned=frames-overlap;
        if(!tail.empty())for(size_t i=0;i<overlap*size_t(channels);++i)
            if(decoded[i]!=tail[i]){error="Sobreposição de blocos Nord inválida";return false;}
        if(owned>size_t(expected)-produced){error="Áudio Nord excede duração declarada";return false;}
        for(size_t i=0;i<owned*size_t(channels);++i)
            pcm.push_back((int16_t)std::clamp<int64_t>(decoded[i],-32768,32767));
        produced+=owned;tail.assign(decoded.begin()+owned*size_t(channels),decoded.end());
        for(int ch=0;ch<channels;++ch)for(int i=0;i<4;++i)
            history[(size_t)ch][(size_t)i]=decoded[(owned-1-size_t(i))*size_t(channels)+size_t(ch)];
    }
    if(produced!=expected){error="Duração Nord não corresponde ao diretório";return false;}
    return true;
}
void smoothLoop(Stroke& s,int channels){
    int start=(int)s.loopStart,end=(int)s.loopEnd,length=end-start;
    if(length<8||end>(int)(s.pcm.size()/size_t(channels)))return;
    int maxFade=(int)(length*.45),aim=std::min(maxFade,(int)std::lround(.55*sampleRate));
    double frequency=440.0*std::pow(2.0,(s.root-69)/12.0),period=sampleRate/frequency;
    int low=std::max(2,(int)std::lround(aim-period)),high=std::min(maxFade,(int)std::lround(aim+period));
    if(low>high)return;int step=std::max(1,(int)std::lround(period/24.0)),fade=low;double best=-2,corr=0;
    auto read=[&](int frame,int ch){return double(s.pcm[size_t(frame)*size_t(channels)+size_t(ch)]);};
    for(int candidate=low;candidate<=high;candidate+=step){
        double dot=0,head=0,tail=0;
        for(int f=0;f<candidate;f+=8)for(int ch=0;ch<channels;++ch){double a=read(start+f,ch),b=read(end-candidate+f,ch);dot+=a*b;head+=a*a;tail+=b*b;}
        double norm=std::sqrt(head*tail),c=norm>0?dot/norm:1.0;
        double score=c-.01*std::abs(candidate-aim)/std::max(1,aim);
        if(score>best){best=score;fade=candidate;corr=std::clamp(c,0.0,1.0);}
    }
    auto blend=[&](int f,int ch,bool compensate){
        double weight=.5-.5*std::cos(3.141592653589793*f/(fade-1));
        double norm=compensate?std::sqrt((1-weight)*(1-weight)+weight*weight+2*corr*weight*(1-weight)):1.0;
        return ((1-weight)*read(end-fade+f,ch)+weight*read(start+f,ch))/norm;
    };
    bool compensate=true;
    for(int f=0;f<fade&&compensate;++f)for(int ch=0;ch<channels;++ch)
        if(std::abs(blend(f,ch,true))>32767){compensate=false;break;}
    for(int f=0;f<fade;++f)for(int ch=0;ch<channels;++ch)
        s.pcm[size_t(end-fade+f)*size_t(channels)+size_t(ch)]=(int16_t)std::lround(blend(f,ch,compensate));
    s.loopStart+=(uint32_t)fade;
}
float sample(const Stroke& s,int channels,int ch,double position){
    const int count=(int)(s.pcm.size()/size_t(channels));if(!count)return 0;
    const int first=std::clamp((int)position,0,count-1);float t=(float)(position-first),t2=t*t,t3=t2*t;
    auto at=[&](int frame){
        if(s.loopEnd>s.loopStart&&frame>=(int)s.loopEnd)
            frame=(int)s.loopStart+(frame-(int)s.loopEnd)%int(s.loopEnd-s.loopStart);
        frame=std::clamp(frame,0,count-1);
        return float(s.pcm[size_t(frame)*size_t(channels)+size_t(std::min(ch,channels-1))])/32768.f;
    };
    float a=at(first-1),b=at(first),c=at(first+1),d=at(first+2);
    return .5f*((2*b)+(-a+c)*t+(2*a-5*b+4*c-d)*t2+(-a+3*b-3*c+d)*t3);
}
}

bool load(const std::string& path,size_t budget,Instrument& out,std::string& error){
    std::ifstream stream(path,std::ios::binary|std::ios::ate);
    if(!stream){error="Arquivo NPNO não encontrado";return false;}
    const std::streamoff fileSize=stream.tellg();
    if(fileSize<(std::streamoff)(headerBytes+minimumBody)||fileSize>512ll*1024*1024){error="Tamanho NPNO inválido";return false;}
    stream.seekg(0);std::vector<uint8_t> file((size_t)fileSize);
    if(!stream.read((char*)file.data(),static_cast<std::streamsize>(fileSize))){error="Falha ao ler NPNO";return false;}
    const uint8_t* bytes=file.data();
    if(std::memcmp(bytes,"CBIN",4)||le32(bytes+4)!=1||std::memcmp(bytes+8,"npno",4)){
        error="Não é um NPNO CBIN compatível";return false;
    }
    const uint8_t* body=bytes+headerBytes;size_t bodySize=file.size()-headerBytes;
    if(crc32(body,bodySize)!=le32(bytes+24)||std::memcmp(body,"CNSP",4)){
        error="CRC ou fluxo CNSP inválido";return false;
    }
    uint16_t version=be16(body+4),channels=be16(body+0x61e),count=be16(body+0x620);
    if((version!=0x450&&version!=0x464)||be16(body+0x61c)!=version||channels<1||channels>2){
        error="Versão ou canais NPNO não suportados";return false;
    }
    size_t summed=0;for(int root=0;root<128;++root)summed+=be16(body+0x622+root*2);
    size_t dirEnd=directory+size_t(count)*recordBytes;
    if(summed!=count||dirEnd>bodySize){error="Diretório NPNO inválido";return false;}
    size_t blockBytes=wordsPerBlock*2*size_t(channels);
    size_t firstAudio=((dirEnd+192+blockBytes-1)/blockBytes)*blockBytes-192;
    if(firstAudio>bodySize){error="Alinhamento NPNO inválido";return false;}
    for(size_t i=dirEnd;i<firstAudio;++i)if(body[i]){error="Preenchimento NPNO inválido";return false;}
    uint64_t decodedBytes=0;int highestLayer=0;
    for(int i=0;i<count;++i){const uint8_t* rec=body+directory+size_t(i)*recordBytes;
        if(rec[4]<=1){
            if(be32(rec+6)>uint32_t(std::numeric_limits<int>::max())){error="Gravação NPNO longa demais";return false;}
            decodedBytes+=uint64_t(be32(rec+6))*channels*2u;
            if(rec[4]==0)highestLayer=std::max(highestLayer,int(rec[5]));
        }}
    if(decodedBytes>budget){error="Áudio NPNO decodificado excede limite de memória";return false;}
    Instrument result;result.channels=channels;result.gainTenthsDb=(int8_t)body[0x40c];
    std::copy_n(body+0x8c,128,result.keyMap.begin());
    for(int i=0;i<128;++i)result.fineTune[(size_t)i]=(int8_t)body[0x18c+i];
    result.name=ascii(body+0x1c,0x20);auto hash=result.name.find('#');if(hash!=std::string::npos)result.name.resize(hash);
    if(version==0x464){auto longName=ascii(body+0x3c,0x20);if(!longName.empty())result.name=longName;}
    result.strokes.reserve(count);size_t cursor=firstAudio,index=0;std::array<bool,128> attackRoots{};
    for(int root=0;root<128;++root)for(int j=0,n=be16(body+0x622+root*2);j<n;++j,++index){
        const uint8_t* rec=body+directory+index*recordBytes;
        uint32_t start=be32(rec),frames=be32(rec+6);uint16_t blocks=be16(rec+10);
        size_t bytesForStroke=size_t(blocks)*blockBytes;
        if(cursor>bodySize||start!=cursor||bytesForStroke>bodySize-cursor){error="Deslocamento NPNO inválido";return false;}
        if(rec[4]<=1){Stroke s;s.root=(uint8_t)root;s.bank=rec[4];s.velocityLayer=rec[5];s.trim=be16(rec+0x34);
            uint32_t loopStart=be32(rec+0x1c),loopLength=be32(rec+0x20);
            if(loopStart>0&&loopLength>0&&loopStart<frames&&loopLength<=frames-loopStart){s.loopStart=loopStart;s.loopEnd=loopStart+loopLength;}
            result.signedTrim|=s.trim>=0x8000;
            if(!decode(body+cursor,bytesForStroke,channels,frames,blocks,rec,s.pcm,error))return false;
            if(s.bank==0)attackRoots[(size_t)root]=true;
            smoothLoop(s,channels);result.strokes.push_back(std::move(s));
        }
        cursor+=bytesForStroke;
    }
    if(cursor!=bodySize){error="Bytes extras no fluxo NPNO";return false;}
    for(uint8_t root:result.keyMap)if(root!=0xff&&!attackRoots[root]){error="Mapa de notas NPNO inválido";return false;}
    if(highestLayer>30)result.velocityScale=std::max(31,(highestLayer*31+26)/27);
    out=std::move(result);return true;
}

void noteOn(Layer& layer,int note,int velocity,int channel,bool sustain,int voiceBudget,float attackSeconds){
    if(!layer.instrument||note<0||note>127||velocity<1)return;
    const auto& piano=*layer.instrument;int root=piano.keyMap[(size_t)note];if(root==0xff)return;
    int bank=sustain?1:0,limit=((127-velocity)*piano.velocityScale)/127;
    int chosen=-1,best=-1;
    for(size_t i=0;i<piano.strokes.size();++i){const auto& s=piano.strokes[i];if(s.root==root&&s.bank==bank&&s.velocityLayer<=limit&&s.velocityLayer>best){chosen=(int)i;best=s.velocityLayer;}}
    if(chosen<0&&bank==1)for(size_t i=0;i<piano.strokes.size();++i){const auto& s=piano.strokes[i];if(s.root==root&&s.bank==0&&s.velocityLayer<=limit&&s.velocityLayer>best){chosen=(int)i;best=s.velocityLayer;}}
    if(chosen<0)return;
    Voice* target=nullptr;int budget=std::clamp(voiceBudget,1,maxVoices);
    for(int i=0;i<budget;++i)if(!layer.voices[(size_t)i].active){target=&layer.voices[(size_t)i];break;}
    if(!target){target=&layer.voices[0];for(int i=1;i<budget;++i){auto& v=layer.voices[(size_t)i];
        if((v.released&&!target->released)||(v.released==target->released&&v.serial<target->serial))target=&v;}}
    const auto& s=piano.strokes[(size_t)chosen];int trim=(int)s.trim;if(trim>=0x8000)trim-=0x10000;
    float trimDb=piano.signedTrim?-trim/10.f:-(float)s.trim;
    double cents=piano.fineTune[(size_t)note]*.7;
    *target={};target->active=true;target->note=note;target->channel=channel;target->stroke=chosen;
    target->serial=++layer.serial;target->increment=std::pow(2.0,(note-root+cents/100.0)/12.0)*sampleRate/44100.0;
    target->level=(velocity/127.f)*std::pow(10.f,(piano.gainTenthsDb/10.f+trimDb)/20.f);
    target->attackGain=attackSeconds>0?0.f:1.f;
}
void noteOff(Layer& layer,int note,int channel,bool sustain){
    for(auto& v:layer.voices)if(v.active&&!v.released&&v.note==note&&v.channel==channel){
        if(sustain)v.waitingForSustain=true;else{v.released=true;v.waitingForSustain=false;}
    }
}
void pedalUp(Layer& layer,int channel){for(auto& v:layer.voices)if(v.active&&v.channel==channel&&v.waitingForSustain){v.waitingForSustain=false;v.released=true;}}
void render(Layer& layer,float* stereo,int frames,int outputRate,int budget,float attackSeconds,float releaseSeconds){
    if(!layer.instrument||!stereo)return;const auto& piano=*layer.instrument;
    const int voices=std::clamp(budget,0,maxVoices);
    const float attackStep=attackSeconds>0?1.f/std::max(1.f,attackSeconds*outputRate):1.f;
    const float releaseFactor=std::exp(std::log(.0001f)/std::max(1.f,std::max(.005f,releaseSeconds)*outputRate));
    const uint64_t maxAge=(uint64_t)(outputRate*15.0);
    for(int vi=0;vi<voices;++vi){auto& v=layer.voices[(size_t)vi];if(!v.active||v.stroke<0)continue;
        const auto& s=piano.strokes[(size_t)v.stroke];int total=(int)(s.pcm.size()/size_t(piano.channels));
        for(int f=0;f<frames;++f){
            if(++v.age>=maxAge&&layer.finitePiano){v={};break;}
            if(s.loopEnd>s.loopStart&&v.position>=s.loopEnd)
                v.position=s.loopStart+std::fmod(v.position-s.loopEnd,double(s.loopEnd-s.loopStart));
            else if(v.position>=total){v={};break;}
            v.attackGain=std::min(1.f,v.attackGain+attackStep);
            if(v.released)v.releaseGain*=releaseFactor;
            if(v.releaseGain<.0001f){v={};break;}
            float remaining=std::max(0.f,1.f-float(v.age)/float(outputRate*15));
            float decay=layer.finitePiano?remaining*remaining:1.f;
            float gain=v.level*v.attackGain*v.releaseGain*decay;
            stereo[(size_t)f*2]+=sample(s,piano.channels,0,v.position)*gain;
            stereo[(size_t)f*2+1]+=sample(s,piano.channels,1,v.position)*gain;
            v.position+=v.increment*44100.0/outputRate;
        }
    }
    static constexpr float bands[3][3]={{1.5f,-1.f,-9.f},{-3.f,3.f,-3.f},{-1.f,0.f,6.f}};
    const int mode=std::clamp(layer.eqMode,0,2);
    const float lowGain=std::pow(10.f,bands[mode][0]/20.f);
    const float midGain=std::pow(10.f,bands[mode][1]/20.f);
    const float highGain=std::pow(10.f,bands[mode][2]/20.f);
    const float lowAlpha=1.f-std::exp(-6.2831853f*250.f/outputRate);
    const float highAlpha=1.f-std::exp(-6.2831853f*2000.f/outputRate);
    for(int f=0;f<frames;++f)for(int ch=0;ch<2;++ch){
        size_t i=(size_t)f*2+size_t(ch);float x=stereo[i];
        float& low=layer.eqLowState[(size_t)ch];float& high=layer.eqHighState[(size_t)ch];
        low+=(x-low)*lowAlpha;high+=(x-high)*highAlpha;
        stereo[i]=low*lowGain+(high-low)*midGain+(x-high)*highGain;
    }
}
int activeVoices(const Layer& layer){int count=0;for(const auto& v:layer.voices)if(v.active)++count;return count;}
}
