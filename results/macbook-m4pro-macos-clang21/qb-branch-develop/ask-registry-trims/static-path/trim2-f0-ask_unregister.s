
trim2-f0/qvoprobe-ask-any-scatter:	file format mach-o arm64

Disassembly of section __TEXT,__text:

000000010001c5c0 <__ZN2qb6detail14ask_unregisterEy>:
10001c5c0:     	stp	x20, x19, [sp, #-0x20]!
10001c5c4:     	stp	x29, x30, [sp, #0x10]
10001c5c8:     	mov	x19, x0
10001c5cc:     	adrp	x0, 0x100088000 <__ZN2qb6router4memhINS_5EventELb1EvE14_disposers_mtxE>
10001c5d0:     	add	x0, x0, #0xa98
10001c5d4:     	ldr	x8, [x0]
10001c5d8:     	blr	x8
10001c5dc:     	ldrb	w8, [x0]
10001c5e0:     	cbz	w8, 0x10001c750 <__ZN2qb6detail14ask_unregisterEy+0x190>
10001c5e4:     	adrp	x0, 0x100088000 <__ZN2qb6router4memhINS_5EventELb1EvE14_disposers_mtxE>
10001c5e8:     	add	x0, x0, #0x930
10001c5ec:     	ldr	x8, [x0]
10001c5f0:     	blr	x8
10001c5f4:     	and	x8, x19, #0x3fffff
10001c5f8:     	ldr	w9, [x0, #0xc]
10001c5fc:     	cmp	x8, x9
10001c600:     	b.hs	0x10001c744 <__ZN2qb6detail14ask_unregisterEy+0x184>
10001c604:     	ldr	x9, [x0]
10001c608:     	add	x9, x9, x8, lsl #4
10001c60c:     	ldr	w11, [x9, #0xc]
10001c610:     	add	w10, w11, #0x3
10001c614:     	cmp	w10, #0x1
10001c618:     	b.hi	0x10001c744 <__ZN2qb6detail14ask_unregisterEy+0x184>
10001c61c:     	ldr	w10, [x9, #0x8]
10001c620:     	ubfx	x12, x19, #22, #26
10001c624:     	cmp	w10, w12
10001c628:     	b.ne	0x10001c744 <__ZN2qb6detail14ask_unregisterEy+0x184>
10001c62c:     	cmn	w11, #0x2
10001c630:     	b.ne	0x10001c65c <__ZN2qb6detail14ask_unregisterEy+0x9c>
10001c634:     	ldr	x1, [x9]
10001c638:     	cbz	x1, 0x10001c660 <__ZN2qb6detail14ask_unregisterEy+0xa0>
10001c63c:     	mov	w12, #0x0               ; =0
10001c640:     	mov	x11, x1
10001c644:     	ldr	x1, [x1]
10001c648:     	lsr	x13, x19, #48
10001c64c:     	lsr	x14, x1, #24
10001c650:     	cmp	x13, w14, uxtb
10001c654:     	b.eq	0x10001c6f0 <__ZN2qb6detail14ask_unregisterEy+0x130>
10001c658:     	b	0x10001c744 <__ZN2qb6detail14ask_unregisterEy+0x184>
10001c65c:     	ldr	x1, [x9]
10001c660:     	and	w11, w1, #0xffffff
10001c664:     	sub	w12, w11, #0x1
10001c668:     	mov	w13, #0xfffe            ; =65534
10001c66c:     	movk	w13, #0xfe, lsl #16
10001c670:     	cmp	w12, w13
10001c674:     	b.hi	0x10001c69c <__ZN2qb6detail14ask_unregisterEy+0xdc>
10001c678:     	lsr	x11, x1, #32
10001c67c:     	cbnz	x11, 0x10001c6b8 <__ZN2qb6detail14ask_unregisterEy+0xf8>
10001c680:     	mov	x1, #0x0                ; =0
10001c684:     	mov	w12, #0x1               ; =1
10001c688:     	lsr	x13, x19, #48
10001c68c:     	lsr	x14, x1, #24
10001c690:     	cmp	x13, w14, uxtb
10001c694:     	b.eq	0x10001c6f0 <__ZN2qb6detail14ask_unregisterEy+0x130>
10001c698:     	b	0x10001c744 <__ZN2qb6detail14ask_unregisterEy+0x184>
10001c69c:     	cmp	w11, #0xff0, lsl #12    ; =0xff0000
10001c6a0:     	b.ls	0x10001c6d4 <__ZN2qb6detail14ask_unregisterEy+0x114>
10001c6a4:     	lsr	x12, x1, #32
10001c6a8:     	cmp	x12, #0x0
10001c6ac:     	mov	w12, #0xffffff          ; =16777215
10001c6b0:     	ccmp	w11, w12, #0x0, ne
10001c6b4:     	b.eq	0x10001c6d4 <__ZN2qb6detail14ask_unregisterEy+0x114>
10001c6b8:     	mov	x11, #0x0               ; =0
10001c6bc:     	mov	w12, #0x1               ; =1
10001c6c0:     	lsr	x13, x19, #48
10001c6c4:     	lsr	x14, x1, #24
10001c6c8:     	cmp	x13, w14, uxtb
10001c6cc:     	b.eq	0x10001c6f0 <__ZN2qb6detail14ask_unregisterEy+0x130>
10001c6d0:     	b	0x10001c744 <__ZN2qb6detail14ask_unregisterEy+0x184>
10001c6d4:     	mov	x11, #0x0               ; =0
10001c6d8:     	mov	x1, #0x0                ; =0
10001c6dc:     	mov	w12, #0x1               ; =1
10001c6e0:     	lsr	x13, x19, #48
10001c6e4:     	lsr	x14, x1, #24
10001c6e8:     	cmp	x13, w14, uxtb
10001c6ec:     	b.ne	0x10001c744 <__ZN2qb6detail14ask_unregisterEy+0x184>
10001c6f0:     	str	x1, [x9]
10001c6f4:     	mov	w13, #0x3ffffff         ; =67108863
10001c6f8:     	cmp	w10, w13
10001c6fc:     	b.eq	0x10001c758 <__ZN2qb6detail14ask_unregisterEy+0x198>
10001c700:     	add	w10, w10, #0x1
10001c704:     	str	w10, [x9, #0x8]
10001c708:     	and	w2, w19, #0x3fffff
10001c70c:     	cbnz	w12, 0x10001c76c <__ZN2qb6detail14ask_unregisterEy+0x1ac>
10001c710:     	ldr	x9, [x11, #0x8]
10001c714:     	ldr	x10, [x0]
10001c718:     	add	x8, x10, x8, lsl #4
10001c71c:     	mov	w11, #-0x1              ; =-1
10001c720:     	str	w11, [x8, #0xc]
10001c724:     	ldr	w8, [x9, #0x4]
10001c728:     	cmn	w8, #0x1
10001c72c:     	b.eq	0x10001c73c <__ZN2qb6detail14ask_unregisterEy+0x17c>
10001c730:     	add	x8, x10, x8, lsl #4
10001c734:     	str	w2, [x8, #0xc]
10001c738:     	b	0x10001c740 <__ZN2qb6detail14ask_unregisterEy+0x180>
10001c73c:     	str	w2, [x9]
10001c740:     	str	w2, [x9, #0x4]
10001c744:     	ldp	x29, x30, [sp, #0x10]
10001c748:     	ldp	x20, x19, [sp], #0x20
10001c74c:     	ret
10001c750:     	bl	0x1000665a8 <___tls_init.cold.1>
10001c754:     	b	0x10001c5e4 <__ZN2qb6detail14ask_unregisterEy+0x24>
10001c758:     	mov	w8, #-0x4               ; =-4
10001c75c:     	str	w8, [x9, #0xc]
10001c760:     	ldp	x29, x30, [sp, #0x10]
10001c764:     	ldp	x20, x19, [sp], #0x20
10001c768:     	ret
10001c76c:     	ldp	x29, x30, [sp, #0x10]
10001c770:     	ldp	x20, x19, [sp], #0x20
10001c774:     	b	0x10002ee34 <__ZN2qb6detail12_GLOBAL__N_19ask_table15release_unboundENS_7ActorIdEj>
