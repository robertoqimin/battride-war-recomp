// Keep the SDK audio state/HLE intact; supply a device-paced POSIX mixer loop.
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
/* The SDK starts its mixer through _beginthreadex here. */
#include <process.h>
static uintptr_t port_audio_thread_begin(void*,unsigned,unsigned(__stdcall*)(void*),void*,unsigned,unsigned*);
#define _beginthreadex port_audio_thread_begin
#include PORT_AUDIO_SOURCE
#undef _beginthreadex
#else
static int port_audio_thread_create(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
#define pthread_create port_audio_thread_create
#include PORT_AUDIO_SOURCE
#undef pthread_create
#endif
static unsigned long long port_audio_ns(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (unsigned long long)t.tv_sec*1000000000ull+t.tv_nsec;
}
/* PORT_AUDIO_STATS=1: every 5 s, how far ahead of the device the mix ran
 * and how often a port's block was still empty when its turn came while the
 * port had been playing: the guest's producer missing its deadline. The
 * mixer zeroes each block after reading it, so an unwritten block is zero. */
typedef struct { unsigned long long t0; unsigned blocks,qmin,starved;
                 unsigned read[CELL_AUDIO_PORT_MAX],gap[CELL_AUDIO_PORT_MAX],live[CELL_AUDIO_PORT_MAX]; } port_audio_stats;
static void port_audio_scan(port_audio_stats* st) {
    mutex_lock(&s_audio_mutex);
    for(int p=0;p<CELL_AUDIO_PORT_MAX;p++) {
        AudioPortSlot* port=&s_ports[p];
        if(!port->in_use||!port->running||!port->buffer)continue;
        unsigned nch=port->param.nChannel,nblock=port->param.nBlock,n=CELL_AUDIO_BLOCK_SAMPLES*nch;
        const float* src=port->buffer+(unsigned)(port->read_index%nblock)*n;
        int nz=0;for(unsigned i=0;i<n;i++)if(src[i]!=0.0f){nz=1;break;}
        st->read[p]++;
        if(nz)st->live[p]=nblock;            /* "playing" for one more ring */
        else if(st->live[p]){st->gap[p]++;st->live[p]--;}
    }
    mutex_unlock(&s_audio_mutex);
}
static void* port_audio_mix(void* unused) {
    (void)unused;
    unsigned long long epoch=port_audio_ns(),blocks=0;
    unsigned report=0;int peak_on=getenv("AUDIO_PEAK")!=NULL;
    const char* path=getenv("AUDIO_WAV");FILE* dump=path?fopen(path,"wb"):NULL;
    int stats_on=getenv("PORT_AUDIO_STATS")!=NULL;port_audio_stats st;memset(&st,0,sizeof st);st.t0=port_audio_ns();
    fprintf(stderr,"[port-audio] POSIX mixer paced to the output device, 48000 Hz\n");
    while(s_mix_thread_running) {
        int room=audio_backend_room();
        if(room>=0 && room<CELL_AUDIO_BLOCK_SAMPLES){audio_backend_wait();continue;}
#ifdef _WIN32
        /* WASAPI drains SDL's queue a device period at a time (about two blocks),
         * so room alone mixes in bursts: the guest wakes on the first block and
         * the second is read empty, every other block silent. Keep the clock's
         * spacing unless the device is about to run out. */
        if(room<0 || audio_backend_queued_samples()>=CELL_AUDIO_BLOCK_SAMPLES) {
#else
        if(room<0) {
#endif
            unsigned long long now=port_audio_ns(),due=epoch+blocks*CELL_AUDIO_BLOCK_SAMPLES*1000000000ull/CELL_AUDIO_SAMPLE_RATE;
            if(now<due) {struct timespec delay={0,1000000};nanosleep(&delay,NULL);continue;}
            if(now-due>50000000)epoch=now-blocks*CELL_AUDIO_BLOCK_SAMPLES*1000000000ull/CELL_AUDIO_SAMPLE_RATE;
        }
        if(stats_on) port_audio_scan(&st);
        audio_mix_one_block();blocks++;
        if(dump)fwrite(s_mix_buffer,sizeof(float),CELL_AUDIO_BLOCK_SAMPLES*2,dump);
        if(peak_on && ++report%200==0) {
            float peak=0;for(unsigned i=0;i<CELL_AUDIO_BLOCK_SAMPLES*2;i++){float x=fabsf(s_mix_buffer[i]);if(x>peak)peak=x;}
            fprintf(stderr,"[port-audio] mixed blocks=%llu peak=%.5f queued=%u\n",blocks,peak,audio_backend_queued_samples());
        }
        if(stats_on) {
            unsigned q=audio_backend_queued_samples();
            if(q==0)st.starved++;
            if(q<st.qmin||!st.blocks)st.qmin=q;
            st.blocks++;
            if(port_audio_ns()-st.t0>=5000000000ull) {
                double sec=(port_audio_ns()-st.t0)/1e9;
                fprintf(stderr,"[port-audio] %.1f blocks/s, device queue min %u samples, empty %u,",st.blocks/sec,st.qmin,st.starved);
                for(int p=0;p<CELL_AUDIO_PORT_MAX;p++) if(st.read[p])
                    fprintf(stderr," port%d underruns %u/%u",p,st.gap[p],st.read[p]);
                fputc('\n',stderr);
                unsigned long long t0=port_audio_ns();memset(&st,0,sizeof st);st.t0=t0;
            }
        }
        audio_backend_submit(s_mix_buffer,CELL_AUDIO_BLOCK_SAMPLES);
        audio_notify_event_queues();
    }
    if(dump)fclose(dump);
    return NULL;
}
#ifdef _WIN32
static unsigned __stdcall port_audio_mix_thread(void* arg) {
    port_audio_mix(arg);
    return 0;
}
static uintptr_t port_audio_thread_begin(void* security,unsigned stack,unsigned(__stdcall* fn)(void*),void* arg,unsigned flags,unsigned* id) {
    return _beginthreadex(security,stack,fn==audio_mix_thread_func?port_audio_mix_thread:fn,arg,flags,id);
}
#else
static int port_audio_thread_create(pthread_t* t,const pthread_attr_t* attr,void*(*fn)(void*),void* arg) {
    return pthread_create(t,attr,fn==audio_mix_thread_func?port_audio_mix:fn,arg);
}
#endif
