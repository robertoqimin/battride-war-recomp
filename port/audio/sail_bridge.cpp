// Port-local cellSail implementation. ABI layouts/signatures were checked
// against RPCS3's Emu/Cell/Modules/cellSail.h (2026-10-21).
// Decode stays on a host worker; every guest callback runs on the frame ticker.
#include "ppu_recomp.h"
#include "sail_decoder.hpp"
#include "sail_audio.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <set>

extern "C" {
extern uint8_t* vm_base;
extern const char* ppu_vfs_root;
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*));
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);
void* sys_heap_malloc(uint32_t, uint32_t);
int32_t sys_heap_free(uint32_t, void*);
void ppu_dump_guest_stack(ppu_context*,const char*) __attribute__((weak));
}
namespace port_sail {
using Clock = std::chrono::steady_clock;
constexpr uint32_t InvalidArg=0x80610701, InvalidState=0x80610702,
    Unsupported=0x80610703, Empty=0x80610705, Using=0x80610707, Memory=0x806107F0;
static bool valid(uint32_t ea, size_t size) { return ea && uint64_t(ea)+size <= 0xE0000000ull; }
static uint32_t r32(uint32_t ea) { uint32_t v; memcpy(&v,vm_base+ea,4); return __builtin_bswap32(v); }
static void w32(uint32_t ea,uint32_t v) { v=__builtin_bswap32(v); memcpy(vm_base+ea,&v,4); }
static void w64(uint32_t ea,uint64_t v) { v=__builtin_bswap64(v); memcpy(vm_base+ea,&v,8); }
static void w16(uint32_t ea,uint16_t v) { v=__builtin_bswap16(v); memcpy(vm_base+ea,&v,2); }
static uint32_t alloc(size_t n) { return uint32_t(uintptr_t(sys_heap_malloc(0,uint32_t(n)))); }
static void release(uint32_t ea) { if(ea) sys_heap_free(0,(void*)(uintptr_t)ea); }
static uint64_t call(uint32_t opd,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0) {
    return opd ? ppu_guest_call(opd,a,b,c,d,0,0,0,0) : 0;
}
struct Adapter {
    uint32_t self=0,arg=0,format_ea=0,table_ea=0;
    std::array<uint32_t,5> callbacks{};
    std::array<uint8_t,32> format{};
    bool video=false,registered=false,madeup=false,guest_buffers=false;
    uint32_t owner=0,pitch=0,row_bytes=0,frame_size=0;
    std::vector<uint32_t> buffers;
    unsigned current=0;
    std::array<uint8_t,24> previous{};
};
struct Descriptor { uint32_t owner=0; std::string uri,path; Video video; };
struct Player {
    uint32_t self=0,callback=0,arg=0,graphics=0,sound=0,selected=0,session=0;
    int state=0,repeat=0;
    bool pending=false,paused=false,first_frame=false,source_ended=false;
    std::set<uint32_t> descriptors;
    std::map<uint32_t,std::pair<uint64_t,uint64_t>> parameters;
    std::unique_ptr<Decoder> decoder;
    std::unique_ptr<AudioDecoder> audio_decoder;
    PamfAudio audio;
    uint64_t audio_delivered=0;
    bool video_done=false,audio_done=false;
    int video_error=0,audio_error=0;
    Video video;
    std::string path;
    Clock::time_point epoch{},pause_begin{};
    uint64_t delivered=0;
    Clock::time_point trace_at{};
};
struct Command { std::shared_ptr<Player> p; uint32_t op; uint64_t token; };
static std::recursive_mutex guard;
static std::map<uint32_t,std::shared_ptr<Player>> players;
static std::map<uint32_t,Adapter> adapters;
static std::map<uint32_t,Descriptor> descriptors;
static std::deque<Command> commands;
static std::deque<std::pair<std::weak_ptr<Player>,bool>> pause_events;
static std::shared_ptr<Player> player(uint32_t ea) {
    auto it=players.find(ea);return it==players.end()?nullptr:it->second;
}
static Adapter* adapter(uint32_t ea) { auto it=adapters.find(ea);return it==adapters.end()?nullptr:&it->second; }
static void result(ppu_context* c,uint32_t v=0) { c->gpr[3]=uint64_t(int64_t(int32_t(v))); }
static void notify(Player& p,uint32_t major,uint32_t minor=0,uint64_t a=0,uint64_t b=0) {
    fprintf(stderr,"[port-sail] event player=%08X major=%u minor=%u arg=%llX/%llX\n",
        p.self,major,minor,(unsigned long long)a,(unsigned long long)b);
    call(p.callback,p.arg,(uint64_t(major)<<32)|minor,a,b);
}
static void state(Player& p,int s) {p.state=s;notify(p,3,0,s);}
static void complete(Player& p,uint32_t op,uint32_t error,uint64_t token) {
    p.pending=false;notify(p,2,op,uint64_t(int64_t(int32_t(error))),token);
}
static bool path_from_uri(const std::string& uri,std::string& path) {
    constexpr const char* prefix="x-cell-fs:///dev_bdvd/";
    if(uri.rfind(prefix,0)!=0 || !ppu_vfs_root) return false;
    std::filesystem::path relative(uri.substr(strlen(prefix)));
    if(relative.is_absolute()) return false;
    for(const auto& part:relative) if(part=="..") return false;
    std::error_code error;
    auto root=std::filesystem::weakly_canonical(ppu_vfs_root,error);
    if(error)return false;
    auto target=std::filesystem::weakly_canonical(root/relative,error);
    if(error)return false;
    auto rel=target.lexically_relative(root);
    if(rel.empty() || *rel.begin()==".." || !std::filesystem::is_regular_file(target,error) || error)return false;
    path=target.string();return true;
}
static void free_buffers(Adapter& a) {
    if(a.guest_buffers) call(a.callbacks[4],a.arg,a.buffers.size(),a.table_ea);
    else for(auto ea:a.buffers) release(ea);
    a.buffers.clear();a.guest_buffers=false;a.previous.fill(0);
    if(a.table_ea) {release(a.table_ea);a.table_ea=0;}
}
static void copy_format(Adapter& a) {memcpy(vm_base+a.format_ea,a.format.data(),a.video?32:24);}
static bool setup_video(Player& p,Adapter& a) {
    int coding=int8_t(a.format[0]);
    if(coding==-1) coding=0;
    if(coding!=0 && coding!=1 && coding!=2 && coding!=4)return false;
    a.row_bytes=p.video.width*((coding==0 || coding==1)?4:2);
    a.pitch=a.row_bytes;
    uint32_t preferred_pitch;memcpy(&preferred_pitch,a.format.data()+8,4);
    preferred_pitch=__builtin_bswap32(preferred_pitch);
    if(preferred_pitch>=a.pitch && preferred_pitch<=65536)a.pitch=preferred_pitch;
    a.frame_size=a.pitch*p.video.height;
    a.format.fill(0xFF);a.format[0]=coding;a.format[1]=0;a.format[2]=8;
    a.format[3]=3; // PAMF used by this title is 30000/1001. Filled from probe below.
    const double fps=double(p.video.rate_num)/p.video.rate_den;
    const double rates[]={24000./1001,24,25,30000./1001,30,50,60000./1001,60};
    auto nearest=std::min_element(std::begin(rates),std::end(rates),[fps](double x,double y){return std::abs(x-fps)<std::abs(y-fps);});
    a.format[3]=nearest-std::begin(rates);
    copy_format(a);w16(a.format_ea+4,p.video.width);w16(a.format_ea+6,p.video.height);
    w32(a.format_ea+8,a.pitch);w32(a.format_ea+12,255);
    vm_base[a.format_ea+16]=1;vm_base[a.format_ea+17]=1;vm_base[a.format_ea+18]=0;
    memcpy(a.format.data(),vm_base+a.format_ea,32);
    call(a.callbacks[2],a.arg,a.format_ea,p.session);
    free_buffers(a);
    constexpr unsigned count=3;
    a.table_ea=alloc(count*4);
    if(!valid(a.table_ea,count*4))return false;
    memset(vm_base+a.table_ea,0,count*4);
    if(a.callbacks[3]) {
        int32_t error=int32_t(call(a.callbacks[3],a.arg,a.frame_size,count,a.table_ea));
        if(error<0) {release(a.table_ea);a.table_ea=0;return false;}
        a.guest_buffers=true;
    } else {
        for(unsigned i=0;i<count;i++)w32(a.table_ea+i*4,alloc(a.frame_size));
    }
    for(unsigned i=0;i<count;i++)a.buffers.push_back(r32(a.table_ea+i*4));
    for(auto ea:a.buffers)if(!valid(ea,a.frame_size)) {free_buffers(a);return false;}
    a.current=count-1;
    fprintf(stderr,"[port-sail] video %dx%d fps=%d/%d coding=%d pitch=%u buffers=%08X,%08X,%08X\n",
        p.video.width,p.video.height,p.video.rate_num,p.video.rate_den,coding,a.pitch,a.buffers[0],a.buffers[1],a.buffers[2]);
    return true;
}
static bool setup_audio(Player& p,Adapter& a) {
    free_buffers(a);a.format.fill(0xFF);a.format[0]=1;a.format[1]=p.audio.channels;
    copy_format(a);w16(a.format_ea+2,256);w32(a.format_ea+4,p.audio.rate);w32(a.format_ea+8,p.audio.channels==2?2:1);
    memcpy(a.format.data(),vm_base+a.format_ea,24);
    call(a.callbacks[2],a.arg,a.format_ea,p.session);
    constexpr unsigned count=8;
    for(unsigned i=0;i<count;i++) {
        auto ea=alloc(2048*p.audio.channels*sizeof(float));a.buffers.push_back(ea);
        if(!valid(ea,2048*p.audio.channels*sizeof(float))){free_buffers(a);return false;}
        memset(vm_base+ea,0,2048*p.audio.channels*sizeof(float));
    }
    a.current=count-1;
    fprintf(stderr,"[port-sail] audio ATRAC3plus channels=%u rate=%u PCM=float32 BE\n",p.audio.channels,p.audio.rate);
    return true;
}
static void init_player(ppu_context* c) {
    std::lock_guard lock(guard);uint32_t ea=c->gpr[3];
    if(!valid(ea,256)||!valid(c->gpr[5],8))return result(c,InvalidArg);
    if(player(ea))return result(c,InvalidState);
    auto p=std::make_shared<Player>();p->self=ea;p->callback=c->gpr[5];p->arg=c->gpr[6];
    memset(vm_base+ea,0,256);players.emplace(ea,p);
    fprintf(stderr,"[port-sail] initialize player=%08X callback=%08X arg=%08X\n",ea,p->callback,p->arg);result(c);
}
template<bool VideoAdapter> static void init_adapter(ppu_context* c) {
    std::lock_guard lock(guard);uint32_t ea=c->gpr[3],table=c->gpr[4];
    if(!valid(ea,64)||!valid(table,VideoAdapter?20:12))return result(c,InvalidArg);
    if(adapter(ea))return result(c,InvalidState);
    Adapter a;a.self=ea;a.arg=c->gpr[5];a.video=VideoAdapter;
    a.format_ea=ea+(VideoAdapter?28:24);a.format.fill(0xFF);
    for(unsigned i=0;i<(VideoAdapter?5:3);i++)a.callbacks[i]=r32(table+i*4);
    memset(vm_base+ea,0,VideoAdapter?64:48);vm_base[ea]=1;
    for(unsigned i=0;i<(VideoAdapter?5:3);i++)w32(ea+4+i*4,a.callbacks[i]);
    w32(ea+(VideoAdapter?24:16),a.arg);
    if(!VideoAdapter) {
        a.format.fill(0);a.format[0]=1;a.format[1]=2;
        uint16_t samples=__builtin_bswap16(256);memcpy(a.format.data()+2,&samples,2);
        uint32_t fs=__builtin_bswap32(48000);memcpy(a.format.data()+4,&fs,4);
    }
    copy_format(a);adapters.emplace(ea,std::move(a));result(c);
}
template<bool IsVideo> static void set_adapter(ppu_context* c) {
    std::lock_guard lock(guard);auto p=player(c->gpr[3]);auto a=adapter(c->gpr[5]);
    if(!p||c->gpr[4]!=0||!a||a->video!=IsVideo)return result(c,InvalidArg);
    if(a->registered)return result(c,InvalidState);
    a->registered=true;a->owner=p->self;vm_base[a->self+1]=1;
    (IsVideo?p->graphics:p->sound)=a->self;result(c);
}
template<bool IsVideo,bool Set> static void format(ppu_context* c) {
    std::lock_guard lock(guard);auto a=adapter(c->gpr[3]);uint32_t out=c->gpr[4];
    if(!a||a->video!=IsVideo||!valid(out,IsVideo?32:24))return result(c,InvalidArg);
    if constexpr(Set) {memcpy(a->format.data(),vm_base+out,IsVideo?32:24);copy_format(*a);}
    else memcpy(vm_base+out,a->format.data(),IsVideo?32:24);
    result(c);
}
static void create_descriptor(ppu_context* c) {
    std::lock_guard lock(guard);auto p=player(c->gpr[3]);uint32_t uri=c->gpr[6],out=c->gpr[7];
    if(!p||c->gpr[4]!=0||!valid(out,4)||!valid(uri,1))return result(c,InvalidArg);
    std::string text;
    for(unsigned i=0;i<1024;i++){if(!valid(uri+i,1))return result(c,InvalidArg);char v=vm_base[uri+i];if(!v)break;text+=v;}
    if(text.size()==1024)return result(c,InvalidArg);
    Descriptor d;d.owner=p->self;d.uri=text;
    if(!path_from_uri(text,d.path))return result(c,InvalidArg);
    uint32_t ea=alloc(256);if(!valid(ea,256))return result(c,Memory);
    memset(vm_base+ea,0,256);w32(ea+4,0);descriptors.emplace(ea,std::move(d));w32(out,ea);
    fprintf(stderr,"[port-sail] descriptor=%08X uri=%s\n",ea,text.c_str());result(c);
}
template<int Op> static void descriptor(ppu_context* c) {
    std::lock_guard lock(guard);auto p=player(c->gpr[3]);uint32_t ea=c->gpr[4];auto it=descriptors.find(ea);
    if(!p||it==descriptors.end()||it->second.owner!=p->self)return result(c,InvalidArg);
    if constexpr(Op==0) {p->descriptors.insert(ea);vm_base[ea+1]=1;if(!p->selected)p->selected=ea;}
    if constexpr(Op==1) {if(p->state!=2)return result(c,InvalidState);p->descriptors.erase(ea);vm_base[ea+1]=0;if(p->selected==ea)p->selected=0;}
    if constexpr(Op==2) {if(p->descriptors.count(ea))return result(c,Using);descriptors.erase(it);release(ea);}
    result(c);
}
static void count_descriptors(ppu_context* c) {std::lock_guard lock(guard);auto p=player(c->gpr[3]);result(c,p?uint32_t(p->descriptors.size()):InvalidArg);}
template<uint32_t Op> static void enqueue(ppu_context* c) {
    std::lock_guard lock(guard);auto p=player(c->gpr[3]);
    if(!p)return result(c,InvalidArg);
    if(p->pending)return result(c,Using);
    if((Op==1 && p->state!=0)||(Op==2 && p->state!=2)||(Op==10 && p->state!=4)||
       (Op==3 && p->state!=4)||(Op==11 && p->state!=6))return result(c,InvalidState);
    // Start takes (self, command*, userParam); all other calls use (self,userParam).
    uint64_t token=Op==10?c->gpr[5]:c->gpr[4];
    if constexpr(Op==10) {
        uint32_t command=c->gpr[4];
        if(command && !valid(command,32))return result(c,InvalidArg);
        if(command && (r32(command+4)!=0 || (r32(command)!=0 && r32(command)!=1)))return result(c,Unsupported);
    }
    p->pending=true;commands.push_back({p,Op,token});result(c);
}
static void set_parameter(ppu_context* c) {
    std::lock_guard lock(guard);auto p=player(c->gpr[3]);
    if(!p||c->gpr[4]>=36)return result(c,InvalidArg);
    p->parameters[c->gpr[4]]={c->gpr[5],c->gpr[6]};result(c);
}
static void repeat(ppu_context* c) {std::lock_guard lock(guard);auto p=player(c->gpr[3]);if(!p)return result(c,InvalidArg);if(c->gpr[4])return result(c,Unsupported);p->repeat=0;result(c);}
template<bool Set> static void paused(ppu_context* c) {
    std::lock_guard lock(guard);auto p=player(c->gpr[3]);if(!p)return result(c,InvalidArg);
    if constexpr(Set) {
        bool value=c->gpr[4]!=0;
        if(value!=p->paused) {
            if(value)p->pause_begin=Clock::now();else p->epoch+=Clock::now()-p->pause_begin;
            p->paused=value;pause_events.push_back({p,value});
        }
        result(c);
    } else result(c,p->paused);
}
static void frame_info(uint32_t out,uint32_t buffer,uint32_t session,uint32_t tag,uint32_t status,uint64_t pts) {
    w32(out,buffer);w32(out+4,session);w32(out+8,tag);w32(out+12,status);w64(out+16,pts);
}
static uint64_t pts_for(Player& p,uint64_t index) {return index*90000ull*p.video.rate_den/p.video.rate_num;}
static double seconds(Player& p) {return std::chrono::duration<double>((p.paused?p.pause_begin:Clock::now())-p.epoch).count();}
static void get_video(ppu_context* c) {
    std::lock_guard lock(guard);auto a=adapter(c->gpr[3]);uint32_t out=c->gpr[4],previous=c->gpr[5],flip=c->gpr[6];
    if(!a||!a->video||!valid(out,24)||(previous&&!valid(previous,24))||(flip&&!valid(flip,8)))return result(c,InvalidArg);
    auto p=player(a->owner);
    memset(vm_base+out,0,24);if(previous)memcpy(vm_base+previous,a->previous.data(),24);
    if(flip)w64(flip,std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
    if(p && p->source_ended && p->state==4) {memcpy(vm_base+out,a->previous.data(),24);return result(c,48);}
    if(!p||p->state!=6||a->buffers.empty())return result(c,1);
    static bool stack_dumped=false;
    if(!stack_dumped && getenv("PORT_SAIL_TRACE") && ppu_dump_guest_stack) {
        stack_dumped=true;ppu_dump_guest_stack(c,"sail-video-caller");
    }
    if(!p->decoder) {memcpy(vm_base+out,a->previous.data(),24);return result(c,48);}
    const double due=p->delivered*double(p->video.rate_den)/p->video.rate_num;
    Frame frame;
    if(p->paused || (p->first_frame && seconds(*p)<due) || !p->decoder->take(frame)) {
        memcpy(vm_base+out,a->previous.data(),24);return result(c,p->paused?36:1);
    }
    if(!p->first_frame) {p->epoch=Clock::now();p->first_frame=true;}
    a->current=(a->current+1)%a->buffers.size();uint32_t buffer=a->buffers[a->current];
    for(int y=0;y<p->video.height;y++)memcpy(vm_base+buffer+y*a->pitch,frame.bytes.data()+y*a->row_bytes,a->row_bytes);
    frame_info(out,buffer,p->session,a->current,0,pts_for(*p,frame.index));
    memcpy(a->previous.data(),vm_base+out,24);p->delivered=frame.index+1;
    if(p->delivered==1 || p->delivered%300==0)fprintf(stderr,"[port-sail] delivered video frame=%llu pts=%llu buffer=%08X\n",
        (unsigned long long)p->delivered,(unsigned long long)pts_for(*p,frame.index),buffer);
    result(c,0x100|4); // one fresh frame plus ON_TIME sync flags
}
static void get_sound(ppu_context* c) {
    std::lock_guard lock(guard);auto a=adapter(c->gpr[3]);uint32_t out=c->gpr[5];
    if(!a||a->video||!valid(out,24)||!c->gpr[4]||c->gpr[4]>2048)return result(c,InvalidArg);
    memset(vm_base+out,0,24);auto p=player(a->owner);unsigned samples=c->gpr[4];
    if(!p || !p->audio.present)return result(c,Empty);
    if(p->source_ended && p->state==4)return result(c,48);
    if(p->state!=6 || !p->audio_decoder || !p->first_frame || a->buffers.empty())return result(c,1);
    if(p->paused)return result(c,36);
    if(seconds(*p)+0.05<double(p->audio_delivered)/p->audio.rate)return result(c,12);
    std::vector<float> pcm;unsigned copied=p->audio_decoder->read(samples,p->audio.channels,pcm);
    if(!copied) return result(c,p->audio_done?48:1);
    a->current=(a->current+1)%a->buffers.size();uint32_t buffer=a->buffers[a->current];
    memset(vm_base+buffer,0,samples*p->audio.channels*sizeof(float));
    double energy=0;
    for(size_t i=0;i<pcm.size();i++) {uint32_t bits;memcpy(&bits,&pcm[i],4);w32(buffer+i*4,bits);energy+=double(pcm[i])*pcm[i];}
    uint64_t pts=p->audio_delivered*90000ull/p->audio.rate;
    frame_info(out,buffer,p->session,a->current,0,pts);memcpy(a->previous.data(),vm_base+out,24);
    p->audio_delivered+=copied;
    if(p->audio_delivered==copied || p->audio_delivered%(48000*5)<copied)
        fprintf(stderr,"[port-sail] delivered audio samples=%llu rms=%.5f buffer=%08X\n",(unsigned long long)p->audio_delivered,sqrt(energy/pcm.size()),buffer);
    result(c,(copied<<8)|4);
}
template<bool VideoAdapter> static void sync(ppu_context* c) {
    std::lock_guard lock(guard);auto a=adapter(c->gpr[3]);result(c,a&&a->video==VideoAdapter?0:InvalidArg);
}
static void pts_to_time(ppu_context* c) {
    std::lock_guard lock(guard);auto a=adapter(c->gpr[3]);uint32_t out=c->gpr[5];
    if(!a||!valid(out,8))return result(c,InvalidArg);
    w64(out,c->gpr[4]*1000000ull/90000);result(c);
}
static void finalize_adapter(ppu_context* c) {
    std::lock_guard lock(guard);auto a=adapter(c->gpr[3]);if(!a)return result(c,InvalidArg);
    if(a->registered)return result(c,InvalidState);
    free_buffers(*a);vm_base[a->self]=0;adapters.erase(a->self);result(c);
}
static void finalize_player(ppu_context* c) {
    std::lock_guard lock(guard);auto p=player(c->gpr[3]);if(!p)return result(c,InvalidArg);
    if(p->pending||p->state==6)return result(c,InvalidState);
    p->decoder.reset();p->audio_decoder.reset();
    for(auto ea:{p->graphics,p->sound})if(auto a=adapter(ea)) {
        free_buffers(*a);if(a->madeup)call(a->callbacks[1],a->arg);
        a->registered=false;a->owner=0;a->madeup=false;vm_base[ea+1]=0;
    }
    for(auto it=descriptors.begin();it!=descriptors.end();) {
        if(it->second.owner==p->self){release(it->first);it=descriptors.erase(it);}else ++it;
    }
    players.erase(p->self);result(c);
}
static void execute(Command& command) {
    auto& p=*command.p;uint32_t op=command.op,error=0;
    if(op==1) {
        state(p,1);
        for(auto ea:{p.sound,p.graphics})if(auto a=adapter(ea)) {
            int32_t v=call(a->callbacks[0],a->arg);
            if(v<0){error=uint32_t(v);break;}a->madeup=true;
        }
        state(p,error?0:2);complete(p,op,error,command.token);
    } else if(op==2) {
        state(p,3);auto it=descriptors.find(p.selected);
        if(it==descriptors.end())error=InvalidArg;
        else if(!it->second.video.probe(it->second.path))error=Unsupported;
        else {
            p.path=it->second.path;p.video=it->second.video;p.session++;
            // Diagnostic fixture injection: exercise the real guest's movie
            // teardown against a short file without changing game assets.
            if(const char* fixture=getenv("PORT_SAIL_TEST_MOVIE")) {
                p.path=fixture;
                if(!p.video.probe(p.path))error=Unsupported;
                fprintf(stderr,"[port-sail] TEST movie override: %s\n",fixture);
            }
            p.audio=PamfAudio{};
            if(!p.audio.probe(p.path))error=Unsupported;
            auto a=adapter(p.graphics);
            if(!error && (!a || !setup_video(p,*a)))error=Unsupported;
            if(!error && p.audio.present) {
                auto sound=adapter(p.sound);
                if(!sound || !setup_audio(p,*sound))error=Unsupported;
            }
            if(!error){notify(p,4,0,p.selected);notify(p,9,1,0);if(p.audio.present)notify(p,9,0,0);}
        }
        state(p,error?2:4);complete(p,op,error,command.token);
    } else if(op==10) {
        state(p,5);auto a=adapter(p.graphics);
        const char* pixel=a&&a->format[0]==1?"rgba":a&&a->format[0]==2?"uyvy422":a&&a->format[0]==4?"yuyv422":"argb";
        p.decoder=std::make_unique<Decoder>();
        if(!a||!p.decoder->start(p.path,pixel,a->row_bytes*p.video.height))error=Unsupported;
        if(error)p.decoder.reset();
        p.audio_decoder.reset();p.audio_delivered=0;p.audio_done=!p.audio.present;p.video_done=false;p.video_error=0;p.audio_error=0;
        if(!error && p.audio.present){p.audio_decoder=std::make_unique<AudioDecoder>();p.audio_decoder->start(p.path,p.audio);}
        p.delivered=0;p.first_frame=false;p.paused=false;p.source_ended=false;p.epoch=Clock::now();
        if(!error)notify(p,6,0,p.session);
        state(p,error?4:6);complete(p,op,error,command.token);
    } else if(op==11) {
        state(p,7);p.decoder.reset();p.audio_decoder.reset();state(p,4);complete(p,op,0,command.token);
    } else if(op==3) {
        state(p,8);p.decoder.reset();p.audio_decoder.reset();if(auto a=adapter(p.graphics))free_buffers(*a);
        if(auto a=adapter(p.sound))free_buffers(*a);
        notify(p,10,1,0);if(p.audio.present)notify(p,10,0,0);notify(p,5,0,p.selected);state(p,2);complete(p,op,0,command.token);
    }
}
}
extern "C" void port_sail_poll(void) {
    using namespace port_sail;
    std::lock_guard lock(guard);
    while(!pause_events.empty()) {
        auto event=pause_events.front();pause_events.pop_front();
        if(auto p=event.first.lock();p && player(p->self)==p)notify(*p,7,0,event.second);
    }
    if(!commands.empty()) {auto command=commands.front();commands.pop_front();execute(command);}
    for(auto& [ea,p]:players)if(p->state==6 && !p->paused && !p->source_ended) {
        if(p->decoder && p->decoder->done(p->video_error)) {p->video_done=true;p->decoder.reset();}
        if(p->audio_decoder && p->audio_decoder->done(p->audio_error))p->audio_done=true;
        if(p->video_error || p->audio_error) {
            p->decoder.reset();p->audio_decoder.reset();p->video_done=p->audio_done=true;
        }
        double duration=p->delivered*double(p->video.rate_den)/p->video.rate_num;
        if(p->audio.present)duration=std::max(duration,double(p->audio_delivered)/p->audio.rate);
        if(p->video_done && p->audio_done && (!p->first_frame || seconds(*p)>=duration)) {
            fprintf(stderr,"[port-sail] decoder ended frames=%llu audio-samples=%llu status=%d/%d\n",(unsigned long long)p->delivered,(unsigned long long)p->audio_delivered,p->video_error,p->audio_error);
            int code=p->video_error?p->video_error:p->audio_error;
            if(code)notify(*p,1,0,Unsupported);
            else notify(*p,8,0,p->selected);
            // Every decoded frame has now been presented. A normal terminus
            // settles to OPENED; the title's completion path checks both
            // OPENED and its SOURCE_EOS flag. No Stop call was submitted, so
            // do not synthesize a CALL_COMPLETED(Stop) event.
            p->source_ended=!code;p->decoder.reset();p->audio_decoder.reset();state(*p,7);state(*p,4);
        }
    }
    if(getenv("PORT_SAIL_TRACE"))for(auto& [ea,p]:players) {
        if(Clock::now()-p->trace_at<std::chrono::seconds(2))continue;
        p->trace_at=Clock::now();
        if(valid(p->arg,0x7DC))fprintf(stderr,"[port-sail-trace] host=%d pending=%d decoded=%llu guest-state=%08X guest-op=%08X guest-error=%08X eos=%08X\n",
            p->state,p->pending,(unsigned long long)p->delivered,r32(p->arg+0x7C0),r32(p->arg+0x7CC),r32(p->arg+0x7C4),r32(p->arg+0x7D8));
    }
}
extern "C" void port_register_sail(void) {
    using namespace port_sail;
#define SAIL(nid,name,fn) ps3_hle_register_ctx(nid,name,fn)
    SAIL(0x23654375,"cellSailPlayerInitialize2",init_player);
    SAIL(0x18B4629D,"cellSailPlayerFinalize",finalize_player);
    SAIL(0x1C983864,"cellSailGraphicsAdapterInitialize",init_adapter<true>);
    SAIL(0x3D0D3B72,"cellSailSoundAdapterInitialize",init_adapter<false>);
    SAIL(0x18BCD21B,"cellSailPlayerSetGraphicsAdapter",set_adapter<true>);
    SAIL(0x1139A206,"cellSailPlayerSetSoundAdapter",set_adapter<false>);
    SAIL(0x2E3CCB5E,"cellSailGraphicsAdapterSetPreferredFormat",(format<true,true>));
    SAIL(0x1C9D5E5A,"cellSailSoundAdapterSetPreferredFormat",(format<false,true>));
    SAIL(0xFFD58AA4,"cellSailGraphicsAdapterGetFormat",(format<true,false>));
    SAIL(0xF25F197D,"cellSailSoundAdapterGetFormat",(format<false,false>));
    SAIL(0x76488BB1,"cellSailGraphicsAdapterFinalize",finalize_adapter);
    SAIL(0xD1462438,"cellSailSoundAdapterFinalize",finalize_adapter);
    SAIL(0xD7938B8D,"cellSailPlayerCreateDescriptor",create_descriptor);
    SAIL(0x7C8DFF3B,"cellSailPlayerAddDescriptor",descriptor<0>);
    SAIL(0x9897FBD1,"cellSailPlayerRemoveDescriptor",descriptor<1>);
    SAIL(0xFC839BD4,"cellSailPlayerDestroyDescriptor",descriptor<2>);
    SAIL(0x752F8585,"cellSailPlayerGetDescriptorCount",count_descriptors);
    SAIL(0xBDF21B0F,"cellSailPlayerBoot",enqueue<1>);
    SAIL(0x34ECC1B9,"cellSailPlayerOpenStream",enqueue<2>);
    SAIL(0x85BEFFCC,"cellSailPlayerCloseStream",enqueue<3>);
    SAIL(0xE535B0D3,"cellSailPlayerStart",enqueue<10>);
    SAIL(0xEBA8D4EC,"cellSailPlayerStop",enqueue<11>);
    SAIL(0x5F7C7A6F,"cellSailPlayerSetParameter",set_parameter);
    SAIL(0xFC5BAF8A,"cellSailPlayerSetRepeatMode",repeat);
    SAIL(0xD1D55A90,"cellSailPlayerSetPaused",paused<true>);
    SAIL(0xAAFA17B8,"cellSailPlayerIsPaused",paused<false>);
    SAIL(0x018281A8,"cellSailGraphicsAdapterGetFrame2",get_video);
    SAIL(0x7EB8D6B5,"cellSailSoundAdapterGetFrame",get_sound);
    SAIL(0x44A20E79,"cellSailGraphicsAdapterUpdateAvSync",sync<true>);
    SAIL(0xEEC22809,"cellSailSoundAdapterUpdateAvSync",sync<false>);
    SAIL(0x1872331B,"cellSailGraphicsAdapterPtsToTimePosition",pts_to_time);
    SAIL(0x4AE979DF,"cellSailSoundAdapterPtsToTimePosition",pts_to_time);
#undef SAIL
    fprintf(stderr,"[port-sail] asynchronous callbacks and FFmpeg video bridge registered\n");
}
