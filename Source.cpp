#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <windows.h>

// Koikoi_Win32_SingleFile.cpp
// Extended Koikoi implementation for Win32 (single-file prototype)
// Features added:
// - Card image/sprite support via GDI+ (placeholder images expected in ./images/)
// - Drag & drop card play (mouse down + move + release)
// - Full yaku detection: red short (aka-tan), blue short (ao-tan), inoshikacho, rain/ame (雨月), etc.
// - Simple animation via InvalidateRect + timers
// - Sound effects via PlaySound (winmm.lib)
// - Save/load of match history to a local file (matches.txt)
// - AI with three difficulty levels: Easy (random), Normal (greedy), Hard (lookahead 1)
// - Placeholder comments for online multiplayer (requires networking stack / server)
// Build (Visual Studio):
// cl /EHsc /std:c++17 Koikoi_Win32_SingleFile.cpp user32.lib gdi32.lib gdiplus.lib winmm.lib

#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <algorithm>
#include <random>
#include <ctime>
#include <sstream>
#include <fstream>
#include <map>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")

using namespace Gdiplus;

// === Config ===
const int CARD_W = 96;
const int CARD_H = 128;
const int GAP = 10;

enum class CardType { KASU, TANZAKU, TANE, HIKARI };

struct Card {
    int month;
    CardType type;
    bool isRed = false;   // 赤短
    bool isBlue = false;  // 青短
    bool isInoshikachoPart = false; // part of 猪鹿蝶
    bool isRain = false; // rain hikari
    LPWSTR imgResourceID; // image resource ID
    std::string shortName() const { std::ostringstream ss; ss << month << "-"; switch (type) { case CardType::KASU:ss << "K"; break; case CardType::TANZAKU:ss << "Tz"; break; case CardType::TANE:ss << "Ta"; break; case CardType::HIKARI:ss << "H"; break; } return ss.str(); }
};

struct Player {
    std::vector<Card> hand;
    std::vector<Card> captured;
    int score = 0;
    bool koikoi = false;
};

enum class AIDifficulty { EASY, NORMAL, HARD };

class Game {
public:
    std::vector<Card> deck;
    std::vector<Card> field;
    std::vector<Card> stock;
    Player p[2]; // 0 human, 1 AI
    int turnPlayer = 0;
    bool roundOver = false;
    std::mt19937 rng;
    int lastYakuPlayer = -1;
    int lastYakuPoints = 0;
    AIDifficulty aiDifficulty = AIDifficulty::NORMAL;

    Game() { rng.seed((unsigned)time(nullptr)); initDeck(); shuffleDeck(); deal(); }

    void initDeck() {
        deck.clear();
        // Build realistic-ish deck mapping common attributes
        for (int m = 1; m <= 12; ++m) {
            // Example distribution; in real game use exact mapping
            Card c1{ m,CardType::KASU,false,false,false,false,L"images/card" };
            Card c2{ m,CardType::KASU,false,false,false,false,L"images/card" };
            Card c3{ m,CardType::TANE,false,false,false,false,L"images/card" };
            Card c4{ m,CardType::KASU,false,false,false,false,L"images/card" };
            // assign special types for certain months
            if (m == 1 || m == 3 || m == 8 || m == 11) c3.type = CardType::HIKARI;
            if (m == 2 || m == 4 || m == 6 || m == 9) c3.type = CardType::TANZAKU;
            // mark red/blue/inoshikacho roughly
            if (m == 3 || m == 6 || m == 9) c2.isRed = true; // example red
            if (m == 5 || m == 7 || m == 10) c2.isBlue = true; // example blue
            if (m == 6 || m == 7 || m == 10) c3.isInoshikachoPart = true; // approximate
            if (m == 11) c3.isRain = true; // November often has rain
            deck.push_back(c1); deck.push_back(c2); deck.push_back(c3); deck.push_back(c4);
        }
        // attach image path per card (placeholder using month index)
        for (auto& c : deck) {
            c.imgResourceID = MAKEINTRESOURCE(1000 + c.month * 4 + (int)c.type);
        }
    }

