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
