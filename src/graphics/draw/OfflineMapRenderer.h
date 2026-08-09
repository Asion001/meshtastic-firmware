#pragma once

#include "configuration.h"

#if defined(M5STACK_CARDPUTER_ADV) && HAS_SCREEN

#include "input/InputBroker.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>

namespace graphics::OfflineMapRenderer
{

void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y);
bool handleInput(const InputEvent &event);

} // namespace graphics::OfflineMapRenderer

#endif
