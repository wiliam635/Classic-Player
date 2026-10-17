// V4 stroke decoding adapted from nord-format 0.8.0, John Moore, BSD-3-Clause.
// See THIRD_PARTY_NORD_SAMPLE_LICENSE.txt. V4 is specimen-derived, not hardware-validated.
#include "NordSampleDecoder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

namespace {
using Byte = std::uint8_t;
std::uint32_t be(const Byte* p) { return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3]; }
std::uint32_t le(const Byte* p) { return (std::uint32_t(p[3])<<24)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[1])<<8)|p[0]; }
int u16(const Byte* p) { return int(p[0])*256+p[1]; }
int u24(const Byte* p) { return int(p[0])*65536+int(p[1])*256+p[2]; }
int s24(const Byte* p) { const auto v=u24(p); return v&0x800000 ? v-0x1000000 : v; }
float f32(const Byte* p) { auto bits=be(p); float v; std::memcpy(&v,&bits,4); return v; }
void require(bool b, const char* s) { if(!b) throw std::runtime_error(s); }
std::uint32_t crc(const Byte* p,size_t n) {
    std::uint32_t v=0xffffffffu;
    for(size_t i=0;i<n;++i) { v^=p[i]; for(int j=0;j<8;++j)v=(v>>1)^((v&1)?0xedb88320u:0); }
    return v^0xffffffffu;
}
struct Section { const Byte* data=nullptr; size_t length=0, at=0; std::uint32_t version=0; };
bool terminal(std::uint32_t raw) { return raw==0x800020 || raw==0x800040; }
void stroke(const Section& s, NordSampleDecoder::Zone& z) {
    require(s.length>=72 && (s.length-68)%4==0,"NSMP4 stroke truncado.");
    const auto* stream=s.data+68; const auto words=(s.length-68)/4;
    const auto base=((s.at+68)/4)%65536;
    const auto pointer=[&](int offset) { return (size_t(u16(s.data+offset))+65536-base)%65536; };
    auto first=pointer(20), last=pointer(47);
    while(last+65536<words)last+=65536;
    require(first<words && last<words && terminal(be(stream+last*4)),"Diretorio NSMP4 invalido.");
    z.channels=be(stream+last*4)==0x800040 ? 2 : 1;
    const auto peak=std::max(1,std::abs(s24(s.data+13)));
    int bits=0;for(auto p=peak;p;p>>=1)++bits;
    const int shift=int(s.data[12])+bits-22-((peak&(peak-1))==0 ? 1 : 0);
    require(shift>=-32 && shift<=32,"Escala NSMP4 invalida.");
    const auto db=f32(s.data+57);
    require(!std::isnan(db) && db<=48,"Ganho NSMP4 invalido.");
    z.gain=std::isinf(db)&&db<0 ? 0 : std::pow(10.0f,db/20.0f);
    std::int64_t history[2][4] {};
    const int coefficients[5][4]={{0,0,0,0},{-1,0,0,0},{-2,1,0,0},{-3,3,-1,0},{-4,6,-4,1}};
    const auto marked=pointer(38);
    auto i=first; bool hasLoop=false; size_t clipped=0;
    while(i<last) {
        const auto raw=be(stream+i*4); const bool direct=(raw>>23)&1;
        const int width=int((raw>>19)&15)+1, order=direct?0:int((raw>>14)&7);
        const size_t count=raw&0x3fff, per=count/size_t(z.channels);
        require(!(raw>>24) && !(raw&0x20000) && order<=4 && count>0
                && count%size_t(z.channels)==0 && (direct || count%size_t(32*z.channels)==0),"Registro NSMP4 nao suportado.");
        const auto channelWords=(per*size_t(width)+31)/32;
        const auto span=z.channels==2 ? 1+2*channelWords : 1+(count*size_t(width)+31)/32;
        require(span<=last-i && z.pcm.size()+count<=35002u*120u*2u,"Registro NSMP4 fora dos limites.");
        if(i==marked || (raw&0x40000)) { z.loopStart=z.frames();hasLoop=true; }
        const auto before=z.pcm.size();z.pcm.resize(before+count);
        for(int ch=0;ch<z.channels;++ch) for(size_t k=0;k<per;++k) {
            std::uint32_t v=0;
            const auto start=k*size_t(width);
            for(int b=0;b<width;++b) {
                const auto bit=start+size_t(b);
                const auto word=i+1+(z.channels==2 ? (bit/32)*2+size_t(ch) : bit/32);
                const auto byte=word*4+(bit%32)/8;
                v=(v<<1)|((stream[byte]>>(7-bit%8))&1);
            }
            std::int64_t value=(v&(1u<<(width-1))) ? std::int64_t(v)-(1ll<<width) : v;
            for(int j=0;j<order;++j)value-=coefficients[order][j]*history[ch][j];
            require(value>-(1ll<<40) && value<(1ll<<40),"Preditor NSMP4 divergente.");
            for(int j=3;j>0;--j)history[ch][j]=history[ch][j-1];history[ch][0]=value;
            const double scaled=std::floor(std::ldexp(double(value),shift));
            if(scaled < -32768 || scaled > 32767)++clipped;
            z.pcm[before+k*size_t(z.channels)+size_t(ch)]=float(std::clamp(scaled,-32768.0,32767.0)/32768.0);
        }
        i+=span;while(i<last && be(stream+i*4)==0)++i;
    }
    require(i==last && !z.pcm.empty(),"Fim do stroke NSMP4 invalido.");
    // The vendor codec's quantized lattice can overshoot the i16 source peaks;
    // nord-format saturates these explicitly too. Reject gross divergence, not
    // occasional full-scale samples (0.18% in the fixture's highest stroke).
    require(clipped*20<=z.pcm.size(),"NSMP4 com saturacao excessiva; reproducao recusada.");
    z.clippedSamples=clipped;
    if(hasLoop && z.loopStart+8<z.frames())z.loopEnd=z.frames();
}
}
juce::Result NordSampleDecoder::decode(const juce::File& file, Instrument& output) {
    output={};
    try {
        require(file.existsAsFile() && file.getSize()>=100 && file.getSize()<=256ll*1024*1024,"Arquivo NSMP4 invalido ou grande demais.");
        juce::MemoryBlock bytes;require(file.loadFileAsData(bytes),"Falha ao ler NSMP4.");
        const auto* b=static_cast<const Byte*>(bytes.getData());const auto n=bytes.getSize()-44;
        require(!std::memcmp(b,"CBIN",4) && le(b+4)==1 && !std::memcmp(b+8,"nsmp",4)
                && le(b+20)>=400 && le(b+20)<500,"Geracao NSMP4 nao suportada.");
        b+=44;require(crc(b,n)==le(b-20),"Checksum NSMP4 invalido.");
        Section map,header;std::map<std::uint32_t,Section> strokes;size_t at=0;
        while(at<n) {
            require(n-at>=12,"Secao NSMP4 truncada.");const auto length=size_t(be(b+at+8));
            require(length<=n-at-12,"Tamanho de secao NSMP4 invalido.");
            const Section s{b+at+12,length,at+12,be(b+at+4)};
            if(at==0)require(!std::memcmp(b,"NSMP",4),"Corpo NSMP4 invalido.");
            if(!std::memcmp(b+at,"\0map",4))map=s;
            if(!std::memcmp(b+at,"\0hdr",4))header=s;
            if(!std::memcmp(b+at,"\0stk",4)) { require(length>=68,"Stroke NSMP4 curto.");require(strokes.emplace(be(s.data),s).second,"ID de stroke NSMP4 duplicado."); }
            at+=12+length;
        }
        require(map.data && header.data && header.length>=10 && map.version==21 && map.length>=1318,"Mapa NSMP4 nao suportado.");
        const auto count=size_t(map.data[1317]);require(count>0 && count<=128 && 1318+count*16<=map.length,"Zonas NSMP4 invalidas.");
        require(header.length>=76,"Nome NSMP4 truncado.");
        Instrument parsed;parsed.name=juce::String::fromUTF8(reinterpret_cast<const char*>(header.data+10),66);
        const auto global=float(u24(map.data))/1048576.0f;
        for(int note=0;note<128;++note) {
            const auto* p=map.data+6+size_t(note)*10;
            parsed.gain[size_t(note)]=global*float(u24(p))/1048576.0f;
            parsed.detune[size_t(note)]=float(s24(p+3))/256.0f;
        }
        size_t total=0;
        for(size_t k=0;k<count;++k) {
            const auto* r=map.data+1318+k*16;Zone z;
            z.root=r[0];z.high=r[1];z.low=r[2];z.velocityLow=r[14];z.velocityHigh=r[15];
            require(z.root<=127 && z.low<=z.high && z.high<=127 && z.velocityLow<=z.velocityHigh && z.velocityHigh<=127,"Limites de zona NSMP4 invalidos.");
            const auto found=strokes.find(be(r+8));require(found!=strokes.end() && found->second.data[5]==z.root,"Stroke da zona NSMP4 nao encontrado.");
            stroke(found->second,z);total+=z.pcm.size();require(total<=128u*1024u*1024u,"NSMP4 excede 512 MiB de PCM.");parsed.zones.push_back(std::move(z));
        }
        output=std::move(parsed);return juce::Result::ok();
    } catch(const std::exception& e) { return juce::Result::fail(juce::String::fromUTF8(e.what())); }
}
