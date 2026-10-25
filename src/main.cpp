// TZU-style systematic-options dashboard: heatmap + trade bubbles, DOM, dealer gamma ladder,
// print tape + grade, status tiles. Native OpenGL via Dear ImGui. Consumes tzu::Ring.
#include "tzu/demo_feed.hpp"
#include "tzu/engine.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif
#ifdef __APPLE__
  #include <OpenGL/gl3.h>
#else
  #include <GL/gl.h>
#endif
#ifndef GL_CLAMP_TO_EDGE
  #define GL_CLAMP_TO_EDGE 0x812F
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <thread>

using namespace tzu;

// ---------- palette (cinematic red/blue on near-black) ----------
static ImU32 C(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
static const ImU32 kBg = C(8, 6, 9), kPanel = C(14, 11, 15), kEdge = C(40, 24, 32), kTxt = C(214, 208, 214), kDim = C(120, 108, 120),
                   kPink = C(255, 64, 118), kBlue = C(72, 124, 255), kGreen = C(70, 220, 130), kAmber = C(255, 190, 70);

static void txt(ImDrawList* dl, float x, float y, ImU32 c, const char* s) { dl->AddText(ImVec2(x, y), c, s); }
static void txtR(ImDrawList* dl, float xr, float y, ImU32 c, const char* s) { dl->AddText(ImVec2(xr - ImGui::CalcTextSize(s).x, y), c, s); }
static void txtC(ImDrawList* dl, float xc, float y, ImU32 c, const char* s) { dl->AddText(ImVec2(xc - ImGui::CalcTextSize(s).x * 0.5f, y), c, s); }

static std::string money(double v) {
    char b[48]; const double a = std::fabs(v); const char* sg = v < 0 ? "-" : "+";
    if (a >= 1e9) std::snprintf(b, sizeof b, "%s$%.2fB", sg, a / 1e9);
    else if (a >= 1e6) std::snprintf(b, sizeof b, "%s$%.1fM", sg, a / 1e6);
    else std::snprintf(b, sizeof b, "%s$%.0fK", sg, a / 1e3);
    return b;
}
static std::string hms(uint64_t ns) {
    std::time_t t = std::time_t(ns / 1'000'000'000ull); std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[16]; std::snprintf(b, sizeof b, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec); return b;
}

static void panelTitle(ImDrawList* dl, ImVec2 p, const char* title, const char* sub = "") {
    txt(dl, p.x + 10, p.y + 8, kTxt, title);
    if (*sub) txt(dl, p.x + 14 + ImGui::CalcTextSize(title).x, p.y + 8, kDim, sub);
    dl->AddLine(ImVec2(p.x + 8, p.y + 26), ImVec2(p.x + 80, p.y + 26), kPink, 1.5f);
}

// ---------- status tiles ----------
static void tile(ImDrawList* dl, ImVec2 p, ImVec2 s, const char* label, const char* big, const char* sub, ImU32 bigCol) {
    dl->AddRectFilled(p, ImVec2(p.x + s.x, p.y + s.y), kPanel, 6); dl->AddRect(p, ImVec2(p.x + s.x, p.y + s.y), kEdge, 6);
    txt(dl, p.x + 10, p.y + 6, kDim, label);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.35f, ImVec2(p.x + 10, p.y + 20), bigCol, big);
    txt(dl, p.x + 10, p.y + 42, kDim, sub);
}

static void tiles(Engine& e, ImVec2 p, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList(); const GexProfile& g = e.gex; char a[64], b[64];
    const float tw = (w - 6 * 8) / 7.f; float x = p.x;
    auto nextp = [&] { ImVec2 q(x, p.y); x += tw + 8; return q; };
    const bool shortG = g.valid && g.netGex < 0;
    tile(dl, nextp(), {tw, h}, "GAMMA REGIME", !g.valid ? "--" : shortG ? "SHORT GAMMA" : "LONG GAMMA", shortG ? "moves run" : "moves fade", shortG ? kPink : kBlue);
    tile(dl, nextp(), {tw, h}, "NET DEALER GAMMA", g.valid ? money(g.netGex).c_str() : "--", "per 1% move", g.netGex < 0 ? kPink : kBlue);
    std::snprintf(a, sizeof a, "%.0f", g.flip); std::snprintf(b, sizeof b, "ES %+.0f", g.flip ? e.spot() - g.flip : 0.0);
    tile(dl, nextp(), {tw, h}, "GAMMA FLIP", g.flip ? a : "--", b, kTxt);
    std::snprintf(a, sizeof a, "%s / %s", g.accel ? std::to_string(int(g.accel)).c_str() : "-", g.callWall ? std::to_string(int(g.callWall)).c_str() : "-");
    tile(dl, nextp(), {tw, h}, "ACCEL / CALL WALL", a, "dealer levels", kTxt);
    tile(dl, nextp(), {tw, h}, "NET DRIFT", money(e.drift).c_str(), (std::string("5m ") + money(e.drift5m)).c_str(), e.drift < 0 ? kPink : kGreen);
    std::snprintf(a, sizeof a, "\xC2\xB1%.1f", g.em); std::snprintf(b, sizeof b, "ATM IV %.1f%%", g.atmIv * 100);
    tile(dl, nextp(), {tw, h}, "EXPECTED MOVE", g.valid ? a : "--", b, kAmber);
    std::snprintf(a, sizeof a, "%.2f", e.spot()); std::snprintf(b, sizeof b, "day %+.2f", e.dayRef > 0 ? e.spot() - e.dayRef : 0.0);
    tile(dl, nextp(), {tw, h}, "ES", a, b, e.spot() >= e.dayRef ? kGreen : kPink);
}

// ---------- heatmap + bubbles ----------
static void chart(Engine& e, GLuint tex, ImVec2 s, uint64_t now) {
    ImDrawList* dl = ImGui::GetWindowDrawList(); const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + s.x, p0.y + s.y);
    const Heatmap& h = e.heat; const float axisW = 58.f, R = p1.x - axisW, W = R - p0.x, H = p1.y - p0.y;
    ImGui::Dummy(s);
    dl->PushClipRect(p0, p1, true);
    dl->AddRectFilled(p0, p1, kBg);
    const int cols = h.cols(), head = h.head(); const float u = float(head) / cols, wA = W * float(cols - head) / cols;
    if (head < cols) dl->AddImage((ImTextureID)(intptr_t)tex, p0, ImVec2(p0.x + wA, p1.y), ImVec2(u, 1), ImVec2(1, 0));   // older half
    if (head > 0)    dl->AddImage((ImTextureID)(intptr_t)tex, ImVec2(p0.x + wA, p0.y), ImVec2(R, p1.y), ImVec2(0, 1), ImVec2(u, 0)); // newer half
    const double base = double(h.baseTick()), rows = h.rows();
    auto yOf = [&](double tick) { return p1.y - float((tick - base + 0.5) / rows) * H; };

    // mid line
    ImVec2 prev(0, 0); bool have = false;
    for (int k = 0; k < cols; ++k) {
        const int idx = (head + k) % cols; const int64_t mt = h.midTickAt(idx);
        if (!mt) { have = false; continue; }
        ImVec2 cur(p0.x + (k + 0.5f) / cols * W, yOf(double(mt)));
        if (have && cur.y > p0.y && cur.y < p1.y) dl->AddLine(prev, cur, C(255, 255, 255, 150), 1.f);
        prev = cur; have = true;
    }
    // trade bubbles
    const double span = double(cols) * double(e.cfg.heatDtNs);
    for (const Bubble& b : e.bubbles) {
        const double age = now > b.ts ? double(now - b.ts) : 0.0; const float x = R - float(age / span) * W;
        const float y = yOf(b.px / h.tickSize()); if (y < p0.y || y > p1.y) continue;
        const float rad = 2.f + std::sqrt(float(b.qty)) * 0.9f;
        const ImU32 c = b.aggr == 1 ? C(70, 220, 130, 150) : b.aggr == 2 ? C(255, 64, 118, 150) : C(200, 200, 200, 120);
        dl->AddCircleFilled(ImVec2(x, y), rad, c); dl->AddCircle(ImVec2(x, y), rad, C(255, 255, 255, 60));
    }
    // gamma levels as dashed guides
    auto guide = [&](double px, ImU32 col, const char* lab) {
        const float y = yOf(px / h.tickSize()); if (y < p0.y || y > p1.y) return;
        for (float x = p0.x; x < R; x += 12) dl->AddLine(ImVec2(x, y), ImVec2(x + 6, y), col, 1.f);
        txt(dl, p0.x + 6, y - 14, col, lab);
    };
    if (e.gex.valid) { if (e.gex.callWall) guide(e.gex.callWall, kBlue, "CALL WALL"); if (e.gex.accel) guide(e.gex.accel, kPink, "ACCEL"); if (e.gex.flip) guide(e.gex.flip, kAmber, "FLIP"); }
    // price axis
    dl->AddRectFilled(ImVec2(R, p0.y), p1, C(10, 8, 11));
    const int64_t stepT = std::max<int64_t>(4, int64_t(rows / 14)) / 4 * 4;
    for (int64_t t = (h.baseTick() / stepT) * stepT; t < h.baseTick() + h.rows(); t += stepT) {
        const float y = yOf(double(t)); if (y < p0.y + 6 || y > p1.y - 6) continue;
        char b[24]; std::snprintf(b, sizeof b, "%.2f", t * h.tickSize()); txtR(dl, p1.x - 4, y - 6, kDim, b);
    }
    const double sp = e.spot();
    if (sp > 0) { const float y = yOf(sp / h.tickSize()); char b[24]; std::snprintf(b, sizeof b, "%.2f", sp);
        dl->AddRectFilled(ImVec2(R, y - 8), ImVec2(p1.x, y + 8), kTxt, 3); txtR(dl, p1.x - 4, y - 7, C(8, 6, 9), b); }
    dl->PopClipRect();
    dl->AddRect(p0, p1, kEdge);
    panelTitle(dl, p0, "ES", "  liquidity heatmap \xC2\xB7 trades");
}

