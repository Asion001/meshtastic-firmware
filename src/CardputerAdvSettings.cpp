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
constexpr uint8_t SETTINGS_VERSION = 1;
constexpr uint8_t NEW_NODE_NOTIFICATIONS_ENABLED = 1U << 0;

struct __attribute__((packed)) SettingsRecord {
    uint32_t magic;
    uint8_t version;
    uint8_t flags;
    uint16_t reserved;
};

static_assert(sizeof(SettingsRecord) == 8, "Unexpected Cardputer ADV settings layout");

bool newNodeNotifications = true;
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

    if (bytesRead == sizeof(record) && record.magic == SETTINGS_MAGIC && record.version == SETTINGS_VERSION) {
        newNodeNotifications = (record.flags & NEW_NODE_NOTIFICATIONS_ENABLED) != 0;
        LOG_INFO("Cardputer ADV new-node notifications %s", newNodeNotifications ? "enabled" : "disabled");
    } else {
        LOG_INFO("Using default Cardputer ADV notification settings");
    }
#endif
}

bool newNodeNotificationsEnabled()
{
#if defined(M5STACK_CARDPUTER_ADV)
    return newNodeNotifications;
#else
    return false;
#endif
}

bool setNewNodeNotificationsEnabled(bool enabled)
{
#if defined(M5STACK_CARDPUTER_ADV) && defined(FSCom)
    SettingsRecord record = {SETTINGS_MAGIC, SETTINGS_VERSION,
                             static_cast<uint8_t>(enabled ? NEW_NODE_NOTIFICATIONS_ENABLED : 0), 0};

    SafeFile file(SETTINGS_FILE, true);
    spiLock->lock();
    size_t written = file.write(reinterpret_cast<const uint8_t *>(&record), sizeof(record));
    spiLock->unlock();
    bool closed = file.close();
    if (written != sizeof(record) || !closed) {
        LOG_ERROR("Failed to save Cardputer ADV notification settings");
        return false;
    }

    newNodeNotifications = enabled;
    LOG_INFO("Cardputer ADV new-node notifications %s", enabled ? "enabled" : "disabled");
    return true;
#else
    (void)enabled;
    return false;
#endif
}

} // namespace cardputerAdv
