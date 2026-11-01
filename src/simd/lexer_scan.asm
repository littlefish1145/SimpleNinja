; lexer_scan.asm - bulk byte scanners for the sninja lexer.
;
; Win64 ABI: rcx = const char* p, rdx = const char* end (exclusive)
; returns rax = pointer to the first matching byte, or rdx (= "not found",
; and also the current position when the scan ran off the end).
;
; Chosen at runtime: AVX2 > SSE2 > scalar C++ fallback (scan_dispatch.cpp).
; Every routine is safe with respect to |end|: vector loads only happen when a
; full 16/32-byte block is still available.
;
; The constant tables live in .text so that all RIP-relative references stay
; intra-section (no section-crossing rel32 relocations).

default rel

section .text

; ---------------------------------------------------------------------------
; Epilogue helpers
; ---------------------------------------------------------------------------

; Vector-hit epilogue: turn the movemask bit index into a pointer.
%macro EMIT_HIT 0-1
    bsf eax, eax
    lea rax, [rcx + rax]
%if %0 = 1
    %1
%endif
    ret
%endmacro

; ---------------------------------------------------------------------------
; Tail scanners (scalar).  Every exit path returns rcx.
; ---------------------------------------------------------------------------

%macro EVAL_STOP_BODY 0
.tail:
    cmp rcx, rdx
    jae .done
    movzx eax, byte [rcx]
    test al, al
    jz .done
    cmp al, 0x0A
    je .done
    cmp al, 0x0D
    je .done
    cmp al, 0x20
    je .done
    cmp al, 0x24
    je .done
    cmp al, 0x3A
    je .done
    cmp al, 0x7C
    je .done
    inc rcx
    jmp .tail
.done:
    mov rax, rcx
%endmacro

%macro DEF_EVAL_STOP_TAIL 0
    EVAL_STOP_BODY
    ret
%endmacro

%macro DEF_EVAL_STOP_TAIL_AVX 0
    EVAL_STOP_BODY
    vzeroupper
    ret
%endmacro

%macro LINE_END_BODY 0
.tail:
    cmp rcx, rdx
    jae .done
    movzx eax, byte [rcx]
    test al, al
    jz .done
    cmp al, 0x0A
    je .done
    inc rcx
    jmp .tail
.done:
    mov rax, rcx
%endmacro

%macro DEF_LINE_END_TAIL 0
    LINE_END_BODY
    ret
%endmacro

%macro DEF_LINE_END_TAIL_AVX 0
    LINE_END_BODY
    vzeroupper
    ret
%endmacro

%macro IDENT_BODY 0
.tail:
    cmp rcx, rdx
    jae .done
    movzx eax, byte [rcx]
    cmp al, '0'
    jb .punct
    cmp al, '9'
    jbe .is_ident
    cmp al, 'A'
    jb .punct
    cmp al, 'Z'
    jbe .is_ident
    cmp al, 'a'
    jb .punct
    cmp al, 'z'
    jbe .is_ident
.punct:
    cmp al, '-'
    je .is_ident
    cmp al, '.'
    je .is_ident
    cmp al, '_'
    je .is_ident
    jmp .done
.is_ident:
    inc rcx
    jmp .tail
.done:
    mov rax, rcx
%endmacro

%macro DEF_IDENT_TAIL 0
    IDENT_BODY
    ret
%endmacro

%macro DEF_IDENT_TAIL_AVX 0
    IDENT_BODY
    vzeroupper
    ret
%endmacro

%macro SPACE_BODY 0
.tail:
    cmp rcx, rdx
    jae .done
    cmp byte [rcx], ' '
    jne .done
    inc rcx
    jmp .tail
.done:
    mov rax, rcx
%endmacro

%macro DEF_SPACE_TAIL 0
    SPACE_BODY
    ret
%endmacro

%macro DEF_SPACE_TAIL_AVX 0
    SPACE_BODY
    vzeroupper
    ret
%endmacro

; ===========================================================================
; find_eval_stop: first byte of {0x00, 0x0A, 0x0D, 0x20, 0x24, 0x3A}
; ===========================================================================

global sn_sse2_find_eval_stop
sn_sse2_find_eval_stop:
    cmp rcx, rdx
    jae .done
.vloop:
    mov rax, rdx
    sub rax, rcx
    cmp rax, 16
    jb .tail
    movdqu xmm1, [rcx]
    movdqa xmm0, xmm1
    pcmpeqb xmm0, [rel kByte00]
    movdqa xmm2, xmm1
    pcmpeqb xmm2, [rel kByte0A]
    por xmm0, xmm2
    movdqa xmm2, xmm1
    pcmpeqb xmm2, [rel kByte0D]
    por xmm0, xmm2
    movdqa xmm2, xmm1
    pcmpeqb xmm2, [rel kByte20]
    por xmm0, xmm2
    movdqa xmm2, xmm1
    pcmpeqb xmm2, [rel kByte24]
    por xmm0, xmm2
    movdqa xmm2, xmm1
    pcmpeqb xmm2, [rel kByte7C]
    por xmm0, xmm2
    pcmpeqb xmm1, [rel kByte3A]
    por xmm0, xmm1
    pmovmskb eax, xmm0
    test eax, eax
    jnz .hit
    add rcx, 16
    jmp .vloop