// ---------- DOM ladder ----------
static void dom(Engine& e, ImVec2 s) {
    ImDrawList* dl = ImGui::GetWindowDrawList(); const ImVec2 p0 = ImGui::GetCursorScreenPos(); ImGui::Dummy(s);
    dl->AddRectFilled(p0, ImVec2(p0.x + s.x, p0.y + s.y), kPanel, 6); panelTitle(dl, p0, "DOM", "  MBP 10");
    const Book* b = e.primaryBook(); if (!b) return;
    const float rowH = std::min(26.f, (s.y - 44) / 21.f), cx = p0.x + s.x * 0.5f, maxW = s.x * 0.5f - 12; const float inv = 1.f / float(b->maxQty());
    float y = p0.y + 36; char t[32];
    for (int i = b->n[1] - 1; i >= 0; --i, y += rowH) {
        const float w = float(b->s[1][i].qty) * inv * maxW; dl->AddRectFilled(ImVec2(p0.x + s.x - 8 - w, y), ImVec2(p0.x + s.x - 8, y + rowH - 2), C(255, 64, 118, 110));
        std::snprintf(t, sizeof t, "%.2f", px2d(b->s[1][i].px)); txtC(dl, cx, y + 2, kTxt, t);
        std::snprintf(t, sizeof t, "%d", b->s[1][i].qty); txt(dl, p0.x + 10, y + 2, kPink, t);
    }
    dl->AddLine(ImVec2(p0.x + 8, y), ImVec2(p0.x + s.x - 8, y), kEdge); y += 4;
    for (int i = 0; i < b->n[0]; ++i, y += rowH) {
        const float w = float(b->s[0][i].qty) * inv * maxW; dl->AddRectFilled(ImVec2(p0.x + 8, y), ImVec2(p0.x + 8 + w, y + rowH - 2), C(72, 124, 255, 110));
        std::snprintf(t, sizeof t, "%.2f", px2d(b->s[0][i].px)); txtC(dl, cx, y + 2, kTxt, t);
        std::snprintf(t, sizeof t, "%d", b->s[0][i].qty); txtR(dl, p0.x + s.x - 10, y + 2, kBlue, t);
    }
}