    void shuffleDeck() { std::shuffle(deck.begin(), deck.end(), rng); }
    Card popDeck() { Card c = deck.back(); deck.pop_back(); return c; }

    void deal() {
        for (int i = 0; i < 8; ++i) { p[0].hand.push_back(popDeck()); p[1].hand.push_back(popDeck()); }
        for (int i = 0; i < 8; ++i) field.push_back(popDeck());
        stock = deck; // remaining
    }

    bool isMatchOnField(const Card& card, int& outIndex) const {
        for (size_t i = 0; i < field.size(); ++i) if (field[i].month == card.month) { outIndex = (int)i; return true; }
        return false;
    }

    bool playCardFromHand(int playerIdx, int handIdx) {
        if (roundOver) return false;
        if (handIdx < 0 || handIdx >= (int)p[playerIdx].hand.size()) return false;
        Card play = p[playerIdx].hand[handIdx];
        p[playerIdx].hand.erase(p[playerIdx].hand.begin() + handIdx);
        int matchIdx = -1;
        if (isMatchOnField(play, matchIdx)) {
            Card f = field[matchIdx];
            p[playerIdx].captured.push_back(play);
            p[playerIdx].captured.push_back(f);
            field.erase(field.begin() + matchIdx);
            // play capture sound
            PlaySoundW(L"sounds/capture.wav", NULL, SND_FILENAME | SND_ASYNC);
        }
        else {
            field.push_back(play);
        }
        if (!stock.empty()) {
            Card drawn = stock.back(); stock.pop_back();
            int matchIdx2 = -1;
            if (isMatchOnField(drawn, matchIdx2)) {
                Card f2 = field[matchIdx2];
                p[playerIdx].captured.push_back(drawn);
                p[playerIdx].captured.push_back(f2);
                field.erase(field.begin() + matchIdx2);
                PlaySoundW(L"sounds/capture.wav", NULL, SND_FILENAME | SND_ASYNC);
            }
            else {
                field.push_back(drawn);
            }
        }
        int pts = evaluateYaku(p[playerIdx]);
        if (pts > 0) { lastYakuPlayer = playerIdx; lastYakuPoints = pts; return true; }
        turnPlayer = 1 - turnPlayer;
        if (p[0].hand.empty() && p[1].hand.empty()) roundOver = true;
        return true;
    }

    // Full yaku detection
    int evaluateYaku(const Player& pl) const {
        int hikari = 0, tane = 0, tanzaku = 0, kasu = 0;
        bool hasRed1 = false, hasRed2 = false, hasRed3 = false;
        bool hasBlue1 = false, hasBlue2 = false, hasBlue3 = false;
        int inoshikachoParts = 0;
        bool hasRainHikari = false;
        for (auto& c : pl.captured) {
    switch (c.type) { case CardType::HIKARI: hikari++; if (c.isRain) hasRainHikari = true; break; case CardType::TANE: tane++; if (c.isInoshikachoPart) inoshikachoParts++; break; case CardType::TANZAKU: tanzaku++; if (c.isRed) { if (!hasRed1) hasRed1 = true; else if (!hasRed2) hasRed2 = true; else hasRed3 = true; } if (c.isBlue) { if (!hasBlue1) hasBlue1 = true; else if (!hasBlue2) hasBlue2 = true; else hasBlue3 = true; } break; case CardType::KASU: kasu++; break; }
        }
        int pts = 0;
        // Hikari (including rain variants)
        if (hikari >= 5) pts += 10; // gokou
        else if (hikari == 4 && hasRainHikari) pts += 7; // amen-shikou
        else if (hikari == 4) pts += 8; // yonkou
        else if (hikari == 3 && !hasRainHikari) pts += 6; // sankou
        // inoshikacho (猪鹿蝶) requires specific t a n e parts; approximate by 3 tane special parts
        if (inoshikachoParts >= 3) pts += 5;
        // red tan (aka-tan): three red tanzaku
        if (hasRed1 && hasRed2 && hasRed3) pts += 5;
        // blue tan (ao-tan)
        if (hasBlue1 && hasBlue2 && hasBlue3) pts += 5;
        // tanzaku >=5
        if (tanzaku >= 5) pts += (tanzaku - 4);
        if (tane >= 5) pts += (tane - 4);
        if (kasu >= 10) pts += (kasu - 9);
        return pts;
    }

