/* unwind_pr.c - ARM-defined model personality routines
 *
 * Copyright 2002-2007 ARM Limited. All rights reserved.
 *
 * Your rights to use this code are set out in the accompanying licence
 * text file LICENCE.txt (ARM contract number LEC-ELA-00080 v2.0).
 */
/*
 * RCS $Revision: 150876 $
 * Checkin $Date: 2009-12-22 16:25:04 +0000 (Tue, 22 Dec 2009) $
 * Revising $Author: agrant $
 */

#include <cstdlib>
/* Environment: */
#include "unwind_env.h"
/* Language-independent unwinder declarations: */
#include "unwinder.h"

/* Define PR_DIAGNOSTICS for printed diagnostics from the personality routine */

#ifdef PR_DIAGNOSTICS
extern int printf(const char *, ...);
#endif


/* Forward decl: */
extern _Unwind_Reason_Code __ARM_unwind_cpp_prcommon(_Unwind_State state,
                                                     _Unwind_Control_Block *ucbp,
                                                     _Unwind_Context *context,
                                                     uint32_t idx);

/* Personality routines - external entry points.
 * pr0: short unwind description, 16 bit EHT offsets.
 * pr1: long unwind description, 16 bit EHT offsets.
 * pr2: long unwind description, 32 bit EHT offsets.
 */

#ifdef pr0_c
_Unwind_Reason_Code __aeabi_unwind_cpp_pr0(_Unwind_State state,
                                           _Unwind_Control_Block *ucbp,
                                           _Unwind_Context *context) {
  return __ARM_unwind_cpp_prcommon(state, ucbp, context, 0);
}
#endif

#ifdef pr1_c
_Unwind_Reason_Code __aeabi_unwind_cpp_pr1(_Unwind_State state,
                                           _Unwind_Control_Block *ucbp,
                                           _Unwind_Context *context) {
  return __ARM_unwind_cpp_prcommon(state, ucbp, context, 1);
}
#endif

#ifdef pr2_c
_Unwind_Reason_Code __aeabi_unwind_cpp_pr2(_Unwind_State state,
                                           _Unwind_Control_Block *ucbp,
                                           _Unwind_Context *context) {
  return __ARM_unwind_cpp_prcommon(state, ucbp, context, 2);
}
#endif

/* The rest of the file deals with the common routine */

#ifdef prcommon_c

/* C++ exceptions ABI required here:
 * Declare protocol routines called by the personality routine.
 * These are weak references so that referencing them here is
 * insufficient to pull them into the image - they will only be
 * included if application code uses a __cxa routine.
 */

typedef unsigned char bool;
static const bool false = 0;
static const bool true = !false;

typedef struct _ZSt9type_info type_info; /* This names C++ type_info type */

WEAKDECL void __cxa_call_unexpected(_Unwind_Control_Block *ucbp);
WEAKDECL bool __cxa_begin_cleanup(_Unwind_Control_Block *ucbp);
typedef enum {
    ctm_failed = 0,
    ctm_succeeded = 1,
    ctm_succeeded_with_ptr_to_base = 2
  } __cxa_type_match_result;
WEAKDECL __cxa_type_match_result __cxa_type_match(_Unwind_Control_Block *ucbp,
                                                  const type_info *rttip,
                                                  bool is_reference_type,
                                                  void **matched_object);

/* ----- Helper routines, private ----- */

/* R_ARM_PREL31 is a place-relative 31-bit signed relocation.  The
 * routine takes the address of a location that was relocated by
 * R_ARM_PREL31, and returns an absolute address.
 */
static FORCEINLINE uint32_t __ARM_resolve_prel31(void *p)
{
  return (uint32_t)((((*(int32_t *)p) << 1) >> 1) + (int32_t)p);
}

/* --------- VRS manipulation: --------- */

#define R_SP 13
#define R_LR 14
#define R_PC 15

static FORCEINLINE uint32_t core_get(_Unwind_Context *context, uint32_t regno)
{
  uint32_t val;
  /* This call is required to never fail if given a valid regno */
  _Unwind_VRS_Get(context, _UVRSC_CORE, regno, _UVRSD_UINT32, &val);
  return val;
}

static FORCEINLINE void core_set(_Unwind_Context *context, uint32_t regno, uint32_t newval)
{
  /* This call is required to never fail if given a valid regno */
  _Unwind_VRS_Set(context, _UVRSC_CORE, regno, _UVRSD_UINT32, &newval);
}

static FORCEINLINE uint32_t count_to_mask(uint32_t count) {
  return (1 << count) - 1;
}

/* --------- Support for unwind instruction stream: --------- */

#define CODE_FINISH (0xb0)