// ---------- dealer gamma ladder ----------
static void gammaLadder(Engine& e, ImVec2 s) {
    ImDrawList* dl = ImGui::GetWindowDrawList(); const ImVec2 p0 = ImGui::GetCursorScreenPos(); ImGui::Dummy(s);
    dl->AddRectFilled(p0, ImVec2(p0.x + s.x, p0.y + s.y), kPanel, 6); panelTitle(dl, p0, "DEALERS", "  SHORT gamma | LONG gamma");
    const GexProfile& g = e.gex; if (!g.valid) { txt(dl, p0.x + 12, p0.y + 40, kDim, "waiting for option definitions + OI..."); return; }
    std::vector<const GexRow*> v;
    for (auto it = g.rows.rbegin(); it != g.rows.rend(); ++it) if (std::fabs(it->strike - g.spot) < 0.025 * g.spot) v.push_back(&*it);
    if (v.empty()) return;
    const float top = p0.y + 34, bot = p0.y + s.y - 40, rowH = std::min(18.f, (bot - top) / float(v.size())), cx = p0.x + s.x * 0.5f, half = s.x * 0.5f - 46;
    char t[32]; float y = top; bool spotDrawn = false;
    for (const GexRow* r : v) {
        if (!spotDrawn && r->strike <= g.spot) { dl->AddLine(ImVec2(p0.x + 6, y), ImVec2(p0.x + s.x - 6, y), kTxt, 1.f);
            std::snprintf(t, sizeof t, "ES %.2f", g.spot); dl->AddRectFilled(ImVec2(p0.x + 6, y - 8), ImVec2(p0.x + 6 + ImGui::CalcTextSize(t).x + 8, y + 8), kTxt, 3); txt(dl, p0.x + 10, y - 7, kBg, t); spotDrawn = true; }
        const float w = float(std::fabs(r->net) / g.maxAbs) * half;
        const bool neg = r->net < 0;
        if (neg) { dl->AddRectFilledMultiColor(ImVec2(cx - 24 - w, y + 1), ImVec2(cx - 24, y + rowH - 1), C(255, 64, 118, 40), C(255, 64, 118, 235), C(255, 64, 118, 235), C(255, 64, 118, 40)); }
        else     { dl->AddRectFilledMultiColor(ImVec2(cx + 24, y + 1), ImVec2(cx + 24 + w, y + rowH - 1), C(72, 124, 255, 235), C(72, 124, 255, 40), C(72, 124, 255, 40), C(72, 124, 255, 235)); }
        const bool special = r->strike == g.callWall || r->strike == g.accel;
        std::snprintf(t, sizeof t, "%.0f", r->strike); txtC(dl, cx, y + 1, special ? kTxt : kDim, t);
        if (r->strike == g.callWall) { dl->AddRectFilled(ImVec2(p0.x + s.x - 70, y + 1), ImVec2(p0.x + s.x - 8, y + rowH - 1), C(72, 124, 255, 200), 3); txt(dl, p0.x + s.x - 66, y + 1, kBg, "CALL WALL"); }
        if (r->strike == g.accel)    { dl->AddRectFilled(ImVec2(p0.x + s.x - 70, y + 1), ImVec2(p0.x + s.x - 8, y + rowH - 1), C(255, 64, 118, 220), 3); txt(dl, p0.x + s.x - 56, y + 1, kBg, "ACCEL"); }
        y += rowH;
    }
    std::snprintf(t, sizeof t, "Net gamma %s", money(g.netGex).c_str()); txt(dl, p0.x + 10, p0.y + s.y - 34, g.netGex < 0 ? kPink : kBlue, t);
    std::snprintf(t, sizeof t, "EM \xC2\xB1%.1f", g.em); txtR(dl, p0.x + s.x - 10, p0.y + s.y - 34, kTxt, t);
    std::snprintf(t, sizeof t, "Gamma flip %.0f", g.flip); txt(dl, p0.x + 10, p0.y + s.y - 18, kDim, t);
}

