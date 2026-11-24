#include "NordPianoEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

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
    if(length<8||end>(int)(s.samples/size_t(channels)))return;
    int maxFade=(int)(length*.45),aim=std::min(maxFade,(int)std::lround(.55*sampleRate));
    double frequency=440.0*std::pow(2.0,(s.root-69)/12.0),period=sampleRate/frequency;
    int low=std::max(2,(int)std::lround(aim-period)),high=std::min(maxFade,(int)std::lround(aim+period));
    if(low>high)return;int step=std::max(1,(int)std::lround(period/24.0)),fade=low;double best=-2,corr=0;
    auto read=[&](int frame,int ch){return double(s.data[size_t(frame)*size_t(channels)+size_t(ch)]);};
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
        s.owned[size_t(end-fade+f)*size_t(channels)+size_t(ch)]=(int16_t)std::lround(blend(f,ch,compensate));
    s.loopStart+=(uint32_t)fade;
}
void sampleStereo(const Stroke& s,int channels,double position,float& left,float& right){
    const int count=(int)(s.samples/size_t(channels));
    if(!count){left=right=0.f;return;}
    const int first=std::clamp((int)position,0,count-1);
    const float t=(float)(position-first),t2=t*t,t3=t2*t;
    const float coefficients[4]={
        .5f*(-t+2.f*t2-t3),
        .5f*(2.f-5.f*t2+3.f*t3),
        .5f*(t+4.f*t2-3.f*t3),
        .5f*(-t2+t3)
    };
    int frames[4]={first-1,first,first+1,first+2};
    const bool loop=s.loopEnd>s.loopStart;
    for(int& frame:frames){
        if(loop&&frame>=(int)s.loopEnd)
            frame=(int)s.loopStart+(frame-(int)s.loopEnd)%int(s.loopEnd-s.loopStart);
        frame=std::clamp(frame,0,count-1);
    }
    float l=0.f,r=0.f;
    for(int i=0;i<4;++i){
        const size_t offset=size_t(frames[i])*size_t(channels);
        l+=coefficients[i]*float(s.data[offset]);
        r+=coefficients[i]*float(s.data[offset+size_t(channels>1)]);
    }
    left=l/32768.f;right=r/32768.f;
}
}

bool loadDecoded(const std::string& path,size_t budget,Instrument& out,std::string& error);

