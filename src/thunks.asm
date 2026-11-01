; ============================================================================
; thunks.asm - d3d12 导出转发 thunk（x64, ml64）
; thunk 清单由 gen_exports.exe 生成于 generated_thunks.inc，
; 与 generated_names.h 的 kExportNames[] 索引一一对应。
; ============================================================================

EXTERN g_procs:QWORD

.code

THUNK MACRO tname, idx
tname proc
    jmp qword ptr [g_procs + idx*8]
tname endp
ENDM

INCLUDE generated_thunks.inc

END
