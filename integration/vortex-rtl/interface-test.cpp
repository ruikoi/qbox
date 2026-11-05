// SPDX-License-Identifier: Apache-2.0
#include <vortex.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <vector>
#define CHECK(x) do { if ((x) != 0) { std::fprintf(stderr,"INTERFACE_FAIL line=%d call=%s\n",__LINE__,#x); return 1; } } while(0)
int main() {
  vx_device_h dev=nullptr;
  vx_buffer_h memory=nullptr;
  CHECK(vx_device_open(0,&dev));
  vx_queue_h queue;
  vx_queue_info_t qi{sizeof(qi),nullptr,VX_QUEUE_PRIORITY_NORMAL,0};
  CHECK(vx_queue_create(dev,&qi,&queue));
  auto write=[&](vx_buffer_h b,const void *data,uint64_t off,uint64_t len) { auto rc=vx_enqueue_write(queue,b,off,data,len,0,nullptr,nullptr); return rc==VX_SUCCESS ? vx_queue_finish(queue,VX_TIMEOUT_INFINITE) : rc; };
  auto read=[&](void *data,vx_buffer_h b,uint64_t off,uint64_t len) { auto rc=vx_enqueue_read(queue,data,b,off,len,0,nullptr,nullptr); return rc==VX_SUCCESS ? vx_queue_finish(queue,VX_TIMEOUT_INFINITE) : rc; };
  CHECK(vx_buffer_create(dev,16384,VX_MEM_READ_WRITE,&memory));
  std::vector<unsigned char> expected(16384,0xa5), got(16384), patch(8192);
  CHECK(write(memory,expected.data(),0,expected.size()));
  const unsigned lengths[]={1,3,31,63,64,65,127,128,129,255,256,257,4095,4096,4097};
  const unsigned offsets[]={0,1,63,64,65,127};
  unsigned cases=0;
  for (unsigned len:lengths) for (unsigned off:offsets) {
    for(unsigned i=0;i<len;++i) patch[i]=(i*29+cases*17)&255;
    CHECK(write(memory,patch.data(),off,len));
    std::memcpy(expected.data()+off,patch.data(),len);
    CHECK(read(got.data(),memory,0,got.size()));
    if(got!=expected) { std::fprintf(stderr,"DMA_GUARD_FAIL offset=%u length=%u\n",off,len); for(unsigned j=0,n=0;j<got.size() && n<8;++j) if(got[j]!=expected[j]) { std::fprintf(stderr,"DMA_DIFF idx=%u actual=%u expected=%u\n",j,got[j],expected[j]); ++n; } return 1; }
    std::vector<unsigned char> partial(len+16,0x7c);
    CHECK(read(partial.data()+8,memory,off,len));
    for(unsigned j=0;j<len;++j) if(partial[j+8]!=expected[off+j]) return 1;
    for(unsigned j=0;j<8;++j) if(partial[j]!=0x7c || partial[len+8+j]!=0x7c) return 1;
    ++cases;
  }
  std::printf("VORTEX_DMA_BOUNDARY_PASS cases=%u whole_buffer_guard=1\n",cases);
  for(unsigned i=0;i<1100;++i) {
    unsigned off=(i*67)%8192;
    unsigned char value=(i*13)&255;
    CHECK(write(memory,&value,off,1));
    expected[off]=value;
    if(i%100==0) {
      CHECK(read(got.data(),memory,0,got.size()));
      if(got!=expected) return 1;
    }
  }
  CHECK(read(got.data(),memory,0,got.size()));
  if(got!=expected) return 1;
  std::puts("VORTEX_RING_WRAP_PASS submissions=1100 ring_entries=1024");
  usleep(20000);
  CHECK(read(got.data(),memory,0,got.size()));
  if(got!=expected) return 1;
  std::puts("VORTEX_IDLE_WAKE_PASS");
  CHECK(vx_buffer_release(memory));
  CHECK(vx_queue_release(queue));
  CHECK(vx_device_release(dev));
  std::puts("VORTEX_INTERFACE_ALL_PASS");
}
