# MapLibre Native — Renderer Backend C ABI Design Proposal

- **Status:** Draft for discussion
- **Date:** 2026-06-03
- **Author:** (you) with Claude
- **Scope:** Expose the existing `mbgl::gfx` rendering-backend abstraction across a stable C ABI, cut at the *backend / context / buffer* layer.

> This proposal builds directly on
> [`2022-10-27-rendering-modularization.md`](2022-10-27-rendering-modularization.md),
> which introduced the `gfx` abstraction (Context, RendererBackend,
> CommandEncoder, UploadPass, Drawables/Builders, ShaderRegistry) and the
> multi-backend (GL / Metal / Vulkan / WebGPU) split. That work created a clean
> **C++ virtual** seam. This proposal turns that seam into a stable **C ABI**.

---

## Motivation

MapLibre Native already has a well-factored rendering abstraction. Concrete
backends (`gl::`, `mtl::`, `vulkan::`, `webgpu::`) implement a set of C++ virtual
interfaces — `gfx::RendererBackend`, `gfx::Context`, `gfx::CommandEncoder`,
`gfx::UploadPass`, `gfx::RenderPass`, plus resource types (`VertexBufferResource`,
`IndexBufferResource`, `UniformBuffer`, `Texture2D`, `Drawable`, …). The core
(`Renderer` → `RendererImpl` → `PaintParameters`) drives a frame entirely through
these virtual calls.

That seam is currently a **C++ ABI**, which limits us in four ways the project
wants to overcome:

1. **Out-of-tree backends.** Adding a backend for a new GPU API (or a custom /
   proprietary one) today means landing C++ inside the `mbgl` tree and matching
   its compiler/STL/`-fno-exceptions` build settings. We want a backend to live
   in a *separate* library.
2. **Language-agnostic / Rust.** A C++ vtable cannot be implemented or consumed
   from Rust (or any non-C++ language) without a fragile, compiler-specific shim.
   A C ABI can.
3. **Stable ABI / decoupling.** The C++ ABI churns with every compiler and
   library change. A C ABI lets core and a renderer be built and versioned as
   independent binaries.
4. **Embedder-driven.** Hosts that want to create buffers and *push data into
   them* themselves (custom layers, data overlays) currently need C++ access to
   `gfx`. A C boundary makes that a first-class, supported surface.

The goal: **a stable, versioned C interface at the "instantiate a backend → create
buffers → push into buffers → encode passes → present" level**, while the
drawable/layer/render-loop machinery stays in core C++.

### A note on direction (recording a deliberate tension)

During scoping we chose:

- **Boundary:** backend + context + buffers (not a thin GPU device, not the whole
  `Map`/`Renderer`).
- **Direction:** *handle-based* C functions (`mln_context_create_vertex_buffer(ctx, …)`)
  operating on **opaque handles**, rather than a single fat callback `struct`
  (vtable) that a backend fills in.

These two answers are in mild tension with goal #1 (out-of-tree backends imply
*someone other than core* supplies the implementation). The reconciliation, which
this proposal adopts, is:

> The C ABI is expressed as **opaque handles + free functions** (the chosen
> handle style). Core ships **in-tree reference backends** (GL/Vulkan/Metal/WebGPU)
> behind those functions. An **out-of-tree provider** plugs in by registering one
> implementation table at load time; core then dispatches the same handle-based
> functions to it. Embedders and FFI callers only ever see the handle functions
> and never need to know which provider is behind them.

In other words: handle functions are the *public* surface (one API for callers in
C, Rust, Swift, …); a single registration entrypoint is the *private* mechanism
that lets the implementation be in-tree **or** out-of-tree. This keeps "core
implements (handles)" true for every *caller*, while still enabling out-of-tree
backends.

---

## Proposed Change

### 1. Where the seam is cut

