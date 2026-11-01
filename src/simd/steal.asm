; Claim one slot of a work-stealing deque's shared top index.
;
; The whole correctness of the deque rests on this: advancing the top index and
; deciding who owns slot [expected] must be a single indivisible step, so that
; two thieves can never both come away thinking they took the same item.
;
;   bool sninja_claim_top(int64_t* top, int64_t expected)
;     RCX = top      (shared, atomic)
;     EDX = expected (the value *top must still hold)
; Returns 1 if this caller won the claim, 0 if somebody else changed *top first.
; On success *top has been advanced to expected + 1.
;
; Clobbers: RAX, RCX, RDX, R9.

        default rel
        section .text

        global sninja_claim_top
sninja_claim_top:
        movsxd  rax, edx                 ; RAX = expected (CMPXCHG compares RAX)
        lea     r9, [rax + 1]            ; the value to store on success
        lock cmpxchg [rcx], r9            ; if *top == expected then *top = expected+1
        ; On success ZF is set; on failure ZF is clear and RAX has been
        ; reloaded with the destination.  sete only writes the low byte, so
        ; zero-extend it explicitly for both cases.
        sete    al
        movzx   eax, al
        ret

; Publish the owner's new bottom index.
;   void sninja_publish_bottom(int64_t* bottom, int64_t value)
; Clobbers: none.
        global sninja_publish_bottom
sninja_publish_bottom:
        mov     [rcx], rdx
        ret

        section .note.GNU-stack noalloc noexec nowrite progbits