typedef struct uwdata {
  uint32_t unwind_word;                  /* current word of unwind description */
  uint32_t *unwind_word_pointer;         /* ptr to next word */
  uint8_t unwind_word_bytes_remaining;   /* count of bytes left in current word */
  uint8_t unwind_words_remaining;        /* count of words left, at ptr onwards */
} uwdata;

static INLINE uint8_t next_unwind_byte(uwdata *u) {
  uint8_t ub;
  if (u->unwind_word_bytes_remaining == 0) {  /* Load another word */
    if (u->unwind_words_remaining == 0) return CODE_FINISH; /* nothing left - yield NOP */
    u->unwind_words_remaining--;
    u->unwind_word = *(u->unwind_word_pointer++);
    u->unwind_word_bytes_remaining = 4;
  }
  
  u->unwind_word_bytes_remaining--;
  ub = (u->unwind_word & 0xff000000) >> 24;
  u->unwind_word <<= 8;
  return ub;
}


/* --------- Personality routines: --------- */

/* The C++ Standard is silent on what is supposed to happen if an internal
 * inconsistency occurs during unwinding. In our design, we return to the
 * caller with _URC_FAILURE. During phase 1 this causes a return from the
 * language-independent unwinder to its caller (__cxa_throw or __cxa_rethrow)
 * which will then call terminate(). If an error occurs during phase 2, the
 * caller will call abort().
 */

/* Types to assist with reading EHT's */

typedef struct {
  uint16_t length;
  uint16_t offset;
} EHT16;

typedef struct {
  uint32_t length;
  uint32_t offset;
} EHT32;

typedef uint32_t landingpad_t;

typedef struct {
  landingpad_t landingpad;
} EHT_cleanup_tail;

typedef struct {
  landingpad_t landingpad;
  uint32_t rtti_ref;
} EHT_catch_tail;

typedef struct {
  uint32_t rtti_count;           /* table count (possibly 0) */
  uint32_t (rtti_refs[1]);       /* variable length table, possibly followed by landing pad */
} EHT_fnspec_tail;


/* Macros: */

/* Barrier cache: */
/* Requirement imposed by C++ semantics module - pointer to match object in slot 0: */
#define BARRIER_HANDLEROBJECT (0)
/* Requirement imposed by C++ semantics module - function exception spec info */
#define BARRIER_FNSPECCOUNT  (1)
#define BARRIER_FNSPECBASE   (2)
#define BARRIER_FNSPECSTRIDE (3)
#define BARRIER_FNSPECARRAY  (4)
/* Private use for us until catch handler entry complete: */
#define BARRIER_TEMPORARYMATCHOBJECT (1)
/* Private use for us between phase 1 & 2: */
#define BARRIER_EHTP (2)

#define SAVE_CATCH_PROPAGATION_BARRIER(UCB_PTR,VSP,EHTP,HANDLEROBJECT) \
  (UCB_PTR)->barrier_cache.sp = (VSP);    \
  (UCB_PTR)->barrier_cache.bitpattern[BARRIER_EHTP] = (uint32_t)(EHTP); \
  (UCB_PTR)->barrier_cache.bitpattern[BARRIER_HANDLEROBJECT] = (uint32_t)(HANDLEROBJECT);

#define SAVE_CATCH_OF_BASEPTR_PROPAGATION_BARRIER(UCB_PTR,VSP,EHTP,HANDLEROBJECT) \
  (UCB_PTR)->barrier_cache.sp = (VSP);    \
  (UCB_PTR)->barrier_cache.bitpattern[BARRIER_EHTP] = (uint32_t)(EHTP); \
  (UCB_PTR)->barrier_cache.bitpattern[BARRIER_TEMPORARYMATCHOBJECT] = (uint32_t)(HANDLEROBJECT); \
  (UCB_PTR)->barrier_cache.bitpattern[BARRIER_HANDLEROBJECT] = (uint32_t)&((UCB_PTR)->barrier_cache.bitpattern[BARRIER_TEMPORARYMATCHOBJECT]);

#define SAVE_FNSPEC_PROPAGATION_BARRIER(UCB_PTR,VSP,EHTP) \
  (UCB_PTR)->barrier_cache.sp = (VSP);    \
  (UCB_PTR)->barrier_cache.bitpattern[BARRIER_EHTP] = (uint32_t)(EHTP); \
  (UCB_PTR)->barrier_cache.bitpattern[BARRIER_HANDLEROBJECT] = (uint32_t)0;

#define CHECK_FOR_PROPAGATION_BARRIER(UCB_PTR,VSP,EHTP) \
   ((UCB_PTR)->barrier_cache.sp == (VSP) &&    \
    (UCB_PTR)->barrier_cache.bitpattern[BARRIER_EHTP] == (uint32_t)(EHTP))


