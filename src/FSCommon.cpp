/**
 * @file FSCommon.cpp
 * @brief This file contains functions for common filesystem operations such as copying, renaming, listing and deleting files and
 * directories.
 *
 * The functions in this file are used to perform common filesystem operations such as copying, renaming, listing and deleting
 * files and directories. These functions are used in the Meshtastic-device project to manage files and directories on the
 * device's filesystem.
 *
 */
#include "FSCommon.h"
#include "SPILock.h"
#include "configuration.h"
#include <cstring>

#if defined(ARCH_PORTDUINO)
#include <filesystem>
#endif

#if defined(ARCH_NRF52) || defined(ARCH_STM32)
// Adafruit_LittleFS (nRF52) and STM32_LittleFS both wrap littlefs v1, which predates lfs_fs_size().
// Count the blocks currently in use with lfs_traverse() instead.
static int fsCountBlockCb(void *ctx, lfs_block_t)
{
    *static_cast<size_t *>(ctx) += 1;
    return 0;
}

size_t fsTotalBytes()
{
    lfs_t *fs = FSCom._getFS();
    if (!fs || !fs->cfg)
        return 0;
    return (size_t)fs->cfg->block_count * (size_t)fs->cfg->block_size;
}

size_t fsUsedBytes()
{
    lfs_t *fs = FSCom._getFS();
    if (!fs || !fs->cfg)
        return 0;
    size_t blocks = 0;
    FSCom._lockFS();
    int err = lfs_traverse(fs, fsCountBlockCb, &blocks);
    FSCom._unlockFS();
    if (err < 0)
        return fsTotalBytes(); // report "full" so capacity checks fail safe
    return blocks * (size_t)fs->cfg->block_size;
}
#elif defined(ARCH_RP2040)
// arduino-pico reports capacity through FSInfo rather than as methods.
size_t fsTotalBytes()
{
    FSInfo info;
    return FSCom.info(info) ? (size_t)info.totalBytes : 0;
}

size_t fsUsedBytes()
{
    FSInfo info;
    return FSCom.info(info) ? (size_t)info.usedBytes : 0;
}
#elif defined(ARCH_PORTDUINO)
// Portduino is backed by the host filesystem; ask the OS about the volume holding the working
// directory. std::filesystem keeps this working on native-windows too, where statvfs() does not
// exist. On error we report "full" so capacity checks fail safe.
size_t fsTotalBytes()
{
    std::error_code ec;
    const auto info = std::filesystem::space(".", ec);
    return ec ? 0 : (size_t)info.capacity;
}

size_t fsUsedBytes()
{
    std::error_code ec;
    const auto info = std::filesystem::space(".", ec);
    if (ec || info.capacity < info.available)
        return fsTotalBytes();
    return (size_t)(info.capacity - info.available);
}
#else
// ESP32 LittleFS and the nRF54L15 wrapper expose these directly.
size_t fsTotalBytes()
{
    return FSCom.totalBytes();
}

size_t fsUsedBytes()
{
    return FSCom.usedBytes();
}
#endif

// Software SPI is used by MUI so disable SD card here until it's also implemented
#if defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI)
#include <SD.h>
#include <SPI.h>

#ifdef SDCARD_USE_SPI1
SPIClass SPI_HSPI(HSPI);
#define SDHandler SPI_HSPI
#else
#define SDHandler SPI
#endif

#ifndef SD_SPI_FREQUENCY
#define SD_SPI_FREQUENCY 4000000U
#endif

#endif // HAS_SDCARD

