#ifndef HSD_NATIVE_IMAGE
#error HSD_NATIVE_IMAGE must name the built native helper
#endif
.section .rdata,"dr"
.balign 4
.global _hsd_native_image
.global _hsd_native_image_end
_hsd_native_image:
.incbin HSD_NATIVE_IMAGE
_hsd_native_image_end:
