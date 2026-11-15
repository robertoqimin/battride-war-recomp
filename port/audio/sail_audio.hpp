#pragma once
#include "sail_decoder.hpp"
#include <array>
#include <cstring>
#include <fstream>
#include <cmath>
extern "C" {
void* at3p_open(int channels,int* block_align);
void at3p_close(void*);
int at3p_decode(void*,float*,float*,int*,const uint8_t*,int);
}
namespace port_sail {
struct PamfAudio {
    bool present=false;
    uint8_t stream_id=0;
    unsigned channels=0,rate=48000;
    uint32_t data_offset=0;
    static uint32_t be32(const uint8_t* p) {return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
    bool probe(const std::string& path) {
        std::ifstream f(path,std::ios::binary);if(!f)return false;
        std::array<uint8_t,2048> h{};f.read((char*)h.data(),h.size());
        if(f.gcount()<4 || memcmp(h.data(),"PAMF",4))return true; // ordinary MPEG fixture, no private PAMF track
        if(f.gcount()!=2048 || h[0x6F]!=1 || h[0x81]!=1)return false;
        uint64_t offset=uint64_t(be32(h.data()+8))*2048;
        if(offset<2048 || offset>16*1024*1024)return false;
        data_offset=offset;
        unsigned count=h[0x87];if(!count || 0x88+count*48>h.size())return false;
        for(unsigned i=0;i<count;i++) {
            const uint8_t* s=h.data()+0x88+i*48;
            if(s[0]!=0xDC)continue;
            if(s[4]!=0xBD || s[18]<1 || s[18]>2 || s[19]!=1)return false;
            present=true;stream_id=s[5];channels=s[18];return true;
        }
        return true;
    }
    // MPEG-PS private_stream_1 has a four-byte substream header before the
    // ATRAC-X ATS bytes. AU boundaries can cross PES packet boundaries.
    bool extract(const std::string& path,std::vector<uint8_t>& encoded,const std::atomic<bool>& cancel) const {
        std::ifstream f(path,std::ios::binary);if(!f || !present)return false;
        f.seekg(0,std::ios::end);auto end=f.tellg();if(end<0 || uint64_t(end)<data_offset)return false;
        uint64_t pos=data_offset,size=uint64_t(end);
        std::array<uint8_t,6> header{};
        while(pos+6<=size && !cancel) {
            f.seekg(pos);f.read((char*)header.data(),6);if(!f)return false;
            if(header[0] || header[1] || header[2]!=1)return false;
            if(header[3]==0xB9)break;
            if(header[3]==0xBA) {
                std::array<uint8_t,8> pack{};f.read((char*)pack.data(),8);if(!f)return false;
                pos+=14+(pack[7]&7);continue;
            }
            size_t length=uint16_t(header[4])<<8|header[5];
            if(!length || pos+6+length>size)return false;
            if(header[3]==0xBD) {
                if(length<7)return false;
                std::vector<uint8_t> pes(length);f.read((char*)pes.data(),length);if(!f)return false;
                size_t start=3+pes[2];
                if(start+4>pes.size())return false;
                if(pes[start]==stream_id) {
                    start+=4;
                    if(encoded.size()+pes.size()-start>32*1024*1024)return false;
                    encoded.insert(encoded.end(),pes.begin()+start,pes.end());
                }
            }
            pos+=6+length;
        }
        return !cancel && !encoded.empty();
    }
};
struct PcmFrame { std::vector<float> data; uint64_t first_sample=0; };
class AudioDecoder {
    std::atomic<bool> cancelled{false};
    std::thread worker;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<PcmFrame> frames;
    size_t consumed=0;
    bool ended=false;
    int result=0;
public:
    ~AudioDecoder(){stop();}
    void stop(){cancelled=true;changed.notify_all();if(worker.joinable())worker.join();}
    void start(std::string path,PamfAudio meta) {
        worker=std::thread([this,path=std::move(path),meta] {
            std::vector<uint8_t> es;
            int error=0;void* decoder=nullptr;uint64_t total=0;
            if(!meta.extract(path,es,cancelled))error=-1;
            for(size_t pos=0;!error && !cancelled && pos<es.size();) {
                if(pos+8>es.size() || es[pos]!=0x0F || es[pos+1]!=0xD0){error=-2;break;}
                size_t bytes=((((es[pos+2]&3)<<8)|es[pos+3])+1)*8;
                if(bytes>4096 || pos+8+bytes>es.size()){error=-3;break;}
                if(!decoder){int align=bytes;decoder=at3p_open(meta.channels,&align);if(!decoder){error=-4;break;}}
                std::vector<uint8_t> padded(bytes+64,0);memcpy(padded.data(),es.data()+pos+8,bytes);
                std::array<float,2048> left{},right{};int samples=0;
                int used=at3p_decode(decoder,left.data(),right.data(),&samples,padded.data(),bytes);
                if(used<0 || samples<1 || samples>2048){error=-5;break;}
                PcmFrame pcm;pcm.first_sample=total;pcm.data.resize(samples*meta.channels);
                for(int i=0;i<samples;i++) {
                    if(!std::isfinite(left[i]) || (meta.channels==2&&!std::isfinite(right[i]))){error=-6;break;}
                    pcm.data[i*meta.channels]=left[i];if(meta.channels==2)pcm.data[i*2+1]=right[i];
                }
                if(error)break;
                total+=samples;pos+=8+bytes;
                std::unique_lock lock(mutex);
                changed.wait(lock,[this]{return cancelled || frames.size()<16;});
                if(cancelled)break;
                frames.push_back(std::move(pcm));
            }
            if(decoder)at3p_close(decoder);
            std::lock_guard lock(mutex);result=error;ended=true;
        });
    }
    unsigned read(unsigned wanted,unsigned channels,std::vector<float>& out) {
        out.clear();
        if(!channels || !wanted)return 0;
        std::lock_guard lock(mutex);
        size_t available=0;for(const auto& frame:frames)available+=frame.data.size()/channels;
        available-=consumed;
        if(available<wanted && !ended)return 0;
        wanted=std::min<size_t>(wanted,available);out.reserve(wanted*channels);
        unsigned copied=0;
        while(copied<wanted) {
            auto& frame=frames.front();unsigned n=std::min<size_t>(wanted-copied,frame.data.size()/channels-consumed);
            out.insert(out.end(),frame.data.begin()+consumed*channels,frame.data.begin()+(consumed+n)*channels);
            copied+=n;consumed+=n;
            if(consumed==frame.data.size()/channels){frames.pop_front();consumed=0;changed.notify_all();}
        }
        return copied;
    }
    bool done(int& code) {std::lock_guard lock(mutex);code=result;return ended&&frames.empty();}
};
}
