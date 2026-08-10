#include "CardputerAdvSettings.h"

#include "DebugConfiguration.h"
#include <cstdint>

#if defined(M5STACK_CARDPUTER_ADV)
#include "FSCommon.h"
#include "SPILock.h"
#include "SafeFile.h"

namespace
{
constexpr const char *SETTINGS_FILE = "/prefs/cardputer_adv_ui.bin";
constexpr uint32_t SETTINGS_MAGIC = 0x43414456; // "CADV"
constexpr uint8_t SETTINGS_VERSION = 2;
constexpr uint8_t LEGACY_SETTINGS_VERSION = 1;
constexpr uint8_t NEW_NODE_NOTIFICATIONS_ENABLED = 1U << 0;

struct __attribute__((packed)) SettingsRecord {
    uint32_t magic;
    uint8_t version;
    uint8_t flags;
    uint16_t reserved;
};

static_assert(sizeof(SettingsRecord) == 8, "Unexpected Cardputer ADV settings layout");

cardputerAdv::NewNodeNotificationMode newNodeMode = cardputerAdv::NewNodeNotificationMode::ToAll;

const char *newNodeModeName(cardputerAdv::NewNodeNotificationMode mode)
{
    switch (mode) {
    case cardputerAdv::NewNodeNotificationMode::ToAll:
        return "to all";
    case cardputerAdv::NewNodeNotificationMode::OnlyThis:
        return "only this device";
    case cardputerAdv::NewNodeNotificationMode::Off:
        return "off";
    default:
        return "invalid";
    }
}
} // namespace
#endif

namespace cardputerAdv
{

void loadSettings()
{
#if defined(M5STACK_CARDPUTER_ADV) && defined(FSCom)
    SettingsRecord record{};
    size_t bytesRead = 0;

    spiLock->lock();
    File file = FSCom.open(SETTINGS_FILE, FILE_O_READ);
    if (file) {
        bytesRead = file.read(reinterpret_cast<uint8_t *>(&record), sizeof(record));
        file.close();
    }
    spiLock->unlock();

    if (bytesRead == sizeof(record) && record.magic == SETTINGS_MAGIC && record.version == SETTINGS_VERSION &&
        record.flags <= static_cast<uint8_t>(NewNodeNotificationMode::Off)) {
        newNodeMode = static_cast<NewNodeNotificationMode>(record.flags);
        LOG_INFO("Cardputer ADV new-node alerts %s", newNodeModeName(newNodeMode));
    } else if (bytesRead == sizeof(record) && record.magic == SETTINGS_MAGIC &&
               record.version == LEGACY_SETTINGS_VERSION) {
        newNodeMode = (record.flags & NEW_NODE_NOTIFICATIONS_ENABLED) ? NewNodeNotificationMode::ToAll
                                                                     : NewNodeNotificationMode::Off;
        LOG_INFO("Migrated Cardputer ADV new-node alerts to %s", newNodeModeName(newNodeMode));
    } else {
        LOG_INFO("Using default Cardputer ADV notification settings");
    }
#endif
}

NewNodeNotificationMode newNodeNotificationMode()
{
#if defined(M5STACK_CARDPUTER_ADV)
    return newNodeMode;
#else
    return NewNodeNotificationMode::Off;
#endif
}

bool setNewNodeNotificationMode(NewNodeNotificationMode mode)
{
#if defined(M5STACK_CARDPUTER_ADV) && defined(FSCom)
    if (mode > NewNodeNotificationMode::Off) {
        return false;
    }

    SettingsRecord record = {SETTINGS_MAGIC, SETTINGS_VERSION, static_cast<uint8_t>(mode), 0};

    SafeFile file(SETTINGS_FILE, true);
    spiLock->lock();
    size_t written = file.write(reinterpret_cast<const uint8_t *>(&record), sizeof(record));
    spiLock->unlock();
    bool closed = file.close();
    if (written != sizeof(record) || !closed) {
        LOG_ERROR("Failed to save Cardputer ADV notification settings");
        return false;
    }

    newNodeMode = mode;
    LOG_INFO("Cardputer ADV new-node alerts %s", newNodeModeName(newNodeMode));
    return true;
#else
    (void)mode;
    return false;
#endif
}

} // namespace cardputerAdv
