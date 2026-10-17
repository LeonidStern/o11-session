/*
    Jamsa - editor.cpp
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

#include "editor.h"

#include <math.h>
#include <string.h>
#include <algorithm>
#include <chrono>
#include <vector>

#include "processor.h"
#include "uitext.h"
#include "version.h"

namespace jamsa {

using namespace ui;

enum { kRemoteVolume, kRemotePan, kRemoteMute, kRemoteSolo };

// the layout, at the default size (logical pixels)
static const float PAD = 3, GAP = 3;            // around the window, between the rows and panes
static const float HEADER_H = 24, TRANSPORT_H = 26, HOSTBAR_H = 24;
static const float CHAT_W = 250;                // the chat pane; the mixer takes the rest
static const float ITEM_H = 18;                 // value boxes and buttons in the strips
static const float ROW_H = 22, ROW_GAP = 2;     // the mixer's rows
static const float COLS_H = 12, FOOTER_H = 12;  // the mixer's column titles, its footer line

static double now_seconds()
{
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static juce::String str(const std::string &s) { return juce::String::fromUTF8(s.c_str()); }

static void setLabel(juce::Label *label, const std::string &text, bool tooltip = false)
{
  if (!label) return;
  const juce::String t = str(text);
  if (label->getText() == t) return;
  label->setText(t, juce::dontSendNotification);
  if (tooltip) label->setTooltip(t); // the whole text, when the label cuts it off
}

static void setTooltip(juce::SettableTooltipClient *v, const std::string &text)
{
  if (!v) return;
  const juce::String t = str(text);
  if (v->getTooltip() != t) v->setTooltip(t);
}

static void setLabelColor(juce::Label *label, juce::Colour c)
{
  if (label && label->findColour(juce::Label::textColourId) != c) label->setColour(juce::Label::textColourId, c);
}

static void setControlValue(DragControl *c, float v)
{
  if (!c || c->isEditing()) return;
  if (fabsf(c->value() - v) > 0.0001f) c->setValue(v);
}

static void showView(juce::Component *v, bool on)
{
  if (v && v->isVisible() != on) v->setVisible(on);
}

static int px(float v) { return (int)lround(v); }

static void place(juce::Component *v, float x0, float y0, float x1, float y1)
{
  if (!v) return;
  v->setBounds(px(x0), px(y0), px(x1) - px(x0), px(y1) - px(y0));
}

static std::string codecFromFourcc(const std::string &f)
{
  if (f == "FLAC") return "FLAC";
  if (f == "OPUr" || f == "OPUS") return "Opus";
  if (f == "OGGv") return "Vorbis";
  return f;
}

// a container of views made with new, which it deletes with itself
class Pane : public Strip
{
public:
  explicit Pane(juce::Colour color, float radius = 0) : Strip(color, radius) {}
  ~Pane() override { deleteAllChildren(); }
};

double JamsaEditor::zoomFor(double width, double height)
{
  return std::max(1.0, std::min(width / DEFAULT_WIDTH, height / DEFAULT_HEIGHT));
}

JamsaEditor::JamsaEditor(JamsaProcessor &plugin)
  : juce::AudioProcessorEditor(plugin), m_plugin(plugin), m_lookAndFeel(m_fonts), m_tooltips(this, 500)
{
  setLookAndFeel(&m_lookAndFeel);
  setOpaque(true);
  buildViews();

  const Settings s = engine().getSettings();
  int w = DEFAULT_WIDTH, h = DEFAULT_HEIGHT;
  if (s.editorWidth > 0 && s.editorHeight > 0)
  {
    w = std::min<int>(MAX_WIDTH, std::max<int>(MIN_WIDTH, s.editorWidth));
    h = std::min<int>(MAX_HEIGHT, std::max<int>(MIN_HEIGHT, s.editorHeight));
  }
  setResizable(true, true);
  setResizeLimits(MIN_WIDTH, MIN_HEIGHT, MAX_WIDTH, MAX_HEIGHT);
  setSize(w, h);

  syncSettings();
  update();
  startTimerHz(30);
}

JamsaEditor::~JamsaEditor()
{
  stopTimer();
  m_license.reset();
  m_rows.clear();
  m_root.reset();
  setLookAndFeel(nullptr);
}

Engine &JamsaEditor::engine()
{
  return m_plugin.engine();
}

void JamsaEditor::paint(juce::Graphics &g)
{
  g.fillAll(Colors::window());
}

// The views are laid out in logical pixels (720 x 330 at the default size); the root's
// transform scales them to the window.
void JamsaEditor::resized()
{
  const int w = std::max(1, getWidth()), h = std::max(1, getHeight());
  engine().setEditorSize(w, h);
  m_zoom = zoomFor(w, h);
  m_root->setTransform(juce::AffineTransform::scale((float)m_zoom));
  m_root->setBounds(0, 0, (int)ceil(w / m_zoom), (int)ceil(h / m_zoom));
  layout();
}

// ---------------------------------------------------------------------------
// views
// ---------------------------------------------------------------------------

static juce::Label *makeValueLabel(const juce::Font &font)
{
  // text in a value box that can't be edited ("3 / 16")
  juce::Label *l = makeLabel("", font, Colors::value(), juce::Justification::centred);
  l->setColour(juce::Label::backgroundColourId, Colors::box());
  l->setColour(juce::Label::outlineColourId, Colors::boxLine());
  return l;
}

void JamsaEditor::buildViews()
{
  const Fonts &f = m_fonts;
  m_root.reset(new Pane(Colors::window()));
  addAndMakeVisible(*m_root);
  auto add = [](juce::Component *parent, auto *child) {
    parent->addAndMakeVisible(child);
    return child;
  };

  // header: Jamsa  Name [..]  Server [..]  Connect          (o) Connected · Opus
  m_header = add(m_root.get(), new Pane(Colors::strip(), 2));
  m_title = add(m_header, makeLabel(JAMSA_NAME, f.bold, juce::Colour(0xffe8e8e8)));
  m_nameLabel = add(m_header, makeLabel("Name", f.label, Colors::label()));
  m_nameEdit = add(m_header, makeEdit(f.value, "Your name"));
  m_nameEdit->onTextChange = [this]() { engine().setUserName(m_nameEdit->getText().toStdString()); };
  m_serverLabel = add(m_header, makeLabel("Server", f.label, Colors::label()));
  m_serverEdit = add(m_header, makeEdit(f.value, "host:port"));
  m_serverEdit->onTextChange = [this]() { engine().setServerText(m_serverEdit->getText().toStdString()); };
  m_serverEdit->onReturnKey = [this]() {
    Snapshot s;
    engine().getSnapshot(s);
    if (s.state != ConnState::Connected && s.state != ConnState::Connecting) connectOrDisconnect();
  };
  m_connect = add(m_header, new FlatButton("Connect", FlatButton::Push, f.button));
  m_connect->onChange = [this]() { connectOrDisconnect(); };
  m_connLed = add(m_header, new Led);
  m_connText = add(m_header, makeLabel("", f.value, Colors::status(), juce::Justification::centredRight));
  m_connText->setInterceptsMouseClicks(true, false); // for its tooltip

  // transport: BPM [120.00]  BPI [16]  | Sync (o) Live 120.00 | Click (knob) | beats  [3 / 16]
  m_transport = add(m_root.get(), new Pane(Colors::strip(), 2));
  m_bpmLabel = add(m_transport, makeLabel("BPM", f.label, Colors::label()));
  m_bpm = add(m_transport, new DragNumberBox(f.value));
  m_bpm->dragMap = [](int start, double dy, bool fine) { return uitext::dragBpm(start, dy, fine); };
  m_bpm->format = [](int v) { return uitext::bpmBoxText(v); };
  m_bpm->onDragged = [this](int bpm) {
    Snapshot s;
    engine().getSnapshot(s);
    if (s.state == ConnState::Connected && bpm != (int)lround(s.bpm)) engine().vote("bpm", bpm);
  };
  m_bpm->onTyped = [this](const std::string &text) {
    int bpm = 0;
    if (!uitext::parseBpm(text, &bpm)) return;
    Snapshot s;
    engine().getSnapshot(s);
    if (s.state == ConnState::Connected && bpm != (int)lround(s.bpm)) engine().vote("bpm", bpm);
  };
  m_bpm->setTooltip("The room's tempo: drag up or down, or double-click and type, to vote for another");
  m_bpiLabel = add(m_transport, makeLabel("BPI", f.label, Colors::label()));
  m_bpi = add(m_transport, new MenuBox(f.value));
  for (int b : uitext::BPI_CHOICES) m_bpi->addEntry(std::to_string(b));
  m_bpi->onSelect = [this](int i) {
    Snapshot s;
    engine().getSnapshot(s);
    if (i >= 0 && i < uitext::NUM_BPI_CHOICES && s.state == ConnState::Connected && uitext::BPI_CHOICES[i] != s.bpi)
      engine().vote("bpi", uitext::BPI_CHOICES[i]);
  };
  m_bpi->setTooltip("Beats per interval: pick one to vote for it");
  for (auto &sep : m_sep) sep = add(m_transport, new Strip(Colors::separator()));
  m_sync = add(m_transport, new FlatButton("Sync", FlatButton::Toggle, f.button));
  m_sync->onChange = [this]() { engine().setFollowHostTempo(m_sync->value()); };
  m_syncLed = add(m_transport, new Led);
  m_syncText = add(m_transport, makeLabel("", f.value, Colors::status()));
  m_syncText->setInterceptsMouseClicks(true, false);
  m_click = add(m_transport, new FlatButton("Click", FlatButton::Toggle, f.button));
  m_click->setTooltip("The metronome: the first beat of each interval higher");
  m_click->onChange = [this]() { engine().setMetronome(sliderToGain(m_clickVolume->value()), !m_click->value()); };
  m_clickVolume = add(m_transport, new ArcKnob(false));
  m_clickVolume->setDefaultValue(gainToSlider(0.5f));
  m_clickVolume->setTooltip("The click's volume (double-click for the default)");
  m_clickVolume->onChange = [this]() { engine().setMetronome(sliderToGain(m_clickVolume->value()), !m_click->value()); };
  m_beats = add(m_transport, new BeatCells);
  m_beatBox = add(m_transport, makeValueLabel(f.value));

  // mixer: the column titles, your channel, the others, a line at the bottom
  m_mixer = add(m_root.get(), new Pane(Colors::pane(), 2));
  m_colChannel = add(m_mixer, makeLabel("CHANNEL", f.small, Colors::label()));
  m_colVolume = add(m_mixer, makeLabel("VOLUME", f.small, Colors::label()));
  m_colPan = add(m_mixer, makeLabel("PAN", f.small, Colors::label(), juce::Justification::centred));
  m_colLevel = add(m_mixer, makeLabel("LEVEL", f.small, Colors::label()));

  m_myRow = add(m_mixer, new Pane(Colors::myRow(), 1));
  m_tx = add(m_myRow, new FlatButton("TX", FlatButton::Toggle, f.tiny));
  m_tx->setOnColors(Colors::orange(), Colors::orangeLine());
  m_tx->setTooltip("Transmit: send your input to the room");
  m_tx->onChange = [this]() { engine().setTransmit(m_tx->value()); };
  m_in = add(m_myRow, new FlatButton("IN", FlatButton::Toggle, f.tiny));
  m_in->setTooltip("Monitor: hear your input in the plug-in's output");
  m_in->onChange = [this]() { engine().setMonitor(m_in->value()); };
  m_myName = add(m_myRow, makeLabel("", f.value, Colors::value()));
  m_myName->setInterceptsMouseClicks(true, false);
  m_inputVolume = add(m_myRow, new FaderBox(f.small));
  m_inputVolume->format = [](float v) { return uitext::dbText(sliderToGain(v)); };
  m_inputVolume->setDefaultValue(gainToSlider(1.0f));
  m_inputVolume->setTooltip("The volume of what you send (double-click for 0 dB)");
  m_inputVolume->onChange = [this]() { engine().setInputVolume(sliderToGain(m_inputVolume->value())); };
  m_codec = add(m_myRow, new MenuBox(f.small));
  m_codec->addEntry("Opus"); // the order of the choices
  m_codec->addEntry("FLAC");
  m_codec->onSelect = [this](int i) {
    Snapshot s;
    engine().getSnapshot(s);
    const char *codec = i == 1 ? "FLAC" : "Opus";
    if (i >= 0 && s.state == ConnState::Connected && s.roomCodec != codec) engine().setRoomCodec(i == 1 ? "flac" : "opus");
  };
  m_bitrate = add(m_myRow, new MenuBox(f.small));
  for (int k : Settings::OPUS_KBPS) m_bitrate->addEntry(uitext::kbpsText(k));
  m_bitrate->onSelect = [this](int i) {
    if (i >= 0 && i < Settings::NUM_OPUS_KBPS) engine().setOpusKbps(Settings::OPUS_KBPS[i]);
  };
  m_myMeter = add(m_myRow, new SegmentMeter);

  m_players = add(m_mixer, new juce::Viewport);
  m_players->setScrollBarsShown(true, false, true, false);
  m_players->setScrollBarThickness(5);
  m_playersContent = new Pane(juce::Colours::transparentBlack);
  m_players->setViewedComponent(m_playersContent, true);
  m_noPlayers = add(m_mixer, makeLabel("", f.label, Colors::label(), juce::Justification::centred));
  m_footer = add(m_mixer, makeLabel("", f.label, Colors::label()));
  m_footer->setInterceptsMouseClicks(true, false);

  // chat
  m_chatPane = add(m_root.get(), new Pane(Colors::pane(), 2));
  m_chat = add(m_chatPane, new TextLinesView(f.chat, f.chatBold));
  m_chatEdit = add(m_chatPane, makeEdit(f.chat, juce::String::fromUTF8("Chat, !vote bpm 120, /me \xe2\x80\xa6")));
  m_chatEdit->onReturnKey = [this]() { sendChatLine(); };

  // host bar: Host  3 in room  LAN [..][Copy]                          (o) Forward port 2049
  m_hostBar = add(m_root.get(), new Pane(Colors::strip(), 2));
  m_host = add(m_hostBar, new FlatButton("Host", FlatButton::Toggle, f.button));
  m_host->setTooltip("Host a NINJAM server in this plug-in, and join it");
  m_host->onChange = [this]() {
    m_hostPending = true;
    m_hostPendingSince = now_seconds();
    engine().setHosting(m_host->value());
  };
  m_roomCount = add(m_hostBar, makeLabel("", f.value, Colors::status()));
  m_hostHint = add(m_hostBar, makeLabel("", f.label, Colors::label()));
  m_hostHint->setInterceptsMouseClicks(true, false);
  m_lan = add(m_hostBar, new AddressBox(f, "LAN")); // WAN once the public address is known (uitext::hostBar)
  m_lan->onCopy = [this]() { copyAddress(m_lan); };
  m_portLed = add(m_hostBar, new Led);
  m_portText = add(m_hostBar, makeLabel("", f.value, Colors::status(), juce::Justification::centredRight));
  m_portText->setInterceptsMouseClicks(true, false);
}

// the mixer's columns in a row of width w: CHANNEL, VOLUME (the rest), PAN, M, S, LEVEL
JamsaEditor::Columns JamsaEditor::columns(float w)
{
  Columns c;
  const float g = 5;
  c.name0 = 4;
  c.name1 = 100;
  c.level1 = w - 4;
  c.level0 = c.level1 - 54;
  c.solo1 = c.level0 - g;
  c.solo0 = c.solo1 - 18;
  c.mute1 = c.solo0 - g;
  c.mute0 = c.mute1 - 18;
  c.pan1 = c.mute0 - g;
  c.pan0 = c.pan1 - 26;
  c.vol1 = c.pan0 - g;
  c.vol0 = c.name1 + g;
  if (c.vol1 - c.vol0 < 50)
  {
    // a very narrow mixer: the names give way
    c.vol0 = c.vol1 - 50;
    c.name1 = std::max(c.name0 + 20, c.vol0 - g);
  }
  return c;
}

void JamsaEditor::layout()
{
  if (!m_root) return;
  const float rw = (float)m_root->getWidth(), rh = (float)m_root->getHeight();
  const float W = std::max<float>(rw, MIN_WIDTH), H = std::max<float>(rh, MIN_HEIGHT);

  const float transportY = PAD + HEADER_H + GAP, bodyY = transportY + TRANSPORT_H + GAP;
  const float hostY = H - PAD - HOSTBAR_H, bodyBottom = hostY - GAP;
  place(m_header, PAD, PAD, W - PAD, PAD + HEADER_H);
  place(m_transport, PAD, transportY, W - PAD, transportY + TRANSPORT_H);
  place(m_mixer, PAD, bodyY, W - PAD - CHAT_W - GAP, bodyBottom);
  place(m_chatPane, W - PAD - CHAT_W, bodyY, W - PAD, bodyBottom);
  place(m_hostBar, PAD, hostY, W - PAD, hostY + HOSTBAR_H);

  layoutHeader(W - 2 * PAD);
  layoutTransport(W - 2 * PAD);
  layoutMixer(m_mixer->getBounds().toFloat());
  {
    const float cw = CHAT_W, ch = bodyBottom - bodyY;
    place(m_chat, PAD, PAD, cw - PAD, ch - PAD - ITEM_H - 2);
    place(m_chatEdit, PAD, ch - PAD - ITEM_H, cw - PAD, ch - PAD);
  }
  layoutHostBar(W - 2 * PAD);
  layoutLicense();
  m_root->repaint();
}

void JamsaEditor::layoutHeader(float w)
{
  const float y0 = (HEADER_H - ITEM_H) / 2, y1 = y0 + ITEM_H;
  float x = 4;
  auto label = [&](juce::Label *l, float padLeft) {
    const float tw = textWidth(l->getFont(), l->getText().toStdString());
    place(l, x + padLeft, 0, x + padLeft + tw + 1, HEADER_H);
    x += padLeft + tw + 1 + 4;
  };
  label(m_title, 4);
  x += 4;
  label(m_nameLabel, 4);
  place(m_nameEdit, x, y0, x + 70, y1);
  x += 70 + 4;
  label(m_serverLabel, 4);
  const float connectW = std::max(textWidth(m_fonts.button, "Disconnect"), textWidth(m_fonts.button, "Connect")) + 12;
  // the connection's state on the right: as much of it as there is room for
  const float stateW = std::min<float>(200, textWidth(m_fonts.value, m_connText->getText().toStdString()) + 2);
  const float serverW = std::max<float>(90, std::min<float>(150, w - 4 - (x + 4 + connectW + 12 + 11 + std::max<float>(60, stateW))));
  place(m_serverEdit, x, y0, x + serverW, y1);
  x += serverW + 4;
  place(m_connect, x, y0, x + connectW, y1);
  x += connectW + 12;
  const float textLeft = std::max(x + 11, w - 4 - stateW);
  place(m_connText, textLeft, 0, w - 4, HEADER_H);
  place(m_connLed, textLeft - 11, 0, textLeft - 2, HEADER_H);
}

void JamsaEditor::layoutTransport(float w)
{
  const float y0 = (TRANSPORT_H - ITEM_H) / 2, y1 = y0 + ITEM_H;
  float x = 4;
  auto label = [&](juce::Label *l) {
    const float tw = textWidth(l->getFont(), l->getText().toStdString());
    place(l, x + 4, 0, x + 4 + tw + 1, TRANSPORT_H);
    x += 4 + tw + 1 + 4;
  };
  auto sep = [&](Strip *s) {
    place(s, x + 3, (TRANSPORT_H - 16) / 2, x + 4, (TRANSPORT_H - 16) / 2 + 16);
    x += 7 + 4;
  };
  label(m_bpmLabel);
  const float bpmW = textWidth(m_fonts.value, "888.88") + 10;
  place(m_bpm, x, y0, x + bpmW, y1);
  x += bpmW + 4;
  label(m_bpiLabel);
  const float bpiW = textWidth(m_fonts.value, "64") + 22;
  place(m_bpi, x, y0, x + bpiW, y1);
  x += bpiW + 4;
  sep(m_sep[0]);
  const float syncW = m_sync->preferredWidth();
  place(m_sync, x, y0, x + syncW, y1);
  x += syncW + 4;
  place(m_syncLed, x, 0, x + 7, TRANSPORT_H);
  x += 7 + 4;
  // room for the host's name and a tempo, so the row doesn't move when they change
  const float syncTextW = textWidth(m_fonts.value, "Host 888.88") + 2;
  place(m_syncText, x, 0, x + syncTextW, TRANSPORT_H);
  x += syncTextW + 4;
  sep(m_sep[1]);
  const float clickW = m_click->preferredWidth();
  place(m_click, x, y0, x + clickW, y1);
  x += clickW + 4;
  place(m_clickVolume, x, (TRANSPORT_H - 18) / 2, x + 18, (TRANSPORT_H - 18) / 2 + 18);
  x += 18 + 4;
  sep(m_sep[2]);
  const float beatBoxW = textWidth(m_fonts.value, "88 / 88") + 10;
  place(m_beatBox, w - 4 - beatBoxW, y0, w - 4, y1);
  place(m_beats, x, 0, std::max(x + 16, w - 4 - beatBoxW - 4), TRANSPORT_H);
}

void JamsaEditor::layoutRow(Row &row, float rw)
{
  const Columns c = columns(rw);
  place(row.name, c.name0, 0, c.name1, ROW_H);
  place(row.volume, c.vol0, 4, c.vol1, ROW_H - 4);
  const float pc = floorf((c.pan0 + c.pan1) / 2);
  place(row.pan, pc - 9, 2, pc + 9, 20);
  place(row.mute, c.mute0, 2, c.mute1, 20);
  place(row.solo, c.solo0, 2, c.solo1, 20);
  place(row.meter, c.level0, 0, c.level1, ROW_H);
}

void JamsaEditor::layoutMixer(juce::Rectangle<float> pane)
{
  const float pw = pane.getWidth(), ph = pane.getHeight();
  const float rw = pw - 2 * PAD; // a row's width
  const Columns c = columns(rw);
  const float x0 = PAD;
  place(m_colChannel, x0 + c.name0, PAD, x0 + c.name1, PAD + COLS_H);
  place(m_colVolume, x0 + c.vol0, PAD, x0 + c.vol1, PAD + COLS_H);
  place(m_colPan, x0 + c.pan0 - 4, PAD, x0 + c.pan1 + 4, PAD + COLS_H);
  place(m_colLevel, x0 + c.level0, PAD, x0 + c.level1, PAD + COLS_H);

  // your channel: TX IN name | volume | the room's codec and bitrate | meter
  float y = PAD + COLS_H + 2;
  place(m_myRow, x0, y, x0 + rw, y + ROW_H);
  {
    float x = c.name0;
    const float txW = textWidth(m_fonts.tiny, "TX") + 6, inW = textWidth(m_fonts.tiny, "IN") + 6;
    place(m_tx, x, 4, x + txW, ROW_H - 4);
    x += txW + 2;
    place(m_in, x, 4, x + inW, ROW_H - 4);
    x += inW + 4;
    // the two boxes end where the solo buttons do, each as wide as its longest text (a room the
    // plug-in can't send in plays Vorbis); MenuBox draws 4 + the text + 3 + its arrow (7) + 4
    auto boxW = [&](std::initializer_list<const char *> texts) {
      float w = 0;
      for (const char *t : texts) w = std::max(w, textWidth(m_fonts.small, t));
      return w + 18;
    };
    const float codecW = boxW({ "Opus", "FLAC", "Vorbis" }), bitrateW = boxW({ "510k" });
    const float bitrate0 = c.solo1 - bitrateW, codec0 = bitrate0 - 4 - codecW;
    // the volume makes room for them, and keeps the others' 50 px at least: in a narrow mixer
    // it starts further left, and the name gives way
    const float vol1 = std::min(c.vol1, codec0 - 5), vol0 = std::max(x + 10, std::min(c.vol0, vol1 - 50));
    place(m_myName, x, 0, std::max(x + 10, std::min(c.name1, vol0 - 5)), ROW_H);
    place(m_inputVolume, vol0, 4, std::max(vol0 + 30, vol1), ROW_H - 4);
    place(m_codec, codec0, 2, bitrate0 - 4, ROW_H - 2);
    place(m_bitrate, bitrate0, 2, c.solo1, ROW_H - 2);
    place(m_myMeter, c.level0, 0, c.level1, ROW_H);
  }
  y += ROW_H + ROW_GAP;

  // the others, then the footer
  const float footerTop = ph - PAD - FOOTER_H;
  const float playersTop = y, playersBottom = std::max(y + ROW_H, footerTop - 2);
  place(m_players, x0, playersTop, x0 + rw, playersBottom);
  place(m_noPlayers, x0 + 6, playersTop + 4, x0 + rw - 6, playersTop + 4 + ROW_H);
  place(m_footer, x0 + 4, footerTop, x0 + rw - 4, footerTop + FOOTER_H);

  float rowY = 0;
  for (Row &row : m_rows)
  {
    place(row.container.get(), 0, rowY, rw, rowY + ROW_H);
    layoutRow(row, rw);
    rowY += ROW_H + ROW_GAP;
  }
  m_playersContent->setSize(px(rw), px(std::max(rowY, playersBottom - playersTop)));
}

void JamsaEditor::layoutHostBar(float w)
{
  const float y0 = (HOSTBAR_H - ITEM_H) / 2, y1 = y0 + ITEM_H;
  float x = 4;
  const float hostW = m_host->preferredWidth();
  place(m_host, x, y0, x + hostW, y1);
  x += hostW + 6;

  // the port's state on the right
  const float portW = std::min<float>(220, textWidth(m_fonts.value, m_portText->getText().toStdString()) + 2);
  const float portLeft = w - 4 - portW;
  place(m_portText, portLeft, 0, w - 4, HOSTBAR_H);
  place(m_portLed, portLeft - 11, 0, portLeft - 2, HOSTBAR_H);

  if (m_hosting)
  {
    const float countW = textWidth(m_fonts.value, m_roomCount->getText().toStdString()) + 2;
    place(m_roomCount, x, 0, x + countW, HOSTBAR_H);
    x += countW + 8;
    const float lanW = std::min(m_lan->preferredWidth(), std::max<float>(80, portLeft - 15 - x));
    place(m_lan, x, y0, x + lanW, y1);
  }
  else place(m_hostHint, x, 0, w - 4, HOSTBAR_H);
}

void JamsaEditor::layoutLicense()
{
  if (!m_license) return;
  const float w = (float)m_root->getWidth(), h = (float)m_root->getHeight();
  place(m_license.get(), 0, 0, w, h);
  const float bw = std::min<float>(w - 20, 560), bh = std::min<float>(h - 20, 300);
  const float bx = floorf((w - bw) / 2), by = floorf((h - bh) / 2);
  place(m_licenseBox, bx, by, bx + bw, by + bh);
  place(m_licenseTitle, 8, 4, bw - 8, 22);
  place(m_licenseText, 8, 24, bw - 8, bh - 8 - ITEM_H - 6);
  const float declineW = m_decline->preferredWidth() + 8, acceptW = m_accept->preferredWidth() + 8;
  const float dx0 = bw - 8 - declineW, dy0 = bh - 8 - ITEM_H, dx1 = bw - 8, dy1 = bh - 8;
  place(m_decline, dx0, dy0, dx1, dy1);
  place(m_accept, dx0 - 6 - acceptW, dy0, dx0 - 6, dy1);
  m_license->repaint();
}

// ---------------------------------------------------------------------------
// keeping the views up to date
// ---------------------------------------------------------------------------

void JamsaEditor::update()
{
  if (!m_root) return;
  Snapshot s;
  engine().getSnapshot(s);

  if (s.settingsRevision != m_settingsRevision) syncSettings();
  m_settingsRevision = s.settingsRevision;

  updateHeader(s);
  updateTransport(s);

  // your channel
  const bool connected = s.state == ConnState::Connected;
  {
    std::string name = connected ? s.userName : engine().getSettings().userName;
    if (name.empty()) name = "You";
    setLabel(m_myName, name, true);
    m_myMeter->setPeak(std::max(s.localPeak[0], s.localPeak[1]));
    // the room's codec: picking one switches everyone (!codec), so only in a room
    m_codec->setEnabled(connected);
    m_codec->setShown(connected && !s.roomCodec.empty() ? s.roomCodec : "\xe2\x80\x94");
    setTooltip(m_codec, connected ? "The room's codec: pick Opus or FLAC to switch everyone (FLAC is lossless and needs "
                                    "several times the bandwidth)"
                                  : "The room's codec, Opus or FLAC: in a room, pick one to switch everyone");
    m_codec->setCurrentIndex(s.roomCodec == "FLAC" ? 1 : connected && s.roomCodec == "Opus" ? 0 : -1);

    // the bitrate: the room's (picking one changes it for everyone), else your own; FLAC is lossless,
    // and in a room the plug-in can't send in it means nothing
    const bool flac = connected && s.roomCodec == "FLAC", mute = connected && !s.canSend;
    m_bitrate->setEnabled(!flac && !mute);
    m_bitrate->setShown(flac || mute ? "\xe2\x80\x94" : uitext::kbpsText(s.sendKbps));
    if (flac) setTooltip(m_bitrate, "The room plays FLAC, which is lossless: the bitrate is for when it plays Opus");
    else if (mute) setTooltip(m_bitrate, "This room plays " + s.roomCodec + ": the plug-in can't send here");
    else if (s.roomOpusKbps > 0)
      setTooltip(m_bitrate, "The room's Opus bitrate: pick one to change it for everyone; it applies from the next interval");
    else if (connected)
      setTooltip(m_bitrate, "The Opus bitrate you send: this server doesn't set one for the room");
    else setTooltip(m_bitrate, "Your Opus bitrate: the room's when you host, and what you send where the server doesn't set one");
    {
      int index = -1;
      for (int i = 0; i < Settings::NUM_OPUS_KBPS; i ++)
        if (Settings::OPUS_KBPS[i] == s.sendKbps) index = i;
      m_bitrate->setCurrentIndex(index);
    }
    std::string tx = "Transmit: send your input to the room";
    if (connected && !s.canSend) tx += " (this room plays " + s.roomCodec + ": the plug-in can't send here)";
    else if (connected && !s.uploadCodec.empty()) tx += " (you send " + codecFromFourcc(s.uploadCodec) + ")";
    setTooltip(m_tx, tx);
  }

  if (s.usersRevision != m_usersRevision)
  {
    rebuildRows(s);
    m_usersRevision = s.usersRevision;
  }
  updateRows(s);
  int channels = 0;
  for (auto &u : s.users) channels += (int)u.channels.size();
  setLabel(m_noPlayers, !connected ? "Connect to a server, or host one, to hear the other players"
                        : channels ? "" : "Nobody else is here yet");
  showView(m_noPlayers, channels == 0);
  updateFooter(s);

  updateChat();
  updateHostBar(s);
  updateLicense(s);

  // "Copied" for a moment
  if (m_copied && now_seconds() > m_copiedUntil)
  {
    m_copied->setCopyTitle("Copy");
    m_copied = nullptr;
  }

  if (m_refocusChat)
  {
    m_refocusChat = false;
    if (!m_license) m_chatEdit->grabKeyboardFocus();
  }
}

void JamsaEditor::updateHeader(const Snapshot &s)
{
  const bool online = s.state == ConnState::Connected || s.state == ConnState::Connecting;
  m_connect->setTitle(online ? "Disconnect" : "Connect");
  std::string text;
  juce::Colour led = Colors::buttonOff();
  bool lit = false;
  switch (s.state)
  {
    case ConnState::Connected:
      text = "Connected" + (s.roomCodec.empty() ? std::string() : " \xc2\xb7 " + s.roomCodec);
      led = Colors::green();
      lit = true;
      break;
    case ConnState::Connecting:
      text = "Connecting\xe2\x80\xa6";
      led = Colors::yellow();
      lit = true;
      break;
    case ConnState::Failed:
      text = s.status;
      led = Colors::red();
      lit = true;
      break;
    default: text = "Not connected"; break;
  }
  if (m_connText->getText() != str(text))
  {
    setLabel(m_connText, text);
    layoutHeader((float)m_header->getWidth());
  }
  setTooltip(m_connText, s.status);
  m_connLed->setColor(led, lit);

  // hosting: the server box shows where the plug-in is connected (its own server) and can't
  // be edited; the server you join otherwise comes back when hosting stops
  const bool ownServer = s.hosting && online && !s.server.empty();
  if (!m_serverEdit->hasKeyboardFocus(true))
  {
    if (ownServer != m_showingOwnServer)
    {
      m_showingOwnServer = ownServer;
      m_serverEdit->setReadOnly(ownServer);
      m_serverEdit->setInterceptsMouseClicks(!ownServer, !ownServer);
      if (!ownServer) m_serverEdit->setText(str(engine().getSettings().server), false);
    }
    if (ownServer && m_serverEdit->getText() != str(s.server)) m_serverEdit->setText(str(s.server), false);
  }
}

void JamsaEditor::updateTransport(const Snapshot &s)
{
  const bool connected = s.state == ConnState::Connected;
  // the room's BPM and BPI: changing them votes, so only in a room
  m_bpm->setEnabled(connected);
  m_bpi->setEnabled(connected);
  m_bpm->setShown(connected ? (int)lround(s.bpm) : 120, uitext::bpmBoxText(connected ? s.bpm : 0));
  m_bpi->setShown(connected && s.bpi > 0 ? std::to_string(s.bpi) : "\xe2\x80\x94");
  {
    int index = -1;
    for (int i = 0; i < uitext::NUM_BPI_CHOICES; i ++)
      if (uitext::BPI_CHOICES[i] == s.bpi) index = i;
    if (index >= 0) m_bpi->setCurrentIndex(index);
  }

  // Sync: the host's tempo, green when it is the room's, amber when not
  const std::string chip = s.hostChip.empty() ? "Host" : s.hostChip;
  setLabel(m_syncText, chip + " " + uitext::bpmBoxText(s.hostTempo));
  const bool known = s.hostTempo > 0 && connected && s.bpm > 0;
  const bool differ = known && fabs(s.hostTempo - s.bpm) > BarSync::SAME_TEMPO;
  m_syncLed->setColor(!known ? Colors::buttonOff() : differ ? Colors::yellow() : Colors::green(), known);
  setTooltip(m_syncText, !s.syncNote.empty() ? s.syncNote
                         : s.hostTempo > 0 ? s.hostLabel + "'s tempo"
                         : s.hostLabel + " doesn't say its tempo (or isn't running the audio)");
  setTooltip(m_sync, syncTooltip(s.hostLabel));

  // the beat of the interval that plays; without the host's audio nothing plays (and the
  // engine's BPI is still the one it started with), so the room's BPI and no beat
  const bool playing = connected && s.audioRunning && s.intervalBeats > 0 && s.intervalLength > 0;
  const int beats = playing ? s.intervalBeats : connected ? s.bpi : 0;
  m_beats->setPosition(playing ? s.intervalPos : 0, playing ? s.intervalLength : 0, beats);
  setLabel(m_beatBox, uitext::beatText(playing ? s.intervalPos : 0, playing ? s.intervalLength : 0, beats));
}

// the mixer's bottom line: the most important of what the editor has to say
void JamsaEditor::updateFooter(const Snapshot &s)
{
  const bool connected = s.state == ConnState::Connected;
  const std::string host = s.hostLabel.empty() ? "The host" : s.hostLabel;
  std::string text;
  bool warn = false;
  if (connected && !s.audioRunning)
  {
    // NINJAM's intervals advance with the audio; without it nobody hears us and we hear nobody
    text = "No audio from " + host + ": turn the plug-in on, or start the audio engine";
    warn = true;
  }
  else if (connected && !s.canSend)
  {
    text = "The room plays " + s.roomCodec + ": the plug-in can't send here, and plays it as silence";
    warn = true;
  }
  else if (connected && s.sendPaused)
  {
    text = host + " is stopped, so you send nothing: press play to send";
    warn = true;
  }
  else if (!s.syncNote.empty())
  {
    text = s.syncNote;
    warn = true;
  }
  else if (s.nextIntervalPpq >= 0)
    text = nextIntervalText(s.hostChip, s.nextIntervalPpq, s.timeSigNum, s.timeSigDen);
  else if (connected)
  {
    if (!s.followTempo) text = "Sync is off: the intervals keep their own time";
    else
      switch (s.barState)
      {
        case BarSync::NoPosition: text = host + " doesn't say where its bars are"; break;
        case BarSync::Stopped: text = "The intervals line up with " + host + "'s bars when it plays"; break;
        case BarSync::OtherTempo: text = "The intervals line up with " + host + "'s bars when its tempo is the room's"; break;
        default: break;
      }
  }
  setLabel(m_footer, text, true);
  setLabelColor(m_footer, warn ? Colors::yellow() : Colors::label());
}

void JamsaEditor::syncSettings()
{
  const Settings st = engine().getSettings();
  if (!m_nameEdit->hasKeyboardFocus(true) && m_nameEdit->getText() != str(st.userName)) m_nameEdit->setText(str(st.userName), false);
  if (!m_serverEdit->hasKeyboardFocus(true) && !m_showingOwnServer && m_serverEdit->getText() != str(st.server))
    m_serverEdit->setText(str(st.server), false);
  m_tx->setValue(st.transmit);
  m_in->setValue(st.monitor);
  m_click->setValue(!st.metronomeMute);
  m_sync->setValue(st.followHostTempo);
  setControlValue(m_inputVolume, gainToSlider(st.inputVolume));
  setControlValue(m_clickVolume, gainToSlider(st.metronomeVolume));
}

void JamsaEditor::setRemote(size_t index, Engine::RemoteField field, float value)
{
  if (index >= m_rows.size()) return;
  const Row &row = m_rows[index];
  engine().setRemote(row.user, row.channel, field, value);
}

void JamsaEditor::rebuildRows(const Snapshot &s)
{
  m_rows.clear();
  size_t index = 0;
  for (const RemoteUserView &u : s.users)
    for (const RemoteChannelView &c : u.channels)
    {
      Row row;
      row.user = u.name;
      row.channel = c.index;
      row.container.reset(new Pane(Colors::row(), 1));
      std::string label = u.displayName;
      if (!c.name.empty()) label += " \xc2\xb7 " + c.name;
      row.name = makeLabel(label, m_fonts.value, Colors::value());
      row.name->setTooltip(str(u.name));
      row.name->setInterceptsMouseClicks(true, false);
      row.container->addAndMakeVisible(row.name);
      row.volume = new FaderBox(m_fonts.small);
      row.volume->format = [](float v) { return uitext::dbText(sliderToGain(v)); };
      row.volume->setDefaultValue(gainToSlider(1.0f));
      row.volume->onChange = [this, index]() { setRemote(index, Engine::RemoteVolume, sliderToGain(m_rows[index].volume->value())); };
      row.container->addAndMakeVisible(row.volume);
      row.pan = new ArcKnob(true);
      row.pan->setTooltip("Pan (double-click for the middle)");
      row.pan->onChange = [this, index]() { setRemote(index, Engine::RemotePan, m_rows[index].pan->value() * 2.0f - 1.0f); };
      row.container->addAndMakeVisible(row.pan);
      row.mute = new FlatButton("M", FlatButton::Toggle, m_fonts.label);
      row.mute->setTooltip("Mute");
      row.mute->onChange = [this, index]() { setRemote(index, Engine::RemoteMute, m_rows[index].mute->value() ? 1.0f : 0.0f); };
      row.container->addAndMakeVisible(row.mute);
      row.solo = new FlatButton("S", FlatButton::Toggle, m_fonts.label);
      row.solo->setOnColors(Colors::blue(), Colors::blueLine());
      row.solo->setTooltip("Solo");
      row.solo->onChange = [this, index]() { setRemote(index, Engine::RemoteSolo, m_rows[index].solo->value() ? 1.0f : 0.0f); };
      row.container->addAndMakeVisible(row.solo);
      row.meter = new SegmentMeter;
      row.container->addAndMakeVisible(row.meter);
      m_playersContent->addAndMakeVisible(*row.container);
      m_rows.push_back(std::move(row));
      index++;
    }
  layout();
}

void JamsaEditor::updateRows(const Snapshot &s)
{
  size_t i = 0;
  for (const RemoteUserView &u : s.users)
    for (const RemoteChannelView &c : u.channels)
    {
      if (i >= m_rows.size()) return;
      Row &row = m_rows[i++];
      if (row.user != u.name || row.channel != c.index) continue;
      setControlValue(row.volume, gainToSlider(c.volume));
      setControlValue(row.pan, (c.pan + 1.0f) * 0.5f);
      row.mute->setValue(c.mute);
      row.solo->setValue(c.solo);
      row.meter->setPeak(std::max(c.peak[0], c.peak[1]));
    }
}

void JamsaEditor::updateChat()
{
  std::vector<ChatLine> lines;
  const unsigned long long rev = engine().getChat(lines, 500);
  if (rev == m_chatRevision) return;
  m_chatRevision = rev;

  std::vector<TextLinesView::Line> out;
  out.reserve(lines.size());
  for (const ChatLine &l : lines)
  {
    TextLinesView::Line v;
    v.text = l.text;
    v.textColor = Colors::text();
    switch (l.kind)
    {
      case ChatLine::Message: v.who = l.who; v.whoColor = Colors::blue(); break;
      case ChatLine::Action: v.who = "* " + l.who; v.whoColor = Colors::blue(); v.textColor = Colors::status(); break;
      case ChatLine::Private: v.who = l.who.compare(0, 3, "-> ") == 0 ? l.who : l.who + " (private)"; v.whoColor = Colors::purple(); break;
      case ChatLine::Topic: v.textColor = Colors::blue(); break;
      case ChatLine::Server: v.textColor = Colors::purple(); break; // the server, votes
      case ChatLine::Error: v.textColor = Colors::red(); break;
      default: v.textColor = Colors::label(); break; // joins, leaves, the plug-in's own notes
    }
    out.push_back(v);
  }
  m_chat->setLines(out);
}

void JamsaEditor::updateHostBar(const Snapshot &s)
{
  if (m_hostPending)
  {
    const bool done = !s.hostBusy && (s.hosting == m_host->value() || now_seconds() - m_hostPendingSince > 3.0);
    if (done) m_hostPending = false;
  }
  if (!m_hostPending) m_host->setValue(s.hosting);

  bool relayout = s.hosting != m_hosting;
  m_hosting = s.hosting;
  showView(m_roomCount, s.hosting);
  showView(m_lan, s.hosting);
  showView(m_hostHint, !s.hosting);

  std::string port, tip;
  juce::Colour portColor = Colors::status(), led = Colors::buttonOff();
  bool lit = false;
  if (s.hosting)
  {
    const std::string count = std::to_string(s.hostUsers) + " in room";
    if (m_roomCount->getText() != str(count))
    {
      setLabel(m_roomCount, count);
      relayout = true;
    }
    // the address box and the port's state, by the rules in uitext::hostBar
    uitext::HostInfo info;
    info.port = s.hostPort;
    info.publicAddress = s.hostPublicAddress;
    info.lookedUpAddress = s.hostLookedUpAddress;
    info.localAddress = s.hostLocalAddress;
    info.portmapNote = s.hostNote;
    info.portmapPending = s.hostPortmapPending;
    info.lookupPending = s.hostLookupPending;
    info.joinedFromInternet = s.hostJoinedFromInternet;
    const uitext::HostBar bar = uitext::hostBar(info);
    m_lan->setKey(bar.addressKey);
    m_lan->setAddress(bar.address, bar.placeholder);
    m_lan->setTooltip(bar.addressTip);
    const std::string shown = bar.addressKey + " " + (bar.address.empty() ? bar.placeholder : bar.address);
    if (shown != m_lanShown)
    {
      m_lanShown = shown;
      relayout = true;
    }
    port = bar.portText;
    tip = bar.portTip;
    if (bar.port == uitext::HostBar::Open)
    {
      portColor = led = Colors::green();
      lit = true;
    }
    else if (bar.port == uitext::HostBar::Forward)
    {
      portColor = led = Colors::yellow();
      lit = true;
    }
  }
  else
  {
    std::string hint;
    if (s.hostBusy) hint = m_host->value() ? "Starting the server\xe2\x80\xa6" : "Stopping the server\xe2\x80\xa6";
    else if (s.hostedElsewherePort)
      hint = "Another Jamsa here hosts a server: join localhost:" + std::to_string(s.hostedElsewherePort);
    else hint = "Host a server in this plug-in: anyone can join without an account, and one vote changes BPM or BPI";
    setLabel(m_hostHint, hint, true);
  }
  if (m_portText->getText() != str(port))
  {
    setLabel(m_portText, port);
    relayout = true;
  }
  setLabelColor(m_portText, portColor);
  setTooltip(m_portText, tip);
  setTooltip(m_portLed, tip);
  m_portLed->setColor(led, lit);
  showView(m_portLed, s.hosting);
  if (relayout) layoutHostBar((float)m_hostBar->getWidth());
}

void JamsaEditor::updateLicense(const Snapshot &s)
{
  if (s.licensePending && !m_license)
  {
    m_license.reset(new Pane(juce::Colour(0, 0, 0).withAlpha(150.0f / 255.0f)));
    m_license->setInterceptsMouseClicks(true, true); // nothing under it gets the mouse
    m_licenseBox = new Pane(Colors::strip(), 2);
    m_license->addAndMakeVisible(m_licenseBox);
    m_licenseTitle = makeLabel("This server asks you to accept its license", m_fonts.bold, Colors::value());
    m_licenseBox->addAndMakeVisible(m_licenseTitle);
    m_licenseText = new TextLinesView(m_fonts.chat, m_fonts.chatBold);
    m_licenseText->setStartAtTop(true);
    m_licenseText->setBackground(Colors::pane());
    m_licenseBox->addAndMakeVisible(m_licenseText);
    m_accept = new FlatButton("Accept and join", FlatButton::Push, m_fonts.button);
    m_accept->onChange = [this]() { engine().answerLicense(true); };
    m_licenseBox->addAndMakeVisible(m_accept);
    m_decline = new FlatButton("Decline", FlatButton::Push, m_fonts.button);
    m_decline->onChange = [this]() { engine().answerLicense(false); };
    m_licenseBox->addAndMakeVisible(m_decline);
    m_root->addAndMakeVisible(*m_license);
    m_license->toFront(false);
    layoutLicense();
    m_licenseShown.clear();
  }
  if (m_license && s.licensePending && s.licenseText != m_licenseShown)
  {
    m_licenseShown = s.licenseText;
    std::vector<TextLinesView::Line> lines;
    TextLinesView::Line l;
    l.text = s.licenseText;
    l.textColor = Colors::text();
    lines.push_back(l);
    m_licenseText->setLines(lines);
    m_licenseText->scrollToStart();
  }
  if (!s.licensePending && m_license)
  {
    m_root->removeChildComponent(m_license.get());
    m_license.reset(); // deletes the views in it
    m_licenseBox = nullptr;
    m_licenseTitle = nullptr;
    m_licenseText = nullptr;
    m_accept = m_decline = nullptr;
  }
}

// ---------------------------------------------------------------------------
// what the user does
// ---------------------------------------------------------------------------

void JamsaEditor::connectOrDisconnect()
{
  Snapshot s;
  engine().getSnapshot(s);
  if (s.state == ConnState::Connected || s.state == ConnState::Connecting)
  {
    engine().disconnect();
    return;
  }
  engine().connect(m_serverEdit->getText().toStdString(), m_nameEdit->getText().toStdString());
}

void JamsaEditor::sendChatLine()
{
  const std::string text = m_chatEdit->getText().toStdString();
  if (text.find_first_not_of(" \t") == std::string::npos) return;
  engine().sendChat(text);
  m_chatEdit->setText("", false);
  m_refocusChat = true;
}

void JamsaEditor::copyAddress(AddressBox *box)
{
  if (!box || box->address().empty()) return;
  juce::SystemClipboard::copyTextToClipboard(str(box->address()));
  box->setCopyTitle("Copied");
  setTooltip(box, "Copied " + box->address());
  if (m_copied && m_copied != box) m_copied->setCopyTitle("Copy");
  m_copied = box;
  m_copiedUntil = now_seconds() + 1.5;
}

} // namespace jamsa
