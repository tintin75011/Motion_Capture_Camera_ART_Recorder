// DTrack 6DOF - visualisation live + enregistrement + export/import CSV
// Compiler en C++17 minimum (std::filesystem).

#include "DTrackSDK.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static const char* CONTROLLER_IP = "192.168.1.1";
static const unsigned short LOCAL_PORT = 5000;
static const size_t HISTORY_SIZE = 10000;
static const double RAD2DEG = 57.29577951308232;

// ---------------------------------------------------------------------------
// Données live
// ---------------------------------------------------------------------------
struct BodyState {
    bool   tracked = false;
    double quality = 0.0;
    double pos[3]  = {0, 0, 0};   // mm
    double eul[3]  = {0, 0, 0};   // degrés (rx, ry, rz)
    double rot[9]  = {0};         // matrice, colonne par colonne
    std::vector<float> hist[3];   // historique x, y, z
};

// ---------------------------------------------------------------------------
// Données enregistrées / importées
// ---------------------------------------------------------------------------
struct Series {
    std::vector<double> t, quality;
    std::vector<double> ch[6];                  // x, y, z, rx, ry, rz
    std::vector<std::array<double, 9>> rot;     // matrice colonne par colonne
    size_t size() const { return t.size(); }
};

struct Dataset {
    std::map<int, Series> bodies;
    size_t samples  = 0;
    double duration = 0.0;

    void clear() { bodies.clear(); samples = 0; duration = 0.0; }

    void add(int id, double t, double q, const double* pos, const double* eul, const double* rot)
    {
        Series& s = bodies[id];
        s.t.push_back(t);
        s.quality.push_back(q);
        for (int k = 0; k < 3; k++) {
            s.ch[k].push_back(pos[k]);
            s.ch[3 + k].push_back(eul[k]);
        }
        std::array<double, 9> r;
        for (int k = 0; k < 9; k++) r[k] = rot[k];
        s.rot.push_back(r);
        samples++;
        duration = std::max(duration, t);
    }
};

using Clock = std::chrono::steady_clock;

static std::mutex                g_mutex;          // protège g_bodies, g_data, g_recStart
static std::map<int, BodyState>  g_bodies;
static Dataset                   g_data;           // enregistrement courant OU CSV importé
static Clock::time_point         g_recStart;
static std::atomic<bool>         g_running{true};
static std::atomic<bool>         g_recording{false};

// Etat purement UI (thread principal uniquement)
static bool                g_unsaved = false;
static std::string         g_status;
static bool                g_statusOk = true;
static std::map<int, int>  g_cursor;               // curseur de lecture par body

// ---------------------------------------------------------------------------
// Conversion rotation -> Euler
// Matrice stockée par colonnes : R[row][col] = rot[col*3 + row]
// Convention : R = Rz(rz) * Ry(ry) * Rx(rx)
// ---------------------------------------------------------------------------
static void rotToEuler(const double* r, double* e)
{
    auto R = [&](int row, int col) { return r[col * 3 + row]; };
    double sy = -R(2, 0);
    sy = std::fmax(-1.0, std::fmin(1.0, sy));
    e[1] = std::asin(sy) * RAD2DEG;                    // ry
    e[0] = std::atan2(R(2, 1), R(2, 2)) * RAD2DEG;     // rx
    e[2] = std::atan2(R(1, 0), R(0, 0)) * RAD2DEG;     // rz
}

// ---------------------------------------------------------------------------
// Thread de tracking
// ---------------------------------------------------------------------------
static void trackingThread()
{
    DTrackSDK* dt = new DTrackSDK(CONTROLLER_IP, LOCAL_PORT);

    if (!dt->isCommandInterfaceValid()) {
        std::cerr << "Connexion au contrôleur impossible\n";
        delete dt;
        g_running = false;
        return;
    }
    if (!dt->startMeasurement()) {
        std::cerr << "Démarrage du tracking impossible\n";
        delete dt;
        g_running = false;
        return;
    }

    while (g_running) {
        if (!dt->receive()) continue;   // timeout ou erreur : on réessaie

        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& kv : g_bodies) kv.second.tracked = false;

        const bool rec = g_recording;
        const double t = std::chrono::duration<double>(Clock::now() - g_recStart).count();

        for (int i = 0; i < dt->getNumBody(); i++) {
            const DTrackBody* body = dt->getBody(i);
            if (!body) continue;

            BodyState& s = g_bodies[body->id];   // crée l'entrée si nouvelle
            s.tracked = body->isTracked();
            s.quality = body->quality;
            if (!s.tracked) continue;

            for (int k = 0; k < 3; k++) s.pos[k] = body->loc[k];
            for (int k = 0; k < 9; k++) s.rot[k] = body->rot[k];
            rotToEuler(s.rot, s.eul);

            for (int k = 0; k < 3; k++) {
                s.hist[k].push_back((float)s.pos[k]);
                if (s.hist[k].size() > HISTORY_SIZE)
                    s.hist[k].erase(s.hist[k].begin());
            }

            // Enregistrement : un échantillon par body détecté et par paquet reçu
            if (rec)
                g_data.add(body->id, t, s.quality, s.pos, s.eul, s.rot);
        }
    }

    dt->stopMeasurement();
    delete dt;
}

