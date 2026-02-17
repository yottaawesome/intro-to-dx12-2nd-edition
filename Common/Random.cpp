#include "Random.h"

bool Random::_initialized = false;
std::random_device Random::_randDevice;
std::mt19937 Random::_mt;