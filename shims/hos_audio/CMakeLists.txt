# libhos_audio.so: audout with a host output (PulseAudio/ALSA via dlopen, HOS_AUDIO)
# or a timer-paced null sink; optional PCM dump. audren as a silent renderer that keeps time.
add_library(hos_audio SHARED
    audio_init.c
    audout.c
    audren.c
    audio_sink.c
)
set_target_properties(hos_audio PROPERTIES
    OUTPUT_NAME hos_audio
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(hos_audio PRIVATE hos_cpu s2pc_common pthread ${CMAKE_DL_LIBS})