namespace {
std::mutex infoMutex;std::string infoText;
void setInfo(const std::string& s){std::lock_guard<std::mutex> l(infoMutex);infoText=s;}
constexpr uint32_t cacheMagic=0x43504643u,cacheVersion=1,cacheEnd=0x21444e45u;
struct Mapping{void* base=nullptr;size_t size=0;~Mapping(){if(base&&base!=MAP_FAILED)munmap(base,size);}};
std::string cachePathFor(const std::string& path){return path+".cpfc";}
std::string cacheKey(const std::string& path){
    struct stat st{};if(stat(path.c_str(),&st)!=0)return {};
    char buf[96];std::snprintf(buf,sizeof buf,"|%lld|%lld|%d",(long long)st.st_size,(long long)st.st_mtime,(int)cacheVersion);
    return path+buf;
}
struct Reader{const uint8_t* p;size_t n,at=0;
    bool u32(uint32_t& v){if(at+4>n)return false;std::memcpy(&v,p+at,4);at+=4;return true;}
    bool u64(uint64_t& v){if(at+8>n)return false;std::memcpy(&v,p+at,8);at+=8;return true;}
    bool u8(uint8_t& v){if(at+1>n)return false;v=p[at++];return true;}
    bool bytes(void* dst,size_t len){if(len>n-at)return false;std::memcpy(dst,p+at,len);at+=len;return true;}
    bool str(std::string& s){uint32_t len;if(!u32(len)||len>n-at)return false;s.assign((const char*)p+at,len);at+=len;return true;}
};
bool loadCache(const std::string& path,size_t budget,Instrument& out){
    const std::string key=cacheKey(path);if(key.empty())return false;
    int fd=open(cachePathFor(path).c_str(),O_RDONLY);if(fd<0)return false;
    struct stat st{};if(fstat(fd,&st)!=0||st.st_size<64){close(fd);return false;}
    auto map=std::make_shared<Mapping>();map->size=(size_t)st.st_size;
    map->base=mmap(nullptr,map->size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);
    if(map->base==MAP_FAILED){map->base=nullptr;return false;}
    Reader r{(const uint8_t*)map->base,map->size};uint32_t magic,version,count;std::string storedKey;
    if(!r.u32(magic)||magic!=cacheMagic||!r.u32(version)||version!=cacheVersion||!r.str(storedKey)||storedKey!=key)return false;
    Instrument result;uint32_t channels,velocityScale;uint32_t gain;uint8_t signedTrim;
    if(!r.str(result.name)||!r.u32(channels)||!r.u32(gain)||!r.u32(velocityScale)||!r.u8(signedTrim))return false;
    if(channels<1||channels>2)return false;
    result.channels=(int)channels;result.gainTenthsDb=(int)(int32_t)gain;result.velocityScale=(int)velocityScale;result.signedTrim=signedTrim!=0;
    if(!r.bytes(result.keyMap.data(),128)||!r.bytes(result.fineTune.data(),128)||!r.u32(count)||count>100000)return false;
    result.strokes.reserve(count);uint64_t total=0;
    for(uint32_t i=0;i<count;++i){
        Stroke s;uint8_t pad;uint16_t trim;uint32_t ls,le;uint64_t samples;
        if(!r.u8(s.root)||!r.u8(s.bank)||!r.u8(s.velocityLayer)||!r.u8(pad))return false;
        if(r.at+2>r.n)return false;std::memcpy(&trim,r.p+r.at,2);r.at+=2;
        if(!r.u32(ls)||!r.u32(le)||!r.u64(samples))return false;
        if(r.at&1)++r.at;
        if(samples>(r.n-r.at)/2||samples%channels!=0)return false;
        s.trim=trim;s.loopStart=ls;s.loopEnd=le;s.data=(const int16_t*)(r.p+r.at);s.samples=(size_t)samples;
        r.at+=(size_t)samples*2;total+=samples*2;result.strokes.push_back(std::move(s));
    }
    uint32_t end;if(!r.u32(end)||end!=cacheEnd||total>budget)return false;
    // Touch every page here, on the import worker: a first read that faults from
    // flash inside the audio callback would stall it and overload the stream.
    madvise(map->base,map->size,MADV_WILLNEED);
    {volatile uint8_t sink=0;uint8_t acc=sink;for(size_t o=0;o<map->size;o+=4096)acc^=((const uint8_t*)map->base)[o];sink=acc;}
    result.backing=map;result.fromCache=true;out=std::move(result);return true;
}
void storeCache(const std::string& path,const Instrument& in){
    const std::string key=cacheKey(path);if(key.empty())return;
    const std::string finalPath=cachePathFor(path),tmp=finalPath+".tmp";
    // The cache holds uncompressed PCM (about 3x the .npno): only write it when
    // the volume keeps at least 512 MB free afterwards.
    uint64_t need=0;for(const auto& s:in.strokes)need+=uint64_t(s.samples)*2;
    struct statvfs vfs{};
    if(statvfs(path.c_str(),&vfs)!=0||uint64_t(vfs.f_bavail)*vfs.f_frsize<need+(uint64_t(512)<<20))return;
    FILE* f=std::fopen(tmp.c_str(),"wb");if(!f)return;bool ok=true;size_t at=0;
    auto put=[&](const void* d,size_t n){if(ok&&std::fwrite(d,1,n,f)!=n)ok=false;at+=n;};
    auto u32=[&](uint32_t v){put(&v,4);};
    auto str=[&](const std::string& s){u32((uint32_t)s.size());put(s.data(),s.size());};
    u32(cacheMagic);u32(cacheVersion);str(key);str(in.name);u32((uint32_t)in.channels);u32((uint32_t)(int32_t)in.gainTenthsDb);u32((uint32_t)in.velocityScale);
    uint8_t st=in.signedTrim?1:0;put(&st,1);put(in.keyMap.data(),128);put(in.fineTune.data(),128);u32((uint32_t)in.strokes.size());
    for(const auto& s:in.strokes){
        uint8_t head[4]={s.root,s.bank,s.velocityLayer,0};put(head,4);uint16_t trim=s.trim;put(&trim,2);
        u32(s.loopStart);u32(s.loopEnd);uint64_t samples=s.samples;put(&samples,8);
        if(at&1){uint8_t z=0;put(&z,1);}
        put(s.data,s.samples*2);
    }
    u32(cacheEnd);
    ok=std::fclose(f)==0&&ok;
    if(!ok||std::rename(tmp.c_str(),finalPath.c_str())!=0)std::remove(tmp.c_str());
}
}

