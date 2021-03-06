#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

#define BUF_SIZE PGSIZE/4

#define PRINT_DEBUG 0

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

struct segdesc gdt[NSEGS];

int deallocCount = 0;

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;
  //struct proc *curproc = 0;
  //struct cpu *curCpu = 0;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);

  // Map cpu, and curproc
  //c->gdt[SEG_KCPU] = SEG(STA_W, &c, 8, 0);

  lgdt(c->gdt, sizeof(c->gdt));
  /*loadgs(SEG_KCPU << 3);
  //mycpu() = c;
  //myproc() = 0;
  curproc = myproc();
  curCpu = mycpu();
  curCpu = c;
  curproc = 0;
  */
  /*
  // Initialize cpu-local storage.
  cpu = c;
  proc = 0;
  */
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

void fifoEntryRecord(char *va){
  int i;
  if(PRINT_DEBUG)
    cprintf("rnp pid:%d count:%d va:0x%x\n", myproc()->pid, myproc()->pagesInPhyMem, va);
  for (i = 0; i < MAX_PSYC_PAGES; i++)
    if (myproc()->pagesFreedARR[i].virtualAddress == (char*)0xffffffff)
      goto foundrnp;
  if(PRINT_DEBUG)
    cprintf("panic follows, pid:%d, name:%s\n", myproc()->pid, myproc()->name);
  panic("NewPageRecord: no free pages");
foundrnp:
  myproc()->pagesFreedARR[i].virtualAddress = va;
  myproc()->pagesFreedARR[i].next = myproc()->head;
  myproc()->head = &myproc()->pagesFreedARR[i];
}


void NewPageRecord(char *va) {
  if(PRINT_DEBUG)
    cprintf("NewPageRecord: %s is calling fifoRecord with: 0x%x\n", myproc()->name, va);
  fifoEntryRecord(va);
  myproc()->pagesInPhyMem++;
  if(PRINT_DEBUG)
    cprintf("\n------------------- proc->pagesinmem ------------------ : %d\n", myproc()->pagesInPhyMem);
}



struct emptyPages *fifoWrite() {
  int i;
  struct emptyPages *temp, *l;
  for (i = 0; i < MAX_PSYC_PAGES; i++){
    if (myproc()->pagesSwappedARR[i].virtualAddress == (char*)0xffffffff)
      goto foundswappedpageslot;
  }
  panic("PageWriteInFile: FIFO no slot for swapped page");
foundswappedpageslot:
  temp = myproc()->head;
  if (temp == 0)
    panic("fifoWrite: proc->head is NULL");
  if (temp->next == 0)
    panic("fifoWrite: single page in phys mem");
  // find the before-last temp in the used pages list
  while (temp->next->next != 0)
    temp = temp->next;
  l = temp->next;
  temp->next = 0;

  if(PRINT_DEBUG){
    cprintf("FIFO chose to page out page starting at 0x%x \n\n", l->virtualAddress);
  }

  myproc()->pagesSwappedARR[i].virtualAddress = l->virtualAddress;
  int num = 0;
  if ((num = writeToSwapFile(myproc(), (char*)PTE_ADDR(l->virtualAddress), i * PGSIZE, PGSIZE)) == 0)
    return 0;
  pte_t *pte1 = walkpgdir(myproc()->pgdir, (void*)l->virtualAddress, 0);
  if (!*pte1)
    panic("PageWriteInFile: pte1 is empty");
  // pte_t *pte2 = walkpgdir(proc->pgdir, addr, 1);
  // // if (!*pte2)
  // //   panic("PageWriteInFile: pte2 is empty");
  // *pte2 = PTE_ADDR(*pte1) | PTE_U | PTE_P | PTE_W;
  kfree((char*)PTE_ADDR(P2V_WO(*walkpgdir(myproc()->pgdir, l->virtualAddress, 0))));
  *pte1 = PTE_W | PTE_U | PTE_PG;
  ++myproc()->pagesInSwapFile;
  if(PRINT_DEBUG) cprintf("writePage:proc->pagesinswapfile:%d\n", myproc()->pagesInSwapFile);
  lcr3(V2P(myproc()->pgdir));
  return l;
}