// ---------------------------------------------------------------------------
// Contrôle de l'enregistrement
// ---------------------------------------------------------------------------
static void startRecording()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_data.clear();
    g_cursor.clear();
    g_recStart = Clock::now();
    g_recording = true;
    g_unsaved = false;
}

static void stopRecording()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_recording = false;
    g_unsaved = g_data.samples > 0;
}

struct DataInfo { size_t samples; double duration; };
static DataInfo getDataInfo()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return { g_data.samples, g_data.duration };
}

// ---------------------------------------------------------------------------
// CSV : export
// Colonnes : t_s, body_id, quality, x_mm, y_mm, z_mm, rx_deg, ry_deg, rz_deg, r0..r8
// Les lignes sont écrites dans l'ordre chronologique (tous bodies confondus).
// ---------------------------------------------------------------------------
static bool saveCsv(const std::string& path, char d, std::string& msg)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_data.samples == 0) { msg = "Aucune donnée à enregistrer"; return false; }

    std::ofstream f(path);
    if (!f) { msg = "Impossible d'écrire dans " + path; return false; }

    const char* cols[] = {"t_s", "body_id", "quality", "x_mm", "y_mm", "z_mm",
                          "rx_deg", "ry_deg", "rz_deg",
                          "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8"};
    for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++)
        f << (i ? std::string(1, d) : std::string()) << cols[i];
    f << "\n";

    std::vector<std::pair<int, const Series*>> list;
    for (const auto& kv : g_data.bodies) list.push_back({kv.first, &kv.second});
    std::vector<size_t> idx(list.size(), 0);

    f << std::fixed;
    while (true) {
        int best = -1;
        double bt = 1e300;
        for (size_t i = 0; i < list.size(); i++) {
            const Series& s = *list[i].second;
            if (idx[i] < s.size() && s.t[idx[i]] < bt) { bt = s.t[idx[i]]; best = (int)i; }
        }
        if (best < 0) break;

        const Series& s = *list[best].second;
        const size_t n = idx[best]++;
        f << std::setprecision(6) << s.t[n] << d << list[best].first << d
          << std::setprecision(4) << s.quality[n];
        for (int k = 0; k < 6; k++) f << d << std::setprecision(4) << s.ch[k][n];
        for (int k = 0; k < 9; k++) f << d << std::setprecision(6) << s.rot[n][k];
        f << "\n";
    }

    if (!f) { msg = "Erreur pendant l'écriture de " + path; return false; }
    msg = "Enregistré : " + path + " (" + std::to_string(g_data.samples) + " lignes)";
    return true;
}

// ---------------------------------------------------------------------------
// CSV : import (accepte ',' ou ';' comme séparateur, et la virgule décimale avec ';')
// ---------------------------------------------------------------------------
static std::vector<std::string> splitLine(const std::string& line, char d)
{
    std::vector<std::string> out;
    std::string cur;
    std::stringstream ss(line);
    while (std::getline(ss, cur, d)) out.push_back(cur);
    return out;
}

static bool parseD(std::string s, char d, double& v)
{
    if (d == ';') std::replace(s.begin(), s.end(), ',', '.');
    const char* b = s.c_str();
    char* e = nullptr;
    v = std::strtod(b, &e);
    return e != b;
}

static bool loadCsv(const std::string& path, std::string& msg)
{
    if (g_recording) { msg = "Arrête l'enregistrement avant d'importer"; return false; }

    std::ifstream f(path);
    if (!f) { msg = "Impossible d'ouvrir " + path; return false; }

    std::string line;
    if (!std::getline(f, line)) { msg = "Fichier vide"; return false; }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const char d = std::count(line.begin(), line.end(), ';') >
                   std::count(line.begin(), line.end(), ',') ? ';' : ',';

    Dataset tmp;
    size_t bad = 0;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        auto c = splitLine(line, d);
        if (c.size() < 9) { bad++; continue; }

        double v[9];
        bool ok = true;
        for (int k = 0; k < 9; k++) ok = parseD(c[k], d, v[k]) && ok;
        if (!ok) { bad++; continue; }

        double rot[9] = {0};
        if (c.size() >= 18)
            for (int k = 0; k < 9; k++) parseD(c[9 + k], d, rot[k]);

        // v: t, id, quality, x, y, z, rx, ry, rz
        tmp.add((int)v[1], v[0], v[2], v + 3, v + 6, rot);
    }

    if (tmp.samples == 0) { msg = "Aucune ligne valide dans " + path; return false; }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_data = std::move(tmp);
        g_cursor.clear();
        msg = "Importé : " + path + " (" + std::to_string(g_data.samples) + " lignes, " +
              std::to_string(g_data.bodies.size()) + " body)";
    }
    if (bad) msg += " - " + std::to_string(bad) + " ligne(s) ignorée(s)";
    return true;
}

