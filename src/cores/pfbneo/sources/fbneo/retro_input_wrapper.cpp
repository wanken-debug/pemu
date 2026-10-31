//
// Created by cpasjuste on 24/02/2022.
//

#include "retro_input.h"

unsigned nGameType = 0;
bool bIsNeogeoCartGame = false;
bool bLightgunHideCrosshairEnabled = true;
struct GameInp *pgi_reset = nullptr;
struct GameInp *pgi_diag = nullptr;
UINT8 *diag_input = nullptr;
struct GameInp *GameInp = nullptr;
UINT32 nGameInpCount = 0;
INT32 nAnalogSpeed = 0x0100;
INT32 nFireButtons = 0;
static int nDIPOffset;

// Some old games give player 1 two sticks: one for movements, one for attacks (ie Data East
// "Chanbara"). FBNeo reports the second stick as player 3 directions (the driver input list
// says "p3 up" / "p3 down" ... for "P1 Up (right)" / "P1 Down (right)" ...), which no physical
// pad of the frontend can reach since every player only has one stick here. When such a game
// is detected, player 1 right stick analog axis is used to feed those inputs.
static bool bDualStickMode = false;
// tangent of 22.5 degrees, same angular zones as the left stick handling in libcross2d
#define DUALSTICK_SLOPE 0.414214f

static void dumpDualStickState(c2d::Input *input);
static int nDumpCalls = 0;
// peak absolute axis values seen while playing (lx, ly, rx, ry), so a short push is not lost
static short sPeak[PLAYER_MAX][4] = {};
static int nPeakSamples = 0;
static int nPort2Calls = 0;

static void getDualStickDirections(short axisX, short axisY,
                                   bool &up, bool &down, bool &left, bool &right) {
    up = down = left = right = false;

    // axis values are signed shorts, y is inverted (sdl/raw axis: down is positive)
    const float x = (float) axisX;
    const float y = -(float) axisY;

    if (x > 0 && y > 0) {                 // upper right quadrant
        if (y > DUALSTICK_SLOPE * x) up = true;
        if (x > DUALSTICK_SLOPE * y) right = true;
    } else if (y > 0 && x <= 0) {         // upper left quadrant
        if (y > DUALSTICK_SLOPE * -x) up = true;
        if (-x > DUALSTICK_SLOPE * y) left = true;
    } else if (y <= 0 && x > 0) {         // lower right quadrant
        if (-y > DUALSTICK_SLOPE * x) down = true;
        if (x > DUALSTICK_SLOPE * -y) right = true;
    } else if (y <= 0 && x <= 0) {        // lower left quadrant
        if (-y > DUALSTICK_SLOPE * -x) down = true;
        if (-x > DUALSTICK_SLOPE * -y) left = true;
    }
}

// Detect games where player 1 second stick is exposed as player 3 directions, and configure
// the input layer accordingly. Called on every driver init, so state is always reset.
void SetDualStickMode() {
    struct BurnInputInfo bii{};
    bool bP1SecondStick = false;
    bool bRealP3 = false;
    int nP3Found = 0;

    for (int i = 0; BurnDrvGetInputInfo(&bii, i) == 0; i++) {
        if (!bii.szInfo || !bii.szName) continue;
        if (strncmp(bii.szInfo, "p3 ", 3) != 0) continue;
        nP3Found++;
        if (strstr(bii.szName, "(right)") != nullptr) {
            bP1SecondStick = true;
        } else if (strncmp(bii.szName, "P3 ", 3) == 0) {
            bRealP3 = true;
        }
    }

    bDualStickMode = bP1SecondStick && !bRealP3;
    printf("SetDualStickMode: %i (p3 inputs: %i, real p3: %i)\n",
           bDualStickMode, nP3Found, bRealP3);

    if (c2d_renderer && c2d_renderer->getInput()) {
        c2d::Input::Player *player = c2d_renderer->getInput()->getPlayer(0);
        if (player) {
            // in dual stick mode the right stick is used by the emulator itself and must not
            // be merged into player 1 direction buttons anymore
            player->rightStickToButtons = !bDualStickMode;
        }
        // immediate dump: tells us the detection result even if the port 2 path never runs
        dumpDualStickState(c2d_renderer->getInput());
    }
}

