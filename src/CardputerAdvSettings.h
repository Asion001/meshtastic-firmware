#pragma once

#include <cstdint>

namespace cardputerAdv
{

enum class NewNodeNotificationMode : uint8_t { ToAll = 0, OnlyThis = 1, Off = 2 };

void loadSettings();
NewNodeNotificationMode newNodeNotificationMode();
bool setNewNodeNotificationMode(NewNodeNotificationMode mode);

} // namespace cardputerAdv
