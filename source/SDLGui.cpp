/*
 * NESticle-style GUI for the SDL front end. See SDLGui.hpp.
 */
#include "SDLGui.hpp"
#include "SDLInput.hpp"
#include "GuiFontDejaVu.h"

#include <SDL2/SDL.h>   // scancodes only

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <direct.h>     // _getcwd
#include <io.h>         // _findfirst / _findnext
#define getcwd _getcwd
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __DJGPP__
#include <dir.h>        // getdisk / setdisk
#elif defined(_WIN32)
#include <direct.h>     // _getdrives
#endif

// ---------------------------------------------------------------- drawing
static constexpr uint16_t rgb(int r, int g, int b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
static const uint16_t C_FACE = rgb(192, 192, 192);
static const uint16_t C_LIGHT = rgb(255, 255, 255);
static const uint16_t C_SHADOW = rgb(128, 128, 128);
static const uint16_t C_DARK = rgb(0, 0, 0);
static const uint16_t C_TITLE = rgb(0, 0, 128);
static const uint16_t C_TEXT = rgb(0, 0, 0);
static const uint16_t C_WHITE = rgb(255, 255, 255);
static const uint16_t C_GRAY = rgb(128, 128, 128);
static const uint16_t C_DIR = rgb(0, 0, 160);
static const uint16_t C_ACCENT = rgb(176, 0, 0);

static const int CHAR_W = 6;     // GUI font cell width (GuiFontDejaVu.h)
static const int LINE_H = 10;
static const int BAR_H = 11;

// The canvas being drawn on
static uint16_t* cv;
static int cvW, cvH, cvY0, cvY1;

static inline void pset(int x, int y, uint16_t c)
{
    if (x >= 0 && x < cvW && y >= cvY0 && y < cvY1) cv[y * cvW + x] = c;
}

static void fill(int x, int y, int w, int h, uint16_t c)
{
    int x0 = std::max(x, 0), x1 = std::min(x + w, cvW);
    int y0 = std::max(y, cvY0), y1 = std::min(y + h, cvY1);
    for (int yy = y0; yy < y1; yy++) {
        uint16_t* row = cv + yy * cvW;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

static void hline(int x, int y, int w, uint16_t c) { fill(x, y, w, 1, c); }
static void vline(int x, int y, int h, uint16_t c) { fill(x, y, 1, h, c); }

// Raised (windows, buttons) or sunken (list boxes, pressed buttons) border
static void bevel(int x, int y, int w, int h, bool raised)
{
    uint16_t tl = raised ? C_LIGHT : C_SHADOW, br = raised ? C_DARK : C_LIGHT;
    hline(x, y, w, tl);
    vline(x, y, h, tl);
    hline(x, y + h - 1, w, br);
    vline(x + w - 1, y, h, br);
    if (raised) {
        hline(x + 1, y + h - 2, w - 2, C_SHADOW);
        vline(x + w - 2, y + 1, h - 2, C_SHADOW);
    }
}

// Mix two RGB565 colours: a/4 of c over d (a = 0..4)
static inline uint16_t mix565(uint16_t d, uint16_t c, int a)
{
    int r = ((d >> 11) * (4 - a) + (c >> 11) * a) / 4;
    int g = (((d >> 5) & 63) * (4 - a) + ((c >> 5) & 63) * a) / 4;
    int b = ((d & 31) * (4 - a) + (c & 31) * a) / 4;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Text in the GUI font (DejaVu Sans Mono, anti-aliased). y is the top of
// the old 8-pixel glyph box: the 6x10 cell starts a line above it, so
// the baseline stays where it was.
static void text(int x, int y, const std::string& s, uint16_t c)
{
    for (size_t i = 0; i < s.size(); i++, x += CHAR_W) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == ' ') continue;
        if (ch < 33 || ch > 126) ch = '?';   // outside ASCII (DOS code page names)
        const unsigned int* g = guiFont[ch - 32];
        for (int row = 0; row < GUI_FONT_H; row++) {
            int py = y - 1 + row;
            if (py < cvY0 || py >= cvY1) continue;
            unsigned int bits = g[row];
            for (int col = 0; col < GUI_FONT_W && bits; col++, bits >>= 3) {
                int a = bits & 7, px = x + col;
                if (!a || px < 0 || px >= cvW) continue;
                uint16_t& d = cv[py * cvW + px];
                d = a >= GUI_FONT_LEVELS ? c : mix565(d, c, a);
            }
        }
    }
}

static int textWidth(const std::string& s) { return (int)s.size() * CHAR_W; }

// Shorten to fit maxW pixels: "long name..." (or "...end" when keepEnd)
static std::string fitText(const std::string& s, int maxW, bool keepEnd = false)
{
    int maxChars = maxW / CHAR_W;
    if ((int)s.size() <= maxChars) return s;
    if (maxChars <= 3) return s.substr(0, std::max(maxChars, 0));
    if (keepEnd) return "..." + s.substr(s.size() - (maxChars - 3));
    return s.substr(0, maxChars - 3) + "...";
}

static void window(int x, int y, int w, int h, const std::string& title)
{
    fill(x, y, w, h, C_FACE);
    bevel(x, y, w, h, true);
    fill(x + 2, y + 2, w - 4, 10, C_TITLE);
    text(x + 4, y + 3, fitText(title, w - 8), C_WHITE);
}

static bool inside(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

// Mouse pointer: 1 = black outline, 2 = white fill
static const char* const cursorShape[] = {
    "1",
    "11",
    "121",
    "1221",
    "12221",
    "122221",
    "1222221",
    "12222221",
    "122222221",
    "1222211111",
    "1221221",
    "121 1221",
    "11  1221",
    "1    1221",
    "     1221",
    "      11",
};

static void drawCursor(int x, int y)
{
    for (int r = 0; r < 16; r++)
        for (int c = 0; cursorShape[r][c]; c++) {
            char k = cursorShape[r][c];
            if (k == '1') pset(x + c, y + r, C_DARK);
            else if (k == '2') pset(x + c, y + r, C_WHITE);
        }
}

// ---------------------------------------------------------------- widgets
struct Button
{
    int x = 0, y = 0, w = 0, h = 0;
    std::string label;
    void set(int bx, int by, int bw, const std::string& l) { x = bx; y = by; w = bw; h = 13; label = l; }
    bool hit(int px, int py) const { return inside(px, py, x, y, w, h); }
    void draw(bool hot, bool down = false, uint16_t textColor = C_TEXT) const
    {
        fill(x, y, w, h, C_FACE);
        bevel(x, y, w, h, !down);
        std::string t = fitText(label, w - 4);
        text(x + (w - textWidth(t)) / 2 + (down ? 1 : 0), y + 3 + (down ? 1 : 0), t, textColor);
        if (hot && !down) hline(x + 3, y + h - 3, w - 6, C_SHADOW);
    }
};

// Keyboard focus marker around a widget
static void focusRect(const Button& b)
{
    hline(b.x - 1, b.y - 1, b.w + 2, C_TITLE);
    hline(b.x - 1, b.y + b.h, b.w + 2, C_TITLE);
    vline(b.x - 1, b.y - 1, b.h + 2, C_TITLE);
    vline(b.x + b.w, b.y - 1, b.h + 2, C_TITLE);
}

// A scrolling list with a scroll bar on the right
struct ListBox
{
    int x = 0, y = 0, w = 0, h = 0;
    int count = 0, sel = 0, scroll = 0;
    bool dragging = false;
    int dragOffset = 0;

    static const int SB_W = 9;
    int rows() const { return std::max(1, (h - 4) / LINE_H); }
    int maxScroll() const { return std::max(0, count - rows()); }
    // Keep sel and scroll in range, without moving the view (the wheel and
    // the scroll bar may leave the selection out of sight)
    void bound()
    {
        if (count <= 0) { sel = 0; scroll = 0; return; }
        sel = std::max(0, std::min(sel, count - 1));
        scroll = std::max(0, std::min(scroll, maxScroll()));
    }
    // After the selection moved (keys, jump to letter): bring it into view
    void clamp()
    {
        bound();
        if (count <= 0) return;
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + rows()) scroll = sel - rows() + 1;
        scroll = std::max(0, std::min(scroll, maxScroll()));
    }
    // Scroll bar thumb (in pixels, along the track)
    void thumb(int& ty, int& th) const
    {
        int track = h - 2;
        if (count <= rows()) { ty = y + 1; th = track; return; }
        th = std::max(8, track * rows() / count);
        ty = y + 1 + (track - th) * scroll / std::max(1, maxScroll());
    }

    template <class LabelFn>
    void draw(LabelFn label) const
    {
        fill(x, y, w, h, C_WHITE);
        bevel(x, y, w, h, false);
        int tw = w - SB_W - 4;
        for (int r = 0; r < rows(); r++) {
            int i = scroll + r;
            if (i >= count) break;
            int ry = y + 2 + r * LINE_H;
            uint16_t color = C_TEXT;
            std::string s = label(i, color);
            if (i == sel) {
                fill(x + 2, ry, tw, LINE_H, C_TITLE);
                color = C_WHITE;
            }
            text(x + 4, ry + 1, fitText(s, tw - 4), color);
        }
        // scroll bar
        int sx = x + w - SB_W - 1;
        fill(sx, y + 1, SB_W, h - 2, rgb(224, 224, 224));
        int ty, th;
        thumb(ty, th);
        fill(sx, ty, SB_W, th, C_FACE);
        bevel(sx, ty, SB_W, th, true);
    }

    // Index of the row at (px, py), or -1
    int rowAt(int px, int py) const
    {
        if (!inside(px, py, x + 2, y + 2, w - SB_W - 4, rows() * LINE_H)) return -1;
        int i = scroll + (py - y - 2) / LINE_H;
        return i < count ? i : -1;
    }

    // Mouse down: returns true when it was on the list (row selection is
    // left to the caller through rowAt)
    bool mouseDown(int px, int py)
    {
        int sx = x + w - SB_W - 1;
        if (inside(px, py, sx, y, SB_W, h)) {
            int ty, th;
            thumb(ty, th);
            if (py < ty) scroll = std::max(0, scroll - rows());
            else if (py >= ty + th) scroll = std::min(maxScroll(), scroll + rows());
            else { dragging = true; dragOffset = py - ty; }
            return true;
        }
        return inside(px, py, x, y, w, h);
    }
    void mouseMove(int py)
    {
        if (!dragging) return;
        int track = h - 2, ty, th;
        thumb(ty, th);
        int span = track - th;
        if (span <= 0) return;
        int pos = py - dragOffset - (y + 1);
        scroll = std::max(0, std::min(maxScroll(), (pos * maxScroll() + span / 2) / span));
    }
    void wheel(int d) { scroll = std::max(0, std::min(maxScroll(), scroll - d * 3)); }

    // Navigation keys; returns true when it was one
    bool key(int sc)
    {
        switch (sc) {
            case SDL_SCANCODE_UP: sel--; break;
            case SDL_SCANCODE_DOWN: sel++; break;
            case SDL_SCANCODE_PAGEUP: sel -= rows() - 1; break;
            case SDL_SCANCODE_PAGEDOWN: sel += rows() - 1; break;
            case SDL_SCANCODE_HOME: sel = 0; break;
            case SDL_SCANCODE_END: sel = count - 1; break;
            default: return false;
        }
        clamp();
        return true;
    }
};

// ---------------------------------------------------------------- dialogs
enum class DialogResult { Keep, Close, CloseGui };

class GuiDialog
{
public:
    GuiDialog(Gui& g, const std::string& t, int width, int height)
        : gui(g), title(t), w(width), h(height) {}
    virtual ~GuiDialog() {}
    virtual void draw(uint32_t now) = 0;
    virtual DialogResult handle(const GuiEvent& e, uint32_t now) = 0;

protected:
    Gui& gui;
    std::string title;
    int x = 0, y = 0, w, h;
    // Center in the visible part of the picture, below the menu bar
    void place()
    {
        int top = gui.top + BAR_H, bottom = gui.bottom;
        h = std::min(h, bottom - top - 2);
        x = (gui.width - w) / 2;
        y = top + std::max(0, (bottom - top - h) / 2);
    }
    int mx() const { return gui.mouseX; }
    int my() const { return gui.mouseY; }
};

// --- message box
class MessageDialog : public GuiDialog
{
public:
    MessageDialog(Gui& g, const std::string& t, const std::string& msg) : GuiDialog(g, t, 200, 60)
    {
        // word wrap
        int maxChars = (w - 12) / CHAR_W;
        std::string line, word;
        auto flush = [&]() { if (!line.empty()) lines.push_back(line); line.clear(); };
        for (size_t i = 0; i <= msg.size(); i++) {
            char c = i < msg.size() ? msg[i] : ' ';
            if (c == ' ' || c == '\n') {
                if ((int)(line.size() + word.size() + 1) > maxChars) flush();
                if (!word.empty()) line += (line.empty() ? "" : " ") + word;
                word.clear();
                if (c == '\n') flush();
            } else {
                word += c;
            }
        }
        flush();
        h = 36 + (int)lines.size() * LINE_H;
    }
    void draw(uint32_t) override
    {
        place();
        window(x, y, w, h, title);
        for (size_t i = 0; i < lines.size(); i++) text(x + 6, y + 16 + (int)i * LINE_H, lines[i], C_TEXT);
        ok.set(x + (w - 50) / 2, y + h - 17, 50, "OK");
        ok.draw(ok.hit(mx(), my()));
    }
    DialogResult handle(const GuiEvent& e, uint32_t) override
    {
        if (e.type == GuiEvent::KeyDown &&
            (e.scancode == SDL_SCANCODE_ESCAPE || e.scancode == SDL_SCANCODE_RETURN || e.scancode == SDL_SCANCODE_SPACE))
            return DialogResult::Close;
        if (e.type == GuiEvent::MouseDown && ok.hit(e.x, e.y)) return DialogResult::Close;
        return DialogResult::Keep;
    }

private:
    std::vector<std::string> lines;
    Button ok;
};

// --- game browser
class FileDialog : public GuiDialog
{
public:
    FileDialog(Gui& g) : GuiDialog(g, "Load Game", 244, 230)
    {
        std::string start = gui.host.gameDirectory();
        if (start.empty() || !readDir(start)) {
            char cwd[1024];
            if (getcwd(cwd, sizeof(cwd))) start = cwd; else start = ".";
            readDir(normalize(start));
        }
    }

    void draw(uint32_t) override
    {
        place();
        window(x, y, w, h, title);
        text(x + 6, y + 15, fitText(path, w - 12, true), C_TEXT);
        list.x = x + 4; list.y = y + 26; list.w = w - 8; list.h = h - 26 - 20;
        list.count = (int)entries.size();
        list.bound();
        list.draw([&](int i, uint16_t& color) {
            const Entry& en = entries[i];
            if (en.kind != E_FILE) color = C_DIR;
            switch (en.kind) {
                case E_PARENT: return std::string("[..]");
                case E_DIR: return "[" + en.name + "]";
                case E_DRIVE: return "[-" + en.name + "-]";
                default: return en.name;
            }
        });
        openBtn.set(x + w - 112, y + h - 17, 52, "Open");
        cancelBtn.set(x + w - 56, y + h - 17, 52, "Cancel");
        openBtn.draw(openBtn.hit(mx(), my()));
        cancelBtn.draw(cancelBtn.hit(mx(), my()));
        char info[32];
        snprintf(info, sizeof(info), "%d games", games);
        text(x + 6, y + h - 14, info, C_GRAY);
    }

    DialogResult handle(const GuiEvent& e, uint32_t now) override
    {
        switch (e.type) {
            case GuiEvent::MouseDown:
                if (e.button != 1) break;
                if (openBtn.hit(e.x, e.y)) return activate();
                if (cancelBtn.hit(e.x, e.y)) return DialogResult::Close;
                if (list.mouseDown(e.x, e.y)) {
                    int r = list.rowAt(e.x, e.y);
                    if (r >= 0) {
                        bool dbl = (r == lastClickRow && now - lastClickTime < 450);
                        list.sel = r;
                        lastClickRow = r;
                        lastClickTime = now;
                        if (dbl) { lastClickRow = -1; return activate(); }
                    }
                }
                break;
            case GuiEvent::MouseUp:
                list.dragging = false;
                break;
            case GuiEvent::MouseMove:
                list.mouseMove(e.y);
                break;
            case GuiEvent::MouseWheel:
                list.wheel(e.wheel);
                break;
            case GuiEvent::KeyDown:
                if (e.scancode == SDL_SCANCODE_ESCAPE) return DialogResult::Close;
                if (e.scancode == SDL_SCANCODE_RETURN || e.scancode == SDL_SCANCODE_KP_ENTER) return activate();
                if (e.scancode == SDL_SCANCODE_BACKSPACE) { goUp(); break; }
                if (list.key(e.scancode)) break;
                jumpToLetter(e.scancode);
                break;
            default:
                break;
        }
        return DialogResult::Keep;
    }

private:
    enum Kind { E_PARENT, E_DRIVE, E_DIR, E_FILE };
    struct Entry { std::string name; Kind kind; };
    std::vector<Entry> entries;
    std::string path;
    ListBox list;
    Button openBtn, cancelBtn;
    int games = 0;
    int lastClickRow = -1;
    uint32_t lastClickTime = 0;

    static std::string normalize(std::string p)
    {
        std::replace(p.begin(), p.end(), '\\', '/');
        while (p.size() > 1 && p.back() == '/' && !(p.size() == 3 && p[1] == ':')) p.pop_back();
        if (p.size() == 2 && p[1] == ':') p += '/';
        return p;
    }
    static bool isRoot(const std::string& p) { return p == "/" || (p.size() == 3 && p[1] == ':' && p[2] == '/'); }
    static std::string join(const std::string& dir, const std::string& name)
    {
        return dir.back() == '/' ? dir + name : dir + "/" + name;
    }
    static std::string parentOf(const std::string& p)
    {
        if (isRoot(p)) return p;
        size_t s = p.find_last_of('/');
        if (s == std::string::npos) return p;
        if (s == 0) return "/";
        if (s == 2 && p[1] == ':') return p.substr(0, 3);
        return p.substr(0, s);
    }
    static std::string lower(std::string s)
    {
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        return s;
    }
    static bool isGame(const std::string& name)
    {
        std::string l = lower(name);
        return l.size() > 4 && l.compare(l.size() - 4, 4, ".nes") == 0;
    }

    bool readDir(const std::string& p)
    {
        std::vector<Entry> dirs, files;
#ifdef _MSC_VER
        // MSVC has no dirent.h: the C runtime's _findfirst/_findnext
        struct _finddata_t fd;
        intptr_t h = _findfirst(join(p, "*").c_str(), &fd);
        if (h == -1) return false;
        do {
            std::string name = fd.name;
            if (name == "." || name == "..") continue;
            if (fd.attrib & _A_SUBDIR) dirs.push_back({ name, E_DIR });
            else if (isGame(name)) files.push_back({ name, E_FILE });
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
#else
        DIR* d = opendir(p.c_str());
        if (!d) return false;
        while (struct dirent* de = readdir(d)) {
            std::string name = de->d_name;
            if (name == "." || name == "..") continue;
            struct stat st;
            if (stat(join(p, name).c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) dirs.push_back({ name, E_DIR });
            else if (isGame(name)) files.push_back({ name, E_FILE });
        }
        closedir(d);
#endif
        auto byName = [](const Entry& a, const Entry& b) { return lower(a.name) < lower(b.name); };
        std::sort(dirs.begin(), dirs.end(), byName);
        std::sort(files.begin(), files.end(), byName);

        entries.clear();
        if (!isRoot(p)) entries.push_back({ "..", E_PARENT });
        entries.insert(entries.end(), dirs.begin(), dirs.end());
        entries.insert(entries.end(), files.begin(), files.end());
#ifdef __DJGPP__
        // Drive letters (NESticle style, at the end of the list)
        int current = getdisk();
        int last = setdisk(current);
        for (int dr = 0; dr < last && dr < 26; dr++) {
            setdisk(dr);
            if (getdisk() == dr) entries.push_back({ std::string(1, (char)('A' + dr)) + ":", E_DRIVE });
        }
        setdisk(current);
#elif defined(_WIN32)
        // Drive letters, as on DOS
        unsigned long drives = _getdrives();
        for (int dr = 0; dr < 26; dr++)
            if (drives & (1UL << dr)) entries.push_back({ std::string(1, (char)('A' + dr)) + ":", E_DRIVE });
#endif
        games = (int)files.size();
        path = p;
        list.sel = 0;
        list.scroll = 0;
        if (!isRoot(p) && entries.size() > 1) list.sel = 1;  // first entry after [..]
        return true;
    }

    void goUp()
    {
        std::string old = path, up = parentOf(path);
        if (up == path) return;
        if (readDir(up)) {
            // select the folder we came from
            std::string name = old.substr(old.find_last_of('/') + 1);
            for (size_t i = 0; i < entries.size(); i++)
                if (entries[i].kind == E_DIR && entries[i].name == name) list.sel = (int)i;
            list.count = (int)entries.size();
            list.clamp();
        }
    }

    DialogResult activate()
    {
        if (entries.empty()) return DialogResult::Keep;
        const Entry& en = entries[list.sel];
        switch (en.kind) {
            case E_PARENT: goUp(); return DialogResult::Keep;
            case E_DIR:
                if (!readDir(join(path, en.name))) gui.showMessage("Error", "Can't open folder " + en.name);
                return DialogResult::Keep;
            case E_DRIVE:
                if (!readDir(en.name + "/")) gui.showMessage("Error", "Drive " + en.name + " is not ready.");
                return DialogResult::Keep;
            case E_FILE: {
                std::string file = join(path, en.name);
                if (gui.host.loadGame(file)) {
                    gui.host.setGameDirectory(path);
                    return DialogResult::CloseGui;
                }
                gui.showMessage("Error", "Couldn't load " + en.name + " (not a NES ROM, or an unsupported mapper).");
                return DialogResult::Keep;
            }
        }
        return DialogResult::Keep;
    }

    void jumpToLetter(int sc)
    {
        char c = 0;
        if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) c = (char)('a' + (sc - SDL_SCANCODE_A));
        else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) c = (char)('1' + (sc - SDL_SCANCODE_1));
        else if (sc == SDL_SCANCODE_0) c = '0';
        if (!c) return;
        int n = (int)entries.size();
        for (int k = 1; k <= n; k++) {
            int i = (list.sel + k) % n;
            if (!entries[i].name.empty() && tolower((unsigned char)entries[i].name[0]) == c) {
                list.sel = i;
                list.clamp();
                return;
            }
        }
    }
};

// --- display mode
class VideoDialog : public GuiDialog
{
public:
    VideoDialog(Gui& g) : GuiDialog(g, "Display Mode", 180, 170)
    {
        list.count = gui.host.videoModeCount();
        current = gui.host.currentVideoMode();
        list.sel = std::max(0, current);
    }
    void draw(uint32_t) override
    {
        place();
        window(x, y, w, h, title);
        list.x = x + 4; list.y = y + 15; list.w = w - 8; list.h = h - 15 - 20;
        list.count = gui.host.videoModeCount();
        // the current mode in view when the dialog opens; after that the
        // view stays where the user scrolls it
        if (firstDraw) { list.clamp(); firstDraw = false; } else list.bound();
        list.draw([&](int i, uint16_t& color) {
            if (i == current) color = C_ACCENT;
            return (i == current ? "* " : "  ") + gui.host.videoModeName(i);
        });
        okBtn.set(x + w - 112, y + h - 17, 52, "OK");
        cancelBtn.set(x + w - 56, y + h - 17, 52, "Cancel");
        okBtn.draw(okBtn.hit(mx(), my()));
        cancelBtn.draw(cancelBtn.hit(mx(), my()));
    }
    DialogResult handle(const GuiEvent& e, uint32_t now) override
    {
        switch (e.type) {
            case GuiEvent::MouseDown:
                if (e.button != 1) break;
                if (okBtn.hit(e.x, e.y)) return apply();
                if (cancelBtn.hit(e.x, e.y)) return DialogResult::Close;
                if (list.mouseDown(e.x, e.y)) {
                    int r = list.rowAt(e.x, e.y);
                    if (r >= 0) {
                        bool dbl = (r == lastRow && now - lastTime < 450);
                        list.sel = r; lastRow = r; lastTime = now;
                        if (dbl) return apply();
                    }
                }
                break;
            case GuiEvent::MouseUp: list.dragging = false; break;
            case GuiEvent::MouseMove: list.mouseMove(e.y); break;
            case GuiEvent::MouseWheel: list.wheel(e.wheel); break;
            case GuiEvent::KeyDown:
                if (e.scancode == SDL_SCANCODE_ESCAPE) return DialogResult::Close;
                if (e.scancode == SDL_SCANCODE_RETURN || e.scancode == SDL_SCANCODE_KP_ENTER) return apply();
                list.key(e.scancode);
                break;
            default: break;
        }
        return DialogResult::Keep;
    }

private:
    ListBox list;
    Button okBtn, cancelBtn;
    bool firstDraw = true;
    int current, lastRow = -1;
    uint32_t lastTime = 0;
    DialogResult apply()
    {
        if (list.sel != current && !gui.host.setVideoMode(list.sel)) {
            std::string why = gui.host.videoError();
            gui.showMessage("Error", "Couldn't switch to " + gui.host.videoModeName(list.sel) + "." +
                                     (why.empty() ? "" : " (" + why + ")"));
            return DialogResult::Keep;
        }
        return DialogResult::Close;
    }
};

// --- input configuration
class InputDialog : public GuiDialog
{
public:
    InputDialog(Gui& g, int p)
        : GuiDialog(g, "Player " + std::to_string(p + 1) + " Controls", 236, 182), player(p)
    {
        saved = gui.host.input().player[p];
    }

    void draw(uint32_t now) override
    {
        place();
        window(x, y, w, h, title);
        InputConfig& in = gui.host.input();
        PlayerInput& pi = in.player[player];

        text(x + 6, y + 17, "Joystick:", C_TEXT);
        std::string jn;
        if (pi.joystick < 0) jn = "None";
        else {
            jn = std::to_string(pi.joystick + 1) + ": ";
            std::string name = in.joystickName(pi.joystick);
            jn += name.empty() ? "(not connected)" : name;
        }
        joyBtn.set(x + 64, y + 14, w - 70, jn);
        joyBtn.draw(joyBtn.hit(mx(), my()));

        int ty = y + 31;
        text(x + 6, ty, "Button", C_GRAY);
        static_assert(NES_BUTTONS == 8, "row order");
        text(x + 56, ty, "Key", C_GRAY);
        text(x + 158, ty, "Joystick", C_GRAY);
        bool blink = (now / 400) % 2 == 0;
        for (int b = 0; b < NES_BUTTONS; b++) {
            int nb = order[b];
            int ry = ty + 11 + b * 13;
            text(x + 6, ry + 3, InputConfig::buttonName(nb), C_TEXT);

            std::string k = (captureKey == nb) ? (blink ? "Press a key" : "") : InputConfig::keyName(pi.key[nb]);
            keyBtn[nb].set(x + 50, ry, 100, k);
            keyBtn[nb].draw(keyBtn[nb].hit(mx(), my()), captureKey == nb);

            std::string j;
            if (captureJoy == nb) j = blink ? "Press button" : "";
            else if (pi.joyButton[nb] >= 0) j = "Button " + std::to_string(pi.joyButton[nb]);
            else j = (nb >= 4) ? "Stick/hat" : "(none)";
            joyCell[nb].set(x + 152, ry, w - 158, j);
            joyCell[nb].draw(joyCell[nb].hit(mx(), my()), captureJoy == nb);
        }
        int by = y + h - 17;
        defaultsBtn.set(x + 6, by, 60, "Defaults");
        okBtn.set(x + w - 112, by, 52, "OK");
        cancelBtn.set(x + w - 56, by, 52, "Cancel");
        defaultsBtn.draw(defaultsBtn.hit(mx(), my()));
        okBtn.draw(okBtn.hit(mx(), my()));
        cancelBtn.draw(cancelBtn.hit(mx(), my()));
        text(x + 6, by - 11, "Click/Enter, then press. Del clears.", C_GRAY);
        if (const Button* f = focused()) focusRect(*f);
    }

    DialogResult handle(const GuiEvent& e, uint32_t) override
    {
        InputConfig& in = gui.host.input();
        PlayerInput& pi = in.player[player];

        if (captureKey >= 0 && e.type == GuiEvent::KeyDown) {
            if (e.scancode != SDL_SCANCODE_ESCAPE) pi.key[captureKey] = e.scancode;
            captureKey = -1;
            return DialogResult::Keep;
        }
        if (captureJoy >= 0) {
            if (e.type == GuiEvent::KeyDown) {          // Esc (or any key) cancels
                captureJoy = -1;
                return DialogResult::Keep;
            }
            if (e.type == GuiEvent::JoyButton) {
                if (pi.joystick < 0) pi.joystick = e.joystick;
                if (e.joystick == pi.joystick) {
                    pi.joyButton[captureJoy] = e.button;
                    captureJoy = -1;
                }
                return DialogResult::Keep;
            }
        }

        switch (e.type) {
            case GuiEvent::MouseDown: {
                captureKey = captureJoy = -1;
                if (joyBtn.hit(e.x, e.y)) {
                    // cycle None, 1, 2, ... (up to the joysticks present, at least 2)
                    int n = std::max(2, in.joystickCount());
                    pi.joystick = (pi.joystick + 2) % (n + 1) - 1;
                    break;
                }
                for (int b = 0; b < NES_BUTTONS; b++) {
                    if (keyBtn[b].hit(e.x, e.y)) {
                        if (e.button == 3) pi.key[b] = 0; else captureKey = b;
                        return DialogResult::Keep;
                    }
                    if (joyCell[b].hit(e.x, e.y)) {
                        if (e.button == 3) pi.joyButton[b] = -1; else captureJoy = b;
                        return DialogResult::Keep;
                    }
                }
                if (e.button != 1) break;
                if (defaultsBtn.hit(e.x, e.y)) { in.setDefaults(player); break; }
                if (okBtn.hit(e.x, e.y)) { gui.host.inputChanged(); return DialogResult::Close; }
                if (cancelBtn.hit(e.x, e.y)) { pi = saved; return DialogResult::Close; }
                break;
            }
            case GuiEvent::KeyDown: {
                // Keyboard use: arrows move the focus, Enter/Space activates,
                // Delete/Backspace clears a binding, Esc cancels
                int cols = focusRow == 0 ? 1 : (focusRow <= NES_BUTTONS ? 2 : 3);
                switch (e.scancode) {
                    case SDL_SCANCODE_ESCAPE: pi = saved; return DialogResult::Close;
                    case SDL_SCANCODE_UP: focusRow = std::max(0, focusRow - 1); break;
                    case SDL_SCANCODE_DOWN: focusRow = std::min(NES_BUTTONS + 1, focusRow + 1); break;
                    case SDL_SCANCODE_LEFT: focusCol = std::max(0, std::min(focusCol, cols - 1) - 1); break;
                    case SDL_SCANCODE_RIGHT: focusCol = std::min(cols - 1, focusCol + 1); break;
                    case SDL_SCANCODE_DELETE:
                    case SDL_SCANCODE_BACKSPACE:
                        if (focusRow >= 1 && focusRow <= NES_BUTTONS) {
                            int nb = order[focusRow - 1];
                            if (focusCol == 0) pi.key[nb] = 0; else pi.joyButton[nb] = -1;
                        }
                        break;
                    case SDL_SCANCODE_RETURN:
                    case SDL_SCANCODE_KP_ENTER:
                    case SDL_SCANCODE_SPACE:
                        if (focusRow == 0) {
                            int n = std::max(2, in.joystickCount());
                            pi.joystick = (pi.joystick + 2) % (n + 1) - 1;
                        } else if (focusRow <= NES_BUTTONS) {
                            int nb = order[focusRow - 1];
                            if (std::min(focusCol, 1) == 0) captureKey = nb; else captureJoy = nb;
                        } else {
                            int c = std::min(focusCol, 2);
                            if (c == 0) in.setDefaults(player);
                            else if (c == 1) { gui.host.inputChanged(); return DialogResult::Close; }
                            else { pi = saved; return DialogResult::Close; }
                        }
                        break;
                    default: break;
                }
                break;
            }
            default: break;
        }
        return DialogResult::Keep;
    }

private:
    // rows in the usual order: directions, then Select, Start, B, A
    static constexpr int order[NES_BUTTONS] = { 4, 5, 6, 7, 2, 3, 1, 0 };
    int player;
    PlayerInput saved;
    int captureKey = -1, captureJoy = -1;
    // keyboard focus: row 0 joystick, 1-8 the buttons (col 0 key, 1 joystick),
    // 9 Defaults / OK / Cancel
    int focusRow = 1, focusCol = 0;
    const Button* focused() const
    {
        if (focusRow == 0) return &joyBtn;
        if (focusRow <= NES_BUTTONS) {
            int nb = order[focusRow - 1];
            return std::min(focusCol, 1) == 0 ? &keyBtn[nb] : &joyCell[nb];
        }
        int c = std::min(focusCol, 2);
        return c == 0 ? &defaultsBtn : (c == 1 ? &okBtn : &cancelBtn);
    }
    Button joyBtn, keyBtn[NES_BUTTONS], joyCell[NES_BUTTONS], defaultsBtn, okBtn, cancelBtn;
};

// --- about / keys
class AboutDialog : public GuiDialog
{
public:
    AboutDialog(Gui& g) : GuiDialog(g, "About WarpNES", 232, 170) {}
    void draw(uint32_t) override
    {
        place();
        window(x, y, w, h, title);
        static const char* const lines[] = {
            "WarpNES - NES emulator",
            "",
            "Esc         Menus (pauses game)",
            "Ctrl+O      Load game",
            "Ctrl+R      Reset",
            "Ctrl+P      Pause",
            "F5-F8       Save state 1-4",
            "Shift+F5-F8 Load state 1-4",
#ifndef __DJGPP__
            "F11         Full screen",
#endif
            "",
            "Controls: Input menu",
            "Zapper: mouse aims, left click",
            "  fires (Input > Zapper)",
        };
        int ly = y + 16;
        for (const char* l : lines) { text(x + 8, ly, l, C_TEXT); ly += LINE_H; }
        ok.set(x + (w - 50) / 2, y + h - 17, 50, "OK");
        ok.draw(ok.hit(mx(), my()));
    }
    DialogResult handle(const GuiEvent& e, uint32_t) override
    {
        if (e.type == GuiEvent::KeyDown &&
            (e.scancode == SDL_SCANCODE_ESCAPE || e.scancode == SDL_SCANCODE_RETURN))
            return DialogResult::Close;
        if (e.type == GuiEvent::MouseDown && ok.hit(e.x, e.y)) return DialogResult::Close;
        return DialogResult::Keep;
    }

private:
    Button ok;
};

// ---------------------------------------------------------------- menus
enum MenuId {
    M_LOAD = 1, M_EXIT,
    M_RESET, M_PAUSE,
    M_SAVE1, M_SAVE2, M_SAVE3, M_SAVE4,
    M_LOAD1, M_LOAD2, M_LOAD3, M_LOAD4,
    M_VIDEO,
    M_INPUT1, M_INPUT2, M_ZAPPER,
    M_ABOUT
};

struct MenuItem { const char* label; const char* keys; int id; };   // label null: separator
struct Menu { const char* title; std::vector<MenuItem> items; };

static const std::vector<Menu>& menus()
{
    static const std::vector<Menu> m = {
        { "File", { { "Load Game...", "Ctrl+O", M_LOAD }, { nullptr, nullptr, 0 }, { "Exit", "", M_EXIT } } },
        { "Game", { { "Reset", "Ctrl+R", M_RESET },
                    { nullptr, nullptr, 0 },
                    { "Save State 1", "F5", M_SAVE1 }, { "Save State 2", "F6", M_SAVE2 },
                    { "Save State 3", "F7", M_SAVE3 }, { "Save State 4", "F8", M_SAVE4 },
                    { nullptr, nullptr, 0 },
                    { "Load State 1", "Sh+F5", M_LOAD1 }, { "Load State 2", "Sh+F6", M_LOAD2 },
                    { "Load State 3", "Sh+F7", M_LOAD3 }, { "Load State 4", "Sh+F8", M_LOAD4 } } },
        { "Video", { { "Display Mode...", "", M_VIDEO } } },
        { "Input", { { "Player 1...", "", M_INPUT1 }, { "Player 2...", "", M_INPUT2 },
                     { nullptr, nullptr, 0 },
                     { "Zapper (mouse)", "", M_ZAPPER } } },
        { "Help", { { "About / Keys...", "", M_ABOUT } } },
    };
    return m;
}

static bool needsGame(int id) { return id >= M_RESET && id <= M_LOAD4; }

// Right-hand column of a menu item: its key, or for the Zapper its setting
static const char* itemKeys(const MenuItem& it, GuiHost& host)
{
    if (it.id == M_ZAPPER) {
        switch (host.zapperSetting()) {
            case 1: return "On";
            case 2: return "Off";
            default: return host.zapperActive() ? "Auto: on" : "Auto: off";
        }
    }
    return it.keys;
}

static int menuTitleX(int menu)
{
    int x = 4;
    for (int i = 0; i < menu; i++) x += textWidth(menus()[i].title) + 12;
    return x;
}

// ---------------------------------------------------------------- Gui
Gui::Gui(GuiHost& h) : host(h) {}
Gui::~Gui() {}

void Gui::open()
{
    barOpen = true;
    openMenu = -1;
    hotItem = -1;
}

void Gui::close()
{
    if (!host.gameLoaded()) return;   // nothing to go back to
    barOpen = false;
    openMenu = -1;
    dialog.reset();
}

void Gui::openGameBrowser()
{
    open();
    dialog.reset(new FileDialog(*this));
}

void Gui::showMessage(const std::string& title, const std::string& text)
{
    // Shown on top of the current dialog; when it closes, that one comes back
    pendingMessage.reset(new MessageDialog(*this, title, text));
}

bool Gui::visible(uint32_t now) const
{
    return barOpen || dialog || pendingMessage || pausedLabel || (mouseSeen && now - lastMouseMove < 2500);
}

void Gui::menuBox(int menu, int& x, int& y, int& w, int& h) const
{
    const Menu& m = menus()[menu];
    int labelW = 0, keysW = 0;
    for (const MenuItem& it : m.items) {
        if (!it.label) continue;
        labelW = std::max(labelW, textWidth(it.label));
        keysW = std::max(keysW, textWidth(it.id == M_ZAPPER ? "Auto: off" : it.keys));
    }
    w = labelW + (keysW ? keysW + 18 : 0) + 16;
    h = 4;
    for (const MenuItem& it : m.items) h += it.label ? LINE_H + 1 : 5;
    x = menuTitleX(menu) - 2;
    if (x + w > width - 1) x = width - 1 - w;
    y = top + BAR_H;
}

int Gui::menuAt(int x, int y) const
{
    if (y < top || y >= top + BAR_H) return -1;
    for (size_t i = 0; i < menus().size(); i++) {
        int tx = menuTitleX((int)i) - 4;
        if (x >= tx && x < tx + textWidth(menus()[i].title) + 12) return (int)i;
    }
    return -1;
}

int Gui::itemAt(int menu, int px, int py) const
{
    if (menu < 0) return -1;
    int x, y, w, h;
    menuBox(menu, x, y, w, h);
    if (!inside(px, py, x, y, w, h)) return -1;
    int iy = y + 2;
    const Menu& m = menus()[menu];
    for (size_t i = 0; i < m.items.size(); i++) {
        int ih = m.items[i].label ? LINE_H + 1 : 5;
        if (py >= iy && py < iy + ih) return m.items[i].label ? (int)i : -1;
        iy += ih;
    }
    return -1;
}

void Gui::runMenuItem(int menu, int item)
{
    const MenuItem& it = menus()[menu].items[item];
    openMenu = -1;
    hotItem = -1;
    if (needsGame(it.id) && !host.gameLoaded()) return;
    switch (it.id) {
        case M_LOAD: dialog.reset(new FileDialog(*this)); break;
        case M_EXIT: host.quit(); break;
        case M_RESET: host.resetGame(); close(); break;
        case M_SAVE1: case M_SAVE2: case M_SAVE3: case M_SAVE4:
            host.saveState(it.id - M_SAVE1 + 1);
            close();
            break;
        case M_LOAD1: case M_LOAD2: case M_LOAD3: case M_LOAD4:
            if (host.loadState(it.id - M_LOAD1 + 1)) close();
            else showMessage("Load State", "There is no saved state " + std::to_string(it.id - M_LOAD1 + 1) + " for this game.");
            break;
        case M_VIDEO: dialog.reset(new VideoDialog(*this)); break;
        case M_INPUT1: dialog.reset(new InputDialog(*this, 0)); break;
        case M_INPUT2: dialog.reset(new InputDialog(*this, 1)); break;
        case M_ABOUT: dialog.reset(new AboutDialog(*this)); break;
        case M_ZAPPER:
            // Auto -> On -> Off -> Auto; the menu stays open to show it
            host.setZapperSetting((host.zapperSetting() + 1) % 3);
            openMenu = menu;
            hotItem = item;
            break;
    }
}

bool Gui::handle(const GuiEvent& e, uint32_t now)
{
    bool mouse = e.type == GuiEvent::MouseMove || e.type == GuiEvent::MouseDown ||
                 e.type == GuiEvent::MouseUp || e.type == GuiEvent::MouseWheel;
    // Aiming the Zapper in the game: the mouse belongs to the game
    bool aiming = zapperMode && !barOpen && !dialog && !pendingMessage;
    if (mouse) {
        if (!(e.type == GuiEvent::MouseWheel && e.x < 0)) {   // wheel without a position: keep the last
            mouseX = std::max(0, std::min(width - 1, e.x));
            mouseY = std::max(top, std::min(bottom - 1, e.y));
        }
        if (!aiming) {
            lastMouseMove = now;
            mouseSeen = true;
        } else {
            mouseSeen = false;   // no pointer or menu bar over the game
        }
    }
    GuiEvent ev = e;
    if (mouse) { ev.x = mouseX; ev.y = mouseY; }

    // Message box over everything
    if (pendingMessage) {
        if (pendingMessage->handle(ev, now) != DialogResult::Keep) pendingMessage.reset();
        return true;
    }

    // Right click while playing: open the menus
    if (!barOpen) {
        if (ev.type == GuiEvent::MouseDown && ev.button == 3) { open(); return true; }
        if (ev.type == GuiEvent::MouseDown && ev.button == 1) {
            int m = menuAt(ev.x, ev.y);
            if (m >= 0 && visible(now)) { open(); openMenu = m; hotItem = -1; return true; }
        }
        return false;
    }

    if (dialog) {
        DialogResult r = dialog->handle(ev, now);
        if (r == DialogResult::Close) dialog.reset();
        else if (r == DialogResult::CloseGui) { dialog.reset(); barOpen = false; openMenu = -1; }
        return true;
    }

    switch (ev.type) {
        case GuiEvent::MouseDown: {
            int m = menuAt(ev.x, ev.y);
            if (m >= 0) {
                openMenu = (openMenu == m) ? -1 : m;
                hotItem = -1;
                return true;
            }
            if (openMenu >= 0) {
                int it = itemAt(openMenu, ev.x, ev.y);
                if (it >= 0) runMenuItem(openMenu, it);
                else openMenu = -1;
                return true;
            }
            if (ev.button == 3) close();   // right click outside the menus: back to the game
            return true;
        }
        case GuiEvent::MouseMove:
            if (openMenu >= 0) {
                int m = menuAt(ev.x, ev.y);
                if (m >= 0 && m != openMenu) { openMenu = m; hotItem = -1; }
                else hotItem = itemAt(openMenu, ev.x, ev.y);
            }
            return true;
        case GuiEvent::KeyDown: {
            int n = (int)menus().size();
            auto step = [&](int dir) {
                const Menu& mm = menus()[openMenu];
                int cnt = (int)mm.items.size();
                int i = hotItem;
                for (int k = 0; k < cnt; k++) {
                    i = (i < 0) ? (dir > 0 ? 0 : cnt - 1) : (i + dir + cnt) % cnt;
                    if (mm.items[i].label) { hotItem = i; return; }
                }
            };
            switch (ev.scancode) {
                case SDL_SCANCODE_ESCAPE:
                    if (openMenu >= 0) openMenu = -1;
                    else close();
                    break;
                case SDL_SCANCODE_LEFT:
                    openMenu = (openMenu < 0 ? 0 : (openMenu + n - 1) % n);
                    hotItem = -1; step(1);
                    break;
                case SDL_SCANCODE_RIGHT:
                    openMenu = (openMenu < 0 ? 0 : (openMenu + 1) % n);
                    hotItem = -1; step(1);
                    break;
                case SDL_SCANCODE_DOWN:
                    if (openMenu < 0) { openMenu = 0; hotItem = -1; }
                    step(1);
                    break;
                case SDL_SCANCODE_UP:
                    if (openMenu < 0) { openMenu = 0; hotItem = -1; }
                    step(-1);
                    break;
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER:
                case SDL_SCANCODE_SPACE:
                    if (openMenu < 0) { openMenu = 0; hotItem = -1; step(1); }
                    else if (hotItem >= 0) runMenuItem(openMenu, hotItem);
                    break;
                default:
                    // Ctrl+O while the menus are up
                    if (ev.ctrl && ev.scancode == SDL_SCANCODE_O) dialog.reset(new FileDialog(*this));
                    break;
            }
            return true;
        }
        default:
            return true;
    }
}

void Gui::draw(uint16_t* pixels, int w, int h, int y0, int y1, uint32_t now)
{
    cv = pixels; cvW = w; cvH = h; cvY0 = std::max(0, y0); cvY1 = std::min(h, y1);
    width = w; top = cvY0; bottom = cvY1;

    bool recentMouse = mouseSeen && now - lastMouseMove < 2500;
    bool showBar = barOpen || recentMouse;

    if (showBar) {
        fill(0, top, w, BAR_H, C_FACE);
        hline(0, top + BAR_H - 1, w, C_SHADOW);
        for (size_t i = 0; i < menus().size(); i++) {
            int tx = menuTitleX((int)i);
            if ((int)i == openMenu) {
                fill(tx - 3, top + 1, textWidth(menus()[i].title) + 6, BAR_H - 2, C_TITLE);
                text(tx, top + 2, menus()[i].title, C_WHITE);
            } else {
                text(tx, top + 2, menus()[i].title, C_TEXT);
            }
        }
        if (barOpen && host.gameLoaded()) text(w - textWidth("Paused") - 4, top + 2, "Paused", C_ACCENT);
    }

    if (barOpen && openMenu >= 0) {
        int x, y, mw, mh;
        menuBox(openMenu, x, y, mw, mh);
        fill(x, y, mw, mh, C_FACE);
        bevel(x, y, mw, mh, true);
        int iy = y + 2;
        const Menu& m = menus()[openMenu];
        for (size_t i = 0; i < m.items.size(); i++) {
            const MenuItem& it = m.items[i];
            if (!it.label) {
                hline(x + 3, iy + 2, mw - 6, C_SHADOW);
                hline(x + 3, iy + 3, mw - 6, C_LIGHT);
                iy += 5;
                continue;
            }
            bool enabled = !needsGame(it.id) || host.gameLoaded();
            uint16_t col = enabled ? C_TEXT : C_GRAY;
            if ((int)i == hotItem && enabled) {
                fill(x + 2, iy, mw - 4, LINE_H + 1, C_TITLE);
                col = C_WHITE;
            }
            text(x + 8, iy + 2, it.label, col);
            const char* keys = itemKeys(it, host);
            if (keys && *keys) text(x + mw - 6 - textWidth(keys), iy + 2, keys, col);
            iy += LINE_H + 1;
        }
    }

    if (dialog) dialog->draw(now);
    if (pendingMessage) pendingMessage->draw(now);

    if (pausedLabel && !barOpen) {
        int lw = textWidth("PAUSED") + 8;
        int lx = (w - lw) / 2, ly = (top + bottom) / 2 - 6;
        fill(lx, ly, lw, 12, C_TITLE);
        text(lx + 4, ly + 2, "PAUSED", C_WHITE);
    }

    if (barOpen || recentMouse) drawCursor(mouseX, mouseY);
}
