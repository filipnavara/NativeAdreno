/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoMemory.h"
#include "NativeAdrenoPool.h"
/* Where a call that never returns is stuck: the free/unmap/release path stores a stage number here, the board page shows it. */
extern "C" {volatile uint32_t native_adreno_trace_stage;}
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>
extern "C" boolean_t PE_parse_boot_argn(const char *,void *,int);

static bool outputWidth(unsigned bits)
{return bits==32 || bits==36 || bits==40 || bits==42 || bits==44 || bits==48;}
/* Where the last import stopped (10..19, 19 = done), and for a refused page the physical address and offset: the board log shows them. */
enum : IOReturn {kNativeAdrenoProtectionFailure=2};   /* KERN_PROTECTION_FAILURE, as prepare() reports a range that cannot be wired for writing */
extern "C" {volatile uint32_t native_adreno_import_stage;volatile uint64_t native_adreno_import_fail_pa,native_adreno_import_fail_offset;}
/* BO memory for the board log: [0] user BOs from the system (allocateSystem), [1] their spans, [2] refused, [3] the last refusal's status,
 * [4] MiB of them live, [5] the peak, [6] kernel-mapped BOs (allocate), [7] import spans. */
extern "C" {volatile uint32_t native_adreno_bo_memory[8];}
/* Contiguous BO runs: [0] made, [1] refused by the free memory (those BOs fell back to pageable pages). */
extern "C" {volatile uint32_t native_adreno_bo_contig[2];}
/* Who holds the system BOs, for the board log: a row a process {pid, live KiB, live BOs, peak KiB}, the 24 processes that hold the
 * most kept (a process that released everything gives its row up); and the live BOs by size, {count, KiB} for sizes up to 64 KiB,
 * 1 MiB, 4 MiB, 16 MiB, 64 MiB and above. Both change under the driver's lock, as systemBytes does. */
extern "C" {volatile uint32_t native_adreno_bo_owners[24][4];volatile uint32_t native_adreno_bo_sizes[6][2];int proc_selfpid(void);}
/* Imported memory the driver keeps wired: an import pins another mapping's pages (prepare()) from importTask until release(), whether
 * or not the GPU maps them. [0] imports alive, [1] KiB of them, [2] the peak KiB, [3] imports made so far. native_adreno_import_owners
 * holds the importing processes as native_adreno_bo_owners holds the allocating ones: {pid, live KiB, live imports, peak KiB}. A range
 * imported twice counts twice, although its pages are wired once. Both change under the driver's lock. */
extern "C" {volatile uint32_t native_adreno_import_memory[4];volatile uint32_t native_adreno_import_owners[24][4];}
namespace {
uint64_t systemBytes,systemPeak;
bool pressure;
unsigned sizeClass(uint64_t bytes)
{
    static const uint64_t limit[5]={UINT64_C(64)<<10,UINT64_C(1)<<20,UINT64_C(4)<<20,UINT64_C(16)<<20,UINT64_C(64)<<20};
    unsigned c=0;while(c<5 && bytes>limit[c])++c;
    return c;
}
}
void NativeAdrenoNoteHolder(volatile uint32_t (*table)[4],int32_t pid,int64_t bytes)
{
    const uint32_t kib=(uint32_t)((uint64_t)(bytes<0?-bytes:bytes)>>10);
    unsigned row=24,spare=24,least=0;
    for(unsigned i=0;i<24;++i) {
        if(table[i][2] && table[i][0]==(uint32_t)pid) {row=i;break;}
        if(!table[i][2] && spare==24)spare=i;
        if(table[i][1]<table[least][1])least=i;
    }
    if(bytes<0) {
        if(row==24)return;
        table[row][1]=table[row][1]>kib?table[row][1]-kib:0;
        table[row][2]=table[row][2]-1;
        return;
    }
    if(row==24) {
        /* A new holder takes a free row, else the row of the smallest holder once it holds more than that one. */
        row=spare!=24?spare:least;
        if(spare==24 && table[least][1]>=kib)return;
        table[row][0]=(uint32_t)pid;table[row][1]=0;table[row][2]=0;table[row][3]=0;
    }
    table[row][1]=table[row][1]+kib;
    table[row][2]=table[row][2]+1;
    if(table[row][1]>table[row][3])table[row][3]=table[row][1];
}
namespace {
void noteSystem(int64_t bytes,int32_t pid)
{
    systemBytes+=bytes;if(systemBytes>systemPeak)systemPeak=systemBytes;
    native_adreno_bo_memory[4]=(uint32_t)(systemBytes>>20);native_adreno_bo_memory[5]=(uint32_t)(systemPeak>>20);
    const unsigned c=sizeClass((uint64_t)(bytes<0?-bytes:bytes));
    const uint32_t kib=(uint32_t)((uint64_t)(bytes<0?-bytes:bytes)>>10);
    if(bytes>0) {native_adreno_bo_sizes[c][0]=native_adreno_bo_sizes[c][0]+1;native_adreno_bo_sizes[c][1]=native_adreno_bo_sizes[c][1]+kib;}
    else {
        native_adreno_bo_sizes[c][0]=native_adreno_bo_sizes[c][0]?native_adreno_bo_sizes[c][0]-1:0;
        native_adreno_bo_sizes[c][1]=native_adreno_bo_sizes[c][1]>kib?native_adreno_bo_sizes[c][1]-kib:0;
    }
    NativeAdrenoNoteHolder(native_adreno_bo_owners,pid,bytes);
}
void noteImport(int64_t bytes,int32_t pid)
{
    static uint64_t importBytes,importPeak;
    importBytes+=bytes;if(importBytes>importPeak)importPeak=importBytes;
    if(bytes>0) {native_adreno_import_memory[0]=native_adreno_import_memory[0]+1;native_adreno_import_memory[3]=native_adreno_import_memory[3]+1;}
    else native_adreno_import_memory[0]=native_adreno_import_memory[0]?native_adreno_import_memory[0]-1:0;
    native_adreno_import_memory[1]=(uint32_t)(importBytes>>10);native_adreno_import_memory[2]=(uint32_t)(importPeak>>10);
    NativeAdrenoNoteHolder(native_adreno_import_owners,pid,bytes);
}
}
bool NativeAdrenoMemory::memoryPressure() {return pressure;}
/* Which DRAM pages this driver's own BOs hold, a bit a page over the kernel's physical range: an import (another task's pages) aliases
 * one of them only where a bit is set, so proving an import exclusive costs its own pages, not the ranges mapped so far (every BO is
 * scattered pages: a pairwise check over them ran out of budget on each import). Made on first use; without it every page counts as
 * owned (the full alias check). */
