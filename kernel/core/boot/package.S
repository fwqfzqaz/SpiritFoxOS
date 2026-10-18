; 构建期内嵌的小型模块 initrd；不依赖磁盘驱动即可加载第一批服务。
section .rodata align=8
global sfm_package_start
global sfm_package_end
sfm_package_start:
incbin "build/modules/init.sfpack"
sfm_package_end:
section .note.GNU-stack noalloc noexec nowrite progbits
