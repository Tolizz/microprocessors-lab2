	.global myadd
	.p2align 2
	.type myadd,%function
		
		myadd:
		.fnstart
		add r0, r0, r1
		
		bx lr
		
		.fnend