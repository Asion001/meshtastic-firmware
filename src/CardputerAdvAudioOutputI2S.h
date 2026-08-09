#pragma once

#include "configuration.h"

#if defined(M5STACK_CARDPUTER_ADV) && defined(ARCH_ESP32)

#include <AudioOutputI2S.h>

class CardputerAdvAudioOutputI2S : public AudioOutputI2S
{
  public:
    CardputerAdvAudioOutputI2S();
    bool begin() override;
};

#endif
