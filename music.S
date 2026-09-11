/* The organ, linked straight into the binary: Bach BWV 639, CC0. See CREDITS.md. */
	.section .rodata
	.balign 64
	.global music_mp3
music_mp3:
	.incbin "assets/music.mp3"
	.global music_mp3_end
music_mp3_end:
	.balign 4