```
            ┌─────────────────────────────────────────────┐
            │  mbgl core  (stays C++)                      │
            │  Map · Renderer · RendererImpl · Orchestrator │
            │  RenderLayers · Buckets · Drawables · Tweakers│
            │  ShaderRegistry · style · tiles               │
            └───────────────────────┬─────────────────────┘
                                     │   mln_gfx.h   (STABLE C ABI)
   ┌─────────────────────────────────┼──────────────────────────────────┐
   │  mln_renderer_backend / mln_context / mln_command_encoder           │
   │  mln_upload_pass / mln_render_pass / mln_renderable                 │
   │  mln_vertex_buffer / mln_index_buffer / mln_uniform_buffer          │
   │  mln_texture2d / mln_drawable* / mln_shader*                        │
   └─────────────────────────────────┬──────────────────────────────────┘
                                     │  implemented by
        ┌────────────────────────────┼─────────────────────────────┐
        │ in-tree: GL · Vulkan · Metal · WebGPU                      │
        │ out-of-tree provider: custom C / Rust / proprietary        │
        └────────────────────────────────────────────────────────────┘
```

The C ABI corresponds 1:1 to today's C++ virtual classes:

| C++ (`mbgl::gfx::`)            | C ABI handle (`mln_*`)           | Notes |
|--------------------------------|----------------------------------|-------|
| `RendererBackend`              | `mln_renderer_backend`           | instantiate + drive a backend |
| `Renderable` / `RenderableResource` | `mln_renderable`            | the surface/swapchain target |
| `Context`                      | `mln_context`                    | resource factory + frame lifecycle |
| `CommandEncoder`               | `mln_command_encoder`            | per-frame command recording |
| `UploadPass`                   | `mln_upload_pass`                | buffer/texture creation + update |
| `RenderPass`                   | `mln_render_pass`                | draw scope, clears |
| `VertexBufferResource`         | `mln_vertex_buffer`              | "create buffer" |
| `IndexBufferResource`          | `mln_index_buffer`               | "create buffer" |
| `UniformBuffer` / `…Array`     | `mln_uniform_buffer` / `…_array` | "push into buffer" via update |
| `Texture2D` / `DynamicTexture` | `mln_texture2d` / `mln_dynamic_texture` | |
| `Drawable` / `DrawableBuilder` | `mln_drawable` / `mln_drawable_builder` | standardized renderable-buffer bundle — see §4 |
| `VertexAttributeArray`         | `mln_vertex_attr_array`          | per-vertex / per-instance attribute set |
| `PaintParameters` (subset)     | `mln_draw_context`               | opaque per-frame draw context passed to `draw` |
| `ShaderProgramBase` / `ShaderRegistry` | `mln_shader` / `mln_shader_registry` | |

**What stays in C++ (does not cross the ABI):** `Renderer`, `RendererImpl`, the
render-pass orchestration and render loop in `renderer_impl.cpp`, `RenderLayer`s,
`Bucket`s, tweakers, the style/tile system. These *call* the C ABI; they are not
exposed by it.

**What crosses the ABI as objects:** GPU resources (buffers, textures, shaders)
**and drawables** (§4). A drawable is a backend-manufactured object; core's C++
render loop still *owns the orchestration* — it collects drawables into layer
groups, sorts them, and per pass calls across the ABI to draw each. So "layers and
the render loop stay in C++" holds, while the *renderable units themselves* are
created and drawn through the C interface.

### 2. ABI style: opaque handles + free functions

Every object is an **opaque, forward-declared struct pointer**. No layout is ever
exposed across the boundary. All operations are free functions taking the handle
as the first argument. Example (illustrative — not final):