.hit:
    EMIT_HIT
    DEF_EVAL_STOP_TAIL

global sn_avx2_find_eval_stop
sn_avx2_find_eval_stop:
    cmp rcx, rdx
    jae .done
.vloop:
    mov rax, rdx
    sub rax, rcx
    cmp rax, 32
    jb .tail
    vmovdqu ymm1, [rcx]
    vpcmpeqb ymm0, ymm1, [rel kByte00]
    vpcmpeqb ymm2, ymm1, [rel kByte0A]
    vpor ymm0, ymm0, ymm2
    vpcmpeqb ymm2, ymm1, [rel kByte0D]
    vpor ymm0, ymm0, ymm2
    vpcmpeqb ymm2, ymm1, [rel kByte20]
    vpor ymm0, ymm0, ymm2
    vpcmpeqb ymm2, ymm1, [rel kByte24]
    vpor ymm0, ymm0, ymm2
    vpcmpeqb ymm2, ymm1, [rel kByte7C]
    vpor ymm0, ymm0, ymm2
    vpcmpeqb ymm1, ymm1, [rel kByte3A]
    vpor ymm0, ymm0, ymm1
    vpmovmskb eax, ymm0
    test eax, eax
    jnz .hit
    add rcx, 32
    jmp .vloop
.hit:
    EMIT_HIT vzeroupper
    DEF_EVAL_STOP_TAIL_AVX

; ===========================================================================
; find_ident_end: first byte NOT in [0-9A-Za-z_.-]
; ===========================================================================

global sn_sse2_find_ident_end
sn_sse2_find_ident_end:
    cmp rcx, rdx
    jae .done
.vloop:
    mov rax, rdx
    sub rax, rcx
    cmp rax, 16
    jb .tail
    movdqu xmm3, [rcx]
    ; digits [0x30,0x39]: max(c,'0')==c && min(c,'9')==c
    movdqa xmm0, xmm3
    pmaxub xmm0, [rel kByte30]
    pcmpeqb xmm0, xmm3
    movdqa xmm1, xmm3
    pminub xmm1, [rel kByte39]
    pcmpeqb xmm1, xmm3
    pand xmm0, xmm1
    ; upper [0x41,0x5A]
    movdqa xmm1, xmm3
    pmaxub xmm1, [rel kByte41]
    pcmpeqb xmm1, xmm3
    movdqa xmm2, xmm3
    pminub xmm2, [rel kByte5A]
    pcmpeqb xmm2, xmm3
    pand xmm1, xmm2
    por xmm0, xmm1
    ; lower [0x61,0x7A]
    movdqa xmm1, xmm3
    pmaxub xmm1, [rel kByte61]
    pcmpeqb xmm1, xmm3
    movdqa xmm2, xmm3
    pminub xmm2, [rel kByte7A]
    pcmpeqb xmm2, xmm3
    pand xmm1, xmm2
    por xmm0, xmm1
    ; '-', '.', '_'
    movdqa xmm1, xmm3
    pcmpeqb xmm1, [rel kByte2D]
    movdqa xmm2, xmm3
    pcmpeqb xmm2, [rel kByte2E]
    por xmm1, xmm2
    movdqa xmm2, xmm3
    pcmpeqb xmm2, [rel kByte5F]
    por xmm1, xmm2
    por xmm0, xmm1
    ; invert: we are looking for the first NON-ident byte
    pmovmskb eax, xmm0
    xor eax, 0xFFFF
    test eax, eax
    jnz .hit
    add rcx, 16
    jmp .vloop
.hit:
    EMIT_HIT
    DEF_IDENT_TAIL

global sn_avx2_find_ident_end
sn_avx2_find_ident_end:
    cmp rcx, rdx
    jae .done