/* Cleanup cache */
#define CLEANUP_FNSTART   (0)
#define CLEANUP_EHTPSTART (1)
#define CLEANUP_EHTPNEXT  (2)


/* Special catch rtti values */
#define CATCH_ALL               (0xffffffff)
#define CATCH_ALL_AND_TERMINATE (0xfffffffe)
/* Landing pad bit for catching a reference type */
#define CATCH_REFERENCE         (0x80000000)


/* Common personality routine: receives pr index as an argument.
 *
 * Note this implementation contains no explicit check against attempting to
 * unwind off the top of the stack. Instead it relies (in cooperation with
 * the language-independent unwinder) on there being a propagation barrier
 * somewhere on the stack, perhaps the caller to main being not
 * unwindable. An alternative would be to check for the stack pointer
 * addressing a stack limit symbol.
 */

_Unwind_Reason_Code __ARM_unwind_cpp_prcommon(_Unwind_State state,
                                              _Unwind_Control_Block *ucbp,
                                              _Unwind_Context *context,
                                              uint32_t idx)
{
  _Unwind_EHT_Header *eht_startp;  /* EHT start pointer */
  bool is_real_table; /* False if EHT is inline in index table */
  uint8_t *ehtp; /* EHT pointer, incremented as required */
  /* Flag for fnspec violations in which the frame should be unwound before calling unexpected() */
  bool phase2_call_unexpected_after_unwind = false;
  /* Flag for whether we have loaded r15 (pc) with a return address while executing
   * unwind instructions.
   * Set this on any write to r15 while executing the unwind instructions.
   */
  bool wrote_pc = false;
  /* Flag for whether we have loaded r14 (lr) with a return address while executing
   * unwind instructions.
   * Set this on any write to r14 while executing the unwind instructions.
   */
  bool wrote_lr = false;
  /* Flag for whether we loaded r15 from r14 while executing the unwind instructions */
  bool wrote_pc_from_lr = false;
  uwdata ud;

  /* What are we supposed to do? */

  if (state != _US_VIRTUAL_UNWIND_FRAME &&
      state != _US_UNWIND_FRAME_STARTING &&
      state != _US_UNWIND_FRAME_RESUME) {
#ifdef PR_DIAGNOSTICS
    printf("PR entered: state=%d (bad)\n", state);
#endif
    DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_UNSPECIFIED);
    return _URC_FAILURE;
  }

  /* Traverse the current EHT, if there is one.
   * The required behaviours are:
   * _US_VIRTUAL_UNWIND_FRAME: search for a propagation barrier in this frame.
   * otherwise look for the propagation barrier we found in phase 1,
   * performing cleanups on the way. In this case if state will be one of:
   *   _US_UNWIND_FRAME_STARTING  first time with this frame
   *   _US_UNWIND_FRAME_RESUME    not first time, we are part-way through the EHT.
   */

  if (state == _US_UNWIND_FRAME_RESUME) {
    /* Partway through processing a real EHT. Recover saved pointers */
    ucbp->pr_cache.fnstart = ucbp->cleanup_cache.bitpattern[CLEANUP_FNSTART];
    eht_startp = (_Unwind_EHT_Header *)ucbp->cleanup_cache.bitpattern[CLEANUP_EHTPSTART];
    ehtp = (uint8_t *)ucbp->cleanup_cache.bitpattern[CLEANUP_EHTPNEXT];
    is_real_table = true;
#ifdef PR_DIAGNOSTICS
    printf("PR resumed: state=%d, fnstart=0x%x, EHTstart 0x%x, EHTnextentry 0x%x\n",
           state, ucbp->pr_cache.fnstart, (int)eht_startp, (int)ehtp);
#endif

  } else if ((ucbp->pr_cache.additional & 1) == 0) {
    /* A real EHT, not inline in the index table.
     * Point at the first EHT entry.
     * For pr0, the unwind description is entirely within the header word.
     * For pr1 & pr2, an unwind description extension word count is
     * held in bits 16-23 of the header word.
     */
    uint32_t unwind_extension_word_count;
    eht_startp = ucbp->pr_cache.ehtp;
    unwind_extension_word_count = (idx == 0 ? 0 : ((*eht_startp) >> 16) & 0xff);
    ehtp = (uint8_t *)(eht_startp + 1 + unwind_extension_word_count);
    is_real_table = true;
#ifdef PR_DIAGNOSTICS
  printf("PR entered: state=%d, r15=0x%x, fnstart=0x%x, EHTstart 0x%x, EHTfirstentry 0x%x\n",
         state, core_get(context, R_PC),
         ucbp->pr_cache.fnstart, (int)eht_startp, (int)ehtp);
#endif

  } else {
    /* An inline EHT. No actions to process. */
    eht_startp = ucbp->pr_cache.ehtp;
    is_real_table = false;
#ifdef PR_DIAGNOSTICS
  printf("PR entered: state=%d, r15=0x%x, fnstart=0x%x, inline table at 0x%x\n",
         state, core_get(context, R_PC), ucbp->pr_cache.fnstart, (int)eht_startp);
#endif
  }

  /* scan ... */

  if (is_real_table) {

    while (1) {
      
      /* Extract 32 bit length and offset */
      uint32_t length;
      uint32_t offset;
      if (idx == 2) {
        /* 32 bit offsets */
        length = ((EHT32 *)ehtp)->length;
        if (length == 0) break; /* end of table */
        offset = ((EHT32 *)ehtp)->offset;
        ehtp += sizeof(EHT32);
      } else {
        /* 16 bit offsets */
        length = ((EHT16 *)ehtp)->length;
        if (length == 0) break; /* end of table */
        offset = ((EHT16 *)ehtp)->offset;
        ehtp += sizeof(EHT16);
      }
      
#ifdef PR_DIAGNOSTICS
      printf("PR Got entry at 0x%x code=%d, length=0x%x, offset=0x%x\n",
             (int)(ehtp-4), ((offset & 1) << 1) | (length & 1),
             length & ~1, offset & ~1);
#endif

      /* Dispatch on the kind of entry */
      switch (((offset & 1) << 1) | (length & 1)) {
      case 0: /* cleanup */
        if (state == _US_VIRTUAL_UNWIND_FRAME) {
          /* Not a propagation barrier - skip */
        } else {
          /* Phase 2: call the cleanup if the return address is in range */
          uint32_t padaddress;
          uint32_t rangestartaddr = ucbp->pr_cache.fnstart + offset;
          uint32_t rtn_addr = core_get(context, R_PC);
          if (rangestartaddr <= rtn_addr && rtn_addr < rangestartaddr + length) {
            /* It is in range. */
            landingpad_t *landingpadp = &((EHT_cleanup_tail *)ehtp)->landingpad;
            ehtp += sizeof(EHT_cleanup_tail);
            /* Dump state into the ECO so we resume correctly after the cleanup. */
            /* We simply save the address of the next EHT entry. */
            ucbp->cleanup_cache.bitpattern[CLEANUP_FNSTART] = ucbp->pr_cache.fnstart;
            ucbp->cleanup_cache.bitpattern[CLEANUP_EHTPSTART] = (uint32_t)eht_startp;
            ucbp->cleanup_cache.bitpattern[CLEANUP_EHTPNEXT] = (uint32_t)ehtp;
            if (!__cxa_begin_cleanup(ucbp)) {
              /* Should be impossible, using ARM's library */
              DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_UNSPECIFIED);
              return _URC_FAILURE;
            }
            /* Set up the VRS to enter the landing pad. */
            padaddress = __ARM_resolve_prel31(landingpadp);
            core_set(context, R_PC, padaddress);
#ifdef PR_DIAGNOSTICS
            printf("PR Got cleanup in range, cleanup addr=0x%x\n", core_get(context, R_PC));
            printf("PR Saving fnstart 0x%x, EHTstart 0x%x, EHTnextentry 0x%x\n",
                   ucbp->pr_cache.fnstart, (int)eht_startp, (int)ehtp);
#endif
            DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_PADENTRY, padaddress);
            /* Exit requesting upload the VRS to the real machine. */
           return _URC_INSTALL_CONTEXT;
          }
        }
        /* Phase 1, or phase 2 and not in range */
        ehtp += sizeof(EHT_cleanup_tail);
        break;
      case 1: /* catch */
        {
          if (state == _US_VIRTUAL_UNWIND_FRAME) {
            /* In range, and with a matching type? */
            uint32_t rangestartaddr = ucbp->pr_cache.fnstart + offset;
            uint32_t rtn_addr = core_get(context, R_PC);
            void *matched_object;
            length -= 1;   /* length had low bit set - clear it */
            if (rangestartaddr <= rtn_addr && rtn_addr < rangestartaddr + length) {
              /* In range */
              __cxa_type_match_result matched_result;
              uint32_t *rtti_ref = &((EHT_catch_tail *)ehtp)->rtti_ref;
              uint32_t rtti_val = *rtti_ref;
              if (rtti_val == CATCH_ALL_AND_TERMINATE) {
                /* Always matches and causes propagation failure in phase 1 */
#ifdef PR_DIAGNOSTICS
                printf("PR Got CATCH_ALL_AND_TERMINATE in phase 1\n");
#endif
                DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_NOUNWIND);
                return _URC_FAILURE;    
              } else if (rtti_val == CATCH_ALL) {
                matched_object = ucbp + 1;
                matched_result = ctm_succeeded;
              } else {
                bool is_reference_type = ((uint32_t)(((EHT_catch_tail *)ehtp)->landingpad) & CATCH_REFERENCE)
                                                           == CATCH_REFERENCE;
                rtti_val = (uint32_t)__ARM_resolve_target2((void *)rtti_ref);
                matched_result =__cxa_type_match(ucbp,
                                                 (type_info *)rtti_val,
                                                 is_reference_type,
                                                 &matched_object);
              }
              if (matched_result != ctm_failed) {
                /* In range and matches.
                 * Record the propagation barrier details for ease of detection in phase 2.
                 * We save a pointer to the middle of the handler entry -
                 * this is fine, so long as we are consistent about it.
                 */
#ifdef PR_DIAGNOSTICS
                printf("PR Got barrier in phase 1, result %d\n", (int)matched_result);
                printf("PR Matched object address 0x%8.8x\n", matched_object); 
#endif
                if (matched_result == ctm_succeeded_with_ptr_to_base) {
                  SAVE_CATCH_OF_BASEPTR_PROPAGATION_BARRIER(ucbp, core_get(context, R_SP),
                                                            ehtp, matched_object);

                } else {
                  SAVE_CATCH_PROPAGATION_BARRIER(ucbp, core_get(context, R_SP),
                                                 ehtp, matched_object);
                }
                DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_BARRIERFOUND,
                                    __ARM_resolve_prel31(&((EHT_catch_tail *)ehtp)->landingpad));
                DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_CPP_TYPEINFO, rtti_val);
                return _URC_HANDLER_FOUND;
              }
            }
            /* Not in range or no type match - fall thru to carry on scanning the table */
          }
          else {
            /* Else this is phase 2: have we encountered the saved barrier? */
            if (CHECK_FOR_PROPAGATION_BARRIER(ucbp, core_get(context, R_SP), ehtp)) {
              /* Yes we have.
               * Set up the VRS to enter the landing pad,
               * and upload the VRS to the real machine.
               */
              landingpad_t *landingpadp = &((EHT_catch_tail *)ehtp)->landingpad;
              uint32_t padaddress = __ARM_resolve_prel31(landingpadp);
#ifdef PR_DIAGNOSTICS
              printf("PR Got catch barrier in phase 2\n");
#endif
              core_set(context, R_PC, padaddress);
              core_set(context, 0, (uint32_t)ucbp);
              DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_PADENTRY, padaddress);
              /* Exit requesting upload the VRS to the real machine. */
              return _URC_INSTALL_CONTEXT;
            }
          }
          /* Else carry on scanning the table */
          ehtp += sizeof(EHT_catch_tail);
          break;
        }
      case 2: /* function exception specification (fnspec) */
        {
          uint32_t counter_word = ((EHT_fnspec_tail *)ehtp)->rtti_count;
          uint32_t rtti_count = counter_word & 0x7fffffff;   /* Extract offset count */
          if (state == _US_VIRTUAL_UNWIND_FRAME) {
            /* Phase 1 */
            /* In range? Offset had low bit set - clear it */
            uint32_t rangestartaddr = ucbp->pr_cache.fnstart + offset - 1;
            uint32_t rtn_addr = core_get(context, R_PC);
            if (rangestartaddr <= rtn_addr && rtn_addr < rangestartaddr + length) {
              /* See if any type matches */
              uint32_t *rttipp = &((EHT_fnspec_tail *)ehtp)->rtti_refs[0];
              uint32_t i;
              for (i = 0; i < rtti_count; i++) {
                 void *matched_object;
                 if (__cxa_type_match(ucbp,
                                      (type_info *)__ARM_resolve_target2(rttipp),
                                      false,
                                      &matched_object) != ctm_failed) {
#ifdef PR_DIAGNOSTICS
                   printf("PR Fnspec matched in phase 1\n");
#endif
                   break;
                 }
                 rttipp++;
              }

              if (i == rtti_count) { /* NB case rtti_count==0 forces no match [for throw()] */
                /* No match - fnspec violation is a propagation barrier */
#ifdef PR_DIAGNOSTICS
                printf("PR Got fnspec barrier in phase 1\n");
#endif
                SAVE_FNSPEC_PROPAGATION_BARRIER(ucbp, core_get(context, R_SP), ehtp); /* save ptr to the count of types */
                /* Even if this is a fnspec with a landing pad, we always end up in
                 * __cxa_call_unexpected so tell the debugger thats where we're going
                 */
                DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_BARRIERFOUND, &__cxa_call_unexpected);
                return _URC_HANDLER_FOUND;
              }
            } /* if (in range...) */

            /* Fall out of the 'if' to continue table scanning */

          } else {

            bool unwind_fnspec_stop;

            /* Else this is phase 2: have we encountered the saved barrier? */
            unwind_fnspec_stop =
                CHECK_FOR_PROPAGATION_BARRIER(ucbp, core_get(context, R_SP), ehtp);

            if (unwind_fnspec_stop) {
              /* Stop here. Fill in the UCB barrier_cache for entry to __cxa_call_unexpected */
              uint32_t *p = (uint32_t *)ehtp; /* ptr to rtti count */
              ucbp->barrier_cache.bitpattern[BARRIER_FNSPECCOUNT] = rtti_count;
              ucbp->barrier_cache.bitpattern[BARRIER_FNSPECBASE] = 0; /* was base address - unused now */
              ucbp->barrier_cache.bitpattern[BARRIER_FNSPECSTRIDE] = 4; /* stride */
              ucbp->barrier_cache.bitpattern[BARRIER_FNSPECARRAY]  = (uint32_t)(p + 1); /* address of rtti offset list */

              /* If this is a fnspec with an attached landing pad, we must enter
               * the pad immediately. Otherwise we need to unwind the frame before
               * calling __cxa_call_unexpected() so set a flag to make this happen.
               */
              if (counter_word == rtti_count)
                phase2_call_unexpected_after_unwind = true; /* no pad, enter later */
              else { /* pad */
                landingpad_t *landingpadp;
                uint32_t padaddress;
#ifdef PR_DIAGNOSTICS
                printf("PR Got fnspec barrier in phase 2 (immediate entry)\n");
#endif
                ehtp += (sizeof(((EHT_fnspec_tail *)ehtp)->rtti_count) +
                         sizeof(uint32_t) * rtti_count);  /* point at pad offset */
                landingpadp = (landingpad_t *)ehtp;
                padaddress = __ARM_resolve_prel31(landingpadp);
                core_set(context, 0, (uint32_t)ucbp);
                core_set(context, R_PC, padaddress);
                /* Even if this is a fnspec with a landing pad, in phase 1 we said we'd
                 * end up in __cxa_call_unexpected so show the same thing now
                 */
                DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_PADENTRY, &__cxa_call_unexpected);
                return _URC_INSTALL_CONTEXT;
              }
            } /* endif (barrier match) */
          } /* endif (which phase) */
          
          /* Advance to the next item, remembering to skip the landing pad if present */
          ehtp += (sizeof(((EHT_fnspec_tail *)ehtp)->rtti_count) +
                   sizeof(uint32_t) * rtti_count +
                   (counter_word == rtti_count ? 0 : sizeof(landingpad_t)));
          break;
        }
      case 3: /* unallocated */
        DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_TABLECORRUPT);
        return _URC_FAILURE;
      } /* switch */

    } /* while (1) */
    
