cmake_minimum_required(VERSION 3.20)

set(project_name "WastesEdge")
project(${project_name} NONE)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(CLANG_EXECUTABLE NAMES clang REQUIRED)
find_program(WASM_LD_EXECUTABLE NAMES wasm-ld REQUIRED)
set(WASM2VIRCON_EXECUTABLE "" CACHE FILEPATH "Path to the wasm2vircon executable")
if(NOT WASM2VIRCON_EXECUTABLE)
  unset(WASM2VIRCON_EXECUTABLE CACHE)
  find_program(WASM2VIRCON_EXECUTABLE NAMES wasm2vircon)
endif()
if(NOT WASM2VIRCON_EXECUTABLE)
  message(FATAL_ERROR
    "wasm2vircon was not found on PATH; set WASM2VIRCON_EXECUTABLE explicitly")
endif()
find_program(ASSEMBLE_EXECUTABLE NAMES assemble REQUIRED)
find_program(PACKROM_EXECUTABLE NAMES packrom REQUIRED)
find_program(PNG2VIRCON_EXECUTABLE NAMES png2vircon REQUIRED)
find_program(WAV2VIRCON_EXECUTABLE NAMES wav2vircon REQUIRED)
find_program(FFMPEG_EXECUTABLE NAMES ffmpeg REQUIRED)

set(VIRCON32_EMULATOR "/usr/local/Vircon32/Emulator/Vircon32"
    CACHE FILEPATH "Path to the Vircon32 emulator")

set(content_compiler "${CMAKE_CURRENT_SOURCE_DIR}/tools/content_compiler/compile_content.py")
set(generated_dir "${CMAKE_CURRENT_SOURCE_DIR}/generated")
set(generated_asset_manifest "${generated_dir}/content_assets.cmake")

# Generate once while configuring so CMake can discover the exact texture and
# sound lists. CONFIGURE_DEPENDS causes an authoring-data change to reconfigure
# the project before the next build, refreshing this manifest automatically.
# Watch only normal authoring inputs. Archived extraction sources under
# assets-original may be absent and must not trigger content regeneration.
file(GLOB_RECURSE gamedata_inputs CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/animations/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/assets/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/audio/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/dialogue/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/entities/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/maps/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/schedules/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/scripts/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/tilesets/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/ui/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/manifest.jsonc")
execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${content_compiler}"
          --project-root "${CMAKE_CURRENT_SOURCE_DIR}"
  RESULT_VARIABLE content_result
  COMMAND_ECHO STDOUT)
if(NOT content_result EQUAL 0)
  message(FATAL_ERROR "Content compilation failed during configuration")
endif()
include("${generated_asset_manifest}")

set(generated_content_outputs
  "${generated_dir}/content_manifest.json"
  "${generated_dir}/content_ids.h"
  "${generated_dir}/content_data.c"
  "${generated_dir}/content_scene_data.h"
  "${generated_dir}/content_scene_data.c"
  "${generated_asset_manifest}")
foreach(asset IN LISTS WASTESEDGE_GENERATED_TEXTURES WASTESEDGE_GENERATED_SOUNDS)
  list(APPEND generated_content_outputs "${CMAKE_CURRENT_SOURCE_DIR}/${asset}")
endforeach()

set(content_stamp "${CMAKE_CURRENT_BINARY_DIR}/content.stamp")
add_custom_command(
  OUTPUT "${content_stamp}"
  BYPRODUCTS ${generated_content_outputs}
  COMMAND "${Python3_EXECUTABLE}" "${content_compiler}"
          --project-root "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${content_stamp}"
  DEPENDS "${content_compiler}" ${gamedata_inputs}
  COMMENT "Compiling Tiled, JSONC, graphics, and audio content"
  VERBATIM)

set(runtime_sources
  "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/savegame.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/world.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/vircon32/platform.c"
  "${generated_dir}/content_data.c"
  "${generated_dir}/content_scene_data.c")
set(runtime_headers
  "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/vircon32/vircon.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/savegame.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/world.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/vircon32/platform.h"
  "${generated_dir}/content_ids.h"
  "${generated_dir}/content_scene_data.h")

set(project_entry "__original_main")
set(project_wasm "${CMAKE_CURRENT_BINARY_DIR}/${project_name}.wasm")
set(project_asm "${CMAKE_CURRENT_BINARY_DIR}/${project_name}.asm")
set(project_vbin "${CMAKE_CURRENT_BINARY_DIR}/${project_name}.vbin")
set(project_xml "${CMAKE_CURRENT_BINARY_DIR}/${project_name}.xml")
set(project_rom "${CMAKE_CURRENT_BINARY_DIR}/${project_name}.v32")