// ---------- tape + grade ----------
static void tapePanel(Engine& e, ImVec2 s) {
    ImDrawList* dl = ImGui::GetWindowDrawList(); const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float tapeH = s.y * 0.58f;
    dl->AddRectFilled(p0, ImVec2(p0.x + s.x, p0.y + tapeH), kPanel, 6); panelTitle(dl, p0, "PRINT TAPE", "  option prints");
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 6, p0.y + 32));
    if (ImGui::BeginTable("tape", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(s.x - 12, tapeH - 38))) {
        for (const char* h : {"ET", "STRIKE", "READ", "G", "SIZE", "PX"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (const TapeRow& r : e.tape) {
            ImGui::TableNextRow(); const bool bull = (r.call && r.aggr == 1) || (!r.call && r.aggr == 2);
            const ImVec4 col = r.aggr == 0 ? ImVec4(.7f, .7f, .7f, 1) : bull ? ImVec4(.28f, .86f, .5f, 1) : ImVec4(1.f, .25f, .46f, 1);
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", hms(r.ts).c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(col, "%.0f%c", r.strike, r.call ? 'C' : 'P');
            ImGui::TableNextColumn(); ImGui::TextColored(r.sweep ? ImVec4(1, .25f, .46f, 1) : ImVec4(.6f, .6f, .6f, 1), "%s", r.sweep ? "SWEEP" : r.aggr == 1 ? "BOUGHT" : r.aggr == 2 ? "SOLD" : "PRINT");
            ImGui::TableNextColumn(); ImGui::TextColored(r.grade == 'A' ? ImVec4(.28f, .86f, .5f, 1) : r.grade == 'B' ? ImVec4(1, .75f, .27f, 1) : ImVec4(.5f, .5f, .5f, 1), "%c", r.grade);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.qty);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", r.px);
        }
        ImGui::EndTable();
    }
    const ImVec2 g0(p0.x, p0.y + tapeH + 8); const float gh = s.y - tapeH - 8;
    dl->AddRectFilled(g0, ImVec2(g0.x + s.x, g0.y + gh), kPanel, 6); panelTitle(dl, g0, "GRADE", "  last print");
    if (!e.tape.empty()) {
        const TapeRow& r = e.tape.front(); char t[64];
        const ImU32 gc = r.grade == 'A' ? kGreen : r.grade == 'B' ? kAmber : kDim;
        dl->AddRectFilled(ImVec2(g0.x + 12, g0.y + 36), ImVec2(g0.x + 62, g0.y + 86), C(20, 40, 28), 6); dl->AddRect(ImVec2(g0.x + 12, g0.y + 36), ImVec2(g0.x + 62, g0.y + 86), gc, 6);
        char gl[2] = {r.grade, 0}; txtC(dl, g0.x + 37, g0.y + 44, gc, gl); std::snprintf(t, sizeof t, "%d/%d", r.score, int(G_COUNT)); txtC(dl, g0.x + 37, g0.y + 66, kDim, t);
        std::snprintf(t, sizeof t, "%.0f%c %s", r.strike, r.call ? 'C' : 'P', r.sweep ? "SWEEP" : "PRINT"); txt(dl, g0.x + 74, g0.y + 38, kTxt, t);
        std::snprintf(t, sizeof t, "%d @ %.2f", r.qty, r.px); txt(dl, g0.x + 74, g0.y + 58, kDim, t);
        float y = g0.y + 96; const float lh = std::min(20.f, (gh - 104) / float(G_COUNT));
        for (int i = 0; i < G_COUNT; ++i, y += lh) {
            const bool on = r.bits & (1u << i);
            dl->AddRectFilled(ImVec2(g0.x + 12, y + 2), ImVec2(g0.x + 26, y + 16), on ? C(60, 170, 90) : C(30, 24, 32), 3);
            txt(dl, g0.x + 34, y + 1, on ? kTxt : kDim, gradeLabel(i));
        }
    }
}