#ifdef PR_DIAGNOSTICS
    printf("PR Reached end of EHT\n");
#endif

  } /* if out-of-line EHT */


  /* Do a virtual unwind of this frame - load the first unwind bytes then loop.
   * Loop exit is by executing opcode CODE_FINISH.
   */

  ud.unwind_word = *(uint32_t *)eht_startp;             /* first word */
  ud.unwind_word_pointer = (uint32_t *)eht_startp + 1;  /* ptr to extension words, if any */
  if (idx == 0) {                  /* short description */
    ud.unwind_words_remaining = 0; /* no further words */
    ud.unwind_word <<= 8;          /* 3 explicit unwind bytes in this word */
    ud.unwind_word_bytes_remaining = 3;
  } else {                         /* long description: extension word count in bits 16-23 */
    ud.unwind_words_remaining = ((ud.unwind_word) >> 16) & 0xff;
    ud.unwind_word <<= 16;         /* 2 explicit unwind bytes in this word */
    ud.unwind_word_bytes_remaining = 2;
  }

#ifdef PR_DIAGNOSTICS
  /*  debug_print_vrs(context); */
#endif

  while (1) {
    uint8_t ub = next_unwind_byte(&ud);

#ifdef PR_DIAGNOSTICS
    printf("PR Unwind byte 0x%x\n", ub);
#endif

    /* decode and execute the current byte ... */

    if (ub == CODE_FINISH) { /* finished unwinding */
      if (!wrote_pc) {
        uint32_t lr;
        if (!wrote_lr) {
          /* If neither pc nor lr was written, the saved return address was
           * not restored. This indicates broken unwind instructions.
           */
          DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_TABLECORRUPT);
          return _URC_FAILURE;
        }
        _Unwind_VRS_Get(context, _UVRSC_CORE, R_LR, _UVRSD_UINT32, &lr);
        core_set(context, R_PC, lr);
        wrote_pc_from_lr = true;
      }
#ifdef PR_DIAGNOSTICS
      {
        uint32_t nextpc;
        _Unwind_VRS_Get(context, _UVRSC_CORE, R_PC, _UVRSD_UINT32, &nextpc);
        printf("PR Next PC is  0x%x\n", nextpc);
      }
#endif
      break;
    }
    if (ub <= 0x3f) { /* 00nnnnnn: vsp += (nnnnnn << 2) + 4 */
      uint32_t increment = ((ub & 0x3f) << 2) + 4;
      core_set(context, R_SP, core_get(context, R_SP) + increment);
      continue;
    }
    if (ub <= 0x7f) { /* 01xxxxxx: vsp -= (xxxxxx << 2) + 4 */
      uint32_t decrement = ((ub & 0x3f) << 2) + 4;
      core_set(context, R_SP, core_get(context, R_SP) - decrement);
      continue;
    }
    if (ub <= 0x8f) { /* 100000000 00000000: refuse, 1000rrrr rrrrrrrr: pop integer regs */
      uint32_t mask = (ub & 0xf) << 12;
      ub = next_unwind_byte(&ud);
      mask |= ub << 4;
      if (mask == 0) { /* 10000000 00000000 refuse to unwind */
        DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_NOUNWIND);
        return _URC_FAILURE;
      }
      if (_Unwind_VRS_Pop(context, _UVRSC_CORE, mask, _UVRSD_UINT32) != _UVRSR_OK) {
        DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_VRSFAILED);
        return _URC_FAILURE;
      }
      if (mask & (1 << R_PC)) wrote_pc = true;
      if (mask & (1 << R_LR)) wrote_lr = true;
      continue;
    }
    if (ub <= 0x9f) { /* 1001nnnn: vsp = r[nnnn] if not 13,15 */
      uint8_t regno = ub & 0xf;
      if (regno == 13 || regno == R_PC) {  /* reserved */
        DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_CPP_BADOPCODE);
        return _URC_FAILURE;
      }
      core_set(context, R_SP, core_get(context, regno));
      continue;
    }
    if (ub <= 0xaf) { /* 1010xnnn: pop r4-r[4+nnn], +r14 if x */
      uint32_t mask = count_to_mask((ub & 0x7) + 1) << 4;
      if (ub & 0x8) {
        mask |= (1 << R_LR);
        wrote_lr = true;
      }
      if (_Unwind_VRS_Pop(context, _UVRSC_CORE, mask, _UVRSD_UINT32) != _UVRSR_OK) {
        DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_VRSFAILED);
        return _URC_FAILURE;
      }
      continue;
    }
    if (ub <= 0xb7) {
      /* if (ub == 0xb0) is CODE_FINISH, handled earlier */
      if (ub == 0xb1) { /* 10110001 0000iiii pop integer regs, others reserved */
        uint32_t mask = next_unwind_byte(&ud);
        if (mask == 0 || mask > 0xf) { /* reserved */
          DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_CPP_BADOPCODE);
          return _URC_FAILURE;
        }
        if (_Unwind_VRS_Pop(context, _UVRSC_CORE, mask, _UVRSD_UINT32) != _UVRSR_OK) {
          DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_VRSFAILED);
          return _URC_FAILURE;
        }
        continue;
      }
      if (ub == 0xb2) { /* 10110010 uleb128 : vsp += (uleb128 << 2) + 0x204 */
        uint32_t u = 0;
        uint32_t n = 0;
        /* decode */
        while (1) {
          ub = next_unwind_byte(&ud);
          u |= (ub & 0x7f) << n;
          if ((ub & 0x80) == 0) break;
          n += 7;
        }
        core_set(context, R_SP, core_get(context, R_SP) + (u << 2) + 0x204);
        continue;
      }
      if (ub == 0xb3) { /* 10110011: pop vfp from FSTMFDX */
        uint32_t discriminator = next_unwind_byte(&ud);
        discriminator = ((discriminator & 0xf0) << 12) | ((discriminator & 0x0f) + 1);
        if (_Unwind_VRS_Pop(context, _UVRSC_VFP, discriminator, _UVRSD_VFPX) != _UVRSR_OK) {
          DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_VRSFAILED);
          return _URC_FAILURE;
        }
        continue;
      }
      { /* 101101nn: was pop fpa, now spare */
        DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_CPP_BADOPCODE);
        return _URC_FAILURE;
      }
    } /* if (ub <= 0xb7) ... */
    if (ub <= 0xbf) { /* 10111nnn: pop vfp from FSTMFDX */
      uint32_t discriminator = 0x80000 | ((ub & 0x7) + 1);
      if (_Unwind_VRS_Pop(context, _UVRSC_VFP, discriminator, _UVRSD_VFPX) != _UVRSR_OK) {
        DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_VRSFAILED);
        return _URC_FAILURE;
      }
      continue;
    }
    if (ub <= 0xc7) {
      if (ub == 0xc7) { /* 11000111: WMMX C regs */
        uint32_t mask = next_unwind_byte(&ud);
        if (mask == 0 || mask > 0xf) { /* reserved */
          DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_CPP_BADOPCODE);
          return _URC_FAILURE;
        }
        if (_Unwind_VRS_Pop(context, _UVRSC_WMMXC, mask, _UVRSD_UINT32) != _UVRSR_OK) {
          DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_VRSFAILED);
          return _URC_FAILURE;
        }
        continue;
      } else if (ub == 0xc6) { /* 11000110: WMMX D regs */
        uint32_t discriminator = next_unwind_byte(&ud);
        discriminator = ((discriminator & 0xf0) << 12) | ((discriminator & 0x0f) + 1);
        if (_Unwind_VRS_Pop(context, _UVRSC_WMMXD, discriminator, _UVRSD_UINT64) != _UVRSR_OK) {
          DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_VRSFAILED);
          return _URC_FAILURE;
        }
        continue;
      } else {
        /* 11000nnn (nnn != 6, 7): WMMX D regs */
        uint32_t discriminator = 0xa0000 | ((ub & 0x7) + 1);
        if (_Unwind_VRS_Pop(context, _UVRSC_WMMXD, discriminator, _UVRSD_UINT64) != _UVRSR_OK) {
          DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_VRSFAILED);
          return _URC_FAILURE;
        }
        continue;
      }
    } /* if (ub <= 0xc7) ... */
    if (ub == 0xc8 || /* 11001000 sssscccc: pop VFP hi regs from FSTMFDD */
        ub == 0xc9) { /* 11001001 sssscccc: pop VFP from FSTMFDD */
      uint32_t discriminator = next_unwind_byte(&ud);
      discriminator = ((discriminator & 0xf0) << 12) | ((discriminator & 0x0f) + 1);
      if (ub == 0xc8) discriminator += 16 << 16;
      if (_Unwind_VRS_Pop(context, _UVRSC_VFP, discriminator, _UVRSD_DOUBLE) != _UVRSR_OK) {
        DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_VRSFAILED);
        return _URC_FAILURE;
      }
      continue;
    }
    if (ub <= 0xcf) { /* spare */
      DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_CPP_BADOPCODE);
      return _URC_FAILURE;
    }
    if (ub <= 0xd7) { /* 11010nnn: pop VFP from FSTMFDD */
      uint32_t discriminator = 0x80000 | ((ub & 0x7) + 1);
      if (_Unwind_VRS_Pop(context, _UVRSC_VFP, discriminator, _UVRSD_DOUBLE) != _UVRSR_OK) {
        DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_VRSFAILED);
        return _URC_FAILURE;
      }
      continue;
    }
    /* and in fact everything else is currently reserved or spare */
    DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_ENDING, _UAARG_ENDING_CPP_BADOPCODE);
    return _URC_FAILURE;
  }
 