extern "C" uint64_t gPhysBase,gPhysSize;
namespace {
uint64_t *ownedBits;
/* The bits of the kernel's own range, then those of the first 4 GiB below it (DRAM a kernel adopts outside its range). */
const uint64_t kOwnedLowPages=UINT64_C(1)<<20;
uint64_t *ownedMap()
{
    static bool tried;
    if(!ownedBits && !tried) {tried=true;ownedBits=static_cast<uint64_t *>(IOMallocZero((size_t)((gPhysSize/4096+kOwnedLowPages+63)/64*8)));}
    return ownedBits;
}
/* The page's bit; false for a page that has none (neither in the kernel's range nor below it within 4 GiB). */
bool ownedIndex(uint64_t page,uint64_t *at)
{
    if(page>=gPhysBase) {
        if(page-gPhysBase>=gPhysSize)return false;
        *at=(page-gPhysBase)/4096;
        return true;
    }
    if(page>>32)return false;
    *at=gPhysSize/4096+page/4096;
    return true;
}
void noteOwned(const NativeAdrenoPhysicalSpan *spans,size_t count,bool owned)
{
    uint64_t *bits=ownedMap();
    if(!bits)return;
    for(size_t i=0;i<count;++i)for(uint64_t page=spans[i].physical;page<spans[i].physical+spans[i].bytes;page+=4096) {
        uint64_t at=0;
        if(!ownedIndex(page,&at))continue;
        const uint64_t bit=UINT64_C(1)<<(at%64);
        if(owned)__atomic_fetch_or(&bits[at/64],bit,__ATOMIC_RELAXED);else __atomic_fetch_and(&bits[at/64],~bit,__ATOMIC_RELAXED);
    }
}
}
bool NativeAdrenoMemory::ownsAnyPage(const NativeAdrenoPhysicalSpan *spans,size_t count)
{
    const uint64_t *bits=ownedMap();
    if(!bits)return true;
    for(size_t i=0;i<count;++i)for(uint64_t page=spans[i].physical;page<spans[i].physical+spans[i].bytes;page+=4096) {
        uint64_t at=0;
        if(!ownedIndex(page,&at))continue;
        if(__atomic_load_n(&bits[at/64],__ATOMIC_RELAXED)>>(at%64)&1)return true;
    }
    return false;
}
/* The BO pool (sheng's, ad01b92, without the adopted range): physically contiguous chunks reserved at bring-up while memory still has
 * long runs, cut into user BOs best fit, neighbours merged on release. A pooled BO is one span of the GPU's table: asteroids T64c's system
 * BOs came scattered, 228876 owned spans live, and every map and unmap moved the VM's whole sorted range array (641 and 841 ms against
 * T63's 61 and 74 ms with the pool), BoAlloc/Map/Free 196/457/940 us against 59/146/222. Each chunk takes the write-combined attribute
 * once and keeps it; nothing else maps its pages, so the owned-page map covers the whole chunk. With no room a BO comes from the system
 * (allocateSystem): no chunk is ever searched for at BoAlloc (untagged-15146dbdcf: the scans held the publication lock for seconds).
 * [0] pool MiB, [1] BOs from the pool, [2] went to the system instead, [3] MiB in use, [4] peak MiB, [5] the reservation's IOReturn,
 * [6] the write-combine's (bit 31 once tried), [7] chunks. */
