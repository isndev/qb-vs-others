
develop-f0/qvoprobe-ask-any-scatter:	file format mach-o arm64

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
10001c378:     	cbz	w8, 0x10001c460 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x114>
10001c37c:     	adrp	x0, 0x100088000 <__ZN2qb6router4memhINS_5EventELb1EvE14_disposers_mtxE>
10001c380:     	add	x0, x0, #0x930
10001c384:     	ldr	x8, [x0]
10001c388:     	blr	x8
10001c38c:     	mov	x21, x0
10001c390:     	ldr	w8, [x0, #0x28]
10001c394:     	lsr	w1, w20, #24
10001c398:     	cmp	w8, w1
10001c39c:     	b.ne	0x10001c468 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x11c>
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
10001c3d0:     	b.eq	0x10001c474 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x128>
10001c3d4:     	ldr	w8, [x22, #0x8]
10001c3d8:     	cmp	x24, x8
10001c3dc:     	b.hs	0x10001c488 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x13c>
10001c3e0:     	ldr	x8, [x22]
10001c3e4:     	add	x22, x8, x24, lsl #3
10001c3e8:     	ldr	x8, [x22]
10001c3ec:     	cbz	x8, 0x10001c4a4 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x158>
10001c3f0:     	lsr	x22, x20, #24
10001c3f4:     	add	x10, x8, w23, uxtb #3
10001c3f8:     	ldr	w8, [x10]
10001c3fc:     	cmn	w8, #0x1
10001c400:     	b.eq	0x10001c4c4 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x178>
10001c404:     	ldr	x9, [x21]
10001c408:     	add	x11, x9, x8, lsl #4
10001c40c:     	ldr	w11, [x11, #0xc]
10001c410:     	str	w11, [x10]
10001c414:     	cmn	w11, #0x1
10001c418:     	b.ne	0x10001c420 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0xd4>
10001c41c:     	str	w11, [x10, #0x4]
10001c420:     	cmp	x19, #0x0
10001c424:     	csel	x10, x20, x19, eq
10001c428:     	mov	w11, #-0x3              ; =-3
10001c42c:     	cinc	w11, w11, ne
10001c430:     	add	x9, x9, x8, lsl #4
10001c434:     	str	x10, [x9]
10001c438:     	str	w11, [x9, #0xc]
10001c43c:     	ubfiz	x10, x22, #48, #8
10001c440:     	ldr	w9, [x9, #0x8]
10001c444:     	orr	x9, x10, x9, lsl #22
10001c448:     	orr	x0, x9, x8
10001c44c:     	ldp	x29, x30, [sp, #0x30]
10001c450:     	ldp	x20, x19, [sp, #0x20]
10001c454:     	ldp	x22, x21, [sp, #0x10]
10001c458:     	ldp	x24, x23, [sp], #0x40
10001c45c:     	ret
10001c460:     	bl	0x100066620 <___tls_init.cold.1>
10001c464:     	b	0x10001c37c <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x30>
10001c468:     	mov	x0, x21
10001c46c:     	bl	0x10002ed44 <__ZN2qb6detail12_GLOBAL__N_19ask_table11select_coreEt>
10001c470:     	b	0x10001c3a0 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x54>
10001c474:     	add	x8, x22, x24, lsl #3
10001c478:     	add	x22, x8, #0x10
10001c47c:     	ldr	x8, [x22]
10001c480:     	cbnz	x8, 0x10001c3f0 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0xa4>
10001c484:     	b	0x10001c4a4 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0x158>
10001c488:     	add	x1, x24, #0x1
10001c48c:     	mov	x0, x22
10001c490:     	bl	0x10002ee00 <__ZN2qb6detail12_GLOBAL__N_19ask_table14grow_directoryERNS2_15owner_directoryEm>
10001c494:     	ldr	x8, [x22]
10001c498:     	add	x22, x8, x24, lsl #3
10001c49c:     	ldr	x8, [x22]
10001c4a0:     	cbnz	x8, 0x10001c3f0 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0xa4>
10001c4a4:     	mov	x0, x22
10001c4a8:     	bl	0x10002ee78 <__ZN2qb6detail12_GLOBAL__N_19ask_table11create_pageEPPNSt3__15arrayINS2_10free_queueELm256EEE>
10001c4ac:     	ldr	x8, [x22]
10001c4b0:     	lsr	x22, x20, #24
10001c4b4:     	add	x10, x8, w23, uxtb #3
10001c4b8:     	ldr	w8, [x10]
10001c4bc:     	cmn	w8, #0x1
10001c4c0:     	b.ne	0x10001c404 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0xb8>
10001c4c4:     	mov	x0, x21
10001c4c8:     	bl	0x10002eca4 <__ZN2qb6detail12_GLOBAL__N_19ask_table15take_unassignedEv>
10001c4cc:     	ldr	x9, [x21]
10001c4d0:     	mov	w8, w0
10001c4d4:     	b	0x10001c420 <__ZN2qb6detail8ask_takeENS_7ActorIdEPNS0_8ask_slotE+0xd4>
10001c4d8:     	bl	0x100001b94 <___clang_call_terminate>
