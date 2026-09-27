# 2ksbox: the d3dpt-vga display adapter (doc 15; overlay of d3dpt/hw),
# which reuses the VGA core, and the executor loader it opens.
i386_system_ss.add(files('d3dpt_exec_load.c'))
i386_system_ss.add(when: 'CONFIG_VGA', if_true: files('d3dpt_vga.c'))
