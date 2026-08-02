// Unity build cpp that aggregates all other cpps so we don't need to manually include them in every project.
// This is not about building speed.

#include "utils/system.cpp"

#include "dlss/DLSS.cpp"
#include "fsr/FSR.cpp"
#include "fsr/D3D11On12Bridge.cpp"
#include "fsr/FSR41.cpp"
