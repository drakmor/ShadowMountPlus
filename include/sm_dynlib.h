#ifndef SM_DYNLIB_H
#define SM_DYNLIB_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

// Fixed SDK object ABI; kernel_dynlib_obj copies the complete object.
typedef struct {
  unsigned long next;
  unsigned long path;
  unsigned long unknown0[2];
  unsigned int refcount;
  unsigned long handle;

  unsigned long mapbase;
  unsigned long mapsize;
  unsigned long textsize;

  unsigned long database;
  unsigned long datasize;

  unsigned long unknown1;
  unsigned long unknown1size;

  unsigned long entry;
  unsigned long unknown2;
  unsigned long vaddrbase;

  unsigned int tlsindex;
  unsigned long tlsinit;
  unsigned long tlsinitsize;
  unsigned long tlssize;
  unsigned long tlsoffset;
  unsigned long tlsalign;

  unsigned long pltgot;

  unsigned long unknown3[6];

  unsigned long init;
  unsigned long fini;

  unsigned long eh_frame_hdr;
  unsigned long eh_frame_hdr_size;

  unsigned long eh_frame;
  unsigned long eh_frame_size;

  int status;
  int flags;

  unsigned long unknown4[5];
  unsigned long dynsec;
  unsigned long unknown5[6];  // fingerprint?
} dynlib_obj_t;


int kernel_dynlib_obj(pid_t pid, unsigned int handle, dynlib_obj_t *object);
_Static_assert(sizeof(dynlib_obj_t) == 0x180, "SDK dynlib object size");
_Static_assert(offsetof(dynlib_obj_t, dynsec) == 0x148, "SDK dynlib dynamic section");

// Only the prefix needed for ELF relocation tables is read.
typedef struct {
  uint64_t next, previous, sysvec;
  uint32_t refcount, padding;
  uint64_t size, symtab, symtabsize, strtab, strtabsize;
  uint64_t pltrela, pltrelasize, rela, relasize;
} sm_dynlib_relocations_t;
_Static_assert(offsetof(sm_dynlib_relocations_t, pltrela) == 0x48, "SDK PLT relocations");
#endif
