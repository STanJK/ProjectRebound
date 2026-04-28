// Utility.h
#pragma once
#include <vector>
#include "../SDK.hpp"

std::vector<SDK::UObject *> getObjectsOfClass(SDK::UClass *theClass, bool includeDefault);
SDK::UObject *GetLastOfType(SDK::UClass *theClass, bool includeDefault);
void PressSpace();