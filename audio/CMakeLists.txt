set(_audio_libs "")
if(WIN32)
    set(_audio_libs ole32 uuid)
endif()
avicap_add_module(audio
    SOURCES dsp.cpp mixer.cpp output.cpp playback.cpp waveform.cpp
    DEPS AviCap::effects AviCap::decode AviCap::timeline ${_audio_libs})
