/*
 * Cardputer ADV offline map frame.
 *
 * Adapted from lunarc3/CardputerGPSMap (GPL-3.0), commit
 * 5ff1fdc6b458a04e45abd253f0c628a822a4a20d. Meshtastic integration,
 * shared GPS/SD/input handling, and monochrome dithering added in 2026.
 */

#include "OfflineMapRenderer.h"

#if defined(M5STACK_CARDPUTER_ADV) && HAS_SCREEN

#include "FSCommon.h"
#include "GPSStatus.h"
#include "SPILock.h"
#include "graphics/ScreenFonts.h"
#include "graphics/TFTColorRegions.h"
#include "graphics/TFTPalette.h"

#include <JPEGDEC.h>
#include <SD.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace graphics::OfflineMapRenderer
{
namespace
{
constexpr const char *MAP_STORAGE_ROOT = "/gpsmap";
constexpr const char *STATE_FILE = "/gpsmap/gpsmap.ini";
constexpr const char *STATE_TEMP_FILE = "/gpsmap/gpsmap.ini.tmp";
constexpr int MAP_WIDTH = 240;
constexpr int MAP_HEIGHT = 135;
constexpr int BITMAP_STRIDE = (MAP_WIDTH + 7) / 8;
constexpr int TILE_SIZE = 256;
constexpr int MIN_ZOOM = 6;
constexpr int MAX_ZOOM = 18;
constexpr int DEFAULT_ZOOM = 12;
constexpr int PAN_STEP = 50;
constexpr size_t MAX_JPEG_SIZE = 36 * 1024;
constexpr uint32_t SAVE_INTERVAL_MS = 60 * 1000UL;

uint8_t mapBitmap[BITMAP_STRIDE * MAP_HEIGHT] = {};
JPEGDEC jpeg;
int zoom = DEFAULT_ZOOM;
double latitude = 0.0;
double longitude = 0.0;
int panX = 0;
int panY = 0;
bool initialized = false;
bool hasPosition = false;
bool hasUserPosition = false;
bool manuallyPanned = false;
bool cacheDirty = true;
bool foundTile = false;
bool attemptedTileFallback = false;
uint32_t lastSaveMillis = 0;
char tileRoot[64] = "/gpsmap";

constexpr uint8_t BAYER_4X4[16] = {8, 136, 40, 168, 200, 72, 232, 104, 56, 184, 24, 152, 248, 120, 216, 88};

class CardputerSDSession
{
  public:
    CardputerSDSession() : mounted(mountCardputerSDLocked()) {}
    ~CardputerSDSession()
    {
        if (mounted)
            unmountCardputerSDLocked();
    }

    explicit operator bool() const { return mounted; }

    CardputerSDSession(const CardputerSDSession &) = delete;
    CardputerSDSession &operator=(const CardputerSDSession &) = delete;

  private:
    bool mounted;
};

static inline double degreesToRadians(double degrees)
{
    return degrees * 0.017453292519943295;
}

int floorDivide(int value, int divisor)
{
    int quotient = value / divisor;
    if (value % divisor < 0)
        quotient--;
    return quotient;
}

void toTile(double lat, double lon, int tileZoom, int &tileX, int &tileY)
{
    const double scale = static_cast<double>(1UL << tileZoom);
    tileX = static_cast<int>((lon + 180.0) / 360.0 * scale);
    tileY = static_cast<int>((1.0 - asinh(tan(degreesToRadians(lat))) / M_PI) * 0.5 * scale);
}

void toPixel(double lat, double lon, int tileZoom, double &pixelX, double &pixelY)
{
    const double scale = static_cast<double>(1UL << tileZoom) * TILE_SIZE;
    pixelX = (lon + 180.0) / 360.0 * scale;
    pixelY = (1.0 - asinh(tan(degreesToRadians(lat))) / M_PI) * 0.5 * scale;
}

const char *baseName(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool parseNonnegativeInteger(const char *text, long &value, const char **remainder = nullptr)
{
    if (!text || !*text)
        return false;
    char *end = nullptr;
    value = strtol(text, &end, 10);
    if (end == text || value < 0)
        return false;
    if (remainder)
        *remainder = end;
    return true;
}

bool parseTilePath(const char *directoryPath, const char *fileName, int &tileZoom, int &tileX, int &tileY,
                   char *root, size_t rootSize)
{
    char directory[128];
    if (!directoryPath || strlen(directoryPath) >= sizeof(directory))
        return false;
    strcpy(directory, directoryPath);

    char *xSeparator = strrchr(directory, '/');
    if (!xSeparator || !xSeparator[1])
        return false;
    long parsedX = 0;
    const char *remainder = nullptr;
    if (!parseNonnegativeInteger(xSeparator + 1, parsedX, &remainder) || *remainder != '\0')
        return false;
    *xSeparator = '\0';

    char *zoomSeparator = strrchr(directory, '/');
    if (!zoomSeparator || !zoomSeparator[1])
        return false;
    const char *zoomText = zoomSeparator + 1;
    if (*zoomText == 'z' || *zoomText == 'Z')
        zoomText++;
    long parsedZoom = 0;
    if (!parseNonnegativeInteger(zoomText, parsedZoom, &remainder) || *remainder != '\0' || parsedZoom < MIN_ZOOM ||
        parsedZoom > MAX_ZOOM)
        return false;

    long parsedY = 0;
    if (!parseNonnegativeInteger(baseName(fileName), parsedY, &remainder) ||
        (strcasecmp(remainder, ".jpg") != 0 && strcasecmp(remainder, ".jpeg") != 0))
        return false;

    const long tileLimit = 1L << parsedZoom;
    if (parsedX >= tileLimit || parsedY >= tileLimit)
        return false;

    const size_t rootLength = static_cast<size_t>(zoomSeparator - directory);
    if (rootLength >= rootSize)
        return false;
    if (rootLength == 0) {
        if (rootSize < 2)
            return false;
        strcpy(root, "/");
    } else {
        memcpy(root, directory, rootLength);
        root[rootLength] = '\0';
    }
    tileZoom = static_cast<int>(parsedZoom);
    tileX = static_cast<int>(parsedX);
    tileY = static_cast<int>(parsedY);
    return true;
}

bool findFirstTileRecursive(const char *directoryPath, uint8_t depth, int &tileZoom, int &tileX, int &tileY, char *root,
                            size_t rootSize)
{
    if (depth > 4)
        return false;
    File directory = SD.open(directoryPath);
    if (!directory || !directory.isDirectory())
        return false;

    for (File entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
        char entryPath[128];
        const char *name = baseName(entry.name());
        if (strcmp(directoryPath, "/") == 0)
            snprintf(entryPath, sizeof(entryPath), "/%s", name);
        else
            snprintf(entryPath, sizeof(entryPath), "%s/%s", directoryPath, name);

        if (entry.isDirectory()) {
            entry.close();
            if (findFirstTileRecursive(entryPath, depth + 1, tileZoom, tileX, tileY, root, rootSize))
                return true;
        } else {
            const bool matched = parseTilePath(directoryPath, name, tileZoom, tileX, tileY, root, rootSize);
            entry.close();
            if (matched)
                return true;
        }
    }
    return false;
}

bool loadPositionFromFirstTile()
{
    concurrency::LockGuard guard(spiLock);
    CardputerSDSession session;
    if (!session)
        return false;

    int tileX = 0;
    int tileY = 0;
    int foundZoom = -1;
    char foundRoot[sizeof(tileRoot)] = {};
    if (!findFirstTileRecursive(MAP_STORAGE_ROOT, 0, foundZoom, tileX, tileY, foundRoot, sizeof(foundRoot)) &&
        !findFirstTileRecursive("/", 0, foundZoom, tileX, tileY, foundRoot, sizeof(foundRoot)))
        return false;

    strncpy(tileRoot, foundRoot, sizeof(tileRoot) - 1);
    tileRoot[sizeof(tileRoot) - 1] = '\0';
    const double scale = static_cast<double>(1UL << foundZoom);
    longitude = (static_cast<double>(tileX) + 0.5) / scale * 360.0 - 180.0;
    latitude = atan(sinh(M_PI * (1.0 - 2.0 * (static_cast<double>(tileY) + 0.5) / scale))) * 180.0 / M_PI;
    zoom = foundZoom;
    hasPosition = true;
    hasUserPosition = false;
    attemptedTileFallback = true;
    LOG_INFO("Map tiles found at %s; centered on z%d/%d/%d", tileRoot, zoom, tileX, tileY);
    return true;
}

void setMapPixel(int x, int y, bool dark)
{
    if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT)
        return;

    uint8_t &value = mapBitmap[y * BITMAP_STRIDE + (x >> 3)];
    const uint8_t mask = static_cast<uint8_t>(1U << (x & 7));
    if (dark)
        value |= mask;
    else
        value &= static_cast<uint8_t>(~mask);
}

int drawJpegBlock(JPEGDRAW *block)
{
    const uint16_t *pixels = static_cast<const uint16_t *>(block->pPixels);
    for (int row = 0; row < block->iHeight; row++) {
        const int screenY = block->y + row;
        if (screenY < 0 || screenY >= MAP_HEIGHT)
            continue;

        for (int column = 0; column < block->iWidth; column++) {
            const int screenX = block->x + column;
            if (screenX < 0 || screenX >= MAP_WIDTH)
                continue;

            const uint16_t color = pixels[row * block->iWidth + column];
            const uint16_t red = ((color >> 11) & 0x1f) * 255 / 31;
            const uint16_t green = ((color >> 5) & 0x3f) * 255 / 63;
            const uint16_t blue = (color & 0x1f) * 255 / 31;
            const uint16_t luminance = (red * 77 + green * 150 + blue * 29) >> 8;
            const uint8_t threshold = BAYER_4X4[(screenY & 3) * 4 + (screenX & 3)];
            setMapPixel(screenX, screenY, (255 - luminance) > threshold);
        }
    }
    return 1;
}

bool drawTile(int tileX, int tileY, int tileZoom, int screenX, int screenY)
{
    char path[64];
    if (strcmp(tileRoot, "/") == 0)
        snprintf(path, sizeof(path), "/%d/%d/%d.jpg", tileZoom, tileX, tileY);
    else
        snprintf(path, sizeof(path), "%s/%d/%d/%d.jpg", tileRoot, tileZoom, tileX, tileY);
    size_t size = 0;
    size_t bytesRead = 0;
    uint8_t *buffer = nullptr;
    {
        // Only hold the shared radio/SD bus while reading. JPEG decoding can
        // then run without delaying LoRa traffic.
        concurrency::LockGuard guard(spiLock);
        CardputerSDSession session;
        if (!session)
            return false;
        File file = SD.open(path, FILE_READ);
        if (!file)
            return false;

        size = file.size();
        if (size == 0 || size > MAX_JPEG_SIZE) {
            file.close();
            return false;
        }

        buffer = static_cast<uint8_t *>(malloc(size));
        if (!buffer) {
            file.close();
            return false;
        }
        bytesRead = file.read(buffer, size);
        file.close();
    }

    bool decoded = false;
    if (bytesRead == size && jpeg.openRAM(buffer, size, drawJpegBlock)) {
        jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
        decoded = jpeg.decode(screenX, screenY, 0) == 1;
        jpeg.close();
    }
    free(buffer);
    return decoded;
}

bool loadState()
{
    concurrency::LockGuard guard(spiLock);
    CardputerSDSession session;
    if (!session)
        return false;
    File file = SD.open(STATE_FILE, FILE_READ);
    if (!file)
        return false;

    String line = file.readStringUntil('\n');
    file.close();
    double savedLatitude = 0.0;
    double savedLongitude = 0.0;
    int savedZoom = DEFAULT_ZOOM;
    if (sscanf(line.c_str(), "%lf,%lf,%d", &savedLatitude, &savedLongitude, &savedZoom) != 3)
        return false;
    if (savedLatitude < -85.0 || savedLatitude > 85.0 || savedLongitude < -180.0 || savedLongitude > 180.0)
        return false;

    latitude = savedLatitude;
    longitude = savedLongitude;
    zoom = constrain(savedZoom, MIN_ZOOM, MAX_ZOOM);
    hasPosition = true;
    hasUserPosition = false;
    return true;
}

void saveState()
{
    if (!hasPosition)
        return;

    lastSaveMillis = millis();
    concurrency::LockGuard guard(spiLock);
    CardputerSDSession session;
    if (!session)
        return;
    SD.mkdir(MAP_STORAGE_ROOT);
    SD.remove(STATE_TEMP_FILE);
    File file = SD.open(STATE_TEMP_FILE, FILE_WRITE);
    if (file) {
        const size_t written = file.printf("%.6f,%.6f,%d\n", latitude, longitude, zoom);
        file.close();
        if (written > 0) {
            SD.remove(STATE_FILE);
            SD.rename(STATE_TEMP_FILE, STATE_FILE);
        }
    }
}

void initialize()
{
    if (initialized)
        return;
    initialized = true;
    if (!loadState())
        loadPositionFromFirstTile();
}

void updatePositionFromMeshtastic()
{
    if (!gpsStatus || (!gpsStatus->getHasLock() && !config.position.fixed_position) || manuallyPanned)
        return;

    const double newLatitude = gpsStatus->getLatitude() * 1e-7;
    const double newLongitude = gpsStatus->getLongitude() * 1e-7;
    if (newLatitude < -85.0 || newLatitude > 85.0 || newLongitude < -180.0 || newLongitude > 180.0)
        return;

    if (!hasPosition || fabs(newLatitude - latitude) > 0.00001 || fabs(newLongitude - longitude) > 0.00001) {
        latitude = newLatitude;
        longitude = newLongitude;
        hasPosition = true;
        hasUserPosition = true;
        panX = 0;
        panY = 0;
        cacheDirty = true;
    }
}

void rebuildCache()
{
    memset(mapBitmap, 0, sizeof(mapBitmap));
    foundTile = false;
    if (!hasPosition) {
        cacheDirty = false;
        return;
    }

    int centerTileX = 0;
    int centerTileY = 0;
    toTile(latitude, longitude, zoom, centerTileX, centerTileY);
    double centerPixelX = 0.0;
    double centerPixelY = 0.0;
    toPixel(latitude, longitude, zoom, centerPixelX, centerPixelY);
    const int baseX = MAP_WIDTH / 2 - static_cast<int>(centerPixelX - centerTileX * TILE_SIZE + panX);
    const int baseY = MAP_HEIGHT / 2 - static_cast<int>(centerPixelY - centerTileY * TILE_SIZE + panY);

    const int firstTileX = floorDivide(-baseX, TILE_SIZE);
    const int lastTileX = floorDivide(MAP_WIDTH - 1 - baseX, TILE_SIZE);
    const int firstTileY = floorDivide(-baseY, TILE_SIZE);
    const int lastTileY = floorDivide(MAP_HEIGHT - 1 - baseY, TILE_SIZE);
    for (int deltaX = firstTileX; deltaX <= lastTileX; deltaX++) {
        for (int deltaY = firstTileY; deltaY <= lastTileY; deltaY++) {
            const int screenX = baseX + deltaX * TILE_SIZE;
            const int screenY = baseY + deltaY * TILE_SIZE;
            foundTile |= drawTile(centerTileX + deltaX, centerTileY + deltaY, zoom, screenX, screenY);
        }
    }
    cacheDirty = false;
}

void drawStatus(OLEDDisplay *display, int16_t x, int16_t y)
{
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setColor(WHITE);
    display->fillRect(x, y, MAP_WIDTH, 12);
    display->setColor(BLACK);

    char left[42];
    snprintf(left, sizeof(left), "MAP Z%d %.4f %.4f", zoom, latitude, longitude);
    display->drawString(x + 3, y, left);

    if (gpsStatus && gpsStatus->getHasLock()) {
        char satellites[12];
        snprintf(satellites, sizeof(satellites), "S%lu", static_cast<unsigned long>(gpsStatus->getNumSatellites()));
        display->setTextAlignment(TEXT_ALIGN_RIGHT);
        display->drawString(x + MAP_WIDTH - 3, y, satellites);
        display->setTextAlignment(TEXT_ALIGN_LEFT);
    }
}

void drawCenteredMessage(OLEDDisplay *display, int16_t x, int16_t y, const char *line1, const char *line2)
{
    display->setColor(BLACK);
    display->fillRect(x, y, MAP_WIDTH, MAP_HEIGHT);
    display->setColor(WHITE);
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(x + MAP_WIDTH / 2, y + MAP_HEIGHT / 2 - FONT_HEIGHT_SMALL, line1);
    display->drawString(x + MAP_WIDTH / 2, y + MAP_HEIGHT / 2 + 2, line2);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
}

} // namespace

void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *, int16_t x, int16_t y)
{
    initialize();
    updatePositionFromMeshtastic();

    if (!hasPosition) {
        drawCenteredMessage(display, x, y, "No map tiles found", "Add /gpsmap/z/x/y.jpg");
        return;
    }

    if (cacheDirty)
        rebuildCache();

    // A saved position can point outside the tiles currently on the card. Locate actual coverage
    // once before reporting a missing tile, and also accept converter output nested below /gpsmap.
    if (!foundTile && !attemptedTileFallback && loadPositionFromFirstTile()) {
        panX = 0;
        panY = 0;
        manuallyPanned = false;
        cacheDirty = true;
        rebuildCache();
    }

    if (!foundTile) {
        drawCenteredMessage(display, x, y, "Map tile not found", "/gpsmap/z/x/y.jpg");
        drawStatus(display, x, y);
    } else {
        display->setColor(WHITE);
        display->drawXbm(x, y, MAP_WIDTH, MAP_HEIGHT, mapBitmap);
        registerTFTColorRegionDirect(x, y, MAP_WIDTH, MAP_HEIGHT, TFTPalette::Black, TFTPalette::Cream);

        if (hasUserPosition) {
            const int markerX = x + MAP_WIDTH / 2 - panX;
            const int markerY = y + MAP_HEIGHT / 2 - panY;
            if (markerX >= x && markerX < x + MAP_WIDTH && markerY >= y && markerY < y + MAP_HEIGHT) {
                display->setColor(WHITE);
                display->fillCircle(markerX, markerY, 5);
                display->setColor(BLACK);
                display->fillCircle(markerX, markerY, 2);
            }
        }
        drawStatus(display, x, y);
    }

    if (hasPosition && millis() - lastSaveMillis >= SAVE_INTERVAL_MS)
        saveState();
}