```c
/* mln_gfx.h  — C99, no C++ types, no STL, extern "C" */
#ifndef MLN_GFX_H
#define MLN_GFX_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ABI version ---------------------------------------------------- */
#define MLN_GFX_ABI_MAJOR 1
#define MLN_GFX_ABI_MINOR 0
uint32_t mln_gfx_abi_version(void);   /* (major<<16)|minor of the loaded impl */

/* ---- opaque handles ------------------------------------------------- */
typedef struct mln_context           mln_context;
typedef struct mln_command_encoder   mln_command_encoder;
typedef struct mln_upload_pass       mln_upload_pass;
typedef struct mln_render_pass       mln_render_pass;
typedef struct mln_renderable        mln_renderable;
typedef struct mln_vertex_buffer     mln_vertex_buffer;
typedef struct mln_index_buffer      mln_index_buffer;
typedef struct mln_uniform_buffer    mln_uniform_buffer;
typedef struct mln_texture2d         mln_texture2d;
/* … */

/* ---- enums (ABI-stable mirrors of gfx/types.hpp) -------------------- */
typedef enum {
    MLN_BUFFER_USAGE_STREAM_DRAW  = 0,
    MLN_BUFFER_USAGE_STATIC_DRAW  = 1,
    MLN_BUFFER_USAGE_DYNAMIC_DRAW = 2,
} mln_buffer_usage;                              /* == gfx::BufferUsageType */

/* ---- result codes (no C++ exceptions cross the boundary) ------------ */
typedef enum {
    MLN_OK = 0,
    MLN_ERR_OUT_OF_MEMORY,
    MLN_ERR_INVALID_ARGUMENT,
    MLN_ERR_UNSUPPORTED,
    MLN_ERR_DEVICE_LOST,
    MLN_ERR_INTERNAL,
} mln_result;

/* ---- frame lifecycle ------------------------------------------------ */
void mln_context_begin_frame(mln_context*);
void mln_context_end_frame(mln_context*);
void mln_context_perform_cleanup(mln_context*);

mln_command_encoder* mln_context_create_command_encoder(mln_context*);
void                 mln_command_encoder_destroy(mln_command_encoder*);

mln_upload_pass* mln_command_encoder_create_upload_pass(
    mln_command_encoder*, const char* name, mln_renderable*);

/* ---- "create buffers / push into buffers" --------------------------- */
mln_vertex_buffer* mln_upload_pass_create_vertex_buffer(
    mln_upload_pass*, const void* data, size_t size,
    mln_buffer_usage usage, int persistent);

void mln_upload_pass_update_vertex_buffer(
    mln_upload_pass*, mln_vertex_buffer*, const void* data, size_t size);

mln_uniform_buffer* mln_context_create_uniform_buffer(
    mln_context*, const void* data, size_t size, int persistent, int ssbo);

void mln_uniform_buffer_update(mln_uniform_buffer*, const void* data, size_t size);

#ifdef __cplusplus
}
#endif
#endif /* MLN_GFX_H */
```

The signatures map straight onto the existing C++ methods (e.g.
`mln_upload_pass_create_vertex_buffer` ↔
`gfx::UploadPass::createVertexBufferResource(const void*, size_t, BufferUsageType, bool)`
in `src/mbgl/gfx/upload_pass.hpp`; `mln_uniform_buffer_update` ↔
`gfx::UniformBuffer::update(const void*, size_t)` in
`include/mbgl/gfx/uniform_buffer.hpp`).

### 3. Buffer creation & "push into buffers" semantics

The two distinct mechanisms in the codebase are preserved verbatim:

- **Vertex/Index buffers** are owned per-`UploadPass`. Create returns an opaque
  handle wrapping the GPU resource (in GL: a `BufferID` behind `UniqueBuffer`,
  see `src/mbgl/gl/upload_pass.cpp`). `update_*` re-uploads into an existing
  handle. The `persistent` hint is forwarded unchanged.
- **Uniform buffers** are created on the `Context` and updated in place via
  `update()`; the `UniformBufferArray` (`mln_uniform_buffer_array`) groups them by
  slot id and is what gets bound to a `RenderPass`. "Push" = `create` once,
  `update` per-frame, `bind` during the pass — exactly today's `createOrUpdate` /
  `bind` flow.

This is the literal "instantiate a backend, create buffers, manage push into
buffers" workflow you described, now expressible in C.

### 4. Drawables and the two-tier rendering surface

