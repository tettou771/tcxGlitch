#include "TrussC.h"
#include "tcApp.h"

void tcApp::setup() {
    setWindowTitle("tcxGlitch — example-basic");
    srcFbo_.allocate(640, 480);

    // Webcam source. If no camera / permission, we silently fall back to the
    // animated test pattern so the example still runs.
    if (!grabber_.listDevices().empty()) {
        grabber_.setDeviceID(0);
        useCamera_ = grabber_.setup(640, 480);
    }

    imguiSetup();
    // Lets AI agents inspect/drive the GUI over MCP (TRUSSC_MCP=1). No-op
    // during a normal run.
    mcp::registerDebuggerTools();
}

void tcApp::draw() {
    clear(0.08f, 0.08f, 0.10f);

    // 1) Render the source (camera or fallback pattern) into the FBO.
    if (useCamera_) grabber_.update();
    drawSource();

    // 2) Decide the seed: animate it, or use the frozen hand-set value.
    if (randomSeed_) seed_ = frame_;

    // 3) Glitch the FBO into out_.
    Glitch* g = currentGlitch();
    g->setSeed((uint32_t)seed_);
    lastOk_ = g->apply(srcFbo_, out_);

    // 4) Draw the glitched result, fit to the window.
    setColor(1, 1, 1);
    out_.getTexture().draw(0, 0, (float)getWindowWidth(), (float)getWindowHeight());

    // 5) GUI.
    imguiBegin();
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("tcxGlitch");

    const char* codecs[] = {"JPEG", "BMP", "PNG"};
    ImGui::Combo("Codec", &codecIndex_, codecs, 3);

    ImGui::Separator();

    if (codecIndex_ == 0) {
        ImGui::SliderInt("Quality", &jpegQuality_, 1, 100);
        ImGui::SliderFloat("Amount", &jpegAmount_, 0.0f, 1.0f, "%.2f");
    } else if (codecIndex_ == 1) {
        ImGui::SliderFloat("Amount", &bmpAmount_, 0.0f, 1.0f, "%.2f");
    } else {
        ImGui::SliderFloat("Amount", &pngAmount_, 0.0f, 1.0f, "%.2f");
    }

    ImGui::Separator();

    ImGui::Checkbox("Random seed", &randomSeed_);
    if (randomSeed_) {
        ImGui::SameLine();
        ImGui::Text("(%d)", seed_);
    } else {
        ImGui::SliderInt("Seed", &seed_, 0, 9999);
    }

    ImGui::Separator();
    ImGui::Text("source: %s", useCamera_ ? "camera" : "test pattern");
    if (!lastOk_) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "decode failed -> black");
    } else {
        ImGui::Text("ok");
    }
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

    ImGui::End();
    imguiEnd();

    frame_++;
}

void tcApp::cleanup() {
    imguiShutdown();
}

// Render whatever source we have into srcFbo_: the live camera when present,
// otherwise the animated test pattern.
void tcApp::drawSource() {
    if (useCamera_ && grabber_.isInitialized()) {
        srcFbo_.begin(0, 0, 0, 1);
        setColor(1, 1, 1);
        grabber_.getTexture().draw(0, 0, (float)srcFbo_.getWidth(),
                                         (float)srcFbo_.getHeight());
        srcFbo_.end();
    } else {
        drawTestPattern();
    }
}

// Draw an animated, detail-rich test pattern so the glitch is easy to read.
void tcApp::drawTestPattern() {
    const float w = (float)srcFbo_.getWidth();
    const float h = (float)srcFbo_.getHeight();
    const float t = frame_ * 0.02f;

    srcFbo_.begin(0.10f, 0.12f, 0.16f);

    // A smooth horizontal gradient. Gradients make JPEG block displacement and
    // BMP scanline shifts very legible.
    const int strips = 96;
    for (int i = 0; i < strips; ++i) {
        float u = i / (float)strips;
        setColor(0.5f + 0.5f * sinf(u * TAU + t),
                 0.5f + 0.5f * sinf(u * TAU + t + 2.0f),
                 0.5f + 0.5f * sinf(u * TAU + t + 4.0f));
        drawRect(u * w, 0, w / strips + 1, h);
    }

    // Hard-edged colour blocks (give the codecs sharp transitions to mangle).
    setColor(0.95f, 0.95f, 0.95f);
    drawRect(w * 0.1f, h * 0.15f, w * 0.25f, h * 0.2f);
    setColor(0.05f, 0.05f, 0.05f);
    drawRect(w * 0.6f, h * 0.6f, w * 0.3f, h * 0.25f);

    // Moving circles.
    for (int i = 0; i < 10; ++i) {
        float a = t + i * (TAU / 10.0f);
        float x = w * 0.5f + cosf(a) * w * 0.30f;
        float y = h * 0.5f + sinf(a * 1.3f) * h * 0.30f;
        float r = 18.0f + 10.0f * sinf(t * 2.0f + i);
        setColor(0.5f + 0.5f * sinf(a),
                 0.5f + 0.5f * sinf(a + 2.0f),
                 0.5f + 0.5f * sinf(a + 4.0f));
        drawCircle(x, y, r);
    }

    // A sweeping bright bar (sharp edges show codec artefacts well).
    float bx = fmodf(t * 60.0f, w);
    setColor(1.0f, 1.0f, 1.0f);
    drawRect(bx, 0, 6, h);

    srcFbo_.end();
}

Glitch* tcApp::currentGlitch() {
    switch (codecIndex_) {
        case 0:
            jpeg_.setQuality(jpegQuality_).setAmount(jpegAmount_);
            return &jpeg_;
        case 1:
            bmp_.setAmount(bmpAmount_);
            return &bmp_;
        default:
            png_.setAmount(pngAmount_);
            return &png_;
    }
}