// ---------- GL helpers ----------
static void uploadHeat(Engine& e, GLuint tex) {
    bool full = false; const std::vector<int> pend = e.heat.takePending(full);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (full) for (int x = 0; x < e.heat.cols(); ++x) glTexSubImage2D(GL_TEXTURE_2D, 0, x, 0, 1, e.heat.rows(), GL_RGBA, GL_UNSIGNED_BYTE, e.heat.column(x));
    for (int x : pend) glTexSubImage2D(GL_TEXTURE_2D, 0, x, 0, 1, e.heat.rows(), GL_RGBA, GL_UNSIGNED_BYTE, e.heat.column(x));
}

int main(int argc, char** argv) {
    std::string shm; Config cfg; float scale = 1.25f;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--shm") && i + 1 < argc) shm = argv[++i];
        else if (!std::strcmp(argv[i], "--primary") && i + 1 < argc) cfg.primaryFuture = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--mult") && i + 1 < argc) cfg.multiplier = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) scale = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--macro")) cfg.macroDay = true;
        else if (!std::strcmp(argv[i], "--short-calls")) cfg.dealerLongCalls = false;
    }
    Ring ring = shm.empty() ? Ring::heap(1u << 18) : Ring::openShm(shm);
    std::atomic<bool> stop{false}; std::thread feedThread; std::unique_ptr<Producer> prod; std::unique_ptr<DemoFeed> feed;
    if (shm.empty()) { prod = std::make_unique<Producer>(ring); feed = std::make_unique<DemoFeed>(*prod); feedThread = std::thread([&] { feed->run(stop); }); }

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3); glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* win = glfwCreateWindow(1700, 960, "TZU // systematic options", nullptr, nullptr);
    if (!win) return 1;
    glfwMakeContextCurrent(win); glfwSwapInterval(1);
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr; io.FontGlobalScale = scale;
    ImGui::StyleColorsDark(); ImGuiStyle& st = ImGui::GetStyle();
    st.Colors[ImGuiCol_WindowBg] = ImVec4(.03f, .024f, .035f, 1); st.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, .02f);
    st.Colors[ImGuiCol_TableHeaderBg] = ImVec4(.06f, .05f, .07f, 1); st.WindowPadding = ImVec2(8, 8);
    ImGui_ImplGlfw_InitForOpenGL(win, true); ImGui_ImplOpenGL3_Init("#version 330");

    Engine eng(cfg);
    GLuint tex = 0; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, eng.heat.cols(), eng.heat.rows(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        ring.drain([&](const Event& ev) { eng.process(ev); }, 400000);     // never starve the frame
        const uint64_t now = wallNs(); eng.tick(now); uploadHeat(eng, tex);

        int fw, fh; glfwGetFramebufferSize(win, &fw, &fh);
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(disp);
        ImGui::Begin("root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);
        const float pad = 8, th = 64; const ImVec2 o = ImGui::GetCursorScreenPos();
        tiles(eng, o, disp.x - 2 * pad, th);
        const float y0 = th + 14, H = disp.y - y0 - 30, W = disp.x - 2 * pad - 3 * pad;
        ImGui::SetCursorPos(ImVec2(pad, y0)); chart(eng, tex, ImVec2(W * 0.42f, H), now);
        ImGui::SetCursorPos(ImVec2(pad + W * 0.42f + pad, y0)); dom(eng, ImVec2(W * 0.12f, H));
        ImGui::SetCursorPos(ImVec2(pad + W * 0.54f + 2 * pad, y0)); gammaLadder(eng, ImVec2(W * 0.18f, H));
        ImGui::SetCursorPos(ImVec2(pad + W * 0.72f + 3 * pad, y0)); tapePanel(eng, ImVec2(W * 0.28f, H));
        ImGui::SetCursorPos(ImVec2(pad, disp.y - 22));
        ImGui::TextDisabled("%s   events %llu   backlog %llu   dropped %llu   %.0f fps   primary secId %d",
            shm.empty() ? "DEMO FEED" : ("LIVE " + shm).c_str(), (unsigned long long)eng.eventsSeen, (unsigned long long)ring.backlog(), (unsigned long long)ring.dropped(), double(ImGui::GetIO().Framerate), eng.cfg.primaryFuture);
        ImGui::End();

        ImGui::Render(); glViewport(0, 0, fw, fh); glClearColor(.03f, .024f, .035f, 1); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); glfwSwapBuffers(win);
    }
    stop = true; if (feedThread.joinable()) feedThread.join();
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext(); glfwDestroyWindow(win); glfwTerminate();
    return 0;
}