The conceptual heart of what we want: a C-ABI **"renderer" that accepts work at two
tiers and can draw both**.

**Tier 1 — Raw primitives (escape hatch, "non-standard rendering").** The caller
brings its own buffers (vertex / index / uniform), a shader, attribute bindings,
and draw state, and issues a draw into a render pass. Maximum flexibility, no
assumptions about layout. This is the §3 buffer + upload-pass + render-pass surface
plus one explicit draw call.

**Tier 2 — Drawables (standardized renderable-buffer bundles).** A *drawable*
packages the common case — the exact bundle MapLibre's layers already emit — into a
single object the renderer knows how to draw: vertex attributes (+ optional
instance attributes), index data + segments, a shader, up to N textures, a
uniform-buffer array, and render state (depth / stencil / color / cull, render pass,
sub-layer, 2D/3D). Drawables map 1:1 onto `gfx::Drawable`
(`include/mbgl/gfx/drawable.hpp`) and are produced through `gfx::DrawableBuilder`
(`include/mbgl/gfx/drawable_builder.hpp`).

A drawable *is* "a standard type of renderable buffer": Tier 2 is built out of
Tier-1 primitives, but standardized and lifecycle-managed (upload → bind → draw →
update-per-frame → teardown) by the backend.

```
host / core (C++ render loop)
        │
        │ Tier 2: standardized                    Tier 1: raw
        ▼                                          ▼
 mln_drawable_builder  ──flush──►  mln_drawable      mln_vertex_buffer
   add_vertices / add_triangles      │ shader         mln_index_buffer
   set_shader / set_texture          │ textures       mln_uniform_buffer
   set_segments / set_raw_vertices   │ ubo array      + bindings + draw state
   set draw state                    │ state               │
                                     ▼                      ▼
                       mln_drawable_draw(d, draw_ctx)   mln_render_pass_draw(pass, …)
                                     └───────────┬──────────┘
                                                 ▼
                                         backend GPU submit
```

#### Who owns what (drawables vs the render loop)

Per the stated constraint, **layers and the render loop stay in C++.** The split:

- **Created over the C ABI:** drawables and their builders. A drawable is a
  *backend-specific* object — in GL it carries VAOs, per-segment objects, and
  attribute buffers (`src/mbgl/gl/drawable_gl_impl.hpp`). So the backend (in-tree,
  or an out-of-tree provider) is what manufactures drawables, exactly as today.
- **Owned / orchestrated in C++:** the render loop (`renderer_impl.cpp`) still
  collects drawables into `LayerGroup` / `TileLayerGroup`, sorts them by priority,
  and visits them per pass. It draws each by calling across the ABI —
  `mln_drawable_draw(d, draw_ctx)` — which dispatches to the backend's drawable
  implementation.

Consequences that satisfy the four goals:

- An **out-of-tree / Rust backend supplies the drawable implementation** (Mode B):
  it decides how a drawable uploads and draws on its GPU API, while core's loop is
  unchanged.
- A **host can inject custom drawables** (Mode A): build one via the C builder,
  flag it custom (`mln_drawable_set_is_custom`, mirroring `Drawable::setIsCustom`),
  and hand it to core to place in a layer group — the same seam the
  [plugin-layers proposal](2025-05-08-plugin-layers.md) targets, but at the
  drawable level rather than a raw context handoff.

#### Builder surface (illustrative)