#if defined(M5STACK_CARDPUTER_ADV) && defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI) && !defined(HAS_SD_MMC)
namespace
{
constexpr const char *SD_STORAGE_ROOT = "/meshtastic";
constexpr const char *SD_PREFS_ROOT = "/meshtastic/prefs";
constexpr const char *SD_BACKUPS_ROOT = "/meshtastic/backups";
constexpr const char *SD_STORAGE_MARKER = "/meshtastic/.storage-v1";
constexpr const char *FLASH_DIRTY_MARKER = "/prefs/.sd-fallback-dirty";

bool sdCardMounted = false;
bool sdSyncInProgress = false;

bool isConfigurationPath(const char *path)
{
    return path && (strncmp(path, "/prefs/", 7) == 0 || strncmp(path, "/backups/", 9) == 0);
}

String sdPathFor(const char *path)
{
    return String(SD_STORAGE_ROOT) + path;
}

void ensureParentDirectories(fs::FS &filesystem, const char *path)
{
    String parent(path);
    for (size_t i = 1; i < parent.length(); ++i) {
        if (parent[i] != '/')
            continue;
        char saved = parent[i];
        parent[i] = '\0';
        filesystem.mkdir(parent.c_str());
        parent[i] = saved;
    }
}

bool removeTree(fs::FS &filesystem, const char *path)
{
    File root = filesystem.open(path, FILE_O_READ);
    if (!root)
        return true;
    if (!root.isDirectory()) {
        root.close();
        return filesystem.remove(path);
    }

    File entry = root.openNextFile();
    while (entry) {
        String entryPath = entry.path();
        bool isDirectory = entry.isDirectory();
        entry.close();
        bool removed = isDirectory ? removeTree(filesystem, entryPath.c_str()) : filesystem.remove(entryPath.c_str());
        if (!removed) {
            root.close();
            return false;
        }
        entry = root.openNextFile();
    }
    root.close();
    return filesystem.rmdir(path);
}

bool copyFile(fs::FS &source, const char *sourcePath, fs::FS &destination, const char *destinationPath)
{
    File input = source.open(sourcePath, FILE_O_READ);
    if (!input || input.isDirectory()) {
        input.close();
        return false;
    }

    ensureParentDirectories(destination, destinationPath);
    String temporaryPath = String(destinationPath) + ".tmp";
    destination.remove(temporaryPath.c_str());
    File output = destination.open(temporaryPath.c_str(), FILE_O_WRITE);
    if (!output) {
        input.close();
        return false;
    }

    size_t expected = input.size();
    size_t copied = 0;
    uint8_t buffer[512];
    while (input.available()) {
        size_t count = input.read(buffer, sizeof(buffer));
        if (count == 0)
            break;
        size_t written = output.write(buffer, count);
        copied += written;
        if (written != count)
            break;
    }
    output.flush();
    output.close();
    input.close();

    if (copied != expected) {
        destination.remove(temporaryPath.c_str());
        return false;
    }
    destination.remove(destinationPath);
    if (!destination.rename(temporaryPath.c_str(), destinationPath)) {
        destination.remove(temporaryPath.c_str());
        return false;
    }
    return true;
}

bool copyTree(fs::FS &source, const char *sourceRoot, fs::FS &destination, const char *destinationRoot)
{
    File root = source.open(sourceRoot, FILE_O_READ);
    if (!root)
        return true;
    if (!root.isDirectory()) {
        root.close();
        return false;
    }

    destination.mkdir(destinationRoot);
    File entry = root.openNextFile();
    while (entry) {
        String sourcePath = entry.path();
        bool isDirectory = entry.isDirectory();
        entry.close();

        if (sourcePath == FLASH_DIRTY_MARKER) {
            entry = root.openNextFile();
            continue;
        }

        String relativePath = sourcePath.substring(strlen(sourceRoot));
        String destinationPath = String(destinationRoot) + relativePath;
        bool copied = isDirectory ? copyTree(source, sourcePath.c_str(), destination, destinationPath.c_str())
                                  : copyFile(source, sourcePath.c_str(), destination, destinationPath.c_str());
        if (!copied) {
            root.close();
            return false;
        }
        entry = root.openNextFile();
    }
    root.close();
    return true;
}

bool replaceFlashTreeFromSD(const char *sdRoot, const char *flashRoot)
{
    String temporaryRoot = String(flashRoot) + ".sdtmp";
    String previousRoot = String(flashRoot) + ".sdold";
    removeTree(FSCom, temporaryRoot.c_str());
    removeTree(FSCom, previousRoot.c_str());
    FSCom.mkdir(temporaryRoot.c_str());
    if (!copyTree(SD, sdRoot, FSCom, temporaryRoot.c_str())) {
        removeTree(FSCom, temporaryRoot.c_str());
        return false;
    }

    bool hadPrevious = FSCom.exists(flashRoot);
    if (hadPrevious && !FSCom.rename(flashRoot, previousRoot.c_str())) {
        removeTree(FSCom, temporaryRoot.c_str());
        return false;
    }
    if (!FSCom.rename(temporaryRoot.c_str(), flashRoot)) {
        if (hadPrevious)
            FSCom.rename(previousRoot.c_str(), flashRoot);
        removeTree(FSCom, temporaryRoot.c_str());
        return false;
    }
    removeTree(FSCom, previousRoot.c_str());
    return true;
}

void markFlashFallbackDirty()
{
    if (FSCom.exists(FLASH_DIRTY_MARKER))
        return;
    FSCom.mkdir("/prefs");
    File marker = FSCom.open(FLASH_DIRTY_MARKER, FILE_O_WRITE);
    if (marker) {
        marker.write((uint8_t)1);
        marker.close();
    }
}

bool seedSDFromFlash()
{
    // The marker is the commit record. If power is lost during the copy, the
    // next boot will retry from flash rather than restoring a partial tree.
    SD.remove(SD_STORAGE_MARKER);
    removeTree(SD, SD_PREFS_ROOT);
    removeTree(SD, SD_BACKUPS_ROOT);
    SD.mkdir(SD_STORAGE_ROOT);
    bool okay = copyTree(FSCom, "/prefs", SD, SD_PREFS_ROOT) && copyTree(FSCom, "/backups", SD, SD_BACKUPS_ROOT);
    if (!okay)
        return false;

    File marker = SD.open(SD_STORAGE_MARKER, FILE_O_WRITE);
    bool markerCreated = (bool)marker;
    if (markerCreated) {
        marker.write((uint8_t)1);
        marker.close();
    }
    return markerCreated;
}
} // namespace
#endif