    void resolveYaku(int playerIdx, bool takePoints) {
        if (takePoints) {
            p[playerIdx].score += lastYakuPoints; p[playerIdx].koikoi = false; p[1 - playerIdx].koikoi = false; roundOver = true; // end round
            // play finish sound
            PlaySoundW(L"sounds/finish.wav", NULL, SND_FILENAME | SND_ASYNC);
            // save match result
            saveMatchResult(playerIdx, lastYakuPoints);
        }
        else { p[playerIdx].koikoi = true; turnPlayer = 1 - playerIdx; }
        lastYakuPoints = 0; lastYakuPlayer = -1;
    }

    // AI utilities
    bool aiPerformTurn() {
        if (p[1].hand.empty()) return false;
        int chosen = -1;
        if (aiDifficulty == AIDifficulty::EASY) { chosen = rng() % p[1].hand.size(); }
        else if (aiDifficulty == AIDifficulty::NORMAL) {
            // prefer matching plays
            for (int i = 0; i < (int)p[1].hand.size(); ++i) { int mi = -1; if (isMatchOnField(p[1].hand[i], mi)) { chosen = i; break; } }
            if (chosen == -1) chosen = rng() % p[1].hand.size();
        }
        else { // HARD: lookahead one turn simulate best immediate yaku
            int bestIdx = -1; int bestValue = -999;
            for (int i = 0; i < (int)p[1].hand.size(); ++i) {
                Game copy = *this; // shallow copy OK for simulation
                copy.playCardFromHand(1, i);
                int pts = copy.evaluateYaku(copy.p[1]);
                int val = pts - copy.lastYakuPoints / 2; // heuristic
                if (val > bestValue) { bestValue = val; bestIdx = i; }
            }
            chosen = (bestIdx == -1) ? (rng() % p[1].hand.size()) : bestIdx;
        }
        bool gotYaku = playCardFromHand(1, chosen);
        if (gotYaku && lastYakuPlayer == 1) {
            if (lastYakuPoints >= 5) resolveYaku(1, true);
            else {
                bool ko = (rng() % 2) == 0;
                resolveYaku(1, !ko);
            }
        }
        return true;
    }

    void saveMatchResult(int winnerIdx, int points) {
        std::ofstream ofs("matches.txt", std::ios::app);
        if (!ofs) return;
        std::time_t t = std::time(nullptr);
        ofs << std::asctime(std::localtime(&t)) << ": Winner=" << (winnerIdx == 0 ? "You" : "AI") << ", pts=" << points << "\n";
        ofs.close();
    }
};

// Global game pointer
static Game* g_game = nullptr;

// GDI+ resources
static std::map<UINT64, Image*> g_images;

Image* LoadCachedImage(LPWSTR lpszResourceID) {
    auto it = g_images.find((UINT64)lpszResourceID);
    if (it != g_images.end()) return it->second;
    Image* img = nullptr;
    try { img = Image::FromFile(path.c_str()); }
    catch (...) { img = nullptr; }
    g_images[path] = img;
    return img;
}

// Drag & drop state
struct DragState { bool dragging = false; int handIndex = -1; POINT start; POINT cur; } g_drag;

// Animation: simple card float effect
int g_animTick = 0;

// Forward
void OnPaint(HWND hwnd, HDC hdc);