.vloop:
    mov rax, rdx
    sub rax, rcx
    cmp rax, 32
    jb .tail
    vmovdqu ymm3, [rcx]
    ; digits
    vpmaxub ymm0, ymm3, [rel kByte30]
    vpcmpeqb ymm0, ymm0, ymm3
    vpminub ymm1, ymm3, [rel kByte39]
    vpcmpeqb ymm1, ymm1, ymm3
    vpand ymm0, ymm0, ymm1
    ; upper
    vpmaxub ymm1, ymm3, [rel kByte41]
    vpcmpeqb ymm1, ymm1, ymm3
    vpminub ymm2, ymm3, [rel kByte5A]
    vpcmpeqb ymm2, ymm2, ymm3
    vpand ymm1, ymm1, ymm2
    vpor ymm0, ymm0, ymm1
    ; lower
    vpmaxub ymm1, ymm3, [rel kByte61]
    vpcmpeqb ymm1, ymm1, ymm3
    vpminub ymm2, ymm3, [rel kByte7A]
    vpcmpeqb ymm2, ymm2, ymm3
    vpand ymm1, ymm1, ymm2
    vpor ymm0, ymm0, ymm1
    ; '-', '.', '_'
    vpcmpeqb ymm1, ymm3, [rel kByte2D]
    vpcmpeqb ymm2, ymm3, [rel kByte2E]
    vpor ymm1, ymm1, ymm2
    vpcmpeqb ymm2, ymm3, [rel kByte5F]
    vpor ymm1, ymm1, ymm2
    vpor ymm0, ymm0, ymm1
    ; invert
    vpmovmskb eax, ymm0
    xor eax, 0xFFFFFFFF
    test eax, eax
    jnz .hit
    add rcx, 32
    jmp .vloop
.hit:
    EMIT_HIT vzeroupper
    DEF_IDENT_TAIL_AVX

; ===========================================================================
; find_space_end: first byte != ' '
; ===========================================================================

global sn_sse2_find_space_end
sn_sse2_find_space_end:
    cmp rcx, rdx
    jae .done
.vloop:
    mov rax, rdx
    sub rax, rcx
    cmp rax, 16
    jb .tail
    movdqu xmm0, [rcx]
    pcmpeqb xmm0, [rel kByte20]
    pmovmskb eax, xmm0
    xor eax, 0xFFFF
    test eax, eax
    jnz .hit
    add rcx, 16
    jmp .vloop
.hit:
    EMIT_HIT
    DEF_SPACE_TAIL

global sn_avx2_find_space_end
sn_avx2_find_space_end:
    cmp rcx, rdx
    jae .done
.vloop:
    mov rax, rdx
    sub rax, rcx
    cmp rax, 32
    jb .tail
    vmovdqu ymm0, [rcx]
    vpcmpeqb ymm0, ymm0, [rel kByte20]
    vpmovmskb eax, ymm0
    xor eax, 0xFFFFFFFF
    test eax, eax
    jnz .hit
    add rcx, 32
    jmp .vloop
.hit:
    EMIT_HIT vzeroupper
    DEF_SPACE_TAIL_AVX

; ===========================================================================
; find_line_end: first byte of {0x00, 0x0A}
; ===========================================================================

global sn_sse2_find_line_end
sn_sse2_find_line_end:
    cmp rcx, rdx
    jae .done
.vloop:
    mov rax, rdx
    sub rax, rcx
    cmp rax, 16
    jb .tail
    movdqu xmm1, [rcx]
    movdqa xmm0, xmm1
    pcmpeqb xmm0, [rel kByte00]
    pcmpeqb xmm1, [rel kByte0A]
    por xmm0, xmm1
    pmovmskb eax, xmm0
    test eax, eax
    jnz .hit
    add rcx, 16
    jmp .vloop
.hit:
    EMIT_HIT
    DEF_LINE_END_TAIL

global sn_avx2_find_line_end
sn_avx2_find_line_end:
    cmp rcx, rdx
    jae .done
.vloop:
    mov rax, rdx
    sub rax, rcx
    cmp rax, 32
    jb .tail
    vmovdqu ymm1, [rcx]
    vpcmpeqb ymm0, ymm1, [rel kByte00]
    vpcmpeqb ymm1, ymm1, [rel kByte0A]
    vpor ymm0, ymm0, ymm1
    vpmovmskb eax, ymm0
    test eax, eax
    jnz .hit
    add rcx, 32
    jmp .vloop
.hit:
    EMIT_HIT vzeroupper
    DEF_LINE_END_TAIL_AVX

; ---------------------------------------------------------------------------
; Constants (32 bytes so the AVX2 loads stay in-bounds; SSE2 uses 16).
; ---------------------------------------------------------------------------

align 32
kByte00: times 32 db 0x00
kByte0A: times 32 db 0x0A
kByte0D: times 32 db 0x0D
kByte20: times 32 db 0x20
kByte24: times 32 db 0x24
kByte2D: times 32 db 0x2D
kByte2E: times 32 db 0x2E
kByte30: times 32 db 0x30
kByte39: times 32 db 0x39
kByte3A: times 32 db 0x3A
kByte41: times 32 db 0x41
kByte5A: times 32 db 0x5A
kByte5F: times 32 db 0x5F
kByte61: times 32 db 0x61
kByte7A: times 32 db 0x7A
kByte7C: times 32 db 0x7C