/**
 * Renames a file from pathFrom to pathTo.
 *
 * @param pathFrom The original path of the file.
 * @param pathTo The new path of the file.
 *
 * @return True if the file was successfully renamed, false otherwise.
 */
bool renameFile(const char *pathFrom, const char *pathTo)
{
#ifdef FSCom
    spiLock->lock();
    bool result = FSCom.rename(pathFrom, pathTo);
    spiLock->unlock();
#if defined(M5STACK_CARDPUTER_ADV)
    if (result)
        mirrorConfigurationFileToSD(pathTo);
#endif
    return result;
#else
    return false;
#endif
}

#include <new>
#include <stdexcept>
#include <vector>

/**
 * @brief Platform-agnostic filesystem format / wipe.
 *
 * On embedded targets (ESP32, NRF52, STM32WL, RP2040) this calls the
 * native FSCom.format() which erases and reinitialises the LittleFS
 * partition.
 *
 * On Portduino the fs::FS backend has no format() method. We instead
 * delete /prefs (the only meshtastic data directory written at runtime)
 * and return. rmDir("/prefs") is already called unconditionally by
 * factoryReset() so this is a proven primitive on Portduino.
 * FSBegin() is a no-op (#define FSBegin() true) on Portduino.
 *
 * @return true on success, false on failure or if no filesystem is configured.
 */
bool fsFormat()
{
#ifdef FSCom
#if defined(ARCH_PORTDUINO)
    rmDir("/prefs");
    return FSBegin();
#else
    return FSCom.format();
#endif
#else
    return false;
#endif
}