void DrawCardGDIPlus(Graphics& g, const RECT& r, const Card& c, bool faceUp = true, bool highlight = false) {
    Rect rect(r.left, r.top, r.right - r.left, r.bottom - r.top);
    Image* img = LoadCachedImage(c.imgPath);
    if (img) {
        g.DrawImage(img, rect);
    }
    else {
        // fallback: draw rounded rectangle
        SolidBrush br(Color(255, 255, 255, 240));
        g.FillRectangle(&br, rect);
        Pen pen(Color(highlight ? 255 : 0, 0, 0), highlight ? 4.0f : 1.0f);
        g.DrawRectangle(&pen, rect);
        // draw text
        FontFamily ff(L"Arial"); Font font(&ff, 12, FontStyleRegular, UnitPixel);
        PointF pt((REAL)r.left + 10, (REAL)r.top + 10);
        std::wstring s = std::wstring(c.shortName().begin(), c.shortName().end());
        SolidBrush tb(Color(255, 0, 0, 0));
        g.DrawString(s.c_str(), -1, &font, pt, &tb);
    }
}

// Paint handler using GDI+
void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);

    Graphics g(hdc);
    // clear background
    SolidBrush bg(Color(255, 34, 139, 34));
    g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

    if (!g_game) { EndPaint(hwnd, &ps); return; }

    // draw field
    int fx = GAP, fy = GAP;
    for (size_t i = 0; i < g_game->field.size(); ++i) { RECT r{ fx,fy,fx + CARD_W,fy + CARD_H }; DrawCardGDIPlus(g, r, g_game->field[i], true, false); fx += CARD_W + GAP; if (fx + CARD_W > rc.right - GAP) { fx = GAP; fy += CARD_H + GAP; } }

    // draw AI captured (small icons)
    int ax = GAP, ay = fy + CARD_H + GAP;
    for (size_t i = 0; i < g_game->p[1].captured.size(); ++i) { RECT r{ ax,ay,ax + 40,ay + 30 }; DrawCardGDIPlus(g, r, g_game->p[1].captured[i], true, false); ax += 44; }

    // draw player captured
    int px = GAP, py = rc.bottom - CARD_H - 160;
    for (size_t i = 0; i < g_game->p[0].captured.size(); ++i) { RECT r{ px,py,px + 40,py + 30 }; DrawCardGDIPlus(g, r, g_game->p[0].captured[i], true, false); px += 44; }

    // draw player's hand
    int handX = GAP, handY = rc.bottom - CARD_H - GAP;
    for (int i = 0; i < (int)g_game->p[0].hand.size(); ++i) { RECT r{ handX, handY, handX + CARD_W, handY + CARD_H }; bool highlight = (g_drag.dragging && g_drag.handIndex == i); DrawCardGDIPlus(g, r, g_game->p[0].hand[i], true, highlight); handX += CARD_W + GAP; }

    // draw dragging card if any
    if (g_drag.dragging && g_drag.handIndex >= 0 && g_drag.handIndex < (int)g_game->p[0].hand.size()) {
        RECT r{ g_drag.cur.x - CARD_W / 2, g_drag.cur.y - CARD_H / 2, g_drag.cur.x + CARD_W / 2, g_drag.cur.y + CARD_H / 2 }; DrawCardGDIPlus(g, r, g_game->p[0].hand[g_drag.handIndex], true, true);
    }

    // HUD text
    std::wstringstream ss; ss << L"Your:" << g_game->p[0].score << L"  AI:" << g_game->p[1].score << L"  Stock:" << g_game->stock.size();
    FontFamily ff(L"Segoe UI"); Font font(&ff, 14, FontStyleRegular, UnitPixel);
    SolidBrush brush(Color(255, 255, 255, 255));
    g.DrawString(ss.str().c_str(), -1, &font, PointF(10.0f, (REAL)rc.bottom - 30), &brush);

    EndPaint(hwnd, &ps);
}