bool handleInput(const InputEvent &event)
{
    initialize();
    bool handled = true;
    if (event.inputEvent == INPUT_BROKER_UP) {
        panY -= PAN_STEP;
        manuallyPanned = true;
    } else if (event.inputEvent == INPUT_BROKER_DOWN) {
        panY += PAN_STEP;
        manuallyPanned = true;
    } else if (event.inputEvent == INPUT_BROKER_LEFT) {
        panX -= PAN_STEP;
        manuallyPanned = true;
    } else if (event.inputEvent == INPUT_BROKER_RIGHT) {
        panX += PAN_STEP;
        manuallyPanned = true;
    } else if ((event.kbchar == 'z' || event.kbchar == 'Z') && zoom > MIN_ZOOM) {
        zoom--;
        panX = 0;
        panY = 0;
    } else if ((event.kbchar == 'x' || event.kbchar == 'X') && zoom < MAX_ZOOM) {
        zoom++;
        panX = 0;
        panY = 0;
    } else if (event.kbchar == '`' || event.inputEvent == INPUT_BROKER_CANCEL) {
        manuallyPanned = false;
        panX = 0;
        panY = 0;
        updatePositionFromMeshtastic();
    } else {
        handled = false;
    }

    if (handled)
        cacheDirty = true;
    return handled;
}

} // namespace graphics::OfflineMapRenderer

#endif