```c
typedef struct mln_drawable          mln_drawable;
typedef struct mln_drawable_builder  mln_drawable_builder;
typedef struct mln_vertex_attr_array mln_vertex_attr_array;

typedef enum { MLN_DRAW_MODE_POINTS, MLN_DRAW_MODE_LINES,
               MLN_DRAW_MODE_TRIANGLES /* … */ } mln_draw_mode;

typedef struct {                 /* mirrors gfx::SegmentBase */
    uint32_t vertex_offset, index_offset;
    uint32_t vertex_length, index_length;
} mln_segment;

mln_drawable_builder* mln_context_create_drawable_builder(mln_context*, const char* name);

void mln_drawable_builder_set_shader (mln_drawable_builder*, mln_shader*);
void mln_drawable_builder_set_texture(mln_drawable_builder*, mln_texture2d*, size_t slot);

/* Tier-2 geometry helpers (mirror DrawableBuilder::addTriangle / addVertices / …) */
size_t mln_drawable_builder_add_vertices (mln_drawable_builder*,
             const int16_t (*verts)[2], size_t offset, size_t count);
void   mln_drawable_builder_add_triangles(mln_drawable_builder*,
             const uint16_t* idx, size_t offset, size_t length, size_t base);

/* Tier-1 escape hatch carried inside a drawable (mirror setRawVertices / setSegments) */
void mln_drawable_builder_set_raw_vertices(mln_drawable_builder*,
             const void* data, size_t bytes, size_t count, mln_attr_data_type);
void mln_drawable_builder_set_segments    (mln_drawable_builder*,
             mln_draw_mode, const uint16_t* idx, size_t idx_count,
             const mln_segment*, size_t seg_count);

/* draw state (mirror the builder setters in drawable_builder.hpp) */
void mln_drawable_builder_set_render_pass(mln_drawable_builder*, mln_render_pass_bits);
void mln_drawable_builder_set_depth_mode (mln_drawable_builder*, mln_depth_mode);
void mln_drawable_builder_set_color_mode (mln_drawable_builder*, mln_color_mode);
void mln_drawable_builder_set_cull_mode  (mln_drawable_builder*, mln_cull_face_mode);
void mln_drawable_builder_set_is_3d      (mln_drawable_builder*, int);

/* flush → take ownership of completed drawables (mirror flush()/clearDrawables()) */
void          mln_drawable_builder_flush         (mln_drawable_builder*, mln_context*);
size_t        mln_drawable_builder_drawable_count(mln_drawable_builder*);
mln_drawable* mln_drawable_builder_take_drawable (mln_drawable_builder*, size_t i);
```

Per-drawable mutation used per-frame by tweakers / hosts maps onto the `Drawable`
setters — uniform-buffer update, texture swap, enable/disable, render-pass mask:

```c
void mln_drawable_set_enabled  (mln_drawable*, int);
void mln_drawable_set_is_custom(mln_drawable*, int);
void mln_drawable_set_tile_id  (mln_drawable*, const mln_overscaled_tile_id*);
void mln_drawable_set_texture  (mln_drawable*, mln_texture2d*, size_t slot);
mln_uniform_buffer_array* mln_drawable_uniform_buffers(mln_drawable*);  /* mutable */
```

#### Drawing

The render loop draws a drawable by passing a **draw context** — an opaque handle
wrapping the C++ `PaintParameters` (encoder, active render pass, matrices, global
UBOs). This keeps the rich per-frame state in C++ while letting the backend's
drawable draw against it:

```c
typedef struct mln_draw_context mln_draw_context;          /* wraps PaintParameters */
void mln_drawable_draw(mln_drawable*, mln_draw_context*);   /* == Drawable::draw(PaintParameters&) */
```

For Tier 1, the same render pass exposes a direct draw taking explicit bindings
instead of a drawable:

```c
void mln_render_pass_draw(mln_render_pass*,
        mln_shader*, const mln_attr_binding* bindings, size_t binding_count,
        mln_index_buffer*, mln_draw_mode, uint32_t index_offset, uint32_t index_count,
        const mln_draw_state*);
```

#### Why both tiers

- **Drawables** cover everything MapLibre's own layers need and everything a typical
  custom layer wants — a stable, high-level, GPU-portable bundle. Most callers and
  every in-tree layer use this tier.
- **Raw primitives** are the escape valve for genuinely non-standard rendering
  (compute-style passes, exotic vertex layouts, third-party effects) that don't fit
  the drawable mold — without forcing those callers to fake a drawable.

### 5. Two consumption modes (one ABI)