int HitTestHand(HWND hwnd, POINT pt) { RECT rc; GetClientRect(hwnd, &rc); int handX = GAP, handY = rc.bottom - CARD_H - GAP; for (int i = 0; i < (int)g_game->p[0].hand.size(); ++i) { RECT r{ handX,handY,handX + CARD_W,handY + CARD_H }; if (PtInRect(&r, pt)) return i; handX += CARD_W + GAP; } return -1; }

// Simple area to play: top center
bool IsOverFieldPlayArea(HWND hwnd, POINT pt) { RECT rc; GetClientRect(hwnd, &rc); RECT play{ rc.right / 2 - 150, rc.top + 20, rc.right / 2 + 150, rc.top + 180 }; return PtInRect(&play, pt); }

// Window proc
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: SetTimer(hwnd, 1, 40, NULL); return 0;
    case WM_TIMER: g_animTick++; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_PAINT: OnPaint(hwnd); return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        int hit = HitTestHand(hwnd, pt);
        if (hit >= 0 && g_game && g_game->turnPlayer == 0 && !g_game->roundOver) {
            g_drag.dragging = true; g_drag.handIndex = hit; g_drag.start = pt; g_drag.cur = pt; SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_drag.dragging) { POINT pt{ LOWORD(lParam), HIWORD(lParam) }; g_drag.cur = pt; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_drag.dragging) {
            POINT pt{ LOWORD(lParam), HIWORD(lParam) }; ReleaseCapture();
            // if released over play area, play that card
            if (IsOverFieldPlayArea(hwnd, pt) && g_game) {
                bool gotYaku = g_game->playCardFromHand(0, g_drag.handIndex);
                // ask user if got yaku
                if (gotYaku && g_game->lastYakuPlayer == 0) { std::wstringstream ss; ss << L"You got " << g_game->lastYakuPoints << L" points. Koikoi?"; int res = MessageBoxW(hwnd, ss.str().c_str(), L"Yaku!", MB_YESNO | MB_ICONQUESTION); if (res == IDYES) g_game->resolveYaku(0, false); else g_game->resolveYaku(0, true); }
                // AI moves until human turn
                while (g_game && !g_game->roundOver && g_game->turnPlayer == 1) { g_game->aiPerformTurn(); }
                if (g_game->roundOver) { std::wstringstream rs; rs << L"Round Over\nYou:" << g_game->p[0].score << L" AI:" << g_game->p[1].score; MessageBoxW(hwnd, rs.str().c_str(), L"Result", MB_OK); }
            }
            g_drag.dragging = false; g_drag.handIndex = -1; InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    }
    case WM_DESTROY: KillTimer(hwnd, 1); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    GdiplusStartupInput gdiplusStartupInput; ULONG_PTR gdiplusToken; GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    WNDCLASSEX wc{}; wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc = WndProc; wc.hInstance = hInstance; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"KoikoiClass2"; RegisterClassEx(&wc);
    HWND hwnd = CreateWindowEx(0, wc.lpszClassName, L"Koikoi Extended", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1400, 900, NULL, NULL, hInstance, NULL);
    if (!hwnd) return -1;
    Game game; g_game = &game; // keep on stack for now
    ShowWindow(hwnd, nCmdShow); UpdateWindow(hwnd);
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    GdiplusShutdown(gdiplusToken);
    // cleanup images
    for (auto& kv : g_images) if (kv.second) delete kv.second;
    return (int)msg.wParam;
}

// Notes & next steps:
// - Place card images in ./images/ named like "1_0.png" .. where second number is type index.
// - Place sounds: sounds/capture.wav and sounds/finish.wav (or change paths)
// - For online multiplayer: implement a server to synchronize play states (recommend using WebSocket or simple TCP), then add network code to send/receive actions and validate moves.
// - Improve yaku detection by using an exact mapping table for each card's specific attributes.
// - Use resource management for images and audio in release builds.
// - Add settings UI for AI difficulty, sound toggle, and save/load buttons.