#ifdef FSCom
namespace
{
bool pathEndsWithDot(const char *path)
{
    if (!path)
        return false;

    size_t length = strlen(path);
    return length > 0 && path[length - 1] == '.';
}

bool copyFilePath(char *dest, size_t destSize, const char *path, bool *wasLimited)
{
    if (!path || destSize == 0) {
        if (wasLimited)
            *wasLimited = true;
        return false;
    }

    if (strlcpy(dest, path, destSize) >= destSize) {
        if (wasLimited)
            *wasLimited = true;
        return false;
    }

    return true;
}

void collectFiles(const char *dirname, uint8_t levels, size_t maxCount, std::vector<meshtastic_FileInfo> &filenames,
                  bool *wasLimited)
{
    if (!dirname)
        return;

    File root = FSCom.open(dirname, FILE_O_READ);
    if (!root)
        return;
    if (!root.isDirectory()) {
        root.close();
        return;
    }

    File file = root.openNextFile();
    // file.name()[0] check is a workaround for a bug in the Adafruit LittleFS nrf52 glue (see issue 4395)
    while (file && file.name()[0]) {
        if (filenames.size() >= maxCount) {
            if (wasLimited)
                *wasLimited = true;
            file.close();
            break;
        }
        const char *fileName = file.name();
        if (file.isDirectory() && !pathEndsWithDot(fileName)) {
            char pathBuffer[sizeof(((meshtastic_FileInfo *)nullptr)->file_name)] = {};
#ifdef ARCH_ESP32
            const char *subDirPath = file.path();
#else
            const char *subDirPath = fileName;
#endif
            bool hasSubDirPath = copyFilePath(pathBuffer, sizeof(pathBuffer), subDirPath, wasLimited);
            file.close();

            if (levels && hasSubDirPath) {
                collectFiles(pathBuffer, levels - 1, maxCount, filenames, wasLimited);
            } else if (wasLimited) {
                *wasLimited = true;
            }
        } else {
            meshtastic_FileInfo fileInfo = {"", static_cast<uint32_t>(file.size())};
#ifdef ARCH_ESP32
            bool hasFilePath = copyFilePath(fileInfo.file_name, sizeof(fileInfo.file_name), file.path(), wasLimited);
#else
            bool hasFilePath = copyFilePath(fileInfo.file_name, sizeof(fileInfo.file_name), file.name(), wasLimited);
#endif
            if (hasFilePath && !pathEndsWithDot(fileInfo.file_name)) {
                filenames.push_back(fileInfo);
            }
            file.close();
        }
        file = root.openNextFile();
    }
    root.close();
}
} // namespace
#endif

/**
 * @brief Get the list of files in a directory.
 *
 * This function returns a list of files in a directory. The list includes the full path of each file.
 * We can't use SPILOCK here because of recursion. Callers of this function should use SPILOCK.
 *
 * @param dirname The name of the directory.
 * @param levels The number of levels of subdirectories to list.
 * @param maxCount The maximum number of files to collect before truncating the walk.
 * @param wasLimited Optional out-param, set to true if the listing was truncated (by maxCount or low memory).
 * @return A vector of meshtastic_FileInfo for each file in the directory.
 */
std::vector<meshtastic_FileInfo> getFiles(const char *dirname, uint8_t levels, size_t maxCount, bool *wasLimited)
{
    std::vector<meshtastic_FileInfo> filenames = {};
    if (wasLimited)
        *wasLimited = false;
#ifdef FSCom
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    size_t reservedCount = maxCount;
    while (reservedCount > 0) {
        try {
            filenames.reserve(reservedCount);
            break;
        } catch (const std::bad_alloc &) {
            reservedCount /= 2;
        } catch (const std::length_error &) {
            reservedCount /= 2;
        }
    }
    if (reservedCount == 0) {
        if (wasLimited)
            *wasLimited = true;
        return filenames;
    }
    if (reservedCount < maxCount) {
        if (wasLimited)
            *wasLimited = true;
        maxCount = reservedCount;
    }
#endif
    collectFiles(dirname, levels, maxCount, filenames, wasLimited);
#endif
    return filenames;
}