# Compile each freestanding source independently, matching wasm2vircon's C
# sample while retaining precise CMake dependencies for generated content.
set(project_objects)
set(source_index 0)
foreach(source IN LISTS runtime_sources)
  set(object "${CMAKE_CURRENT_BINARY_DIR}/${project_name}-${source_index}.o")
  add_custom_command(
    OUTPUT "${object}"
    COMMAND "${CLANG_EXECUTABLE}" --target=wasm32-unknown-unknown -O2
            -fno-jump-tables -ffreestanding -fno-builtin -nostdlib
            -I "${CMAKE_CURRENT_SOURCE_DIR}/src"
            -I "${generated_dir}"
            -c "${source}" -o "${object}"
    DEPENDS "${content_stamp}" "${source}" ${runtime_headers}
    COMMENT "Compiling ${source} for wasm32"
    VERBATIM)
  list(APPEND project_objects "${object}")
  math(EXPR source_index "${source_index} + 1")
endforeach()

add_custom_command(
  OUTPUT "${project_wasm}"
  COMMAND "${WASM_LD_EXECUTABLE}" --no-entry --export=${project_entry}
          --allow-undefined ${project_objects} -o "${project_wasm}"
  DEPENDS ${project_objects}
  COMMENT "Linking ${project_name}.wasm"
  VERBATIM)

add_custom_command(
  OUTPUT "${project_asm}"
  COMMAND "${WASM2VIRCON_EXECUTABLE}" "${project_wasm}"
          --entry "${project_entry}" -o "${project_asm}"
  DEPENDS "${project_wasm}"
  COMMENT "Translating ${project_name}.wasm to Vircon32 assembly"
  VERBATIM)

add_custom_command(
  OUTPUT "${project_vbin}"
  COMMAND "${ASSEMBLE_EXECUTABLE}" -o "${project_vbin}" "${project_asm}"
  DEPENDS "${project_asm}"
  COMMENT "Assembling ${project_name}.vbin"
  VERBATIM)

# Convert cartridge resources as individual CMake outputs. Their list and
# ordering come from the deterministic content compiler manifest.
set(project_texture_outputs)
set(rom_texture_entries "")
set(texture_index 0)
set(all_content_textures ${WASTESEDGE_CONTENT_TEXTURES}
                         ${WASTESEDGE_GENERATED_TEXTURES})
foreach(texture IN LISTS all_content_textures)
  set(texture_source "${CMAKE_CURRENT_SOURCE_DIR}/${texture}")
  set(texture_output
      "${CMAKE_CURRENT_BINARY_DIR}/texture-${texture_index}.vtex")
  add_custom_command(
    OUTPUT "${texture_output}"
    COMMAND "${PNG2VIRCON_EXECUTABLE}" "${texture_source}"
            -o "${texture_output}"
    DEPENDS "${content_stamp}" "${texture_source}"
    COMMENT "Converting texture ${texture_index}"
    VERBATIM)
  list(APPEND project_texture_outputs "${texture_output}")
  string(APPEND rom_texture_entries
         "    <texture path=\"texture-${texture_index}.vtex\" />\n")
  math(EXPR texture_index "${texture_index} + 1")
endforeach()

# A regenerated content manifest may need fewer texture slots than the
# previous build. Remove obsolete converted files so the build directory does
# not retain resources that are no longer part of the cartridge.
file(GLOB existing_texture_outputs
     "${CMAKE_CURRENT_BINARY_DIR}/texture-*.vtex")
foreach(existing_texture IN LISTS existing_texture_outputs)
  if(NOT existing_texture IN_LIST project_texture_outputs)
    file(REMOVE "${existing_texture}")
  endif()
endforeach()

set(project_sound_outputs)
set(rom_sound_entries "")
set(sound_index 0)
foreach(sound IN LISTS WASTESEDGE_GENERATED_SOUNDS)
  set(sound_source "${CMAKE_CURRENT_SOURCE_DIR}/${sound}")
  set(sound_output "${CMAKE_CURRENT_BINARY_DIR}/sound-${sound_index}.vsnd")
  add_custom_command(
    OUTPUT "${sound_output}"
    COMMAND "${CMAKE_COMMAND}" -E env "SDL_AUDIODRIVER=dummy"
            "${WAV2VIRCON_EXECUTABLE}" "${sound_source}"
            -r 0 -o "${sound_output}"
    DEPENDS "${content_stamp}" "${sound_source}"
    COMMENT "Converting sound ${sound_index}"
    VERBATIM)
  list(APPEND project_sound_outputs "${sound_output}")
  string(APPEND rom_sound_entries
         "    <sound path=\"sound-${sound_index}.vsnd\" />\n")
  math(EXPR sound_index "${sound_index} + 1")
endforeach()

set(rom_binary_name "${project_name}.vbin")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/rom.xml.in" "${project_xml}" @ONLY)

add_custom_command(
  OUTPUT "${project_rom}"
  COMMAND "${PACKROM_EXECUTABLE}" -o "${project_rom}" "${project_xml}"
  DEPENDS "${project_vbin}" "${project_xml}"
          ${project_texture_outputs} ${project_sound_outputs}
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  COMMENT "Packaging ${project_name}.v32"
  VERBATIM)

add_custom_target(v32 ALL DEPENDS "${project_rom}")
add_custom_target(run
  COMMAND "${VIRCON32_EMULATOR}" "${project_rom}"
  DEPENDS v32
  USES_TERMINAL)
