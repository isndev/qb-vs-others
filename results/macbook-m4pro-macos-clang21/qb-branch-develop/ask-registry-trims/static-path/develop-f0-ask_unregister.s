
develop-f0/qvoprobe-ask-any-scatter:	file format mach-o arm64

Disassembly of section __TEXT,__text:

000000010001c5b8 <__ZN2qb6detail14ask_unregisterEy>:
10001c5b8:     	sub	sp, sp, #0x40
10001c5bc:     	stp	x22, x21, [sp, #0x10]
10001c5c0:     	stp	x20, x19, [sp, #0x20]
10001c5c4:     	stp	x29, x30, [sp, #0x30]
10001c5c8:     	mov	x20, x0
10001c5cc:     	adrp	x0, 0x100088000 <__ZN2qb6router4memhINS_5EventELb1EvE14_disposers_mtxE>
10001c5d0:     	add	x0, x0, #0xa98
10001c5d4:     	ldr	x8, [x0]
10001c5d8:     	blr	x8
10001c5dc:     	ldrb	w8, [x0]
10001c5e0:     	cbz	w8, 0x10001c760 <__ZN2qb6detail14ask_unregisterEy+0x1a8>
10001c5e4:     	adrp	x0, 0x100088000 <__ZN2qb6router4memhINS_5EventELb1EvE14_disposers_mtxE>
10001c5e8:     	add	x0, x0, #0x930
10001c5ec:     	ldr	x8, [x0]
10001c5f0:     	blr	x8
10001c5f4:     	and	x21, x20, #0x3fffff
10001c5f8:     	ldr	w8, [x0, #0xc]
10001c5fc:     	cmp	x21, x8
10001c600:     	b.hs	0x10001c74c <__ZN2qb6detail14ask_unregisterEy+0x194>
10001c604:     	mov	x19, x0
10001c608:     	ldr	x8, [x0]
10001c60c:     	add	x8, x8, x21, lsl #4
10001c610:     	ldr	w10, [x8, #0xc]
10001c614:     	add	w9, w10, #0x3
10001c618:     	cmp	w9, #0x1
10001c61c:     	b.hi	0x10001c74c <__ZN2qb6detail14ask_unregisterEy+0x194>
10001c620:     	ldr	w9, [x8, #0x8]
10001c624:     	ubfx	x11, x20, #22, #26
10001c628:     	cmp	w9, w11
10001c62c:     	b.ne	0x10001c74c <__ZN2qb6detail14ask_unregisterEy+0x194>
10001c630:     	cmn	w10, #0x2
10001c634:     	b.ne	0x10001c644 <__ZN2qb6detail14ask_unregisterEy+0x8c>
10001c638:     	ldr	x10, [x8]
10001c63c:     	ldr	x1, [x10]
10001c640:     	b	0x10001c68c <__ZN2qb6detail14ask_unregisterEy+0xd4>
10001c644:     	ldr	x1, [x8]
10001c648:     	and	w10, w1, #0xffffff
10001c64c:     	sub	w11, w10, #0x1
10001c650:     	mov	w12, #0xfffe            ; =65534
10001c654:     	movk	w12, #0xfe, lsl #16
10001c658:     	cmp	w11, w12
10001c65c:     	b.hi	0x10001c66c <__ZN2qb6detail14ask_unregisterEy+0xb4>
10001c660:     	lsr	x10, x1, #32
10001c664:     	cbz	x10, 0x10001c688 <__ZN2qb6detail14ask_unregisterEy+0xd0>
10001c668:     	b	0x10001c68c <__ZN2qb6detail14ask_unregisterEy+0xd4>
10001c66c:     	cmp	w10, #0xff0, lsl #12    ; =0xff0000
10001c670:     	b.ls	0x10001c688 <__ZN2qb6detail14ask_unregisterEy+0xd0>
10001c674:     	lsr	x11, x1, #32
10001c678:     	cmp	x11, #0x0
10001c67c:     	mov	w11, #0xffffff          ; =16777215
10001c680:     	ccmp	w10, w11, #0x0, ne
10001c684:     	b.ne	0x10001c68c <__ZN2qb6detail14ask_unregisterEy+0xd4>
10001c688:     	mov	x1, #0x0                ; =0
10001c68c:     	lsr	x10, x20, #48
10001c690:     	lsr	x11, x1, #24
10001c694:     	cmp	x10, w11, uxtb
10001c698:     	b.ne	0x10001c74c <__ZN2qb6detail14ask_unregisterEy+0x194>
10001c69c:     	str	x1, [x8]
10001c6a0:     	mov	w10, #0x3ffffff         ; =67108863
10001c6a4:     	cmp	w9, w10
10001c6a8:     	b.eq	0x10001c768 <__ZN2qb6detail14ask_unregisterEy+0x1b0>
10001c6ac:     	lsr	w10, w1, #24
10001c6b0:     	add	w9, w9, #0x1
10001c6b4:     	str	w9, [x8, #0x8]
10001c6b8:     	and	w20, w20, #0x3fffff
10001c6bc:     	ldr	w8, [x19, #0x28]
10001c6c0:     	cmp	w8, w10
10001c6c4:     	b.ne	0x10001c784 <__ZN2qb6detail14ask_unregisterEy+0x1cc>
10001c6c8:     	ldr	x8, [x19, #0x20]
10001c6cc:     	and	w9, w1, #0xffffff
10001c6d0:     	cmp	w9, #0xff0, lsl #12     ; =0xff0000
10001c6d4:     	cset	w10, hi
10001c6d8:     	mov	w11, #0xffffff          ; =16777215
10001c6dc:     	cmp	w9, w11
10001c6e0:     	cset	w11, ne
10001c6e4:     	and	w12, w1, #0xffff
10001c6e8:     	ands	w10, w10, w11
10001c6ec:     	csel	w22, w12, w9, ne
10001c6f0:     	lsr	x9, x22, #8
10001c6f4:     	cmp	w10, #0x1
10001c6f8:     	b.eq	0x10001c7a0 <__ZN2qb6detail14ask_unregisterEy+0x1e8>
10001c6fc:     	ldr	w10, [x8, #0x8]
10001c700:     	cmp	x9, x10
10001c704:     	b.hs	0x10001c7b4 <__ZN2qb6detail14ask_unregisterEy+0x1fc>
10001c708:     	ldr	x8, [x8]
10001c70c:     	add	x0, x8, x9, lsl #3
10001c710:     	ldr	x8, [x0]
10001c714:     	cbz	x8, 0x10001c7d8 <__ZN2qb6detail14ask_unregisterEy+0x220>
10001c718:     	add	x8, x8, w22, uxtb #3
10001c71c:     	ldr	x9, [x19]
10001c720:     	add	x10, x9, x21, lsl #4
10001c724:     	mov	w11, #-0x1              ; =-1
10001c728:     	str	w11, [x10, #0xc]
10001c72c:     	ldr	w10, [x8, #0x4]
10001c730:     	cmn	w10, #0x1
10001c734:     	b.eq	0x10001c744 <__ZN2qb6detail14ask_unregisterEy+0x18c>
10001c738:     	add	x9, x9, x10, lsl #4
10001c73c:     	str	w20, [x9, #0xc]
10001c740:     	b	0x10001c748 <__ZN2qb6detail14ask_unregisterEy+0x190>
10001c744:     	str	w20, [x8]
10001c748:     	str	w20, [x8, #0x4]
10001c74c:     	ldp	x29, x30, [sp, #0x30]
10001c750:     	ldp	x20, x19, [sp, #0x20]
10001c754:     	ldp	x22, x21, [sp, #0x10]
10001c758:     	add	sp, sp, #0x40
10001c75c:     	ret
10001c760:     	bl	0x100066620 <___tls_init.cold.1>
10001c764:     	b	0x10001c5e4 <__ZN2qb6detail14ask_unregisterEy+0x2c>
10001c768:     	mov	w9, #-0x4               ; =-4
10001c76c:     	str	w9, [x8, #0xc]
10001c770:     	ldp	x29, x30, [sp, #0x30]
10001c774:     	ldp	x20, x19, [sp, #0x20]
10001c778:     	ldp	x22, x21, [sp, #0x10]
10001c77c:     	add	sp, sp, #0x40
10001c780:     	ret
10001c784:     	mov	x0, x19
10001c788:     	mov	x2, x20
10001c78c:     	ldp	x29, x30, [sp, #0x30]
10001c790:     	ldp	x20, x19, [sp, #0x20]
10001c794:     	ldp	x22, x21, [sp, #0x10]
10001c798:     	add	sp, sp, #0x40
10001c79c:     	b	0x10002eeac <__ZN2qb6detail12_GLOBAL__N_19ask_table18release_other_coreENS_7ActorIdEj>
10001c7a0:     	add	x8, x8, x9, lsl #3
10001c7a4:     	add	x0, x8, #0x10
10001c7a8:     	ldr	x8, [x0]
10001c7ac:     	cbnz	x8, 0x10001c718 <__ZN2qb6detail14ask_unregisterEy+0x160>
10001c7b0:     	b	0x10001c7d8 <__ZN2qb6detail14ask_unregisterEy+0x220>
10001c7b4:     	add	x1, x9, #0x1
10001c7b8:     	stp	x9, x8, [sp]
10001c7bc:     	mov	x0, x8
10001c7c0:     	bl	0x10002ee00 <__ZN2qb6detail12_GLOBAL__N_19ask_table14grow_directoryERNS2_15owner_directoryEm>
10001c7c4:     	ldp	x9, x8, [sp]
10001c7c8:     	ldr	x8, [x8]
10001c7cc:     	add	x0, x8, x9, lsl #3
10001c7d0:     	ldr	x8, [x0]
10001c7d4:     	cbnz	x8, 0x10001c718 <__ZN2qb6detail14ask_unregisterEy+0x160>
10001c7d8:     	str	x0, [sp, #0x8]
10001c7dc:     	bl	0x10002ee78 <__ZN2qb6detail12_GLOBAL__N_19ask_table11create_pageEPPNSt3__15arrayINS2_10free_queueELm256EEE>
10001c7e0:     	ldr	x8, [sp, #0x8]
10001c7e4:     	ldr	x8, [x8]
10001c7e8:     	b	0x10001c718 <__ZN2qb6detail14ask_unregisterEy+0x160>
10001c7ec:     	bl	0x100001b94 <___clang_call_terminate>