/**
 * Lists the contents of a directory.
 * We can't use SPILOCK here because of recursion. Callers of this function should use SPILOCK.
 *
 * @param dirname The name of the directory to list.
 * @param levels The number of levels of subdirectories to list.
 * @param del Whether or not to delete the contents of the directory after listing.
 */
void listDir(const char *dirname, uint8_t levels, bool del)
{
#ifdef FSCom
    char buffer[255];
    File root = FSCom.open(dirname, FILE_O_READ);
    if (!root || !root.isDirectory())
        return;

    File file = root.openNextFile();
    while (file && file.name()[0]) { // file.name()[0] check: workaround for Adafruit LittleFS nRF52 bug #4395
#ifdef ARCH_ESP32
        const char *filepath = file.path();
#else
        const char *filepath = file.name();
#endif
        if (file.isDirectory() && !String(file.name()).endsWith(".")) {
            if (levels) {
                listDir(filepath, levels - 1, del);
                if (del) {
                    LOG_DEBUG("Remove %s", filepath);
                    strncpy(buffer, filepath, sizeof(buffer) - 1);
                    buffer[sizeof(buffer) - 1] = '\0';
                    file.close();
                    FSCom.rmdir(buffer);
                } else {
                    file.close();
                }
            }
        } else {
            if (del) {
                LOG_DEBUG("Delete %s", filepath);
                strncpy(buffer, filepath, sizeof(buffer) - 1);
                buffer[sizeof(buffer) - 1] = '\0';
                file.close();
                FSCom.remove(buffer);
            } else {
                LOG_DEBUG(" %s (%i Bytes)", filepath, file.size());
                file.close();
            }
        }
        file = root.openNextFile();
    }
#ifdef ARCH_ESP32
    const char *rootpath = root.path();
#else
    const char *rootpath = root.name();
#endif
    if (del) {
        LOG_DEBUG("Remove %s", rootpath);
        strncpy(buffer, rootpath, sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        root.close();
        FSCom.rmdir(buffer);
    } else {
        root.close();
    }
#endif
}

/**
 * @brief Removes a directory and all its contents.
 *
 * This function recursively removes a directory and all its contents, including subdirectories and files.
 *
 * @param dirname The name of the directory to remove.
 */
void rmDir(const char *dirname)
{
#ifdef FSCom
    listDir(dirname, 10, true);
#if defined(M5STACK_CARDPUTER_ADV) && defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI) && !defined(HAS_SD_MMC)
    if (sdCardMounted && !sdSyncInProgress &&
        (strcmp(dirname, "/prefs") == 0 || strcmp(dirname, "/backups") == 0)) {
        String sdPath = sdPathFor(dirname);
        removeTree(SD, sdPath.c_str());
    }
#endif
#endif
}

/**
 * Some platforms (nrf52) might need to do an extra step before FSBegin().
 */
__attribute__((weak, noinline)) void preFSBegin() {}

void fsInit()
{
#ifdef FSCom
    concurrency::LockGuard g(spiLock);
    preFSBegin();
    if (!FSBegin()) {
        LOG_ERROR("Filesystem mount failed");
        // assert(0); This auto-formats the partition, so no need to fail here.
    }
#if defined(ARCH_ESP32)
    LOG_DEBUG("Filesystem files (%d/%d Bytes):", FSCom.usedBytes(), FSCom.totalBytes());
#else
    LOG_DEBUG("Filesystem files:");
#endif
    listDir("/", 10);
#endif
}

/**
 * Initializes the SD card and mounts the file system.
 */
void setupSDCard()
{
#if defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI) && !defined(HAS_SD_MMC)
    concurrency::LockGuard g(spiLock);
#ifdef M5STACK_CARDPUTER_ADV
    // The radio and SD card share this bus. Keep the radio deselected while
    // the SD driver probes and mounts the card.
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);
#endif
    pinMode(SDCARD_CS, OUTPUT);
    digitalWrite(SDCARD_CS, HIGH);
    SDHandler.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    if (!SD.begin(SDCARD_CS, SDHandler, SD_SPI_FREQUENCY)) {
        LOG_DEBUG("No SD_MMC card detected");
#ifdef M5STACK_CARDPUTER_ADV
        sdCardMounted = false;
#endif
        return;
    }
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        LOG_DEBUG("No SD_MMC card attached");
#ifdef M5STACK_CARDPUTER_ADV
        sdCardMounted = false;
#endif
        return;
    }
