# src/shaders/meson.build — stock shader catalog (Phase 3.C scaffold)
#
# Appends src/shaders/catalog.c to lagfx_sources. Generates the
# catalog_embedded.inc header (bin2c of .spv files) into the build
# tree. If glslangValidator is on PATH, compiles GLSL → SPIR-V per
# pair; otherwise falls back to the pre-committed stub .spv files
# at src/shaders/spv/ (produced by generate_stub_spirv.py).
#
# See paravirt-re/shader-catalog-plan.md §3.3 (build-time
# integration) + paravirt-re/phase-3-metal-vulkan-plan.md §3.C for
# the broader pipeline. This file owns only the Phase 3.C scaffold
# portion — Phase 3.C.2 adds MSL → AIR + hash emission.

# Shader base names — each corresponds to:
#   msl/<name>.metal             (MSL source, for compile_msl.sh)
#   glsl/<name>.vert + .frag     (GLSL twin pair)
#   spv/<name>.vert.spv + .frag.spv  (committed stub SPIR-V)
shader_names = [
  'blit',
  'clear',
  'composite_over',
  'cursor',
  'color_fill',
]

glslang = find_program('glslangValidator', required : false)
python3 = find_program('python3', required : true)

# Build-time SPIR-V artefacts. Either:
#   (a) glslangValidator compiles GLSL → SPIR-V on the fly, or
#   (b) the stub .spv files at src/shaders/spv/ are used as-is.
#
# The output file names are uniform in both branches so the
# downstream embed step doesn't care which path ran.
spirv_outputs = []
spirv_names   = []

foreach name : shader_names
  foreach stage_ext : [['vert', '.vert'], ['frag', '.frag']]
    stage     = stage_ext[0]
    ext       = stage_ext[1]
    glsl_src  = 'glsl' / name + ext
    out_name  = name + '.' + stage + '.spv'
    emit_name = name + '_' + stage   # used as C identifier suffix

    if glslang.found()
      # Real build-time compile via glslangValidator. The output
      # path under meson's build dir is deterministic so the
      # embed step can find it.
      spv = custom_target(
        'spv_' + name + '_' + stage,
        input  : glsl_src,
        output : out_name,
        command : [
          glslang.full_path(), '-V', '-o', '@OUTPUT@', '@INPUT@',
        ],
      )
      spirv_outputs += [spv]
    else
      # Fallback: pre-committed stub .spv files are shipped in
      # the repo. Use configure_file(copy: true) so the embed
      # step gets a path in the build tree (matches the
      # glslangValidator case structurally). See
      # src/shaders/generate_stub_spirv.py for how the stubs
      # were produced and src/shaders/README.md (pending Phase
      # 3.C.2) for the refresh workflow.
      spv = configure_file(
        input  : 'spv' / out_name,
        output : out_name,
        copy   : true,
      )
      spirv_outputs += [spv]
    endif
    spirv_names += [emit_name]
  endforeach
endforeach

if not glslang.found()
  warning(
    'src/shaders: glslangValidator not on PATH; using committed '
    + 'stub SPIR-V from src/shaders/spv/. Install glslang to '
    + 'regenerate from GLSL on build. '
    + 'See paravirt-re/shader-catalog-plan.md §3.3.'
  )
endif

# Embed all SPIR-V blobs into one generated C include file that
# catalog.c #include's. The generator takes <name>=<path> pairs.
embed_args = [python3.full_path(),
              meson.current_source_dir() / 'embed_spirv.py',
              '@OUTPUT@']
i = 0
foreach spv : spirv_outputs
  embed_args += [spirv_names[i] + '=@INPUT' + i.to_string() + '@']
  i += 1
endforeach

catalog_embedded = custom_target(
  'catalog_embedded_inc',
  input  : spirv_outputs,
  output : 'catalog_embedded.inc',
  command : embed_args,
)

# Propagate the build-dir include path so catalog.c's
#   #include "catalog_embedded.inc"
# resolves against the generated file at
#   build/src/shaders/catalog_embedded.inc
# The top-level meson.build picks up `shader_catalog_build_inc`
# and adds it to the library's include_directories.
shader_catalog_build_inc = include_directories('.')

lagfx_sources += files('catalog.c')
# Depend on the generated .inc so meson re-runs the embed step
# whenever a .spv blob changes. Adding the custom_target to
# lagfx_sources makes meson treat it as a build-order dependency
# of any TU in the library.
lagfx_sources += [catalog_embedded]

# --- Apple-pipeline stock-shader SPVs (Phase 3.C.2 M5) -------------------
#
# src/shaders/spv-apple/ contains the SPIR-V blobs produced by the
# full Apple-AIR pipeline (MSL -> .air -> metallib -> extract ->
# retarget -> llc -> signature-transform) via src/shaders/
# compile_msl.sh. They're shipped alongside the GLSL-sourced
# src/shaders/spv/ twins so CI can validate BOTH codepaths —
# the GLSL bypass (already stable) AND the Apple-authored pipeline
# (proves end-to-end shader translation works for real shaders,
# not just the triangle fixture).
#
# We stage each blob into the build tree via configure_file(copy:true)
# and export `apple_shader_spv_dir` so tests/apple-stock-shaders.c
# gets a runtime path that's identical between in-tree and out-of-
# tree builds. When src/shaders/spv-apple/ is empty (non-Mac clone
# that hasn't run compile_msl.sh) the export is still a valid dir —
# the test then exits 77 (meson SKIP).
apple_spv_stage_pairs = []
foreach name : shader_names
  apple_spv_stage_pairs += [
    [name + '_vertex',   name + '_vertex.spv'  ],
    [name + '_fragment', name + '_fragment.spv'],
  ]
endforeach

apple_spv_files = []
foreach pair : apple_spv_stage_pairs
  src_path = 'spv-apple' / pair[1]
  if fs.is_file(meson.current_source_dir() / src_path)
    copied = configure_file(
      input  : src_path,
      output : 'apple_' + pair[1],
      copy   : true,
    )
    apple_spv_files += [copied]
  endif
endforeach

# Build-tree directory that tests/apple-stock-shaders.c points at
# via LAGFX_APPLE_SPV_DIR. All `apple_<name>.spv` files land here
# by virtue of the configure_file(copy:true) pattern above — meson
# writes them next to the local meson.build in the BUILD tree.
apple_shader_spv_build_dir = meson.current_build_dir()
apple_shader_spv_count = apple_spv_files.length()
