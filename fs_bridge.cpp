/* Compile the SDK VFS once in this translation unit so the extension shares
 * its descriptor table. The SDK checkout itself remains unchanged. */
#include PORT_PPU_FS_SOURCE
#include <limits.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
/* pread: a read at an offset that leaves the descriptor's position alone. On a
 * synchronous handle ReadFile moves it even with an OVERLAPPED offset. */
static ssize_t port_pread(int fd, void* data, size_t count, uint64_t offset)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    LARGE_INTEGER zero = {}, position;
    if (h == INVALID_HANDLE_VALUE || !SetFilePointerEx(h, zero, &position, FILE_CURRENT)) { errno = EIO; return -1; }
    OVERLAPPED at = {};
    at.Offset = (DWORD)offset; at.OffsetHigh = (DWORD)(offset >> 32);
    DWORD got = 0;
    const BOOL ok = ReadFile(h, data, (DWORD)count, &got, &at);
    const DWORD failure = ok ? 0 : GetLastError();
    SetFilePointerEx(h, position, NULL, FILE_BEGIN);
    if (ok || failure == ERROR_HANDLE_EOF) return (ssize_t)got;
    errno = EIO;
    return -1;
}
#endif

static void port_fs_read_with_offset(ppu_context* c)
{
    int fd=(int)(uint32_t)c->gpr[3];
    uint64_t offset=c->gpr[4], count=c->gpr[6];
    uint32_t buf=(uint32_t)c->gpr[5], out=(uint32_t)c->gpr[7];
    int32_t error=0;
    if (out && ppu_vm_size && (uint64_t)out+8>ppu_vm_size) {
        c->gpr[3]=(uint64_t)(int64_t)(int32_t)0x8001000Du; return;
    }
    if (out) vm_write64(out,0);
    if (fd<3 || fd>=FS_MAX || !g_files[fd]) error=(int32_t)0x8001002Au;
    else if (offset>INT64_MAX) error=(int32_t)0x80010002u;
    else if ((count && !buf) || (uint64_t)buf+count < count ||
             (ppu_vm_size && (uint64_t)buf+count>ppu_vm_size)) error=(int32_t)0x8001000Du;
    else if (count) {
        fs_prefault(buf,count);
        ssize_t n;
#ifdef _WIN32
        do { n=port_pread(fileno(g_files[fd]),vm_base+buf,(size_t)count,offset); }
#else
        do { n=pread(fileno(g_files[fd]),vm_base+buf,(size_t)count,(off_t)offset); }
#endif
        while(n<0 && errno==EINTR);
        if(n<0) error=(int32_t)CELL_FS_EIO;
        else if(out) vm_write64(out,(uint64_t)n);
        static int logged=0;
        if(logged++<8) fprintf(stderr,"[port-fs] pread fd=%d offset=%llu requested=%llu -> %lld\n",
            fd,(unsigned long long)offset,(unsigned long long)count,(long long)n);
    }
    c->gpr[3]=(uint64_t)(int64_t)error;
}
extern "C" void port_fs_register_extensions(void)
{
    ps3_hle_register_ctx(0x0D5B4A14u,"cellFsReadWithOffset",port_fs_read_with_offset);
}