**Mode A — Core-bundled backend, host drives (FFI / embedder-driven).**
Core exports the `mln_*` symbols, implemented by an in-tree backend selected at
build time via the existing `MLN_RENDER_BACKEND_*` flags (`include/mbgl/gfx/backend.hpp`).
A Rust/Swift/C host obtains a `mln_context` and creates/updates buffers. The
`mln_*` functions are thin wrappers that hold a `gfx::*` pointer and forward.

```
   Rust host ──calls──► mln_context_create_vertex_buffer ──► gl::UploadPass::createVertexBufferResource
```

**Mode B — Out-of-tree provider supplies the backend (out-of-tree / Rust backend / decoupling).**
A provider library exports a *single* registration entrypoint returning an
implementation table; core loads it (static registration or `dlopen`) and routes
the same handle functions to it.

```c
typedef struct {
    uint32_t abi_version;                         /* must match core */
    /* one function pointer per mln_* operation, same signatures */
    mln_context* (*context_create)(void* native_window, mln_result*);
    mln_vertex_buffer* (*upload_pass_create_vertex_buffer)(
        mln_upload_pass*, const void*, size_t, mln_buffer_usage, int);
    /* … full table … */
} mln_backend_provider;

/* exported by the provider library; core calls it once */
const mln_backend_provider* mln_backend_provider_entry(void);
```

Internally, a C++ adapter class `CAbiContext : public gfx::Context` (and peers for
`CommandEncoder`/`UploadPass`/etc.) implements each virtual by calling the
provider's function pointers. So core's `RendererImpl` is unchanged — it still
sees a `gfx::Context&`; that context just happens to be backed by a C provider.

> Note: Mode B's `mln_backend_provider` table *is* effectively a vtable — but it
> is private plumbing, not the public surface. Callers/embedders use only the
> handle functions (Mode A surface). This is the reconciliation of the
> "handles vs vtable" tension recorded above.

### 6. Surfaces, backend instantiation, and `getDefaultRenderable`

`gfx::RendererBackend` carries platform-specific surface creation
(`getDefaultRenderable`, `activate`/`deactivate`, and for GL
`getExtensionFunctionPointer`). The native handle (NSWindow*, ANativeWindow*,
HWND, GL proc-address loader, …) is necessarily opaque, so the C ABI passes it as
`void*` plus a small descriptor enum:

```c
typedef struct {
    mln_backend_api  api;          /* GL / VULKAN / METAL / WEBGPU */
    void*            native_window;/* platform-specific */
    void* (*gl_get_proc_address)(const char* name); /* GL only, may be NULL */
    uint32_t width, height;
    float    pixel_ratio;
} mln_backend_desc;

mln_renderer_backend* mln_renderer_backend_create(const mln_backend_desc*, mln_result*);
mln_renderable*       mln_renderer_backend_default_renderable(mln_renderer_backend*);
mln_context*          mln_renderer_backend_context(mln_renderer_backend*);
void                  mln_renderer_backend_destroy(mln_renderer_backend*);
```

`activate`/`deactivate` map to `BackendScope` usage inside core and need not be
called by hosts directly in Mode A.

### 7. Enums, structs, and value types

`include/mbgl/gfx/types.hpp` enums (`BufferUsageType`, `TexturePixelType`,
`TextureChannelDataType`, `DepthFunctionType`, `StencilOpType`,
`ColorBlendFactorType`, `TextureFilterType`/`WrapType`, `RenderbufferPixelType`,
…) get fixed-width C mirrors with **explicitly numbered values** pinned by
`static_assert` against the C++ enum in a single `abi_asserts.cpp`, so drift is a
compile error. Small POD value types (`Size`, `Color`, `SamplerState`,
`DepthMode`, `StencilMode`, `ColorMode`, segment descriptors) become C `struct`s
with documented, padded layouts.

### 8. Threading, ownership, and errors