bool load(const std::string& path,size_t budget,Instrument& out,std::string& error){
    const auto begin=std::chrono::steady_clock::now();
    auto elapsed=[&]{return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();};
    Instrument cached;
    if(loadCache(path,budget,cached)){measureLevels(cached);cached.loadMs=elapsed();setInfo("cache "+std::to_string((int)cached.loadMs)+" ms");out=std::move(cached);return true;}
    if(!loadDecoded(path,budget,out,error))return false;
    measureLevels(out);
    const double decodeMs=elapsed();storeCache(path,out);out.loadMs=decodeMs;
    setInfo("decodificado "+std::to_string((int)decodeMs)+" ms, cache gravado em "+std::to_string((int)(elapsed()-decodeMs))+" ms");
    return true;
}
std::string lastLoadInfo(){std::lock_guard<std::mutex> l(infoMutex);return infoText;}

bool loadDecoded(const std::string& path,size_t budget,Instrument& out,std::string& error){
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
            if(!decode(body+cursor,bytesForStroke,channels,frames,blocks,rec,s.owned,error))return false;
            s.data=s.owned.data();s.samples=s.owned.size();
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

namespace {
// Reference model, ported from the desktop piano engine. Velocity law in dB below velocity 127.
struct VelocityPoint{int velocity;float db;};
constexpr VelocityPoint velocityTable[18]={{1,-34.3f},{16,-34.3f},{20,-33.4f},{24,-32.5f},{28,-30.8f},{32,-29.6f},
    {40,-25.8f},{48,-21.4f},{56,-18.6f},{64,-14.5f},{72,-12.3f},{80,-9.8f},{88,-8.2f},{96,-6.2f},{104,-4.0f},
    {112,-1.3f},{120,0.0f},{127,0.0f}};
constexpr float dynExponents[4]={1.0f,0.82f,0.65f,0.50f};
constexpr float releaseFloorSeconds=0.55f,releaseScale=0.85f,makeupDb=6.4f,softPedalGain=0.653f,airGain=0.45f;
constexpr double pitchBendSemitones=2.0;
float velocityDb(float velocity01){
    const float v=std::clamp(velocity01*127.f,1.f,127.f);
    for(int i=1;i<18;++i)if(v<=(float)velocityTable[i].velocity){
        const auto& a=velocityTable[i-1];const auto& b=velocityTable[i];
        return a.db+(v-(float)a.velocity)/(float)(b.velocity-a.velocity)*(b.db-a.db);
    }
    return 0.f;
}
float effectiveRelease(const Layer& layer,float seconds){
    float s=seconds;
    if(layer.longRelease)s*=2.f;
    s=std::max(releaseFloorSeconds,releaseScale*s);
    if(layer.softRelease)s=std::min(15.f,std::max(.18f,s*1.5f));
    return std::clamp(s,.005f,15.0f);
}
float strokeGainDb(const Instrument& piano,const Stroke& s){
    int trim=(int)s.trim;if(trim>=0x8000)trim-=0x10000;
    const float trimDb=piano.signedTrim?-trim/10.f:-(float)s.trim;
    return piano.gainTenthsDb/10.f+trimDb;
}
int findStroke(const Instrument& piano,int root,int bank,int limit){
    int chosen=-1,best=-1;
    for(size_t i=0;i<piano.strokes.size();++i){const auto& s=piano.strokes[i];
        if(s.root==root&&s.bank==bank&&s.velocityLayer<=limit&&s.velocityLayer>best){chosen=(int)i;best=s.velocityLayer;}}
    return chosen;
}
}

void measureLevels(Instrument& piano){
    const int channels=std::max(1,piano.channels);
    const size_t window=(size_t)(0.3*sampleRate);
    for(auto& s:piano.strokes){
        const size_t frames=std::min(window,s.samples/(size_t)channels);
        int32_t peak=1;
        for(size_t i=0;i<frames*(size_t)channels;++i)peak=std::max(peak,std::abs((int32_t)s.data[i]));
        s.peakDb=20.f*std::log10((float)peak/32768.f);
    }
}

void resetControllers(Layer& layer){
    layer.pedal.fill(false);layer.sostenutoDown.fill(false);layer.softDown.fill(false);
    layer.bend.fill(0.f);layer.ratioNow.fill(1.f);layer.cc7.fill(1.f);layer.cc11.fill(1.f);layer.gainNow.fill(1.f);
    layer.res=Layer::StringRes{};
}

void prepareResonance(Layer& layer){
    if(layer.resIn.size()<8192){layer.resIn.assign(8192,0.f);layer.resOut.assign(8192,0.f);}
}

void pitchBend(Layer& layer,int channel,int value14){
    if(channel<0||channel>15)return;
    layer.bend[(size_t)channel]=std::clamp((float)(value14-8192)/8192.f,-1.f,1.f);
}

void control(Layer& layer,int channel,int controller,int value){
    if(channel<0||channel>15)return;
    const size_t c=(size_t)channel;
    switch(controller){
        case 66:{
            const bool was=layer.sostenutoDown[c],down=value>=64;
            layer.sostenutoDown[c]=down;
            if(!was&&down){
                // Holds only the keys that are down when the pedal goes down.
                for(auto& v:layer.voices)if(v.active&&v.stroke>=0&&v.channel==channel&&!v.released&&!v.waitingForSustain)v.sostenutoHeld=true;
            }else if(was&&!down){
                for(auto& v:layer.voices)if(v.active&&v.channel==channel&&v.sostenutoHeld){
                    v.sostenutoHeld=false;
                    if(v.waitingForSustain&&!layer.pedal[c]){v.keyDown=false;v.waitingForSustain=false;v.released=true;v.releaseSeconds=effectiveRelease(layer,0.30f);}
                }
            }
            break;
        }
        case 67:layer.softDown[c]=value>=64;break;
        case 7:layer.cc7[c]=(float)value*(float)value/(127.f*127.f);break;
        case 11:layer.cc11[c]=(float)value*(float)value/(127.f*127.f);break;
        default:break;
    }
}

void noteOn(Layer& layer,int note,int velocity,int channel,bool sustain,int voiceBudget,float attackSeconds,int timbreFloor){
    if(!layer.instrument||note<0||note>127||velocity<1)return;
    const auto& piano=*layer.instrument;int root=piano.keyMap[(size_t)note];if(root==0xff)return;
    // Reference model: Dyn raises the normalised velocity to a fixed exponent before layer choice and gain, and
    // the sample layer leans one notch softer (exponent 1.15) so soft/medium notes keep the mellow layer.
    float v01=std::pow(std::clamp(velocity/127.f,0.f,1.f),dynExponents[std::clamp(layer.dynamicCompression,0,3)]);
    const float layerV=std::max(std::pow(v01,1.15f),(float)std::clamp(timbreFloor,1,127)/127.f);
    const int selectVelocity=std::clamp((int)std::lround(layerV*127.f),1,127);
    // The pedal-down bank (bank 1) is a pronounced reflection: only an explicit opt-in (REFLEXOES).
    int bank=(layer.reflections&&sustain)?1:0,limit=((127-selectVelocity)*piano.velocityScale)/127;
    int chosen=findStroke(piano,root,bank,limit);
    if(chosen<0&&bank==1)chosen=findStroke(piano,root,0,limit);
    if(chosen<0)return;
    Voice* target=nullptr;int budget=std::clamp(voiceBudget,1,maxVoices);
    for(int i=0;i<budget;++i)if(!layer.voices[(size_t)i].active){target=&layer.voices[(size_t)i];break;}
    if(!target){target=&layer.voices[0];for(int i=1;i<budget;++i){auto& v=layer.voices[(size_t)i];
        if((v.released&&!target->released)||(v.released==target->released&&v.serial<target->serial))target=&v;}}
    const auto& s=piano.strokes[(size_t)chosen];
    double cents=piano.fineTune[(size_t)note]*.7;
    *target={};target->active=true;target->keyDown=true;target->note=note;target->channel=channel;target->stroke=chosen;
    target->serial=++layer.serial;target->increment=std::pow(2.0,(note-root+cents/100.0)/12.0)*sampleRate/44100.0;
    target->decaySeconds=std::clamp(layer.decaySeconds,3.f,15.f);
    // Loudness follows the fixed table: the loudest layer of this key plays at its natural level and every
    // softer layer is scaled so the file's own layer balance does not matter.
    const int topIndex=findStroke(piano,root,bank,0);
    const Stroke& top=topIndex>=0?piano.strokes[(size_t)topIndex]:s;
    const float levelDb=top.peakDb+strokeGainDb(piano,top)+velocityDb(v01)+makeupDb-s.peakDb;
    target->level=std::pow(10.f,levelDb/20.f);
    if(layer.softDown[(size_t)std::clamp(channel,0,15)])target->level*=softPedalGain;
    {   // Brightness: one-pole low-pass near 2.2-2.9 kHz, darker for louder notes (measured on the reference player).
        static const float toneVel[]{0.f,30.f,64.f,100.f,120.f},toneHz[]{2900.f,2900.f,2300.f,2200.f,2200.f};
        const float v=std::clamp(v01*127.f,0.f,127.f);size_t k=1;
        while(k<4&&v>toneVel[k])++k;
        const float t=(v-toneVel[k-1])/(toneVel[k]-toneVel[k-1]);
        target->toneHz=toneHz[k-1]*std::pow(toneHz[k]/toneHz[k-1],std::clamp(t,0.f,1.f));
    }
    target->attackGain=attackSeconds>0?0.f:1.f;
}
void noteOff(Layer& layer,int note,int channel,bool sustain,float releaseSeconds){
    // MIDI Note Off has no voice identifier. Match the oldest still-held
    // instance of this pitch so repeated notes under sustain do not release
    // voices whose keys are still physically down.
    Voice* target=nullptr;
    for(auto& v:layer.voices)if(v.active&&v.keyDown&&!v.released&&v.note==note&&v.channel==channel){
        if(!target||v.serial<target->serial)target=&v;
    }
    if(!target)return;
    target->keyDown=false;
    if(sustain||target->sostenutoHeld)target->waitingForSustain=true;   // held by the pedal or the sostenuto
    else{
        target->released=true;
        target->waitingForSustain=false;
        target->releaseSeconds=effectiveRelease(layer,releaseSeconds);
    }
}
void pedalUp(Layer& layer,int channel,float releaseSeconds){
    const float smoothRelease=effectiveRelease(layer,releaseSeconds);
    for(auto& v:layer.voices)if(v.active&&v.channel==channel&&v.waitingForSustain&&!v.sostenutoHeld){
        v.keyDown=false;v.waitingForSustain=false;v.released=true;v.releaseSeconds=smoothRelease;
    }
}
namespace {
// Sympathetic string resonance (STRING RES), ported from the desktop piano engine: without the pedal the played
// note gains narrow resonances at and around its own pitch (detuned unison strings); with the pedal down every free
// string rings, adding a low-frequency halo that follows the pedal and stops when it is released.
void processStringResonance(Layer& layer,float* stereo,int frames,int outputRate){
    auto& res=layer.res;
    if(!layer.stringRes&&!res.active)return;
    if(frames<=0||frames>(int)layer.resIn.size())return;
    bool pedal=false;for(bool down:layer.pedal)pedal=pedal||down;
    std::array<bool,128> sounding{};
    for(const auto& v:layer.voices)if(v.active&&v.stroke>=0&&!v.released)sounding[(size_t)std::clamp(v.note,0,127)]=true;
    float* input=layer.resIn.data();float* output=layer.resOut.data();
    for(int i=0;i<frames;++i){input[i]=0.5f*(stereo[i*2]+stereo[i*2+1]);output[i]=0.f;}
    const float riseFollow=1.f-std::exp(-(float)frames/(float)(0.35*outputRate));
    const float fallFollow=1.f-std::exp(-(float)frames/(float)(0.015*outputRate));
    static constexpr double ratios[4]={1.0,0.975,1.0,1.025};
    bool anyActive=false;
    for(int k=0;k<Layer::stringResCount;++k){
        const int note=21+k;
        const double frequency=440.0*std::pow(2.0,(double)(note-69)/12.0);
        if(frequency>outputRate*0.45)continue;
        const float tiltGain=std::pow(10.f,std::min(11.f,4.3f-(float)(k-21))/20.f);
        for(int j=0;j<4;++j){
            float target=0.f;
            if(layer.stringRes){if(j==0)target=pedal?1.f:0.f;else if(sounding[(size_t)note])target=1.f;}
            float& level=res.level[(size_t)k][(size_t)j];
            level+=(target-level)*(target>level?riseFollow:fallFollow);
            float& y1=res.y1[(size_t)k][(size_t)j];float& y2=res.y2[(size_t)k][(size_t)j];
            if(level<1.0e-4f&&target==0.f){level=0.f;y1=0.f;y2=0.f;continue;}
            anyActive=true;
            const double decaySeconds=(j==0?6.0:1.4)-(j==0?3.5:1.0)*(double)k/(Layer::stringResCount-1);
            const double radius=std::exp(-1.0/(decaySeconds*outputRate));
            const double theta=6.283185307179586*frequency*ratios[j]/outputRate;
            const float a1=(float)(-2.0*radius*std::cos(theta)),a2=(float)(radius*radius);
            const float inputGain=(float)(2.0*(1.0-radius)*std::sin(theta));
            const float outputGain=level*tiltGain;
            float s1=y1,s2=y2;
            for(int i=0;i<frames;++i){
                const float value=input[i]*inputGain-a1*s1-a2*s2;
                s2=s1;s1=value;output[i]+=value*outputGain;
            }
            y1=s1;y2=s2;
        }
    }
    for(int i=0;i<frames;++i){stereo[i*2]+=output[i];stereo[i*2+1]+=output[i];}
    res.active=anyActive;
}
}

void render(Layer& layer,float* stereo,int frames,int outputRate,int budget,float attackSeconds,float decayMs,float sustainPercent){
    if(!layer.instrument||!stereo)return;const auto& piano=*layer.instrument;
    const int voices=std::clamp(budget,0,maxVoices);
    const float attackStep=attackSeconds>0?1.f/std::max(1.f,attackSeconds*outputRate):1.f;
    const float decayTarget=std::clamp(sustainPercent*.01f,0.f,1.f);
    const float decayStep=decayMs>0?(1.f-decayTarget)/std::max(1.f,decayMs*.001f*outputRate):0.f;
    // Channel volume/expression (CC7, CC11) and pitch bend move smoothly across the rendered span.
    std::array<float,16> gainStart,gainEnd,ratioStart,ratioEnd;
    bool controlsTrivial=true;
    const float spanSamples=(float)std::max(1,frames);
    const float gainFollow=std::exp(-spanSamples/(0.005f*(float)outputRate)),pitchFollow=std::exp(-spanSamples/(0.012f*(float)outputRate));
    for(size_t c=0;c<16;++c){
        const float gainTarget=layer.cc7[c]*layer.cc11[c];
        const float ratioTarget=layer.bend[c]==0.f?1.f:std::pow(2.f,(float)(pitchBendSemitones/12.0)*layer.bend[c]);
        gainStart[c]=layer.gainNow[c];ratioStart[c]=layer.ratioNow[c];
        gainEnd[c]=gainTarget+(gainStart[c]-gainTarget)*gainFollow;
        ratioEnd[c]=ratioTarget+(ratioStart[c]-ratioTarget)*pitchFollow;
        if(std::abs(gainEnd[c]-gainTarget)<1.0e-5f)gainEnd[c]=gainTarget;
        if(std::abs(ratioEnd[c]-ratioTarget)<1.0e-6f)ratioEnd[c]=ratioTarget;
        layer.gainNow[c]=gainEnd[c];layer.ratioNow[c]=ratioEnd[c];
        if(gainStart[c]!=1.f||gainEnd[c]!=1.f||ratioStart[c]!=1.f||ratioEnd[c]!=1.f)controlsTrivial=false;
    }
    const float airCoefficient=1.f-std::exp(-6.2831853f*9000.f/(float)outputRate);
    for(int vi=0;vi<voices;++vi){auto& v=layer.voices[(size_t)vi];if(!v.active||v.stroke<0)continue;
        const auto& s=piano.strokes[(size_t)v.stroke];int total=(int)(s.samples/size_t(piano.channels));
        const float releaseFactor=std::exp(std::log(.0001f)/std::max(1.f,std::max(.005f,v.releaseSeconds)*outputRate));
        const size_t ci=(size_t)std::clamp(v.channel,0,15);
        const float toneCoefficient=v.toneHz>0.f&&v.toneHz<19000.f?1.f-std::exp(-6.2831853f*v.toneHz/(float)outputRate):1.f;
        for(int f=0;f<frames;++f){
            float channelGain=1.f,channelRatio=1.f;
            if(!controlsTrivial){
                const float fraction=(float)f/spanSamples;
                channelGain=gainStart[ci]+(gainEnd[ci]-gainStart[ci])*fraction;
                channelRatio=ratioStart[ci]+(ratioEnd[ci]-ratioStart[ci])*fraction;
            }
            if(layer.finitePiano&&++v.age>=(uint64_t)(outputRate*v.decaySeconds)){v={};break;}
            if(!layer.finitePiano)++v.age;
            if(s.loopEnd>s.loopStart&&v.position>=s.loopEnd)
                v.position=s.loopStart+std::fmod(v.position-s.loopEnd,double(s.loopEnd-s.loopStart));
            else if(v.position>=total){v={};break;}
            v.attackGain=std::min(1.f,v.attackGain+attackStep);
            if(v.released)v.releaseGain*=releaseFactor;
            if(v.releaseGain<.0001f){v={};break;}
            float remaining=std::max(0.f,1.f-float(v.age)/float(outputRate*v.decaySeconds));
            float decay=layer.finitePiano?remaining*remaining:1.f;
            if(decayStep>0&&!v.released&&v.attackGain>=1.f)v.decayGain=std::max(decayTarget,v.decayGain-decayStep);
            float gain=v.level*v.attackGain*v.releaseGain*decay*v.decayGain*channelGain;
            float left,right;sampleStereo(s,piano.channels,v.position,left,right);
            if(toneCoefficient<1.f){
                v.airLeft+=airCoefficient*(left-v.airLeft);v.airRight+=airCoefficient*(right-v.airRight);
                const float airL=left-v.airLeft,airR=right-v.airRight;
                v.toneLeft+=toneCoefficient*(left-v.toneLeft);v.toneRight+=toneCoefficient*(right-v.toneRight);
                left=v.toneLeft+airGain*airL;right=v.toneRight+airGain*airR;
            }
            stereo[(size_t)f*2]+=left*gain;
            stereo[(size_t)f*2+1]+=right*gain;
            v.position+=v.increment*44100.0/outputRate*(double)channelRatio;
        }
    }
    processStringResonance(layer,stereo,frames,outputRate);
    static constexpr float bands[3][3]={{1.5f,-1.f,-9.f},{-3.f,3.f,-3.f},{-1.f,0.f,6.f}};
    if(layer.eqMode<0)return; // EQ OFF
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
