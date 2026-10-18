/* SPDX-License-Identifier: GPL-3.0-or-later */
.text
.p2align 2
.global wu_capture_call
.type wu_capture_call,%function
wu_capture_call:
    stp x29, x30, [sp, #-96]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    adrp x16, wu_saved_sp
    add x16, x16, :lo12:wu_saved_sp
    mov x17, sp
    str x17, [x16]
    blr x0
.global wu_capture_resume
.hidden wu_capture_resume
wu_capture_resume:
    adrp x16, wu_saved_sp
    ldr x16, [x16, :lo12:wu_saved_sp]
    mov sp, x16
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp], #96
    ret
.size wu_capture_call, .-wu_capture_call
.bss
.p2align 3
.hidden wu_saved_sp
wu_saved_sp:
    .zero 8
.section .note.GNU-stack,"",%progbits
