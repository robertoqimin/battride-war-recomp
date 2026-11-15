// Regression: exact bytes, unchanged cursor, EOF and output initialization.
#define PORT_PPU_FS_SOURCE "../ps3recomp/runtime/ppu/ppu_fs.cpp"
#include "../port/io/fs_bridge.cpp"
#include <cassert>
#include <cstdlib>
extern "C" {
uint8_t* vm_base=nullptr;
uint32_t ppu_vm_size=65536;
void vm_write64(uint64_t a,uint64_t v) {
    for(int i=7;i>=0;i--) {vm_base[a+i]=(uint8_t)v;v>>=8;}
}
}
static uint64_t read_out() {
    uint64_t v=0;for(int i=0;i<8;i++)v=(v<<8)|vm_base[4096+i];return v;
}
static ppu_context request(int fd,uint64_t offset,uint32_t buf,uint64_t count) {
    ppu_context c{};c.gpr[3]=fd;c.gpr[4]=offset;c.gpr[5]=buf;
    c.gpr[6]=count;c.gpr[7]=4096;vm_write64(4096,0xDEADBEEF);
    return c;
}
int main() {
    vm_base=(uint8_t*)calloc(1,ppu_vm_size);assert(vm_base);
    g_files[3]=tmpfile();assert(g_files[3]);
    assert(fwrite("abcdef",1,6,g_files[3])==6);fflush(g_files[3]);
    assert(fseek(g_files[3],1,SEEK_SET)==0);
    auto c=request(3,2,8192,3);port_fs_read_with_offset(&c);
    assert(c.gpr[3]==0 && read_out()==3 && !memcmp(vm_base+8192,"cde",3));
    assert(ftell(g_files[3])==1);
    c=request(3,6,8192,3);port_fs_read_with_offset(&c);assert(c.gpr[3]==0 && read_out()==0);
    c=request(63,0,8192,3);port_fs_read_with_offset(&c);assert((uint32_t)c.gpr[3]==0x8001002A && read_out()==0);
    c=request(3,0,65535,3);port_fs_read_with_offset(&c);assert((uint32_t)c.gpr[3]==0x8001000D && read_out()==0);
    c=request(3,UINT64_MAX,8192,3);port_fs_read_with_offset(&c);assert((uint32_t)c.gpr[3]==0x80010002 && read_out()==0);
    fclose(g_files[3]);free(vm_base);puts("PASS: offset read, cursor preservation, EOF, bad descriptor, guest bounds, offset overflow");
}