// ---------------------------------------------------------------------------
// UI : onglet live (inchangé)
// ---------------------------------------------------------------------------
static void drawBodyTab(const BodyState& s)
{
    if (!s.tracked)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Non détecté");
    else
        ImGui::TextColored(ImVec4(0.3f, 1, 0.4f, 1), "Détecté (qualité %.2f)", s.quality);

    ImGui::SeparatorText("Position (mm)");
    ImGui::Text("X : %10.2f", s.pos[0]);
    ImGui::Text("Y : %10.2f", s.pos[1]);
    ImGui::Text("Z : %10.2f", s.pos[2]);

    ImGui::SeparatorText("Orientation (degrés)");
    ImGui::Text("RX : %8.2f", s.eul[0]);
    ImGui::Text("RY : %8.2f", s.eul[1]);
    ImGui::Text("RZ : %8.2f", s.eul[2]);

    ImGui::SeparatorText("Matrice de rotation");
    for (int row = 0; row < 3; row++)
        ImGui::Text("%8.4f %8.4f %8.4f",
                    s.rot[0 * 3 + row], s.rot[1 * 3 + row], s.rot[2 * 3 + row]);

    ImGui::SeparatorText("Historique de position");
    const char* names[3] = {"X", "Y", "Z"};
    for (int k = 0; k < 3; k++) {
        if (s.hist[k].empty()) continue;
        ImGui::PlotLines(names[k], s.hist[k].data(), (int)s.hist[k].size(),
                         0, nullptr, FLT_MAX, FLT_MAX, ImVec2(0, 60));
    }
}

// ---------------------------------------------------------------------------
// UI : visualisation d'un body enregistré / importé
// ---------------------------------------------------------------------------
static float seriesGetter(void* data, int i)
{
    return (float)(*static_cast<const std::vector<double>*>(data))[i];
}

static void drawRecordedBody(const Series& s, int& cursor)
{
    const int n = (int)s.size();
    if (n == 0) { ImGui::TextDisabled("Aucune donnée pour ce body"); return; }
    cursor = std::max(0, std::min(cursor, n - 1));

    ImGui::SliderInt("Échantillon", &cursor, 0, n - 1);
    ImGui::Text("t = %.3f s   qualité = %.2f", s.t[cursor], s.quality[cursor]);

    ImGui::SeparatorText("Valeurs à l'échantillon sélectionné");
    ImGui::Text("Position (mm)   : X %9.2f  Y %9.2f  Z %9.2f",
                s.ch[0][cursor], s.ch[1][cursor], s.ch[2][cursor]);
    ImGui::Text("Orientation (°) : RX %8.2f  RY %8.2f  RZ %8.2f",
                s.ch[3][cursor], s.ch[4][cursor], s.ch[5][cursor]);
    const auto& r = s.rot[cursor];
    for (int row = 0; row < 3; row++)
        ImGui::Text("%8.4f %8.4f %8.4f", r[0 * 3 + row], r[1 * 3 + row], r[2 * 3 + row]);

    ImGui::SeparatorText("Courbes");
    const char* names[6] = {"X (mm)", "Y (mm)", "Z (mm)", "RX (deg)", "RY (deg)", "RZ (deg)"};
    const float plotW = std::max(100.0f, ImGui::GetContentRegionAvail().x - 80.0f);
    const float plotH = 70.0f;
    const float pad   = ImGui::GetStyle().FramePadding.x;

    for (int k = 0; k < 6; k++) {
        char overlay[64];
        std::snprintf(overlay, sizeof overlay, "%.2f", s.ch[k][cursor]);

        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::PlotLines(names[k], seriesGetter,
                         const_cast<std::vector<double>*>(&s.ch[k]), n,
                         0, overlay, FLT_MAX, FLT_MAX, ImVec2(plotW, plotH));

        // Trait vertical = position du curseur
        float frac = n > 1 ? (float)cursor / (float)(n - 1) : 0.0f;
        float x = p0.x + pad + frac * (plotW - 2 * pad);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + plotH),
                                            IM_COL32(255, 200, 0, 200));
    }
    ImGui::TextDisabled("Axe horizontal : 0 -> %.2f s", s.t.back());
}

