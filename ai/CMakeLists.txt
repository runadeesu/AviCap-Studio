avicap_add_module(ai
    SOURCES analysis.cpp plan.cpp cloud.cpp
    DEPS AviCap::decode AviCap::timeline AviCap::effects AviCap::export AviCap::json AviCap::core)
