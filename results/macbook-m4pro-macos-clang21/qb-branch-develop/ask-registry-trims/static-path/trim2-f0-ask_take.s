
trim2-f0/qvoprobe-ask-any-scatter:	file format mach-o arm64

Disassembly of section __TEXT,__text:

000000010001c34c <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE>:
10001c34c:     	stp	x24, x23, [sp, #-0x40]!
10001c350:     	stp	x22, x21, [sp, #0x10]
10001c354:     	stp	x20, x19, [sp, #0x20]
10001c358:     	stp	x29, x30, [sp, #0x30]
10001c35c:     	mov	x19, x1
10001c360:     	mov	x20, x0
10001c364:     	adrp	x0, 0x100088000 <__ZN2qb6router4memhINS_5EventELb1EvE14_disposers_mtxE>
10001c368:     	add	x0, x0, #0xa98
10001c36c:     	ldr	x8, [x0]
10001c370:     	blr	x8
10001c374:     	ldrb	w8, [x0]
10001c378:     	cbz	w8, 0x10001c45c <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x110>
10001c37c:     	adrp	x0, 0x100088000 <__ZN2qb6router4memhINS_5EventELb1EvE14_disposers_mtxE>
10001c380:     	add	x0, x0, #0x930
10001c384:     	ldr	x8, [x0]
10001c388:     	blr	x8
10001c38c:     	mov	x21, x0
10001c390:     	ldr	w8, [x0, #0x28]
10001c394:     	lsr	w1, w20, #24
10001c398:     	cmp	w8, w1
10001c39c:     	b.ne	0x10001c464 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x118>
10001c3a0:     	ldr	x22, [x21, #0x20]
10001c3a4:     	and	w8, w20, #0xffffff
10001c3a8:     	cmp	w8, #0xff0, lsl #12     ; =0xff0000
10001c3ac:     	cset	w9, hi
10001c3b0:     	mov	w10, #0xffffff          ; =16777215
10001c3b4:     	cmp	w8, w10
10001c3b8:     	cset	w10, ne
10001c3bc:     	and	w11, w20, #0xffff
10001c3c0:     	ands	w9, w9, w10
10001c3c4:     	csel	w23, w11, w8, ne
10001c3c8:     	lsr	x24, x23, #8
10001c3cc:     	cmp	w9, #0x1
10001c3d0:     	b.eq	0x10001c470 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x124>
10001c3d4:     	ldr	w8, [x22, #0x8]
10001c3d8:     	cmp	x24, x8
10001c3dc:     	b.hs	0x10001c484 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x138>
10001c3e0:     	ldr	x8, [x22]
10001c3e4:     	add	x22, x8, x24, lsl #3
10001c3e8:     	ldr	x8, [x22]
10001c3ec:     	cbz	x8, 0x10001c4a0 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x154>
10001c3f0:     	add	x22, x8, w23, uxtb #3
10001c3f4:     	ldr	w8, [x22]
10001c3f8:     	cmn	w8, #0x1
10001c3fc:     	b.eq	0x10001c4bc <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x170>
10001c400:     	ldr	x9, [x21]
10001c404:     	add	x10, x9, x8, lsl #4
10001c408:     	ldr	w10, [x10, #0xc]
10001c40c:     	str	w10, [x22]
10001c410:     	cmn	w10, #0x1
10001c414:     	b.ne	0x10001c41c <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0xd0>
10001c418:     	str	w10, [x22, #0x4]
10001c41c:     	lsr	x10, x20, #24
10001c420:     	cbz	x19, 0x10001c4d4 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x188>
10001c424:     	str	x22, [x19, #0x8]
10001c428:     	mov	w11, #-0x2              ; =-2
10001c42c:     	add	x9, x9, x8, lsl #4
10001c430:     	str	x19, [x9]
10001c434:     	str	w11, [x9, #0xc]
10001c438:     	ubfiz	x10, x10, #48, #8
10001c43c:     	ldr	w9, [x9, #0x8]
10001c440:     	orr	x9, x10, x9, lsl #22
10001c444:     	orr	x0, x9, x8
10001c448:     	ldp	x29, x30, [sp, #0x30]
10001c44c:     	ldp	x20, x19, [sp, #0x20]
10001c450:     	ldp	x22, x21, [sp, #0x10]
10001c454:     	ldp	x24, x23, [sp], #0x40
10001c458:     	ret
10001c45c:     	bl	0x1000665a8 <___tls_init.cold.1>
10001c460:     	b	0x10001c37c <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x30>
10001c464:     	mov	x0, x21
10001c468:     	bl	0x10002eccc <__ZN2qb6detail12_GLOBAL__N_19ask_table11select_coreEt>
10001c46c:     	b	0x10001c3a0 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x54>
10001c470:     	add	x8, x22, x24, lsl #3
10001c474:     	add	x22, x8, #0x10
10001c478:     	ldr	x8, [x22]
10001c47c:     	cbnz	x8, 0x10001c3f0 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0xa4>
10001c480:     	b	0x10001c4a0 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x154>
10001c484:     	add	x1, x24, #0x1
10001c488:     	mov	x0, x22
10001c48c:     	bl	0x10002ed88 <__ZN2qb6detail12_GLOBAL__N_19ask_table14grow_directoryERNS2_15owner_directoryEm>
10001c490:     	ldr	x8, [x22]
10001c494:     	add	x22, x8, x24, lsl #3
10001c498:     	ldr	x8, [x22]
10001c49c:     	cbnz	x8, 0x10001c3f0 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0xa4>
10001c4a0:     	mov	x0, x22
10001c4a4:     	bl	0x10002ee00 <__ZN2qb6detail12_GLOBAL__N_19ask_table11create_pageEPPNSt3__15arrayINS2_10free_queueELm256EEE>
10001c4a8:     	ldr	x8, [x22]
10001c4ac:     	add	x22, x8, w23, uxtb #3
10001c4b0:     	ldr	w8, [x22]
10001c4b4:     	cmn	w8, #0x1
10001c4b8:     	b.ne	0x10001c400 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0xb4>
10001c4bc:     	mov	x0, x21
10001c4c0:     	bl	0x10002ec2c <__ZN2qb6detail12_GLOBAL__N_19ask_table15take_unassignedEv>
10001c4c4:     	ldr	x9, [x21]
10001c4c8:     	mov	w8, w0
10001c4cc:     	lsr	x10, x20, #24
10001c4d0:     	cbnz	x19, 0x10001c424 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0xd8>
10001c4d4:     	mov	w11, #-0x3              ; =-3
10001c4d8:     	mov	x19, x20
10001c4dc:     	b	0x10001c42c <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0xe0>
10001c4e0:     	bl	0x100001b94 <___clang_call_terminate>
