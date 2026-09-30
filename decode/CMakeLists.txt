avicap_add_module(decode
    SOURCES
        video_frame.cpp
        ffmpeg_video_decoder.cpp
        audio_reader.cpp
        frame_cache.cpp
    DEPS
        AviCap::media)
