// saintsrow_config.h - convenience macros for the hand-written sources.
// The image base/size macros (REX_IMAGE_BASE, REX_CODE_BASE, ...) come from
// the generated saintsrow_pch.h.
#pragma once

// PPC_FUNC(name) overrides a recompiled function. The generated code makes
// every recompiled function a weak extern "C" symbol `name` that thunks to
// `__imp__name`; defining a strong symbol with the same name and signature
// replaces it at link time.
#define PPC_FUNC(x) extern "C" void x(PPCContext& __restrict ctx, uint8_t* base)

// Guest memory access (base-relative, byte-swapped); aliases of the SDK's
// REX_LOAD_* / REX_STORE_* macros.
#define PPC_LOAD_U8(x)      REX_LOAD_U8(x)
#define PPC_LOAD_U16(x)     REX_LOAD_U16(x)
#define PPC_LOAD_U32(x)     REX_LOAD_U32(x)
#define PPC_LOAD_U64(x)     REX_LOAD_U64(x)
#define PPC_STORE_U8(x, y)  REX_STORE_U8(x, y)
#define PPC_STORE_U16(x, y) REX_STORE_U16(x, y)
#define PPC_STORE_U32(x, y) REX_STORE_U32(x, y)
#define PPC_STORE_U64(x, y) REX_STORE_U64(x, y)

// PPC_FUNC_IMPL(__imp__Name) overrides the SDK's implementation of a kernel
// import. Same mechanism as PPC_FUNC.
#define PPC_FUNC_IMPL(x) extern "C" void x(PPCContext& __restrict ctx, uint8_t* base)