#ifdef PR_DIAGNOSTICS
  /* debug_print_vrs(context); */
#endif

  /* The VRS has now been updated to reflect the virtual unwind.
   * If we are dealing with an unmatched fnspec, pop intervening frames 
   * and call unexpected(). Else return to our caller with an
   * indication to continue unwinding.
   */

  if (phase2_call_unexpected_after_unwind) {
    /* Set up the VRS to enter __cxa_call_unexpected,
     * and upload the VRS to the real machine.
     * The barrier_cache was initialised earlier.
     */
#ifdef PR_DIAGNOSTICS
    printf("PR Got fnspec barrier in phase 2 (unwinding completed)\n");
#endif
    core_set(context, 0, (uint32_t)ucbp);
    if (!wrote_pc_from_lr) {
      uint32_t pc;
      /* Move the return address to lr to simulate a call */
      _Unwind_VRS_Get(context, _UVRSC_CORE, R_PC, _UVRSD_UINT32, &pc);
      core_set(context, R_LR, pc);
    }
    core_set(context, R_PC, (uint32_t)&__cxa_call_unexpected);
    DEBUGGER_BOTTLENECK(ucbp, _UASUBSYS_CPP, _UAACT_PADENTRY, &__cxa_call_unexpected);
    return _URC_INSTALL_CONTEXT;
  }
  
  /* Else continue with next frame */
  return _URC_CONTINUE_UNWIND;
}

#endif
/* end ifdef prcommon_c */
