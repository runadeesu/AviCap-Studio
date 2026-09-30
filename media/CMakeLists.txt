avicap_add_module(media
    SOURCES
        ffmpeg_util.cpp
        probe.cpp
    DEPS
        AviCap::timeline
        FFmpeg::FFmpeg)
