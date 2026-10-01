avicap_add_module(ai
    SOURCES analysis.cpp plan.cpp
    DEPS AviCap::decode AviCap::timeline AviCap::effects AviCap::json AviCap::core)