// ---------------------------------------------------------------------------
// UI : onglet "Données enregistrées"
// ---------------------------------------------------------------------------
static std::vector<std::string> g_csvFiles;
static void refreshCsvFiles()
{
    g_csvFiles.clear();
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(".", ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".csv")
            g_csvFiles.push_back(e.path().filename().string());
    }
    std::sort(g_csvFiles.begin(), g_csvFiles.end());
}

static void drawDataTab()
{
    static char path[260] = "enregistrement.csv";
    static bool semicolon = false;
    static bool firstTime = true;
    if (firstTime) { refreshCsvFiles(); firstTime = false; }

    const bool rec = g_recording;
    const DataInfo info = getDataInfo();

    ImGui::SeparatorText("Fichier CSV");
    ImGui::InputText("Chemin", path, sizeof path);
    ImGui::Checkbox("Séparateur ';' (Excel en français)", &semicolon);

    ImGui::BeginDisabled(rec || info.samples == 0);
    if (ImGui::Button("Enregistrer en CSV")) {
        g_statusOk = saveCsv(path, semicolon ? ';' : ',', g_status);
        if (g_statusOk) { g_unsaved = false; refreshCsvFiles(); }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(rec);
    if (ImGui::Button("Importer CSV")) {
        g_statusOk = loadCsv(path, g_status);
        if (g_statusOk) g_unsaved = false;
    }
    ImGui::EndDisabled();

    if (!g_status.empty())
        ImGui::TextColored(g_statusOk ? ImVec4(0.3f, 1, 0.4f, 1) : ImVec4(1, 0.4f, 0.3f, 1),
                           "%s", g_status.c_str());

    if (ImGui::CollapsingHeader("Fichiers .csv du dossier courant")) {
        if (ImGui::Button("Rafraîchir")) refreshCsvFiles();
        if (g_csvFiles.empty()) ImGui::TextDisabled("(aucun)");
        for (const auto& name : g_csvFiles) {
            if (ImGui::Selectable(name.c_str(), false))
                std::snprintf(path, sizeof path, "%s", name.c_str());
        }
    }

    ImGui::SeparatorText("Visualisation");
    if (g_unsaved)
        ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1),
                           "Données non sauvegardées (un nouvel enregistrement les écrasera)");

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_data.samples == 0) {
        ImGui::TextDisabled("Aucune donnée : lance un enregistrement ou importe un CSV.");
        return;
    }
    ImGui::Text("%zu échantillons, %.2f s, %zu body", g_data.samples, g_data.duration,
                g_data.bodies.size());

    if (ImGui::BeginTabBar("rec_bodies")) {
        for (const auto& kv : g_data.bodies) {
            std::string label = "Body " + std::to_string(kv.first);
            if (ImGui::BeginTabItem(label.c_str())) {
                ImGui::PushID(kv.first);
                drawRecordedBody(kv.second, g_cursor[kv.first]);
                ImGui::PopID();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

// ---------------------------------------------------------------------------
// UI : barre Play / Stop (toujours visible)
// ---------------------------------------------------------------------------
static void drawToolbar()
{
    const bool rec = g_recording;
    const DataInfo info = getDataInfo();

    ImGui::BeginDisabled(rec);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.5f, 0.2f, 1));
    if (ImGui::Button("Play (enregistrer)")) startRecording();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!rec);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.15f, 0.15f, 1));
    if (ImGui::Button("Stop")) stopRecording();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (rec) {
        bool on = ((int)(ImGui::GetTime() * 2.0)) % 2 == 0;
        ImGui::TextColored(on ? ImVec4(1, 0.2f, 0.2f, 1) : ImVec4(0.6f, 0.1f, 0.1f, 1), "REC");
        ImGui::SameLine();
        ImGui::Text("%.1f s  (%zu échantillons)", info.duration, info.samples);
    } else if (info.samples > 0) {
        ImGui::Text("Arrêté : %zu échantillons, %.2f s", info.samples, info.duration);
    } else {
        ImGui::TextDisabled("Prêt");
    }
}

int main()
{
    if (!glfwInit()) return 1;
    GLFWwindow* window = glfwCreateWindow(640, 860, "DTrack - 6DOF", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    std::thread worker(trackingThread);

    while (!glfwWindowShouldClose(window) && g_running) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("6DOF", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        drawToolbar();
        ImGui::Spacing();

        if (ImGui::BeginTabBar("mode")) {
            if (ImGui::BeginTabItem("Live")) {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_bodies.empty()) {
                    ImGui::Text("En attente de données de %s ...", CONTROLLER_IP);
                } else if (ImGui::BeginTabBar("bodies")) {
                    for (const auto& kv : g_bodies) {
                        std::string label = "Body " + std::to_string(kv.first);
                        if (ImGui::BeginTabItem(label.c_str())) {
                            drawBodyTab(kv.second);
                            ImGui::EndTabItem();
                        }
                    }
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Données enregistrées")) {
                drawDataTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
        ImGui::Render();

        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    g_running = false;
    worker.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}