extern "C" {volatile uint32_t native_adreno_pool_mib=1536;volatile uint32_t native_adreno_pool_stats[8];}
namespace {
struct PoolExtent {size_t offset,bytes;};
struct Chunk {
    IOBufferMemoryDescriptor *descriptor=nullptr;
    unsigned char *virt=nullptr;uint64_t physical=0;size_t bytes=0,used=0;
    PoolExtent *free=nullptr;unsigned count=0,capacity=0;
    bool write_combined=false;
};
enum {PoolChunks=8};
struct Pool {
    Chunk chunks[PoolChunks];unsigned live=0;
    size_t bytes=0,used=0,peak=0;unsigned bits=0;
    bool write_combined=false;
} pool;
bool extentGrow(Chunk &c)
{
    const unsigned size=c.capacity?c.capacity*2:16;
    PoolExtent *made=IONewData(PoolExtent,size);
    if(!made)return false;
    if(c.count)memcpy(made,c.free,sizeof(PoolExtent)*c.count);
    if(c.free)IODeleteData(c.free,PoolExtent,c.capacity);
    c.free=made;c.capacity=size;return true;
}
void noteUse()
{
    if(pool.used>pool.peak)pool.peak=pool.used;
    native_adreno_pool_stats[0]=(uint32_t)(pool.bytes>>20);
    native_adreno_pool_stats[3]=(uint32_t)(pool.used>>20);native_adreno_pool_stats[4]=(uint32_t)(pool.peak>>20);
    native_adreno_pool_stats[7]=pool.live;
}
/* The chunk's pages write-combined for every CPU mapping, then their lines cleaned and dropped by physical address: on xnu-10063 the
 * attribute change cleans no line (asteroids T64b), and DC CIVAC reaches a line whatever the mapping's type now is. */
IOReturn chunkWriteCombine(Chunk &c)
{
    const NativeAdrenoPhysicalSpan span={0,c.physical,c.bytes};
    IOReturn status=NativeAdrenoSetPagesCacheMode(&span,1,true);
    if(status)return status;
    c.write_combined=true;
    status=NativeAdrenoMaintainPhysicalPages(c.physical,c.bytes,NativeAdrenoCleanInvalidate);
    NativeAdrenoNotePhase(NativeAdrenoPhaseAllocate,c.bytes);
    return status;
}
IOReturn chunkMake(size_t bytes)
{
    if(pool.live==PoolChunks)return kIOReturnNoResources;
    const uint64_t limit=UINT64_C(1)<<pool.bits;
    IOBufferMemoryDescriptor *descriptor=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone)|kIOMemoryPhysicallyContiguous,
        bytes,(limit-1)&~UINT64_C(4095));
    if(!descriptor)return kIOReturnNoMemory;
    if(descriptor->prepare()!=kIOReturnSuccess) {descriptor->release();return kIOReturnNoMemory;}
    IOByteCount length=0,tail=0;
    const addr64_t first=descriptor->getPhysicalSegment(0,&length,kIOMemoryMapperNone);
    const addr64_t last=descriptor->getPhysicalSegment(bytes-4096,&tail,kIOMemoryMapperNone);
    unsigned char *virt=static_cast<unsigned char *>(descriptor->getBytesNoCopy());
    Chunk &c=pool.chunks[pool.live];
    c=Chunk{};c.descriptor=descriptor;c.virt=virt;c.physical=first;c.bytes=bytes;
    if(!first || (first&4095) || !virt || ((uintptr_t)virt&4095) || last!=first+bytes-4096 || first>=limit ||
        bytes>limit-first || descriptor->getLength()!=bytes || !extentGrow(c)) {
        if(c.free)IODeleteData(c.free,PoolExtent,c.capacity);
        (void)descriptor->complete();descriptor->release();c=Chunk{};return kIOReturnUnsupported;
    }
    c.free[0]={0,bytes};c.count=1;
    ++pool.live;pool.bytes+=bytes;noteUse();
    const NativeAdrenoPhysicalSpan span={0,first,bytes};
    noteOwned(&span,1,true);
    return kIOReturnSuccess;
}
/* Best fit over every chunk of the attribute asked for; false when none has room. */
bool poolTake(size_t bytes,bool write_combined,unsigned *slot,size_t *offset)
{
    int best=-1;unsigned at=0;size_t fit=0;
    for(unsigned k=0;k<pool.live;++k) {
        const Chunk &c=pool.chunks[k];
        if(c.write_combined!=write_combined || c.bytes-c.used<bytes)continue;
        for(unsigned i=0;i<c.count;++i)if(c.free[i].bytes>=bytes && (best<0 || c.free[i].bytes<fit)) {best=(int)k;at=i;fit=c.free[i].bytes;}
    }
    if(best<0)return false;
    Chunk &c=pool.chunks[best];
    *slot=(unsigned)best;*offset=c.free[at].offset;
    c.free[at].offset+=bytes;c.free[at].bytes-=bytes;
    if(!c.free[at].bytes) {for(unsigned i=at;i+1<c.count;++i)c.free[i]=c.free[i+1];--c.count;}
    c.used+=bytes;pool.used+=bytes;noteUse();
    return true;
}
void poolGive(unsigned slot,size_t offset,size_t bytes)
{
    Chunk &c=pool.chunks[slot];
    c.used-=bytes;pool.used-=bytes;noteUse();
    unsigned at=0;while(at<c.count && c.free[at].offset<offset)++at;
    const bool after=at && c.free[at-1].offset+c.free[at-1].bytes==offset,before=at<c.count && offset+bytes==c.free[at].offset;
    if(after && before) {
        c.free[at-1].bytes+=bytes+c.free[at].bytes;
        for(unsigned i=at;i+1<c.count;++i)c.free[i]=c.free[i+1];
        --c.count;return;
    }
    if(after) {c.free[at-1].bytes+=bytes;return;}
    if(before) {c.free[at].offset=offset;c.free[at].bytes+=bytes;return;}
    if(c.count==c.capacity && !extentGrow(c))return;   /* the extent is lost while the pool lives */
    for(unsigned i=c.count;i>at;--i)c.free[i]=c.free[i-1];
    c.free[at]={offset,bytes};++c.count;
}
}
IOReturn NativeAdrenoMemory::reservePool(unsigned bits,size_t bytes)
{
    if(!bytes || (bytes&4095) || !outputWidth(bits))return kIOReturnBadArgument;
    if(pool.live && pool.bits!=bits)return kIOReturnBusy;
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    pool.bits=bits;
    status=chunkMake(bytes);
    if(!status && pool.write_combined) {
        status=chunkWriteCombine(pool.chunks[pool.live-1]);
        native_adreno_pool_stats[6]=(uint32_t)status|1u<<31;
    }
    return status;
}
IOReturn NativeAdrenoMemory::writeCombinePool()
{
    pool.write_combined=true;
    IOReturn status=kIOReturnSuccess;
    for(unsigned k=0;k<pool.live && !status;++k)if(!pool.chunks[k].write_combined)status=chunkWriteCombine(pool.chunks[k]);
    native_adreno_pool_stats[6]=(uint32_t)status|1u<<31;
    return status;
}
size_t NativeAdrenoMemory::poolBytes() {return pool.bytes;}
IOReturn NativeAdrenoMemory::allocatePooled(size_t bytes,unsigned bits,size_t budget,bool write_combined)
{
    if(descriptor_ || spans_ || quarantined_ || pooled_)return kIOReturnBusy;
    if(!bytes || (bytes&4095) || !outputWidth(bits) || !budget)return kIOReturnBadArgument;
    if(bytes>budget)return kIOReturnNoResources;
    if(!pool.live || pool.bits>bits)return kIOReturnNoSpace;
    unsigned slot=0;size_t offset=0;
    if(!poolTake(bytes,write_combined,&slot,&offset)) {native_adreno_pool_stats[2]=native_adreno_pool_stats[2]+1;return kIOReturnNoSpace;}
    const Chunk &c=pool.chunks[slot];
    spans_=IONewData(NativeAdrenoPhysicalSpan,1);
    if(!spans_) {poolGive(slot,offset,bytes);return kIOReturnNoMemory;}
    span_capacity_=1;span_count_=1;spans_[0]={0,c.physical+offset,bytes};
    mapping_=c.virt+offset;bytes_=bytes;pooled_=true;pool_chunk_=slot;pool_offset_=offset;prepared_=true;
    write_combined_=c.write_combined;
    bzero(mapping_,bytes);
    IOReturn status=kIOReturnSuccess;
    /* Through a write-combined chunk's own mapping the zeros go to DRAM; the barrier completes them. */
    if(c.write_combined)__asm__ volatile("dsb sy":::"memory");
    else {status=NativeAdrenoMaintainOwnedPages(mapping_,bytes,NativeAdrenoClean);NativeAdrenoNotePhase(NativeAdrenoPhaseAllocate,bytes);}
    if(status!=kIOReturnSuccess) {write_combined_=false;(void)release();return status;}
    native_adreno_pool_stats[1]=native_adreno_pool_stats[1]+1;
    return kIOReturnSuccess;
}
/* The descriptor's pages as spans (neighbours merged), each below `limit` and, for a contiguous allocation, following the first. */
IOReturn NativeAdrenoMemory::takeSegments(IOBufferMemoryDescriptor *descriptor,size_t bytes,uint64_t limit,bool contiguous)
{
    span_capacity_=bytes/4096;
    spans_=IONewData(NativeAdrenoPhysicalSpan,span_capacity_);
    if(!spans_)return kIOReturnNoMemory;
    for(size_t offset=0;offset<bytes;offset+=4096) {
        IOByteCount length=0;
        addr64_t physical=descriptor->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        if(!physical || (physical&4095) || length<4096 || physical>=limit || 4096>limit-physical)return kIOReturnUnsupported;
        if(contiguous && offset && physical!=spans_[0].physical+offset)return kIOReturnUnsupported;
        if(span_count_ && physical==spans_[span_count_-1].physical+spans_[span_count_-1].bytes)
            spans_[span_count_-1].bytes+=4096;
        else spans_[span_count_++]={offset,physical,4096};
    }
    return kIOReturnSuccess;
}
/* The grant's runs as spans, each below `limit`. */
IOReturn NativeAdrenoMemory::takeRuns(const NativeAdrenoPoolGrant *grant,size_t bytes,uint64_t limit)
{
    if(!grant->count || grant->bytes!=bytes)return kIOReturnUnsupported;
    span_capacity_=grant->count;
    spans_=IONewData(NativeAdrenoPhysicalSpan,span_capacity_);
    if(!spans_)return kIOReturnNoMemory;
    uint64_t offset=0;
    for(uint32_t i=0;i<grant->count;++i) {
        const NativeAdrenoPoolRun &run=grant->runs[i];
        if(!run.physical || !run.bytes || ((run.physical|run.bytes)&4095) || run.bytes>bytes-offset || run.physical>=limit ||
            run.bytes>limit-run.physical)return kIOReturnUnsupported;
        spans_[span_count_++]={offset,run.physical,run.bytes};
        offset+=run.bytes;
    }
    return offset==bytes?kIOReturnSuccess:kIOReturnUnsupported;
}
/* The first kernel allocation that failed: [0] where (1 no descriptor, 2 prepare, 3 after it), [1] status, [2] bytes, [3] address
 * bits, [4] contiguous, [5] failures so far. */