- **Threading.** Uploads happen on worker threads while rendering happens on the
  render thread (see ARCHITECTURE.md "Threading"). The C ABI does not add
  threading; it documents the same contract: handles are not thread-safe; a
  handle must be used on the thread its owning context/encoder dictates. This is
  already true of `gfx`.
- **Ownership.** Every `_create`/`create_*` that returns a handle has a matching
  `_destroy` **or** is owned by a parent (upload-pass buffers, encoder passes).
  The header documents ownership per function. No ownership transfers implicitly
  across the boundary except where named (`_take_*`).
- **Errors.** No C++ exception may cross the ABI. Functions either return
  `mln_result` or take an `mln_result*` out-param. The boundary wrappers wrap
  bodies in `try { … } catch(...)` and translate. (Core builds with
  `-fno-exceptions` in places; the adapter respects that.)
- **Versioning.** `mln_gfx_abi_version()` + the `abi_version` field in the
  provider table gate compatibility. Additive changes bump minor; layout/removal
  bumps major. New functions are appended; the provider table is
  size-prefixed/extensible.

### 9. How this lands against existing code

- New public header tree: `include/mbgl/capi/mln_gfx.h` (+ split headers).
- New implementation dir: `src/mbgl/capi/` containing (a) thin forwarding
  wrappers for Mode A, and (b) the `CAbi*` adapter classes implementing `gfx::*`
  for Mode B.
- The existing `Backend::Create<Type>` specializations
  (`platform/*/..._renderer_backend.cpp`) gain a sibling that produces a
  `gfx::RendererBackend` from an `mln_backend_provider` (Mode B).
- Zero changes required in `renderer_impl.cpp` — it keeps consuming
  `gfx::Context&` and calling `Drawable::draw(PaintParameters&)`; the
  `mln_drawable_draw` / `mln_draw_context` pairing is just the C spelling of that
  same call, used only when a drawable is backed by an out-of-tree provider.

---

## API Modifications

This is **purely additive** to the public C++ API. New artifacts:

1. A C header set under `include/mbgl/capi/` (stable, versioned, `extern "C"`).
2. New C entry points exported from the core library (Mode A).
3. An `mln_backend_provider` registration entrypoint contract (Mode B).
4. No existing `mbgl::gfx::*` C++ class is changed in signature. (Some may gain a
   factory that accepts a provider table.)

---

## Migration Plan and Compatibility

No breaking changes. Existing in-tree backends and all current SDKs keep working
unchanged — they continue to use the C++ `gfx` interface directly. The C ABI is an
*additional* way to reach the same machinery.