struct emptyPages *PageWriteInFile(char* va) {
  return fifoWrite();
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;
  uint newpage = 1;
  struct emptyPages *l;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){

    if(myproc()->pagesInPhyMem >= MAX_PSYC_PAGES) {
      
      if(PRINT_DEBUG) cprintf("writing to swap file, proc->name: %s, pagesinmem: %d\n", myproc()->name, myproc()->pagesInPhyMem);

      if ((l = PageWriteInFile((char*)a)) == 0)
        panic("allocuvm: error writing page to swap file");

      //TODO: these FIFO specific steps don't belong here!
      // they should move to a FIFO specific functiom!
      //TODO cprintf("allocuvm: FIFO's little part\n");
      l->virtualAddress = (char*)a;
      l->next = myproc()->head;
      myproc()->head = l;
      newpage = 0;

    }

    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    if (newpage){
      //TODO delete 
      if(PRINT_DEBUG) cprintf("\nnewpage = 1\n");
      
      if(PRINT_DEBUG) cprintf("recorded new page, proc->name: %s, pagesinmem: %d\n", myproc()->name, myproc()->pagesInPhyMem);
      NewPageRecord((char*)a);
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;
   int i=0;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      if (myproc()->pgdir == pgdir) {
        /*
        The process itself is deallocating pages via sbrk() with a negative
        argument. Update proc's data structure accordingly.
        */

        for (i = 0; i < MAX_PSYC_PAGES; i++) {
          if (myproc()->pagesFreedARR[i].virtualAddress == (char*)a)
            goto UpdateFIFOarr;
        }

        panic("deallocuvm: entry not found in proc->pagesFreedARR");
UpdateFIFOarr:
        myproc()->pagesFreedARR[i].virtualAddress = (char*) 0xffffffff;
        if (myproc()->head == &myproc()->pagesFreedARR[i])
          myproc()->head = myproc()->pagesFreedARR[i].next;
        else {
          struct emptyPages *l = myproc()->head;
          while (l->next != &myproc()->pagesFreedARR[i])
            l = l->next;
          l->next = myproc()->pagesFreedARR[i].next;
        }
        myproc()->pagesFreedARR[i].next = 0;
        myproc()->pagesInPhyMem--;
      }
      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
    }
    else if (*pte & PTE_PG && myproc()->pgdir == pgdir) {
      /*
      The process itself is deallocating pages via sbrk() with a negative
      argument. Update proc's data structure accordingly.
      */
        for (i = 0; i < MAX_PSYC_PAGES; i++) {
          if (myproc()->pagesSwappedARR[i].virtualAddress == (char*)a)
            goto UpdateFIFOarrG;
        }
        panic("deallocuvm: entry not found in proc->pagesSwappedARR");
UpdateFIFOarrG:
        myproc()->pagesSwappedARR[i].virtualAddress = (char*) 0xffffffff;
        myproc()->pagesSwappedARR[i].swaploc = 0;
        myproc()->pagesInSwapFile--;
    }

  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P) && !(*pte & PTE_PG))
      panic("copyuvm: page not present");
    if (*pte & PTE_PG) {
      cprintf("copyuvm PTR_PG\n"); // TODO delete
      pte = walkpgdir(d, (void*) i, 1);
      *pte = PTE_U | PTE_W | PTE_PG;
      continue;
    }
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0) {
      kfree(mem);
      goto bad;
    }
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}


void fifoSwap(uint addr){
  int i, j;
  char buffer[BUF_SIZE];
  pte_t *pte1, *pte2;
  struct emptyPages *l;
  struct emptyPages *link_temp = myproc()->head;
  
  if (link_temp == 0)
    panic("fifoSwap: proc->head is NULL");
  if (link_temp->next == 0)
    panic("fifoSwap: single page in phys mem");
  // find the before-last link_temp in the used pages list
  while (link_temp->next->next != 0)
    link_temp = link_temp->next;
  l = link_temp->next;
  link_temp->next = 0;

  if(PRINT_DEBUG){
    cprintf("FIFO chose to page out page starting at 0x%x \n\n", l->virtualAddress);
  }

  //find the address of the page table entry to copy into the swap file
  pte1 = walkpgdir(myproc()->pgdir, (void*)l->virtualAddress, 0);
  if (!*pte1)
    panic("swapFile: FIFO pte1 is empty");
  //find a swap file page descriptor slot
  for (i = 0; i < MAX_PSYC_PAGES; i++)
    if (myproc()->pagesSwappedARR[i].virtualAddress == (char*)PTE_ADDR(addr))
      goto SWAPPEDSLOTFOUND;
  panic("swappages");
SWAPPEDSLOTFOUND:
  //update relevant fields in proc
  myproc()->pagesSwappedARR[i].virtualAddress = l->virtualAddress;
  //assign the physical page to addr in the relevant page table
  pte2 = walkpgdir(myproc()->pgdir, (void*)addr, 0);
  if (!*pte2)
    panic("swapFile: FIFO pte2 is empty");
  //set page table entry
  *pte2 = PTE_ADDR(*pte1) | PTE_U | PTE_W | PTE_P;
  for (j = 0; j < 4; j++) {
    int loc = (i * PGSIZE) + ((PGSIZE / 4) * j);
    // cprintf("i:%d j:%d loc:0x%x\n", i,j,loc);//TODO delete
    int addroffset = ((PGSIZE / 4) * j);
    memset(buffer, 0, BUF_SIZE);
    //copy the new page from the swap file to buffer
    readFromSwapFile(myproc(), buffer, loc, BUF_SIZE);
    //copy the old page from the memory to the swap file
    //written =
    writeToSwapFile(myproc(), (char*)(P2V_WO(PTE_ADDR(*pte1)) + addroffset), loc, BUF_SIZE);
    //copy the new page from buffer to the memory
    memmove((void*)(PTE_ADDR(addr) + addroffset), (void*)buffer, BUF_SIZE);
  }
  //update the page table entry flags, reset the physical page address
  *pte1 = PTE_U | PTE_W | PTE_PG;
  //update l to hold the new va
  l->next = myproc()->head;
  myproc()->head = l;
  l->virtualAddress = (char*)PTE_ADDR(addr);
}

void swapPages(uint addr) {
  struct proc *proc = myproc();
  if (strncmp(proc->name, "init",4) == 0 || strncmp(proc->name, "sh", 2) == 0) {
    proc->pagesInPhyMem++;
    return;
  }
  fifoSwap(addr);
  lcr3(V2P(proc->pgdir));
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

