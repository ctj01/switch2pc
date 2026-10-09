# libhos_audio.so: audout (null sink with optional PCM dump); ALSA output later.
add_library(hos_audio SHARED
    audio_init.c
    audout.c
)
set_target_properties(hos_audio PROPERTIES
    OUTPUT_NAME hos_audio
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(hos_audio PRIVATE hos_cpu s2pc_common pthread)