#ifdef M5STACK_CARDPUTER_ADV
    sdCardMounted = true;
#endif
    LOG_DEBUG("SD_MMC Card Type: ");
    if (cardType == CARD_MMC) {
        LOG_DEBUG("MMC");
    } else if (cardType == CARD_SD) {
        LOG_DEBUG("SDSC");
    } else if (cardType == CARD_SDHC) {
        LOG_DEBUG("SDHC");
    } else {
        LOG_DEBUG("UNKNOWN");
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    LOG_DEBUG("SD Card Size: %lu MB", (uint32_t)cardSize);
    LOG_DEBUG("Total space: %lu MB", (uint32_t)(SD.totalBytes() / (1024 * 1024)));
    LOG_DEBUG("Used space: %lu MB", (uint32_t)(SD.usedBytes() / (1024 * 1024)));
#endif
}

bool restoreConfigurationFromSD()
{
#if defined(M5STACK_CARDPUTER_ADV) && defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI) && !defined(HAS_SD_MMC)
    concurrency::LockGuard g(spiLock);
    if (!sdCardMounted) {
        markFlashFallbackDirty();
        LOG_WARN("Cardputer SD config unavailable; using internal flash until the card returns");
        return false;
    }

    sdSyncInProgress = true;
    bool flashIsNewer = FSCom.exists(FLASH_DIRTY_MARKER);
    bool sdHasConfiguration = SD.exists(SD_STORAGE_MARKER);
    bool okay = false;
    if (flashIsNewer || !sdHasConfiguration) {
        okay = seedSDFromFlash();
        if (okay) {
            FSCom.remove(FLASH_DIRTY_MARKER);
            LOG_INFO("Seeded /meshtastic configuration on SD from internal flash");
        }
    } else {
        okay = replaceFlashTreeFromSD(SD_PREFS_ROOT, "/prefs") &&
               replaceFlashTreeFromSD(SD_BACKUPS_ROOT, "/backups");
        if (okay)
            LOG_INFO("Restored Meshtastic configuration from SD");
    }
    sdSyncInProgress = false;

    if (!okay) {
        markFlashFallbackDirty();
        LOG_ERROR("Cardputer SD configuration sync failed; internal flash remains authoritative");
    }
    return okay;
#else
    return false;
#endif
}

bool mirrorConfigurationFileToSD(const char *path)
{
#if defined(M5STACK_CARDPUTER_ADV) && defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI) && !defined(HAS_SD_MMC)
    if (!isConfigurationPath(path) || strcmp(path, FLASH_DIRTY_MARKER) == 0 || sdSyncInProgress)
        return true;

    concurrency::LockGuard g(spiLock);
    if (!sdCardMounted) {
        markFlashFallbackDirty();
        return false;
    }

    String destinationPath = sdPathFor(path);
    bool okay = FSCom.exists(path) ? copyFile(FSCom, path, SD, destinationPath.c_str())
                                   : (!SD.exists(destinationPath.c_str()) || SD.remove(destinationPath.c_str()));
    if (!okay) {
        markFlashFallbackDirty();
        LOG_ERROR("Failed to mirror %s to SD", path);
    }
    return okay;
#else
    (void)path;
    return false;
#endif
}

void markConfigurationFileDirtyForSD(const char *path)
{
#if defined(M5STACK_CARDPUTER_ADV) && defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI) && !defined(HAS_SD_MMC)
    if (isConfigurationPath(path) && strcmp(path, FLASH_DIRTY_MARKER) != 0)
        markFlashFallbackDirty();
#else
    (void)path;
#endif
}