extern "C" {volatile uint32_t native_adreno_kernel_alloc_fail[6];}
static void noteKernelFailure(uint32_t where,IOReturn status,size_t bytes,unsigned bits,bool contiguous)
{
    volatile uint32_t *f=native_adreno_kernel_alloc_fail;
    if(!f[5]) {f[0]=where;f[1]=(uint32_t)status;f[2]=(uint32_t)bytes;f[3]=bits;f[4]=contiguous;}
    f[5]=f[5]+1;
}
IOReturn NativeAdrenoMemory::allocate(size_t bytes,unsigned bits,bool contiguous,size_t budget)
{
    if(descriptor_ || grant_ || spans_ || quarantined_)return kIOReturnBusy;
    if(!bytes || (bytes&4095) || !outputWidth(bits) || !budget)
        return kIOReturnBadArgument;
    if(bytes>budget)return kIOReturnNoResources;
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    uint64_t limit=UINT64_C(1)<<bits;
    IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    if(contiguous)options|=kIOMemoryPhysicallyContiguous;
    descriptor_=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,options,bytes,(limit-1)&~UINT64_C(4095));
    if(!descriptor_) {noteKernelFailure(1,kIOReturnNoMemory,bytes,bits,contiguous);return kIOReturnNoMemory;}
    status=descriptor_->prepare();
    if(status!=kIOReturnSuccess) {noteKernelFailure(2,status,bytes,bits,contiguous);descriptor_->release();descriptor_=nullptr;return status;}
    prepared_=true;bytes_=bytes;mapping_=descriptor_->getBytesNoCopy();
    status=kIOReturnUnsupported;
    if(!mapping_ || ((uintptr_t)mapping_&4095) || bytes>UINTPTR_MAX-(uintptr_t)mapping_ ||
        descriptor_->getLength()!=bytes)goto failed;
    status=takeSegments(descriptor_,bytes,limit,contiguous);
    if(status!=kIOReturnSuccess)goto failed;
    bzero(mapping_,bytes);
    status=NativeAdrenoMaintainOwnedPages(mapping_,bytes,NativeAdrenoClean);
    if(status!=kIOReturnSuccess)goto failed;
    NativeAdrenoNotePhase(NativeAdrenoPhaseAllocate,bytes);
    noteOwned(spans_,span_count_,true);owned_=true;
    native_adreno_bo_memory[6]=native_adreno_bo_memory[6]+1;
    return kIOReturnSuccess;
failed:
    noteKernelFailure(3,status,bytes,bits,contiguous);
    /* release() deliberately retains descriptor/metadata if complete fails.
     * Preserve the allocation's original failure instead of reporting cleanup
     * as success. No object has been exposed to a GPU or userspace yet. */
    (void)release();return status;
}
/* No task: the memory is mapped nowhere until a client view maps it. The cache mode is the memory entry's, so every mapping of it (the
 * client views) and the pages themselves take it. */