Suggested phased delivery (each phase leaves the tree shippable, mirroring the
2022 modularization proposal's PR cadence):

- **Phase 0 — ABI skeleton.** `mln_gfx.h` with handles, enums, result codes,
  `mln_gfx_abi_version`, and the `static_assert` enum-pinning. No behavior.
- **Phase 1 — Tier 1: Context + buffers + raw draw (Mode A).** Wrap `Context`,
  `CommandEncoder`, `UploadPass`, vertex/index/uniform buffers, `Renderable`,
  `RenderPass`, plus `mln_render_pass_draw` (raw primitives). Deliverable: a C/Rust
  test program that creates a context against an offscreen renderable, creates a
  vertex buffer, updates it, binds a shader, and draws a triangle through the raw
  path — validated against the GL backend.
- **Phase 2 — Tier 2: drawables + textures + shaders.** Add `Texture2D`,
  `DynamicTexture`, `ShaderRegistry`, the `mln_drawable` / `mln_drawable_builder`
  surface (§4), and `mln_draw_context` / `mln_drawable_draw`. Deliverable: a full
  frame rendered through drawables built over the C ABI; existing render tests pass
  with the wrapper enabled. This is the phase that proves "a renderer that takes
  both raw buffers and drawables and draws them."
- **Phase 3 — Out-of-tree provider (Mode B).** `mln_backend_provider` +
  `CAbi*` adapters + registration. Deliverable: a trivial out-of-tree backend
  (e.g. a headless/null or software backend) loaded via the provider entrypoint
  and driving `RendererImpl` unmodified.
- **Phase 4 — Rust reference.** A minimal Rust binding crate (handles via
  `bindgen`) exercising Mode A, and a skeleton Rust *provider* exercising Mode B.
- **Phase 5 — Hardening.** ABI test suite, fuzz the boundary, doc the ownership
  table, CI gate on `abi_version` and `static_assert`s.

### Evaluation metrics (in the spirit of the modularization proposal)

1. All existing render tests pass with the Mode-A wrapper enabled (visual parity).
2. No measurable frame-rate regression when rendering through the wrapper vs.
   direct C++ (the wrapper is a non-virtual-to-virtual forward; cost ≈ one extra
   indirect call per op, amortized far below GPU cost).
3. Binary-size increase from the C ABI layer < 2%.
4. A non-C++ language (Rust) can both **drive** (Mode A) and **implement**
   (Mode B) a backend with no C++ shim.

---

## Rejected Alternatives

1. **Single fat callback `struct` (vtable) as the public surface.** Rejected as
   the *public* API per the scoping decision: it's awkward for embedders/FFI and
   for incremental/extensible APIs. We keep handle functions public and use a
   provider table only as private Mode-B plumbing.
2. **Cut the seam lower (thin GPU "device": just buffers/textures/draw).** Smaller
   surface, but it would force the drawable/encoder/pass logic out of the backend
   and into core, diverging from how `gfx` already factors responsibilities and
   making Metal/Vulkan indirect-rendering optimizations harder. Rejected.
3. **Cut the seam higher (expose `Map`/`Renderer` as C — full FFI).** That is a
   different project (a C SDK for the whole engine), not a *renderer backend*
   abstraction, and doesn't satisfy "instantiate a backend / create buffers".
   Worth doing separately; out of scope here.
4. **Generate the ABI from the C++ headers automatically.** Tempting, but the C++
   `gfx` interface uses templates (`VertexBuffer<T>`, `IndexVector<DrawMode>`),
   `std::unique_ptr`/`shared_ptr` ownership, and `std::function` — none of which
   survive mechanical C extraction cleanly. A hand-authored, reviewed ABI with
   `static_assert` pinning is safer.
5. **Depend on an external intermediate graphics toolkit (e.g. wrap WebGPU/Dawn
   only).** Same rationale the 2022 proposal gave for rejecting an intermediate
   toolkit: toolkit-size sensitivity and loss of control over backend-specific
   optimizations. The C ABI is provider-agnostic — a WebGPU provider is *one
   possible* Mode-B implementation, not a hard dependency.

---

## Open Questions for Reviewers

1. **Drawable/Builder surface size (Phase 2):** we commit to exposing drawables
   (§4), but the `DrawableBuilder` surface is large
   (`include/mbgl/gfx/drawable_builder.hpp` — polylines, wide vectors, quads,
   instance attributes, tweakers). Minimum viable for Phase 2 is
   `set_shader` + `set_raw_vertices`/`set_segments` + `set_texture` + draw state +
   `flush`; the geometry-helper conveniences (`add_polyline`,
   `add_wide_vector_*`, instance attributes) can be added incrementally. Which
   conveniences are must-have for the first cut?
2. **Drawable draw context:** `mln_draw_context` wraps `PaintParameters`, which is
   a rich core C++ object. Do we expose it purely opaquely (pass-through, the
   simple choice), or surface a curated subset (matrices, current pass, global
   UBOs) so an out-of-tree backend can render without reaching back into core?
3. **Provider loading:** static registration only, or also `dlopen`-style dynamic
   loading? Dynamic loading maximizes decoupling but adds packaging/signing
   concerns on iOS.
4. **Shader source vs. precompiled:** how the `ShaderRegistry` ABI handles
   Metal/Vulkan precompiled blobs vs. GLSL source (ties into the 2022 shader
   work).
5. **Where should this doc live** — kept here in `design-proposals/` (repo
   convention), or moved under `docs/`?