// Write the current state of every pad to a file next to the .nro, so the user can report
// back what the frontend actually exposes when the second stick does not work.
static void dumpDualStickState(c2d::Input *input) {
    // the working directory is not always the .nro folder on switch: try several locations
    static const char *paths[] = {
            "sdmc:/pfbneo_dualstick_debug.txt",
            "pfbneo_dualstick_debug.txt",
            "sdmc:/switch/pfbneo/pfbneo_dualstick_debug.txt",
            "sdmc:/switch/pfbneo_dualstick_debug.txt",
    };

    FILE *f = nullptr;
    for (const char *path: paths) {
        f = fopen(path, "w");
        if (f) break;
    }
    if (!f) return;

    fprintf(f, "dual stick mode: %s\n", bDualStickMode ? "ON" : "OFF");
    fprintf(f, "dump calls: %d (0 = port 2 path never used)\n", ++nDumpCalls);
    fprintf(f, "peak samples: %d, port2 calls: %d\n", nPeakSamples, nPort2Calls);
    fprintf(f, "players: %d (player count from the frontend)\n", PLAYER_MAX);
    for (int i = 0; i < PLAYER_MAX; i++) {
        c2d::Input::Player *p = input->getPlayer(i);
        if (!p) continue;
        fprintf(f, "player%d enabled=%d data=%p lx=%d ly=%d rx=%d ry=%d dz=%d buttons=0x%08x peak=(%d,%d,%d,%d)\n",
                i, p->enabled ? 1 : 0, p->data, p->lx.value, p->ly.value,
                p->rx.value, p->ry.value, p->dz, p->buttons,
                sPeak[i][0], sPeak[i][1], sPeak[i][2], sPeak[i][3]);
    }
    fclose(f);
}

int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port < PLAYER_MAX) {
        auto *input = c2d_renderer->getInput();
        unsigned int buttons = input->getPlayer((int) port)->buttons;
        //printf("input_cb(joy): port: %i, device: %i, index: %i, id: %x (keys: %x)\n",
        //     port, device, index, id, keys);

        // peak sampling for every call, so the axis values are still recorded when the dual
        // stick path below is never reached
        for (int i = 0; i < PLAYER_MAX; i++) {
            c2d::Input::Player *p = input->getPlayer(i);
            if (!p) continue;
            const short values[4] = {p->lx.value, p->ly.value, p->rx.value, p->ry.value};
            for (int a = 0; a < 4; a++) {
                const short v = values[a] < 0 ? (short) -values[a] : values[a];
                if (v > sPeak[i][a]) sPeak[i][a] = v;
            }
        }
        nPeakSamples++;
        if (port == 2) nPort2Calls++;

        static int nDumpTick = 0;
        static int nDumpCount = 0;
        if (nDumpCount < 40 && ++nDumpTick >= 200) {
            nDumpTick = 0;
            nDumpCount++;
            dumpDualStickState(input);
        }

        // dual stick games: player 1 second stick is reported by fbneo as player 3 directions
        if (bDualStickMode && port == 2) {
            c2d::Input::Player *player = input->getPlayer(0);
            if (!player) {
                return 0;
            }

            short axisX = player->rx.value;
            short axisY = player->ry.value;

            // On some platforms (ie Nintendo Switch with two separate joy-cons) the frontend
            // exposes each joy-con as its own controller: player 1 then has no right stick at
            // all (rx/ry stay at 0) and the right joy-con shows up as player 2, with its stick
            // reported on lx/ly. Fall back to it so the second stick still works.
            if (axisX == 0 && axisY == 0) {
                c2d::Input::Player *second = input->getPlayer(1);
                if (second && second->enabled) {
                    axisX = second->lx.value;
                    axisY = second->ly.value;
                }
            }

            bool up, down, left, right;
            getDualStickDirections(axisX, axisY, up, down, left, right);

            if (id == RETRO_DEVICE_ID_JOYPAD_UP) return up ? 1 : 0;
            if (id == RETRO_DEVICE_ID_JOYPAD_DOWN) return down ? 1 : 0;
            if (id == RETRO_DEVICE_ID_JOYPAD_LEFT) return left ? 1 : 0;
            if (id == RETRO_DEVICE_ID_JOYPAD_RIGHT) return right ? 1 : 0;

            return 0;
        }

        if (device == RETRO_DEVICE_JOYPAD) {
            return buttons & id ? 1 : 0;
        }
    }

    return 0;
}

void poll_cb() {}

static void InpDIPSWGetOffset() {
    BurnDIPInfo bdi{};
    nDIPOffset = 0;

    for (int i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++) {
        if (bdi.nFlags == 0xF0) {
            nDIPOffset = bdi.nInput;
            HandleMessage(RETRO_LOG_INFO, "DIP switches offset: %d.\n", bdi.nInput);
            break;
        }
    }
}

void InpDIPSWResetDIPs() {
    int i = 0;
    BurnDIPInfo bdi{};
    struct GameInp *pgi;

    InpDIPSWGetOffset();

    while (BurnDrvGetDIPInfo(&bdi, i) == 0) {
        if (bdi.nFlags == 0xFF) {
            pgi = GameInp + bdi.nInput + nDIPOffset;
            if (pgi) {
                pgi->Input.Constant.nConst = pgi->Input.nVal =
                        (pgi->Input.Constant.nConst & ~bdi.nMask) | (bdi.nSetting & bdi.nMask);
                if (pgi->Input.pVal) {
                    *(pgi->Input.pVal) = pgi->Input.nVal;
                }
            }
        }
        i++;
    }
}