static IOBufferMemoryDescriptor *systemDescriptor(size_t bytes,bool write_combined,uint64_t *made=nullptr,uint64_t *wired=nullptr)
{
    IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone)|
        static_cast<IOOptionBits>(kIOMemoryPageable);
    if(write_combined)options|=kIOMapWriteCombineCache;
    /* Pageable pages arrive one by one (a BO averaged 69 spans); from `gpu-bo-contig` KiB up to 128 KiB a BO first asks for one
     * physically contiguous run (one span, one table walk), and a request the free memory cannot serve falls back to pageable pages.
     * A contiguous request the kernel cannot serve scans every free page before it gives up: in ibb10331f24 124 such searches (0.1 to
     * 64 MiB) took 163 ms each at the median and 21.7 s in all, synchronously in the caller (the render server's and the widget
     * renderer's allocations, the stalls at an app launch and on the widget page), while every success was 64 to 128 KiB. So larger
     * BOs never ask, and after a failure no request of that size or larger asks again for 5 s. */
    static uint32_t contigKiB=UINT32_MAX;
    if(contigKiB==UINT32_MAX) {contigKiB=0;PE_parse_boot_argn("gpu-bo-contig",&contigKiB,sizeof contigKiB);}
    static const size_t contigMax=size_t(128)*1024;
    static size_t failedBytes=0;
    static uint64_t failedAt=0,backoff=0;
    if(!backoff)nanoseconds_to_absolutetime(UINT64_C(5000000000),&backoff);
    const uint64_t now=mach_absolute_time();
    if(failedBytes && now-failedAt>=backoff)failedBytes=0;
    if(contigKiB && bytes>=size_t(contigKiB)*1024 && bytes<=contigMax && !(failedBytes && bytes>=failedBytes)) {
        IOBufferMemoryDescriptor *run=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(nullptr,
            (options&~static_cast<IOOptionBits>(kIOMemoryPageable))|static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),bytes,UINT64_C(0xfffffffff000));
        if(run && run->prepare()==kIOReturnSuccess) {native_adreno_bo_contig[0]=native_adreno_bo_contig[0]+1;return run;}
        if(run)run->release();
        native_adreno_bo_contig[1]=native_adreno_bo_contig[1]+1;
        if(!failedBytes || bytes<failedBytes)failedBytes=bytes;
        failedAt=now;
    }
    IOBufferMemoryDescriptor *descriptor=IOBufferMemoryDescriptor::inTaskWithOptions(nullptr,options,bytes,4096);
    if(made)*made=mach_absolute_time();
    if(descriptor && descriptor->prepare()!=kIOReturnSuccess) {descriptor->release();descriptor=nullptr;}
    if(wired)*wired=mach_absolute_time();
    return descriptor;
}
IOReturn NativeAdrenoMemory::prepareSystem(size_t bytes,bool write_combined,NativeAdrenoSystemPages *out,unsigned bits)
{
    if(!out || out->held() || !bytes || (bytes&4095) || (bits && !outputWidth(bits)))return kIOReturnBadArgument;
    /* Pool pages first: taken and zero-filled here, outside the driver's lock. Nothing to clean, no cache holds them. */
    if(bits) {
        NativeAdrenoPoolGrant *grant=nullptr;
        if(NativeAdrenoPoolAllocate(bytes,UINT64_C(1)<<bits,write_combined,&grant)==kIOReturnSuccess) {
            out->grant=grant;out->bytes=bytes;out->write_combined=write_combined;out->cleaned=true;
            out->made=out->wired=mach_absolute_time();
            return kIOReturnSuccess;
        }
    }
    uint64_t made=0,wired=0;
    IOBufferMemoryDescriptor *descriptor=systemDescriptor(bytes,write_combined,&made,&wired);
    if(!descriptor)return kIOReturnNoMemory;
    if(!made)made=wired=mach_absolute_time();   /* a contiguous run: made and wired in one step */
    if(descriptor->getLength()!=bytes) {descriptor->complete();descriptor->release();return kIOReturnUnsupported;}
    bool cleaned=false;
    /* Default-attribute pages may still hold the zero fill in a CPU's cache: written back here, by physical runs, as allocateSystem would. */
    if(!write_combined) {
        cleaned=true;
        for(size_t offset=0;offset<bytes && cleaned;) {
            IOByteCount length=0;
            const addr64_t physical=descriptor->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
            if(!physical || (physical&4095) || length<4096) {cleaned=false;break;}
            if(length>bytes-offset)length=bytes-offset;
            length&=~IOByteCount(4095);
            if(NativeAdrenoMaintainPhysicalPages(physical,length,NativeAdrenoClean)!=kIOReturnSuccess)cleaned=false;
            offset+=length;
        }
        if(cleaned)NativeAdrenoNotePhase(NativeAdrenoPhaseAllocate,bytes);
    }
    out->descriptor=descriptor;out->bytes=bytes;out->write_combined=write_combined;out->cleaned=cleaned;out->made=made;out->wired=wired;
    return kIOReturnSuccess;
}
void NativeAdrenoMemory::discardSystem(NativeAdrenoSystemPages *pages)
{
    if(!pages || !pages->held())return;
    if(pages->descriptor) {pages->descriptor->complete();pages->descriptor->release();}
    /* The BO's reference to its pool pages: they go back once no client view holds them either. */
    if(pages->grant)NativeAdrenoPoolRelease(pages->grant);
    *pages=NativeAdrenoSystemPages{};
}
IOReturn NativeAdrenoMemory::allocateSystem(size_t bytes,unsigned bits,size_t budget,bool write_combined,NativeAdrenoSystemPages *prepared)
{
    if(descriptor_ || grant_ || spans_ || quarantined_)return kIOReturnBusy;
    if(!bytes || (bytes&4095) || !outputWidth(bits) || !budget)return kIOReturnBadArgument;
    if(bytes>budget)return kIOReturnNoResources;
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    /* Pages prepared outside the lock for this very request are taken over; any other request makes its own. */
    bool cleaned=false;
    if(prepared && prepared->held() && prepared->bytes==bytes && prepared->write_combined==write_combined) {
        descriptor_=prepared->descriptor;grant_=prepared->grant;cleaned=prepared->cleaned;*prepared=NativeAdrenoSystemPages{};
    } else if(prepared || NativeAdrenoPoolAllocate(bytes,UINT64_C(1)<<bits,write_combined,&grant_)!=kIOReturnSuccess) {
        /* A caller that offers prepared pages holds the driver's lock: the pool's zero fill maps its run in the kernel map and is not
         * done under it (prepareSystem already asked the pool, outside the lock). */
        grant_=nullptr;descriptor_=systemDescriptor(bytes,write_combined);
    }
    if(grant_) {
        /* Pool pages: the grant's runs are the spans. A grant that does not fit this client's physical limit after all (it was
         * prepared for another width) goes back and the VM's pages take its place. */
        const IOReturn taken=takeRuns(grant_,bytes,UINT64_C(1)<<bits);
        if(taken!=kIOReturnSuccess) {
            if(spans_)IODeleteData(spans_,NativeAdrenoPhysicalSpan,span_capacity_);
            spans_=nullptr;span_count_=span_capacity_=0;
            NativeAdrenoPoolRelease(grant_);grant_=nullptr;cleaned=false;
            descriptor_=systemDescriptor(bytes,write_combined);
        }
    }
    status=descriptor_ || grant_?kIOReturnSuccess:kIOReturnNoMemory;
    if(status==kIOReturnSuccess) {
        system_=true;prepared_=true;bytes_=bytes;write_combined_=write_combined;owner_pid_=proc_selfpid();noteSystem((int64_t)bytes,owner_pid_);
        if(descriptor_)status=descriptor_->getLength()==bytes?takeSegments(descriptor_,bytes,UINT64_C(1)<<bits,false):kIOReturnUnsupported;
        /* The zero fill may still sit in the CPU's caches as dirty lines: the VM fills the pages through the kernel's cacheable alias, and
         * on xnu-10063 (asteroids) the attribute change that follows rewrites the PTEs and flushes the TLB but cleans no line
         * (pmap_batch_set_cache_attributes flushes caches for VM_WIMG_RT only). A dirty line written back later lands over what the CPU
         * wrote through a write-combined mapping or the GPU wrote: every first user job hung in asteroids T64b while the kernel's own
         * one-page test passed. Clean every buffer's lines by physical address before anyone uses it, and drop them for write-combined
         * ones, whose pages no CPU should cache again. Pool pages (a grant) have no line in any CPU cache; default-attribute pages
         * prepared and cleaned outside the lock need nothing more. */
        if(status==kIOReturnSuccess && descriptor_ && (write_combined || !cleaned)) {
            status=maintainImported(0,bytes,write_combined?NativeAdrenoCleanInvalidate:NativeAdrenoClean);
            NativeAdrenoNotePhase(NativeAdrenoPhaseAllocate,bytes);
        }
        if(status!=kIOReturnSuccess) {write_combined_=false;(void)release();}
    }
    if(status!=kIOReturnSuccess) {
        pressure=true;
        native_adreno_bo_memory[2]=native_adreno_bo_memory[2]+1;native_adreno_bo_memory[3]=(uint32_t)status;
        return status;
    }
    pressure=false;
    noteOwned(spans_,span_count_,true);owned_=true;
    native_adreno_bo_memory[0]=native_adreno_bo_memory[0]+1;native_adreno_bo_memory[1]=native_adreno_bo_memory[1]+(uint32_t)span_count_;
    return kIOReturnSuccess;
}
/* An import holds the pages through a memory entry of its own (kIOMemoryPersistent), which references the VM object: the owner may unmap
 * its range while the GPU still uses it (Core Animation frees an image's Shmem once its no-copy buffer is released, before the runtime frees
 * the import), and the object then lives on until the import completes instead of being torn down under a wired UPL (int-233..235: the render
 * thread stuck in vm_deallocate). A range no memory entry can be made of is imported as before; [0] counts the persistent imports, [1] those. */
extern "C" {volatile uint32_t native_adreno_import_persistent[2];}
/* Imports whose pages were all non-cacheable ([1]: no CPU maintenance for them) or not ([0]). */
extern "C" {volatile uint32_t native_adreno_pool_stats_uncached[2];}
static IOMemoryDescriptor *importRange(uint64_t address,size_t bytes,IODirection direction,void *task)
{
    const IOOptionBits options=static_cast<IOOptionBits>(direction)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    IOMemoryDescriptor *range=IOMemoryDescriptor::withAddressRange(address,bytes,options|kIOMemoryPersistent,static_cast<task_t>(task));
    if(range) {native_adreno_import_persistent[0]=native_adreno_import_persistent[0]+1;return range;}
    range=IOMemoryDescriptor::withAddressRange(address,bytes,options,static_cast<task_t>(task));
    if(range)native_adreno_import_persistent[1]=native_adreno_import_persistent[1]+1;
    return range;
}
IOReturn NativeAdrenoMemory::importTask(void *task,uint64_t address,size_t bytes,unsigned bits,size_t budget)
{
    native_adreno_import_stage=10;
    if(descriptor_ || grant_ || imported_ || spans_ || quarantined_)return kIOReturnBusy;
    native_adreno_import_stage=11;
    if(!task || !address || (address&4095) || !bytes || (bytes&4095) || address>UINT64_MAX-bytes || !outputWidth(bits) ||
        !budget)return kIOReturnBadArgument;
    native_adreno_import_stage=12;
    if(bytes>budget)return kIOReturnNoResources;
    NativeAdrenoCpuState cpu{};
    native_adreno_import_stage=13;
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    const uint64_t limit=UINT64_C(1)<<bits;
    imported_=importRange(address,bytes,kIODirectionInOut,task);
    native_adreno_import_stage=14;
    if(!imported_)return kIOReturnNoMemory;
    native_adreno_import_stage=15;
    status=imported_->prepare();
    if(status==kNativeAdrenoProtectionFailure) {
        /* Read-only memory of the owner (a mapped file, constant data): cannot be pinned for writing. Pin it for reading only; the GPU then maps it
         * read-only. Metal's no-copy buffers over such memory are only ever read by shaders. */
        imported_->release();imported_=nullptr;
        imported_=importRange(address,bytes,kIODirectionOut,task);
        if(!imported_)return kIOReturnNoMemory;
        status=imported_->prepare();
        if(status==kIOReturnSuccess)read_only_=true;
    }
    if(status!=kIOReturnSuccess) {imported_->release();imported_=nullptr;return status;}
    prepared_=true;bytes_=bytes;
    status=kIOReturnUnsupported;
    native_adreno_import_stage=16;
    if(imported_->getLength()!=bytes)goto failed;
    native_adreno_import_stage=17;
    span_capacity_=bytes/4096;
    spans_=IONewData(NativeAdrenoPhysicalSpan,span_capacity_);
    if(!spans_) {status=kIOReturnNoMemory;goto failed;}
    for(size_t offset=0;offset<bytes;offset+=4096) {
        IOByteCount length=0;
        addr64_t physical=imported_->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        if(!physical || (physical&4095) || length<4096 || physical>=limit || 4096>limit-physical)
            {native_adreno_import_stage=18;native_adreno_import_fail_pa=physical;native_adreno_import_fail_offset=offset;
             status=kIOReturnUnsupported;goto failed;}
        /* A range that maps pool pages (a client view of a pool BO) is not imported: the pin of such a range holds no page, so the
         * pool could give the pages to another client while this import still maps them for the GPU. */
        if(NativeAdrenoPoolContains(physical,4096))
            {native_adreno_import_stage=18;native_adreno_import_fail_pa=physical;native_adreno_import_fail_offset=offset;
             status=kIOReturnNotPermitted;goto failed;}
        if(span_count_ && physical==spans_[span_count_-1].physical+spans_[span_count_-1].bytes)
            spans_[span_count_-1].bytes+=4096;
        else spans_[span_count_++]={offset,physical,4096};
    }
    native_adreno_import_stage=19;
    owner_uncached_=NativeAdrenoPagesUncached(spans_,span_count_);
    native_adreno_pool_stats_uncached[owner_uncached_]=native_adreno_pool_stats_uncached[owner_uncached_]+1;
    native_adreno_bo_memory[7]=native_adreno_bo_memory[7]+(uint32_t)span_count_;
    owner_pid_=proc_selfpid();import_noted_=true;noteImport((int64_t)bytes,owner_pid_);
    return kIOReturnSuccess;
failed:
    (void)release();return status;
}
IOReturn NativeAdrenoMemory::adopt(uint64_t physical,size_t bytes,unsigned bits,size_t budget)
{
    if(descriptor_ || imported_ || map_ || spans_ || quarantined_)return kIOReturnBusy;
    if(!physical || (physical&4095) || !bytes || (bytes&4095) || physical>UINT64_MAX-bytes ||
        !outputWidth(bits) || !budget || budget>256u*1024u*1024u)return kIOReturnBadArgument;
    if(bytes>budget)return kIOReturnNoResources;
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    const uint64_t limit=UINT64_C(1)<<bits;
    if(physical>=limit || bytes>limit-physical)return kIOReturnBadArgument;
    /* The board's reserved GMU pool: IODeviceMemory::withRange wraps memory that already exists at
     * this physical address (the exact hand-off upstream sheng's GpuPowerProbe does over the
     * payloads), so nothing here ever asks the VM for pages. */
    imported_=IODeviceMemory::withRange(physical,bytes);
    if(!imported_)return kIOReturnNoMemory;
    status=imported_->prepare();
    if(status!=kIOReturnSuccess) {imported_->release();imported_=nullptr;return status;}
    prepared_=true;bytes_=bytes;
    status=kIOReturnUnsupported;
    if(imported_->getLength()!=bytes)goto failed;
    /* Write-combined, as sheng's GpuPowerProbe maps the same payload hand-off: outside the kernel's
     * DRAM the mapping is not cacheable, so DC ZVA in bzero and cache maintenance by VA are not
     * defined for it (a device-map bzero faults with an alignment abort, int-155). Zero with stores
     * and leave maintenance out: non-cacheable CPU writes are already visible to the SMMU. */
    map_=imported_->map(kIOMapAnywhere|kIOMapWriteCombineCache);
    if(!map_) {status=kIOReturnNoMemory;goto failed;}
    mapping_=reinterpret_cast<void *>(static_cast<uintptr_t>(map_->getVirtualAddress()));
    if(!mapping_ || ((uintptr_t)mapping_&4095))goto failed;
    span_capacity_=1;
    spans_=IONewData(NativeAdrenoPhysicalSpan,1);
    if(!spans_) {status=kIOReturnNoMemory;goto failed;}
    spans_[0]={0,physical,bytes};span_count_=1;
    {
        volatile uint64_t *words=static_cast<volatile uint64_t *>(mapping_);
        for(size_t at=0;at<bytes/sizeof(uint64_t);++at)words[at]=0;
    }
    return kIOReturnSuccess;
failed:
    (void)release();return status;
}
IOReturn NativeAdrenoMemory::borrow(void *mapping,uint64_t physical,size_t bytes)
{
    if(descriptor_ || grant_ || imported_ || spans_ || quarantined_ || borrowed_)return kIOReturnBusy;
    if(!mapping || ((uintptr_t)mapping&4095) || !physical || (physical&4095) || !bytes || (bytes&4095))return kIOReturnBadArgument;
    spans_=IONewData(NativeAdrenoPhysicalSpan,1);
    if(!spans_)return kIOReturnNoMemory;
    span_capacity_=1;span_count_=1;spans_[0]={0,physical,bytes};
    mapping_=mapping;bytes_=bytes;prepared_=true;borrowed_=true;
    bzero(mapping_,bytes);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::makeWriteCombined()
{
    if(pooled_)return write_combined_ && !quarantined_?kIOReturnSuccess:kIOReturnNotReady;   /* the chunk's attribute */
    if(!descriptor_ || system_ || imported_ || !prepared_ || quarantined_ || device_references_ || write_combined_ || !mapping_ || !span_count_)
        return kIOReturnNotReady;
    IOReturn status=NativeAdrenoMaintainOwnedPages(mapping_,bytes_,NativeAdrenoCleanInvalidate);
    if(status)return status;
    NativeAdrenoNotePhase(NativeAdrenoPhaseAllocate,bytes_);
    status=NativeAdrenoSetPagesCacheMode(spans_,span_count_,true);
    if(status)return status;
    write_combined_=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::declareSynchronized()
{
    if(!descriptor_ || system_ || imported_ || borrowed_ || !prepared_ || quarantined_ || device_references_ || !mapping_ || !span_count_)
        return kIOReturnNotReady;
    const IOReturn status=NativeAdrenoMaintainOwnedPages(mapping_,bytes_,NativeAdrenoCleanInvalidate);
    if(status)return status;
    NativeAdrenoNotePhase(NativeAdrenoPhaseAllocate,bytes_);
    synchronized_=true;return kIOReturnSuccess;
}
/* Frees of an owned BO whose first pages a user task still maps: [0] count, [1] the last mapping's page number. */
extern "C" {volatile uint32_t native_adreno_free_user_mapped[2];}
extern "C" unsigned int native_pmap_user_mappings(ppnum_t,void **,uint64_t *,char *) __attribute__((weak_import));
IOReturn NativeAdrenoMemory::release()
{
    native_adreno_trace_stage=30;
    if(device_references_ || quarantined_)return kIOReturnBusy;
    /* System memory's pages go back to the VM with the default attribute (vm_object_reap), as they came. */
    if(system_ || pooled_)write_combined_=false;   /* a pool chunk's pages keep the attribute */
    if(write_combined_) {
        /* The pages go back to the VM cacheable, as they came. */
        const IOReturn status=NativeAdrenoSetPagesCacheMode(spans_,span_count_,false);
        if(status) {quarantined_=true;return status;}
        write_combined_=false;
    }
    if(imported_) {
        if(map_) {map_->release();map_=nullptr;mapping_=nullptr;}
        if(prepared_) {
            IOReturn status=imported_->complete();
            if(status!=kIOReturnSuccess) {quarantined_=true;return status;}
            prepared_=false;
        }
        imported_->release();imported_=nullptr;
    }
    if(descriptor_ && !system_ && native_pmap_user_mappings && spans_) {
        /* A page a user task still maps (a BO view over physical pages holds no reference to them) is counted before the pages are freed:
         * the first page of up to 64 spans, then which process maps it. XNU frees such a page under the mapping (pmap_assert_free). A view
         * of system memory maps its VM object, which keeps the pages until the view goes. */
        for(size_t i=0;i<span_count_ && i<64;++i) {
            char name[17]={};void *pmap=nullptr;uint64_t va=0;
            if(native_pmap_user_mappings((ppnum_t)(spans_[i].physical>>12),&pmap,&va,name)) {
                native_adreno_free_user_mapped[0]=native_adreno_free_user_mapped[0]+1;
                native_adreno_free_user_mapped[1]=(uint32_t)(va>>12);
                break;
            }
        }
    }
    if(descriptor_) {
        if(prepared_) {
            native_adreno_trace_stage=31;
            IOReturn status=descriptor_->complete();
            native_adreno_trace_stage=32;
            if(status!=kIOReturnSuccess) {quarantined_=true;return status;}
            prepared_=false;
        }
        native_adreno_trace_stage=33;
        descriptor_->release();descriptor_=nullptr;
        native_adreno_trace_stage=34;
    }
    /* Pool pages: the BO's reference goes; the pages return to the pool once no client view holds the grant. */
    if(grant_) {NativeAdrenoPoolRelease(grant_);grant_=nullptr;prepared_=false;}
    if(owned_) {noteOwned(spans_,span_count_,false);owned_=false;}
    if(system_) {noteSystem(-(int64_t)bytes_,owner_pid_);system_=false;}
    if(import_noted_) {noteImport(-(int64_t)bytes_,owner_pid_);import_noted_=false;}
    if(borrowed_) {borrowed_=false;prepared_=false;}
    if(pooled_) {poolGive(pool_chunk_,pool_offset_,bytes_);pooled_=false;prepared_=false;pool_chunk_=0;pool_offset_=0;}
    if(spans_)IODeleteData(spans_,NativeAdrenoPhysicalSpan,span_capacity_);
    mapping_=nullptr;spans_=nullptr;bytes_=span_count_=span_capacity_=0;read_only_=false;owner_uncached_=false;synchronized_=false;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::releaseSystemInto(NativeAdrenoSystemPages *out)
{
    if(!out || out->held() || !system_ || !(descriptor_ || grant_) || !prepared_ || imported_ || device_references_ || quarantined_)return release();
    /* The GPU no longer maps the pages (the free unmapped them first) and every client view holds the descriptor itself, or its own
     * reference to the pool grant. */
    out->descriptor=descriptor_;out->grant=grant_;out->bytes=bytes_;out->write_combined=write_combined_;out->cleaned=false;
    descriptor_=nullptr;grant_=nullptr;prepared_=false;
    return release();
}
IOReturn NativeAdrenoMemory::holdForDevice()
{
    if(!backed() || !prepared_ || !span_count_ || quarantined_)return kIOReturnNotReady;
    if(device_references_==UINT32_MAX)return kIOReturnNoResources;
    ++device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::retireAfterQuiescence()
{
    if(!device_references_)return kIOReturnBadArgument;
    if(quarantined_)return kIOReturnNotReady;
    --device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::publishForDevice(size_t offset,size_t bytes,bool writes,bool include_write_combined,unsigned phase)
{
    if(!backed() || !prepared_ || quarantined_)return kIOReturnNotReady;
    if(!bytes || ((offset|bytes)&4095) || offset>bytes_ || bytes>bytes_-offset)return kIOReturnBadArgument;
    /* Pages every CPU mapping holds non-cacheable carry no line to clean. */
    if(!holdsCpuLines(include_write_combined))return kIOReturnSuccess;
    if(imported_) {NativeAdrenoNotePhase(phase,bytes);return maintainImported(offset,bytes,writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);}
    if(write_combined_) {  /* no CPU mapping should cache these pages; if one does, reach its lines through the physical pages */
        NativeAdrenoNotePhase(phase,bytes);
        return maintainImported(offset,bytes,writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);
    }
    NativeAdrenoNotePhase(phase,bytes);
    if(!mapping_)return maintainImported(offset,bytes,writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);   /* system memory */
    return NativeAdrenoMaintainOwnedPages(static_cast<unsigned char *>(mapping_)+offset,bytes,
        writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);
}
IOReturn NativeAdrenoMemory::acquireDeviceWrites(size_t offset,size_t bytes,unsigned phase)
{
    if(!backed() || !prepared_ || quarantined_)return kIOReturnNotReady;
    if(!bytes || ((offset|bytes)&4095) || offset>bytes_ || bytes>bytes_-offset)return kIOReturnBadArgument;
    if(grant_ || (imported_ && owner_uncached_))return kIOReturnSuccess;
    /* Imported pages may hold the owner's unrelated dirty lines next to the GPU's writes: clean as well as invalidate. */
    if(imported_) {NativeAdrenoNotePhase(phase,bytes);return maintainImported(offset,bytes,NativeAdrenoCleanInvalidate);}
    if(write_combined_)return kIOReturnSuccess;
    NativeAdrenoNotePhase(phase,bytes);
    if(!mapping_)return maintainImported(offset,bytes,NativeAdrenoInvalidate);   /* system memory */
    return NativeAdrenoMaintainOwnedPages(static_cast<unsigned char *>(mapping_)+offset,bytes,NativeAdrenoInvalidate);
}
/* Imported pages have no kernel mapping here (and write-combined ones only a non-cacheable one); maintain each span's part of
 * [offset, offset+bytes) by physical address. */
IOReturn NativeAdrenoMemory::maintainImported(size_t offset,size_t bytes,NativeAdrenoCacheOperation op)
{
    for(size_t i=0;i<span_count_;++i) {
        const NativeAdrenoPhysicalSpan &span=spans_[i];
        const size_t begin=offset>span.offset?offset:span.offset;
        const size_t end=offset+bytes<span.offset+span.bytes?offset+bytes:span.offset+span.bytes;
        if(begin>=end)continue;
        const IOReturn status=NativeAdrenoMaintainPhysicalPages(span.physical+(begin-span.offset),end-begin,op);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
