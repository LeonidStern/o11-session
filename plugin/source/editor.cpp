/*
    O11 Session - editor.cpp
    Copyright (C) 2026 and onward, O11 Session contributors

    Part of the O11 Session plug-in on JUCE. The licence is decided at release (see README.md).
*/

#include "editor.h"

#include <math.h>
#include <string.h>
#include <algorithm>
#include <vector>

#include "processor.h"
#include "uitext.h"
#include "version.h"

namespace o11 {

using namespace ui;

enum { kRemoteVolume, kRemotePan, kRemoteMute, kRemoteSolo };

// the layout, at the default size (logical pixels)
static const float PAD = 3, GAP = 3;            // around the window, between the strips and panes
static const float BAR_H = 22;                  // the header, the transport and the host bar
static const float BAR_X = 6, ITEM_GAP = 5;     // the strips' margins, between their items
static const float ITEM_H = 15;                 // value boxes and buttons
static const float ROOM_W = 200;                // the room panel; the mixer takes the rest
static const float MIX_X = 6, MIX_Y = 4;        // the mixer's margins
static const float MY_ROW_H = 20, ROW_H = 18, ROW_GAP = 2; // the mixer's rows
static const float BUTTON_W = 15, BUTTON_H = 14; // TX, IN, M, S
static const float FOOTER_H = 12;               // the mixer's bottom line
static const float ROOM_X = 6, ROOM_Y = 5;      // the room panel's margins
static const float LABEL_H = 11, CHIP_H = 17;   // the room panel's labels, its notes of changes to come
static const double SILENT_SECONDS = 10;        // a row says "silent" after this long without sound (or two intervals)

// a view's text is already this: its UTF-8 bytes compared where they are (the editor asks 30
// times a second, mostly of texts that haven't changed), without making a juce::String of it
static bool sameText(const juce::String &shown, const std::string &text)
{
  return shown.getNumBytesAsUTF8() == text.size() && memcmp(shown.toRawUTF8(), text.data(), text.size()) == 0;
}

static void setLabel(juce::Label *label, const std::string &text, bool tooltip = false)
{
  if (!label || sameText(label->getText(), text)) return;
  const juce::String t = str(text);
  label->setText(t, juce::dontSendNotification);
  if (tooltip) label->setTooltip(t); // the whole text, when the label cuts it off
}

static void setTooltip(juce::SettableTooltipClient *v, const std::string &text)
{
  if (v && !sameText(v->getTooltip(), text)) v->setTooltip(str(text));
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

double O11Editor::zoomFor(double width, double height)
{
  return std::max(1.0, std::min(width / DEFAULT_WIDTH, height / DEFAULT_HEIGHT));
}

O11Editor::O11Editor(O11Processor &plugin)
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

O11Editor::~O11Editor()
{
  stopTimer();
  m_license.reset();
  m_rows.clear();
  m_root.reset();
  setLookAndFeel(nullptr);
}

Engine &O11Editor::engine()
{
  return m_plugin.engine();
}

void O11Editor::paint(juce::Graphics &g)
{
  g.fillAll(Colors::window());
}

// The views are laid out in logical pixels (720 x 330 at the default size); the root's
// transform scales them to the window.
void O11Editor::resized()
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
  return l;
}

static const char *SERVER_PLACEHOLDER = "IP address of a room";
static const char *SERVER_TIP = "The room's IP address, as 203.0.113.9 (port 2049 unless you add another, as 203.0.113.9:2050)";
static const char *METER_TIP = "Green, amber from -12 dB, red from -3 dB; the light at the right stays red after 0 dB until you click it";

void O11Editor::buildViews()
{
  const Fonts &f = m_fonts;
  m_root.reset(new Pane(Colors::window()));
  addAndMakeVisible(*m_root);
  auto add = [](juce::Component *parent, auto *child) {
    parent->addAndMakeVisible(child);
    return child;
  };

  // header: O11 Session  Name [..]  Server [..]  Connect          (o) Connected · Opus
  m_header = add(m_root.get(), new Pane(Colors::strip(), 2));
  m_title = add(m_header, makeLabel(O11_NAME, f.title, Colors::value()));
  m_nameLabel = add(m_header, makeLabel("Name", f.label, Colors::label()));
  m_nameEdit = add(m_header, makeEdit(f.value, "Your name"));
  m_nameEdit->onTextChange = [this]() { engine().setUserName(m_nameEdit->getText().toStdString()); };
  m_serverLabel = add(m_header, makeLabel("Server", f.label, Colors::label()));
  m_serverEdit = add(m_header, makeEdit(f.value, SERVER_PLACEHOLDER));
  m_serverEdit->setTooltip(SERVER_TIP);
  m_serverEdit->onTextChange = [this]() { engine().setServerText(m_serverEdit->getText().toStdString()); };
  m_serverEdit->onReturnKey = [this]() {
    Snapshot s;
    engine().getSnapshot(s);
    if (s.state != ConnState::Connected && s.state != ConnState::Connecting) connectOrDisconnect();
  };
  m_connect = add(m_header, new FlatButton("Connect", FlatButton::Push, f.button));
  m_connect->onChange = [this]() { connectOrDisconnect(); };
  m_connLed = add(m_header, new Led);
  m_connText = add(m_header, makeLabel("", f.value, Colors::value(), juce::Justification::centredRight));
  m_connText->setInterceptsMouseClicks(true, false); // for its tooltip

  // transport: Click (dial) | beats  [6 / 16]
  m_transport = add(m_root.get(), new Pane(Colors::strip(), 2));
  m_sep = add(m_transport, new Strip(Colors::separator()));
  m_click = add(m_transport, new FlatButton("Click", FlatButton::Toggle, f.button));
  m_click->setTooltip("The metronome: the first beat of each interval higher");
  m_click->onChange = [this]() { engine().setMetronome(sliderToGain(m_clickVolume->value()), !m_click->value()); };
  m_clickVolume = add(m_transport, new ArcKnob(false));
  m_clickVolume->setDefaultValue(gainToSlider(0.5f));
  m_clickVolume->setTooltip("The click's volume: drag up or down (double-click for the default)");
  m_clickVolume->onChange = [this]() { engine().setMetronome(sliderToGain(m_clickVolume->value()), !m_click->value()); };
  m_beats = add(m_transport, new BeatCells);
  m_beatBox = add(m_transport, makeValueLabel(f.value));

  // mixer: your channel, the others, a line at the bottom
  m_mixer = add(m_root.get(), new Pane(Colors::pane(), 2));
  m_myRow = add(m_mixer, new Pane(Colors::myRow(), 2));
  m_tx = add(m_myRow, new FlatButton("TX", FlatButton::Toggle, f.tiny));
  m_tx->setOnColor(Colors::orange());
  m_tx->onChange = [this]() { engine().setTransmit(m_tx->value()); };
  m_in = add(m_myRow, new FlatButton("IN", FlatButton::Toggle, f.tiny));
  m_in->setTooltip("Monitor: hear your input in the plug-in's output");
  m_in->onChange = [this]() { engine().setMonitor(m_in->value()); };
  m_myName = add(m_myRow, new NameCell(f.valueBold, f.small, 5));
  m_inputVolume = add(m_myRow, new FaderBox(f.small));
  m_inputVolume->format = [](float v) { return uitext::dbText(sliderToGain(v)); };
  m_inputVolume->setDefaultValue(gainToSlider(1.0f));
  m_inputVolume->setTooltip("The volume of what you send: drag left or right (double-click for 0 dB)");
  m_inputVolume->onChange = [this]() { engine().setInputVolume(sliderToGain(m_inputVolume->value())); };
  m_myMeter = add(m_myRow, new LevelMeter);
  m_myMeter->setTooltip(std::string("Your input. ") + METER_TIP);

  m_players = add(m_mixer, new juce::Viewport);
  m_players->setScrollBarsShown(true, false, true, false);
  m_players->setScrollBarThickness(5);
  m_playersContent = new Pane(juce::Colours::transparentBlack);
  m_players->setViewedComponent(m_playersContent, true);
  m_emptyText = add(m_mixer, makeLabel("", f.value, Colors::label(), juce::Justification::centred));
  m_emptyAddress = add(m_mixer, new AddressBox(f, ""));
  m_emptyAddress->onCopy = [this]() { copyAddress(m_emptyAddress); };
  m_footer = add(m_mixer, makeLabel("", f.small, Colors::label()));
  m_footer->setInterceptsMouseClicks(true, false);

  // room panel: Room  4 players / BPM (o) Live 120.00, Mode / [Sync] 120.00, Live 28 ms / Codec, Kbps /
  // changes to come / Recent
  m_room = add(m_root.get(), new Pane(Colors::pane(), 2));
  m_roomTitle = add(m_room, makeLabel("Room", f.labelBold, Colors::value()));
  m_roomCount = add(m_room, makeLabel("", f.label, Colors::label(), juce::Justification::centredRight));
  m_bpmLabel = add(m_room, makeLabel("BPM", f.label, Colors::label()));
  m_modeLabel = add(m_room, makeLabel("Mode", f.label, Colors::label()));
  m_syncLed = add(m_room, new Led);
  m_syncText = add(m_room, makeLabel("", f.label, Colors::value(), juce::Justification::centredRight));
  m_syncText->setInterceptsMouseClicks(true, false);
  m_sync = add(m_room, new FlatButton("Sync", FlatButton::Toggle, f.button));
  m_sync->onChange = [this]() { engine().setFollowHostTempo(m_sync->value()); };
  m_codecLabel = add(m_room, makeLabel("Codec", f.label, Colors::label()));
  m_kbpsLabel = add(m_room, makeLabel("Kbps", f.label, Colors::label()));
  m_bpm = add(m_room, new DragNumberBox(f.valueBold));
  m_bpm->setJustificationType(juce::Justification::centredRight);
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
  m_bpm->setTooltip("The room's tempo: drag up or down (Shift for fine steps), or double-click and type, to change it "
                    "for everyone from the next interval");
  // Live, or intervals of so many beats: picking one switches everyone from the next interval
  m_mode = add(m_room, new MenuBox(f.value));
  m_mode->addEntry("Live"); // the order of the choices
  for (int b : uitext::BPI_CHOICES) m_mode->addEntry(uitext::intervalsModeText(b, true));
  m_mode->onSelect = [this](int i) {
    Snapshot s;
    engine().getSnapshot(s);
    if (i < 0 || s.state != ConnState::Connected) return;
    if (i == 0)
    {
      if (s.liveServer && !s.liveSwitchOn) engine().setRoomLive(true);
      return;
    }
    if (i > uitext::NUM_BPI_CHOICES) return;
    if (s.liveServer && s.liveSwitchOn) engine().setRoomLive(false);
    if (uitext::BPI_CHOICES[i - 1] != s.bpi) engine().vote("bpi", uitext::BPI_CHOICES[i - 1]);
  };
  m_codec = add(m_room, new MenuBox(f.value));
  m_codec->addEntry("Opus"); // the order of the choices
  m_codec->addEntry("FLAC");
  m_codec->onSelect = [this](int i) {
    Snapshot s;
    engine().getSnapshot(s);
    const char *codec = i == 1 ? "FLAC" : "Opus";
    if (i >= 0 && s.state == ConnState::Connected && s.roomCodec != codec) engine().setRoomCodec(i == 1 ? "flac" : "opus");
  };
  m_kbps = add(m_room, new MenuBox(f.value));
  for (int k : Settings::OPUS_KBPS) m_kbps->addEntry(uitext::kbpsText(k));
  m_kbps->onSelect = [this](int i) {
    if (i >= 0 && i < Settings::NUM_OPUS_KBPS) engine().setOpusKbps(Settings::OPUS_KBPS[i]);
  };
  for (auto &chip : m_chips)
  {
    chip = add(m_room, new Chip(f.small));
    chip->setVisible(false);
  }
  m_recentLabel = add(m_room, makeLabel("Recent", f.label, Colors::label()));
  m_events = add(m_room, new EventList(f.small));

  // host bar: Host  3 in room  WAN [..] [Copy]                          (o) Port open
  m_hostBar = add(m_root.get(), new Pane(Colors::strip(), 2));
  m_host = add(m_hostBar, new FlatButton("Host", FlatButton::Toggle, f.button));
  m_host->setTooltip("Host a room in this plug-in, and join it");
  m_host->onChange = [this]() {
    m_hostPending = true;
    m_hostPendingSince = nowSeconds();
    engine().setHosting(m_host->value());
  };
  m_hostCount = add(m_hostBar, makeLabel("", f.value, Colors::value()));
  m_hostHint = add(m_hostBar, makeLabel("", f.value, Colors::label()));
  m_hostHint->setInterceptsMouseClicks(true, false);
  m_lan = add(m_hostBar, new AddressBox(f, "LAN")); // WAN once the public address is known (uitext::hostBar)
  m_lan->onCopy = [this]() { copyAddress(m_lan); };
  m_portLed = add(m_hostBar, new Led);
  m_portText = add(m_hostBar, makeLabel("", f.value, Colors::value(), juce::Justification::centredRight));
  m_portText->setInterceptsMouseClicks(true, false);
}

// the mixer's columns in a row of width w: the light, the name (the rest), the volume, pan, M, S,
// the meter
O11Editor::Columns O11Editor::columns(float w)
{
  Columns c;
  const float g = 5;
  c.led0 = 2;
  c.led1 = c.led0 + 6;
  c.name0 = c.led1 + g;
  c.meter1 = w - 2;
  c.meter0 = c.meter1 - 56;
  c.solo1 = c.meter0 - g;
  c.solo0 = c.solo1 - BUTTON_W;
  c.mute1 = c.solo0 - g;
  c.mute0 = c.mute1 - BUTTON_W;
  c.pan1 = c.mute0 - g;
  c.pan0 = c.pan1 - 16;
  c.vol1 = c.pan0 - g;
  c.vol0 = c.vol1 - 150;
  c.name1 = c.vol0 - g;
  if (c.name1 - c.name0 < 90)
  {
    // a narrow mixer: the volume gives way, down to 70 px
    c.vol0 = std::min(c.vol1 - 70, c.name0 + 90 + g);
    c.name1 = c.vol0 - g;
  }
  return c;
}

void O11Editor::layout()
{
  if (!m_root) return;
  const float rw = (float)m_root->getWidth(), rh = (float)m_root->getHeight();
  const float W = std::max<float>(rw, MIN_WIDTH), H = std::max<float>(rh, MIN_HEIGHT);

  const float transportY = PAD + BAR_H + GAP, bodyY = transportY + BAR_H + GAP;
  const float hostY = H - PAD - BAR_H, bodyBottom = hostY - GAP;
  place(m_header, PAD, PAD, W - PAD, PAD + BAR_H);
  place(m_transport, PAD, transportY, W - PAD, transportY + BAR_H);
  place(m_mixer, PAD, bodyY, W - PAD - ROOM_W - GAP, bodyBottom);
  place(m_room, W - PAD - ROOM_W, bodyY, W - PAD, bodyBottom);
  place(m_hostBar, PAD, hostY, W - PAD, hostY + BAR_H);

  layoutHeader(W - 2 * PAD);
  layoutTransport(W - 2 * PAD);
  layoutMixer(m_mixer->getBounds().toFloat());
  layoutRoom(m_room->getBounds().toFloat());
  layoutHostBar(W - 2 * PAD);
  layoutLicense();
  m_root->repaint();
}

void O11Editor::layoutHeader(float w)
{
  const float y0 = (BAR_H - ITEM_H) / 2, y1 = y0 + ITEM_H;
  float x = BAR_X;
  auto label = [&](juce::Label *l, float after) {
    const float tw = textWidth(l->getFont(), l->getText().toStdString());
    place(l, x, 0, x + tw + 1, BAR_H);
    x += tw + 1 + after;
  };
  label(m_title, 6 + ITEM_GAP);
  label(m_nameLabel, ITEM_GAP);
  place(m_nameEdit, x, y0, x + 64, y1);
  x += 64 + ITEM_GAP;
  label(m_serverLabel, ITEM_GAP);
  const float connectW = std::max(textWidth(m_fonts.button, "Disconnect"), textWidth(m_fonts.button, "Connect")) + 12;
  // the connection's state on the right: as much of it as there is room for
  const float stateW = std::min<float>(200, textWidth(m_fonts.value, m_connText->getText().toStdString()) + 2);
  const float serverW = std::max<float>(90, std::min<float>(118, w - BAR_X - (x + ITEM_GAP + connectW + 12 + 11 + std::max<float>(60, stateW))));
  place(m_serverEdit, x, y0, x + serverW, y1);
  x += serverW + ITEM_GAP;
  place(m_connect, x, y0, x + connectW, y1);
  x += connectW + 12;
  const float textLeft = std::max(x + 11, w - BAR_X - stateW);
  place(m_connText, textLeft, 0, w - BAR_X, BAR_H);
  place(m_connLed, textLeft - 11, 0, textLeft - 5, BAR_H);
}

void O11Editor::layoutTransport(float w)
{
  const float y0 = (BAR_H - ITEM_H) / 2, y1 = y0 + ITEM_H;
  float x = BAR_X;
  auto sep = [&](Strip *s) {
    place(s, x, (BAR_H - 14) / 2, x + 1, (BAR_H - 14) / 2 + 14);
    x += 1 + ITEM_GAP;
  };
  const float clickW = m_click->preferredWidth();
  place(m_click, x, y0, x + clickW, y1);
  x += clickW + ITEM_GAP;
  place(m_clickVolume, x, (BAR_H - 15) / 2, x + 15, (BAR_H - 15) / 2 + 15);
  x += 15 + ITEM_GAP;
  sep(m_sep);
  const float beatBoxW = std::max(38.0f, textWidth(m_fonts.value, "88 / 88") + 10);
  place(m_beatBox, w - BAR_X - beatBoxW, y0, w - BAR_X, y1);
  place(m_beats, x, 0, std::max(x + 16, w - BAR_X - beatBoxW - ITEM_GAP), BAR_H);
}

void O11Editor::layoutRow(Row &row, float rw)
{
  const Columns c = columns(rw);
  const float b0 = (ROW_H - BUTTON_H) / 2, b1 = b0 + BUTTON_H;
  place(row.led, c.led0, 0, c.led1, ROW_H);
  place(row.name, c.name0, 0, c.name1, ROW_H);
  place(row.volume, c.vol0, b0, c.vol1, b1);
  place(row.pan, c.pan0 + 1, b0, c.pan1 - 1, b1);
  place(row.mute, c.mute0, b0, c.mute1, b1);
  place(row.solo, c.solo0, b0, c.solo1, b1);
  place(row.meter, c.meter0, 0, c.meter1, ROW_H);
}

void O11Editor::layoutMixer(juce::Rectangle<float> pane)
{
  const float pw = pane.getWidth(), ph = pane.getHeight();
  const float rw = pw - 2 * MIX_X; // a row's width
  const Columns c = columns(rw);

  // your channel: TX IN name status | volume | meter
  float y = MIX_Y;
  place(m_myRow, MIX_X, y, MIX_X + rw, y + MY_ROW_H);
  {
    const float b0 = (MY_ROW_H - BUTTON_H) / 2, b1 = b0 + BUTTON_H;
    float x = c.led0;
    place(m_tx, x, b0, x + BUTTON_W, b1);
    x += BUTTON_W + 3;
    place(m_in, x, b0, x + BUTTON_W, b1);
    x += BUTTON_W + 5;
    place(m_myName, x, 0, std::max(x + 10, c.name1), MY_ROW_H);
    place(m_inputVolume, c.vol0, b0, c.vol1, b1);
    place(m_myMeter, c.meter0, 0, c.meter1, MY_ROW_H);
  }
  y += MY_ROW_H + ROW_GAP;

  // the others, then the footer
  const float footerTop = ph - MIX_Y - FOOTER_H;
  const float playersTop = y, playersBottom = std::max(y + ROW_H, footerTop - 2);
  place(m_players, MIX_X, playersTop, MIX_X + rw, playersBottom);
  place(m_footer, MIX_X + 2, footerTop, MIX_X + rw - 2, footerTop + FOOTER_H);

  // more rows than fit: they make room for the scroll bar
  const float rowsH = (float)m_rows.size() * (ROW_H + ROW_GAP);
  const float contentW = rowsH > playersBottom - playersTop ? rw - 6 : rw;
  float rowY = 0;
  for (Row &row : m_rows)
  {
    place(row.container.get(), 0, rowY, contentW, rowY + ROW_H);
    layoutRow(row, contentW);
    rowY += ROW_H + ROW_GAP;
  }
  m_playersContent->setSize(px(contentW), px(std::max(rowY, playersBottom - playersTop)));
  layoutEmpty();
}

// the message where the others would be, and your room's address under it
void O11Editor::layoutEmpty()
{
  const juce::Rectangle<float> a = m_players->getBounds().toFloat();
  const int lines = std::max(1, juce::StringArray::fromLines(m_emptyText->getText()).size());
  const float lineH = ceilf(m_fonts.value.getHeight() * 1.1f);
  const float textH = lineH * (float)lines, addressH = m_emptyAddressShown ? 8 + ITEM_H : 0;
  const float top = floorf(a.getY() + (a.getHeight() - textH - addressH) / 2) - 6;
  place(m_emptyText, a.getX() + 6, top, a.getRight() - 6, top + textH);
  if (m_emptyAddressShown)
  {
    const float w = std::min(m_emptyAddress->preferredWidth(), a.getWidth() - 12);
    const float x = floorf(a.getCentreX() - w / 2);
    place(m_emptyAddress, x, top + textH + 8, x + w, top + textH + 8 + ITEM_H);
  }
}

void O11Editor::layoutRoom(juce::Rectangle<float> pane)
{
  const float w = pane.getWidth(), h = pane.getHeight();
  const float x0 = ROOM_X, x1 = w - ROOM_X, colW = floorf((x1 - x0 - 6) / 2), xb = x0 + colW + 6;
  float y = ROOM_Y;
  place(m_roomTitle, x0, y, x0 + 60, y + LABEL_H);
  place(m_roomCount, x0 + 60, y, x1, y + LABEL_H);
  y += LABEL_H + 5;
  // BPM, and the DAW's tempo at the right of its label: (o) Ableton 120.00
  const float syncTextW = std::min(colW - textWidth(m_fonts.label, "BPM") - 14,
                                   textWidth(m_fonts.label, m_syncChip + " 888.88") + 2);
  m_syncTextW = syncTextW;
  place(m_bpmLabel, x0, y, x0 + colW - syncTextW - 10, y + LABEL_H);
  place(m_syncLed, x0 + colW - syncTextW - 10, y, x0 + colW - syncTextW - 4, y + LABEL_H);
  place(m_syncText, x0 + colW - syncTextW, y, x0 + colW, y + LABEL_H);
  place(m_modeLabel, xb, y, x1, y + LABEL_H);
  y += LABEL_H + 2;
  const float syncW = m_sync->preferredWidth();
  place(m_sync, x0, y, x0 + syncW, y + ITEM_H);
  place(m_bpm, x0 + syncW + 4, y, x0 + colW, y + ITEM_H);
  place(m_mode, xb, y, x1, y + ITEM_H);
  y += ITEM_H + 5;
  place(m_codecLabel, x0, y, x0 + colW, y + LABEL_H);
  place(m_kbpsLabel, xb, y, x1, y + LABEL_H);
  y += LABEL_H + 2;
  place(m_codec, x0, y, x0 + colW, y + ITEM_H);
  place(m_kbps, xb, y, x1, y + ITEM_H);
  y += ITEM_H + 5;
  for (Chip *chip : m_chips)
    if (chip->isVisible())
    {
      place(chip, x0, y, x1, y + CHIP_H);
      y += CHIP_H + 5;
    }
  y += 3;
  place(m_recentLabel, x0, y, x1, y + LABEL_H);
  y += LABEL_H + 3;
  place(m_events, x0, y, x1, std::max(y + EventList::LINE_H, h - ROOM_Y));
}

void O11Editor::layoutHostBar(float w)
{
  const float y0 = (BAR_H - ITEM_H) / 2, y1 = y0 + ITEM_H;
  float x = BAR_X;
  const float hostW = m_host->preferredWidth() + 6;
  place(m_host, x, y0, x + hostW, y1);
  x += hostW + ITEM_GAP;

  // the port's state on the right
  const float portW = std::min<float>(240, textWidth(m_fonts.value, m_portText->getText().toStdString()) + 2);
  const float portLeft = w - BAR_X - portW;
  place(m_portText, portLeft, 0, w - BAR_X, BAR_H);
  place(m_portLed, portLeft - 11, 0, portLeft - 5, BAR_H);

  if (m_hosting)
  {
    const float countW = textWidth(m_fonts.value, m_hostCount->getText().toStdString()) + 2;
    place(m_hostCount, x, 0, x + countW, BAR_H);
    x += countW + ITEM_GAP + 4;
    const float lanW = std::min(m_lan->preferredWidth(), std::max<float>(120, portLeft - 15 - x));
    place(m_lan, x, y0, x + lanW, y1);
  }
  else place(m_hostHint, x + 4, 0, w - BAR_X, BAR_H);
}

void O11Editor::layoutLicense()
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

void O11Editor::update()
{
  if (!m_root) return;
  Snapshot s;
  engine().getSnapshot(s);
  const double now = nowSeconds();

  if (s.settingsRevision != m_settingsRevision) syncSettings();
  m_settingsRevision = s.settingsRevision;

  // your room's address and its port, by the rules in uitext::hostBar: for the host bar, and for
  // the mixer while nobody else is in the room
  uitext::HostBar bar;
  if (s.hosting)
  {
    uitext::HostInfo info;
    info.port = s.hostPort;
    info.publicAddress = s.hostPublicAddress;
    info.lookedUpAddress = s.hostLookedUpAddress;
    info.localAddress = s.hostLocalAddress;
    info.portmapNote = s.hostNote;
    info.portmapPending = s.hostPortmapPending;
    info.lookupPending = s.hostLookupPending;
    info.joinedFromInternet = s.hostJoinedFromInternet;
    bar = uitext::hostBar(info);
  }

  updateHeader(s);
  updateTransport(s);
  updateMyRow(s, now);
  if (s.usersRevision != m_usersRevision)
  {
    rebuildRows(s);
    m_usersRevision = s.usersRevision;
  }
  updateRows(s, now);
  updateEmpty(s, bar);
  updateFooter(s);
  updateRoom(s, now);
  updateEvents(s, now);
  updateHostBar(s, bar);
  updateLicense(s);

  // "Copied" for a moment
  if (m_copied && now > m_copiedUntil)
  {
    m_copied->setCopyTitle("Copy");
    m_copied = nullptr;
  }
}

void O11Editor::updateHeader(const Snapshot &s)
{
  const bool online = s.state == ConnState::Connected || s.state == ConnState::Connecting;
  m_connect->setTitle(online ? "Disconnect" : "Connect");
  std::string text;
  juce::Colour led = Colors::ledOff();
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
  if (!sameText(m_connText->getText(), text))
  {
    setLabel(m_connText, text);
    layoutHeader((float)m_header->getWidth());
  }
  setLabelColor(m_connText, s.state == ConnState::Disconnected ? Colors::label() : Colors::value());
  setTooltip(m_connText, s.status);
  m_connLed->setColor(led, lit);

  // hosting: the server box says "this computer" (the plug-in joins its own server) and can't
  // be edited; the server you join otherwise comes back when hosting stops
  const bool ownServer = s.hosting && online && !s.server.empty();
  if (!m_serverEdit->hasKeyboardFocus(true) && ownServer != m_showingOwnServer)
  {
    m_showingOwnServer = ownServer;
    m_serverEdit->setReadOnly(ownServer);
    m_serverEdit->setInterceptsMouseClicks(!ownServer, !ownServer);
    m_serverEdit->setTextToShowWhenEmpty(ownServer ? "this computer" : SERVER_PLACEHOLDER, Colors::faint());
    m_serverEdit->setText(ownServer ? juce::String() : str(engine().getSettings().server), false);
    m_serverEdit->setTooltip(ownServer ? str("You host this room, on this computer (" + s.server + ")") : juce::String(SERVER_TIP));
  }
}

void O11Editor::updateTransport(const Snapshot &s)
{
  const bool connected = s.state == ConnState::Connected;

  // Sync: the host's tempo, green when it is the room's, amber when not, grey while Sync is
  // off; "Link" while the host follows the room through Link
  const std::string chip = s.linkLeads ? "Link" : s.hostChip.empty() ? "DAW" : s.hostChip;
  if (chip != m_syncChip)
  {
    m_syncChip = chip;
    layoutRoom(m_room->getBounds().toFloat());
  }
  // "Ableton 120.00", or "Ableton 120" where the panel is too narrow for it
  std::string syncText = chip + " " + uitext::bpmBoxText(s.hostTempo);
  if (s.hostTempo > 0 && textWidth(m_fonts.label, syncText) + 2 > m_syncTextW) syncText = chip + " " + bpmText(s.hostTempo);
  setLabel(m_syncText, syncText);
  const bool known = s.hostTempo > 0 && connected && s.bpm > 0 && m_sync->value();
  const bool differ = known && fabs(s.hostTempo - s.bpm) > BarSync::SAME_TEMPO;
  m_syncLed->setColor(differ ? Colors::amber() : Colors::green(), known);
  setLabelColor(m_syncText, !known ? Colors::label() : differ ? Colors::amber() : Colors::value());
  setTooltip(m_syncText, !s.syncNote.empty() ? s.syncNote
                         : s.hostTempo > 0 ? s.hostLabel + "'s tempo" + (s.linkLeads ? ", which follows the room through Link" : "")
                         : s.hostLabel + " doesn't say its tempo (or isn't running the audio)");
  setTooltip(m_sync, syncTooltip(s.hostLabel, s.linkState, s.linkPeers));
  m_clickVolume->setAlpha(m_click->value() ? 1.0f : 0.4f);

  // the beat of the interval that plays; without the host's audio nothing plays (and the
  // engine's interval is still the one it started with), so the room's interval and no beat
  const bool playing = connected && s.audioRunning && s.intervalBeats > 0 && s.intervalLength > 0;
  const int beats = playing ? s.intervalBeats : connected ? s.bpi : 0;
  m_beats->setPosition(playing ? s.intervalPos : 0, playing ? s.intervalLength : 0, beats);
  setLabel(m_beatBox, uitext::beatText(playing ? s.intervalPos : 0, playing ? s.intervalLength : 0, beats));
}

void O11Editor::updateMyRow(const Snapshot &s, double now)
{
  const bool connected = s.state == ConnState::Connected;
  std::string name = connected ? s.userName : engine().getSettings().userName;
  if (name.empty()) name = "You";
  m_myName->setPlayer(name, "");
  m_myMeter->setLevel(std::max(s.localPeak[0], s.localPeak[1]), now);

  // what the room hears from you
  const std::string host = s.hostLabel.empty() ? "The host" : s.hostLabel;
  std::string status, tip;
  juce::Colour color = Colors::label();
  if (!connected)
  {
    status = "not in a room";
    tip = "Your channel: in a room, your input goes to everyone while TX is on";
  }
  else if (!s.canSend)
  {
    status = "can't send here";
    color = Colors::amber();
    tip = "This room plays " + s.roomCodec + ": the plug-in can't send here";
  }
  else if (!m_tx->value())
  {
    status = "not sending";
    tip = "TX is off: the room doesn't hear you";
  }
  else if (!s.audioRunning)
  {
    status = "not sending";
    color = Colors::amber();
    tip = "No audio from " + host + ", so nothing is sent: turn the plug-in on, or start the audio engine";
  }
  else if (s.sendPaused)
  {
    status = "paused: " + (s.hostChip.empty() ? std::string("DAW") : s.hostChip) + " stopped";
    color = Colors::amber();
    tip = host + " is stopped, so you send nothing: press play to send";
  }
  else
  {
    status = "sending";
    color = Colors::orange();
    tip = "The room hears you" + (s.uploadCodec.empty() ? std::string() : " (" + codecFromFourcc(s.uploadCodec) + ")");
  }
  m_myName->setStatus(status, color);
  setTooltip(m_myName, tip);

  std::string tx = "Transmit: send your input to the room";
  if (connected && !s.canSend) tx += " (this room plays " + s.roomCodec + ": the plug-in can't send here)";
  setTooltip(m_tx, tx);
}

// what the mixer shows where the others would be: how to get into a room, or that nobody else
// is here yet (with your room's address, when you host it)
void O11Editor::updateEmpty(const Snapshot &s, const uitext::HostBar &bar)
{
  const bool connected = s.state == ConnState::Connected;
  int channels = 0;
  for (auto &u : s.users) channels += (int)u.channels.size();
  std::string text;
  bool address = false;
  if (s.state == ConnState::Connecting) text = "Joining the room\xe2\x80\xa6";
  else if (!connected) text = "Type the IP address of a room and press Connect,\nor press Host to start a room here.";
  else if (channels == 0 && s.hosting)
  {
    text = "Nobody else is here yet. Send them your address:";
    address = true;
  }
  else if (channels == 0) text = "Nobody else is here yet.";

  bool relayout = false;
  if (!sameText(m_emptyText->getText(), text))
  {
    setLabel(m_emptyText, text);
    relayout = true;
  }
  showView(m_emptyText, !text.empty());
  if (address)
  {
    m_emptyAddress->setAddress(bar.address, bar.placeholder);
    setTooltip(m_emptyAddress, bar.addressTip);
    const std::string shown = bar.address.empty() ? bar.placeholder : bar.address;
    if (shown != m_emptyAddressText)
    {
      m_emptyAddressText = shown;
      relayout = true;
    }
  }
  if (address != m_emptyAddressShown)
  {
    m_emptyAddressShown = address;
    relayout = true;
  }
  showView(m_emptyAddress, address);
  if (relayout) layoutEmpty();
}

// the mixer's bottom line: the most important of what the editor has to say
void O11Editor::updateFooter(const Snapshot &s)
{
  const bool connected = s.state == ConnState::Connected;
  const std::string host = s.hostLabel.empty() ? "The host" : s.hostLabel;
  std::string text;
  bool warn = false;
  if (connected && !s.audioRunning)
  {
    // the intervals advance with the audio; without it nobody hears us and we hear nobody
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
    text = nextIntervalText(s.hostChip, s.nextIntervalPpq, s.timeSigNum, s.timeSigDen) + (s.linkLeads ? " (Link)" : "");
  else if (connected)
  {
    if (!s.followTempo) text = "Sync is off: the intervals keep their own time";
    else if (s.linkLeads) text = host + " follows the room through Link: its tempo, and its bars on the intervals";
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
  setLabelColor(m_footer, warn ? Colors::amber() : Colors::label());
}

void O11Editor::updateRoom(const Snapshot &s, double)
{
  const bool connected = s.state == ConnState::Connected;
  setLabel(m_roomCount, connected ? uitext::playersText((int)s.users.size() + 1) : std::string());

  // the tempo and interval playing now; the room's new ones wait for the next interval (and
  // without the host's audio nothing plays, so the room's)
  const bool playing = connected && s.audioRunning && s.intervalBeats > 0 && s.intervalBpm > 0;
  const double bpm = playing ? s.intervalBpm : s.bpm;
  const int bpi = playing ? s.intervalBeats : s.bpi;
  m_bpm->setEnabled(connected);
  m_bpm->setShown(connected && bpm > 0 ? (int)lround(bpm) : 120, uitext::bpmBoxText(connected ? bpm : 0));

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
  m_kbps->setEnabled(!flac && !mute);
  m_kbps->setShown(flac || mute ? "\xe2\x80\x94" : uitext::kbpsText(s.sendKbps));
  if (flac) setTooltip(m_kbps, "The room plays FLAC, which is lossless: the bitrate is for when it plays Opus");
  else if (mute) setTooltip(m_kbps, "This room plays " + s.roomCodec + ": the plug-in can't send here");
  else if (s.roomOpusKbps > 0)
    setTooltip(m_kbps, "The room's Opus bitrate: pick one to change it for everyone; it applies from the next interval");
  else if (connected) setTooltip(m_kbps, "The Opus bitrate you send: this server doesn't set one for the room");
  else setTooltip(m_kbps, "Your Opus bitrate: the room's when you host, and what you send where the server doesn't set one");
  {
    int index = -1;
    for (int i = 0; i < Settings::NUM_OPUS_KBPS; i ++)
      if (Settings::OPUS_KBPS[i] == s.sendKbps) index = i;
    m_kbps->setCurrentIndex(index);
  }

  // the room's mode: Live (picking it switches everyone, from an interval's start), or intervals of
  // so many beats (picking one switches everyone to that length from the next interval)
  const bool liveHere = connected && s.liveServer;
  // without the host's audio nothing plays, so the room's switch
  const bool liveShown = liveHere && (s.audioRunning ? s.liveRoom : s.liveSwitchOn);
  m_mode->setEnabled(connected);
  m_mode->setEntryEnabled(0, liveHere);
  m_mode->setShown(!connected ? "\xe2\x80\x94"
                   : liveShown ? uitext::liveModeText(true, s.liveHeardMs)
                                            : uitext::intervalsModeText(bpi, false));
  {
    int index = -1;
    for (int i = 0; i < uitext::NUM_BPI_CHOICES; i ++)
      if (uitext::BPI_CHOICES[i] == s.bpi) index = i + 1;
    m_mode->setCurrentIndex(!connected ? -1 : liveHere && s.liveSwitchOn ? 0 : index);
  }
  const std::string lengths = "pick a length to switch everyone to intervals of that many beats from the next interval";
  if (!connected)
    setTooltip(m_mode, "Live plays everyone in real time, about 30 ms late, on servers that have it; Intervals plays each "
                       "player one interval late, in time with the beat");
  else if (!s.liveServer)
    setTooltip(m_mode, "This server doesn't have Live, so everyone plays through intervals: pick a length to change it for "
                       "everyone from the next interval");
  else if (s.liveRoom)
    setTooltip(m_mode, std::string("The room plays Live") +
                       (s.liveHeardMs > 0 ? ": you hear the others about " + uitext::msText(s.liveHeardMs) + " late" : "") +
                       ". Or " + lengths);
  else if (s.liveSwitchOn)
    setTooltip(m_mode, "Live is on: everyone goes Live together when the next interval starts. Or " + lengths);
  else setTooltip(m_mode, "The room plays intervals: pick Live to play everyone in real time, from an interval's start, or " + lengths);

  // what the room changes to at the next interval, and who voted for it (when the server says)
  int shown = 0;
  auto chip = [&](bool on, const std::string &text, const std::string &tip, const Vote &vote, int value,
                  std::function<void()> click = nullptr) {
    if (!on) return;
    Chip *c = m_chips[shown++];
    const std::string who = vote.value == value ? vote.who : std::string();
    c->set(text, who);
    c->setClick(std::move(click));
    setTooltip(c, tip + (who.empty() ? "" : who == "You" ? ": you voted for it" : ": " + who + " voted for it"));
  };
  const int roomBpm = (int)lround(s.bpm);
  chip(playing && roomBpm > 0 && roomBpm != (int)lround(s.intervalBpm), "BPM " + std::to_string(roomBpm) + " from next interval",
       "The room's tempo changes to " + std::to_string(roomBpm) + " BPM when the next interval starts", m_lastVote[0], roomBpm);
  chip(playing && s.bpi > 0 && s.bpi != s.intervalBeats, uitext::beatsText(s.bpi) + " from next interval",
       "The intervals last " + uitext::beatsText(s.bpi) + " from the next one on", m_lastVote[1], s.bpi);
  // Live: a player in trouble (the room stays Live; a click switches everyone to intervals), or
  // the switch to come at the next interval
  const Vote nobody;
  const std::string advice = liveHere && s.liveSwitchOn && s.liveRoom
                               ? uitext::liveAdviceText((int)s.liveReason, s.liveReasonWho, s.liveReasonOthers) : "";
  if (!advice.empty())
    chip(true, advice, std::string(s.liveReason == Snapshot::LiveCantLive
                                     ? "A player can't reach the others directly, so they don't hear each other in Live"
                                     : "Live audio has dropouts that intervals wouldn't have")
                         + ". Click to switch the room to intervals for everyone", nobody, -1,
         [this]() { engine().setRoomLive(false); });
  else if (liveHere && s.liveSwitchOn && !s.liveRoom)
    chip(true, "Live from next interval", "Everyone goes Live together when the next interval starts", nobody, -1);
  else if (liveHere && !s.liveSwitchOn && s.liveRoom)
    chip(true, "Intervals from next interval", "Everyone goes back to intervals when the next interval starts", nobody, -1);
  for (int i = 0; i < 3; i ++) showView(m_chips[i], i < shown);
  if (shown != m_chipsShown)
  {
    m_chipsShown = shown;
    layoutRoom(m_room->getBounds().toFloat());
  }
}

// the room panel's kind of a line the engine keeps
static uitext::LineKind lineKind(const ChatLine &l)
{
  switch (l.kind)
  {
    case ChatLine::System: return uitext::LineKind::Note;
    case ChatLine::Server: return uitext::LineKind::Server;
    case ChatLine::Host: return uitext::LineKind::HostLog;
    case ChatLine::Join: return uitext::LineKind::Join;
    case ChatLine::Leave: return uitext::LineKind::Leave;
    case ChatLine::Message: return uitext::LineKind::Chat;
    case ChatLine::Live: return uitext::LineKind::Live;
    case ChatLine::Private: return l.who == "*" ? uitext::LineKind::Server : uitext::LineKind::Other; // the server's message of the day
    default: return uitext::LineKind::Other;
  }
}

// what happened in the room since you joined it, newest first
void O11Editor::updateEvents(const Snapshot &s, double now)
{
  m_events->setNow(now);
  if (s.state != ConnState::Connected)
  {
    m_events->setEvents({});
    m_events->setPlaceholder("What happens in the room shows here");
    m_eventsSince = -1;
    return;
  }
  m_events->setPlaceholder("");
  if (s.chatRevision == m_chatRevision && s.roomSince == m_eventsSince) return;

  std::vector<ChatLine> lines;
  m_chatRevision = engine().getChat(lines, 400);
  m_eventsSince = s.roomSince;

  // who cast the votes the server passed on: the voting system line a vote brought about says
  // who, instead of the vote's own line
  std::vector<std::string> voterOf(lines.size());
  std::vector<bool> merged(lines.size(), false);
  m_lastVote[0] = m_lastVote[1] = Vote();
  size_t vote = lines.size();
  bool voteBpm = false;
  int voteValue = 0;
  for (size_t i = 0; i < lines.size(); i ++)
  {
    const ChatLine &l = lines[i];
    if (l.time < s.roomSince) continue;
    bool bpm = false;
    int value = 0;
    if (l.kind == ChatLine::Message && uitext::parseVote(l.text, &bpm, &value))
    {
      vote = i;
      voteBpm = bpm;
      voteValue = value;
      m_lastVote[bpm ? 0 : 1] = Vote { l.who == s.userName ? std::string("You") : l.who, value };
    }
    else if (l.kind == ChatLine::Server && vote < lines.size() && uitext::voteMatches(voteBpm, voteValue, l.text))
    {
      voterOf[i] = m_lastVote[voteBpm ? 0 : 1].who;
      merged[vote] = true;
      vote = lines.size();
    }
  }

  std::vector<EventList::Event> events;
  for (size_t i = lines.size(); i-- > 0 && events.size() < 100;)
  {
    const auto it = lines.begin() + (long)i;
    if (it->time < s.roomSince) break;
    if (merged[i]) continue;
    if ((it->kind == ChatLine::Join || it->kind == ChatLine::Leave) && it->who == s.userName) continue;
    const std::string who = it->kind == ChatLine::Message && it->who == s.userName ? std::string("You") : it->who;
    const uitext::RoomEvent e = uitext::roomEvent(lineKind(*it), who, it->text, s.hosting, voterOf[i]);
    if (!e.shown) continue;
    EventList::Event v;
    v.text = e.text;
    v.color = e.tone == uitext::RoomEvent::Change ? Colors::purple() : e.tone == uitext::RoomEvent::Warn ? Colors::amber() : Colors::label();
    v.time = it->time;
    events.push_back(v);
  }
  m_events->setEvents(events);
}

void O11Editor::syncSettings()
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

void O11Editor::setRemote(size_t index, Engine::RemoteField field, float value)
{
  if (index >= m_rows.size()) return;
  const Row &row = m_rows[index];
  engine().setRemote(row.user, row.channel, field, value);
}

static std::string rowKey(const std::string &user, int channel)
{
  return user + "/" + std::to_string(channel);
}

void O11Editor::rebuildRows(const Snapshot &s)
{
  m_rows.clear();
  std::map<std::string, RowTrack> tracks;
  size_t index = 0;
  for (const RemoteUserView &u : s.users)
    for (const RemoteChannelView &c : u.channels)
    {
      Row row;
      row.user = u.name;
      row.channel = c.index;
      const auto was = m_tracks.find(rowKey(u.name, c.index));
      if (was != m_tracks.end()) tracks.insert(*was);
      row.container.reset(new Pane(juce::Colours::transparentBlack));
      row.led = new Led;
      row.container->addAndMakeVisible(row.led);
      row.name = new NameCell(m_fonts.value, m_fonts.small, 6);
      row.name->setPlayer(u.displayName, c.name);
      row.container->addAndMakeVisible(row.name);
      row.volume = new FaderBox(m_fonts.small);
      row.volume->format = [](float v) { return uitext::dbText(sliderToGain(v)); };
      row.volume->setDefaultValue(gainToSlider(1.0f));
      row.volume->setTooltip("Their volume in your mix: drag left or right (double-click for 0 dB)");
      row.volume->onChange = [this, index]() { setRemote(index, Engine::RemoteVolume, sliderToGain(m_rows[index].volume->value())); };
      row.container->addAndMakeVisible(row.volume);
      row.pan = new ArcKnob(true);
      row.pan->setTooltip("Pan: drag up or down (double-click for the middle)");
      row.pan->onChange = [this, index]() { setRemote(index, Engine::RemotePan, m_rows[index].pan->value() * 2.0f - 1.0f); };
      row.container->addAndMakeVisible(row.pan);
      row.mute = new FlatButton("M", FlatButton::Toggle, m_fonts.tiny);
      row.mute->setTooltip("Mute");
      row.mute->onChange = [this, index]() { setRemote(index, Engine::RemoteMute, m_rows[index].mute->value() ? 1.0f : 0.0f); };
      row.container->addAndMakeVisible(row.mute);
      row.solo = new FlatButton("S", FlatButton::Toggle, m_fonts.tiny);
      row.solo->setOnColor(Colors::blue());
      row.solo->setTooltip("Solo: only the soloed players play");
      row.solo->onChange = [this, index]() { setRemote(index, Engine::RemoteSolo, m_rows[index].solo->value() ? 1.0f : 0.0f); };
      row.container->addAndMakeVisible(row.solo);
      row.meter = new LevelMeter;
      row.meter->setTooltip(METER_TIP);
      row.container->addAndMakeVisible(row.meter);
      m_playersContent->addAndMakeVisible(*row.container);
      m_rows.push_back(std::move(row));
      index++;
    }
  m_tracks.swap(tracks); // forget the channels that are gone
  layout();
}

void O11Editor::updateRows(const Snapshot &s, double now)
{
  bool anySolo = false;
  for (const RemoteUserView &u : s.users)
    for (const RemoteChannelView &c : u.channels) anySolo = anySolo || c.solo;
  // a channel is silent after two intervals without sound (or SILENT_SECONDS, if longer): the
  // first interval of a player who just joined is still on its way
  const double interval = s.intervalBpm > 0 && s.intervalBeats > 0 ? s.intervalBeats * 60.0 / s.intervalBpm : 0;
  const double silentAfter = std::max(SILENT_SECONDS, 2 * interval);

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
      // what they play, muted or not
      const float level = std::max(c.level[0], c.level[1]);
      row.meter->setLevel(level, now);
      // muted: the meter dims; another player soloed: the whole row
      row.meter->setAlpha(c.mute ? 0.3f : 1.0f);
      row.container->setAlpha(anySolo && !c.solo ? 0.4f : 1.0f);

      // the status: dropouts a moment ago, joined a moment ago, or silent for a while (only
      // while the audio runs: the intervals, and their sound, come with it)
      RowTrack &track = m_tracks.emplace(rowKey(u.name, c.index), RowTrack { now, -1, -1 }).first->second;
      if (level > 0.0001f || !s.audioRunning) track.heard = now;
      if (track.dropouts >= 0 && c.dropouts > track.dropouts) track.droppedAt = now;
      track.dropouts = c.dropouts;
      const double joined = u.joinedAt > 0 ? now - u.joinedAt : -1;
      std::string status;
      juce::Colour color = Colors::green(), led = Colors::green();
      bool silent = false, dropping = false;
      if (track.droppedAt >= 0 && now - track.droppedAt < uitext::DROPOUT_SECONDS && c.dropouts > 0)
      {
        status = uitext::dropoutsText(c.dropouts);
        color = led = Colors::amber();
        dropping = true;
      }
      else if (joined >= 0 && joined < uitext::JOINED_SECONDS) status = uitext::joinedText(joined);
      else if (now - track.heard > silentAfter)
      {
        status = "silent";
        color = Colors::label();
        led = Colors::ledOff();
        silent = true;
      }
      row.name->setStatus(status, color);
      row.led->setColor(led, !silent);
      std::string tip = u.displayName + (c.name.empty() ? std::string() : " \xc2\xb7 " + c.name) + " (" + u.name + "): ";
      tip += c.playerOut > 0 ? "also plays alone on the DAW output Player " + std::to_string(c.playerOut)
                             : "plays on the main output only (the " + std::to_string(Engine::NUM_PLAYER_OUTS) +
                                 " player outputs are taken)";
      if (silent) tip += ". No sound from them for a while: their TX may be off, or their DAW stopped";
      if (dropping)
      {
        const double ago = now - track.droppedAt;
        tip += ". Gaps in their audio: " + uitext::dropoutsText(c.dropouts) + " since they came in, the last one " +
               (ago < 5 ? std::string("just now") : uitext::ageText(ago) + " ago") + " (their connection, or yours, may be too slow)";
      }
      setTooltip(row.name, tip);
      setTooltip(row.led, tip);
    }
}

void O11Editor::updateHostBar(const Snapshot &s, const uitext::HostBar &bar)
{
  if (m_hostPending)
  {
    const bool done = !s.hostBusy && (s.hosting == m_host->value() || nowSeconds() - m_hostPendingSince > 3.0);
    if (done) m_hostPending = false;
  }
  if (!m_hostPending) m_host->setValue(s.hosting);

  bool relayout = s.hosting != m_hosting;
  m_hosting = s.hosting;
  showView(m_hostCount, s.hosting);
  showView(m_lan, s.hosting);
  showView(m_hostHint, !s.hosting);

  std::string port, tip;
  juce::Colour portColor = Colors::label(), led = Colors::ledOff();
  bool lit = false;
  if (s.hosting)
  {
    const bool full = s.hostMaxUsers > 0 && s.hostUsers >= s.hostMaxUsers;
    const std::string count = std::to_string(s.hostUsers) + " in room" + (full ? " \xc2\xb7 full" : "");
    if (!sameText(m_hostCount->getText(), count))
    {
      setLabel(m_hostCount, count);
      relayout = true;
    }
    setTooltip(m_hostCount, full ? "The room is full: it lets " + std::to_string(s.hostMaxUsers) + " players in"
                                 : "The room lets " + std::to_string(s.hostMaxUsers) + " players in");
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
      led = Colors::green();
      portColor = Colors::value();
      lit = true;
    }
    else if (bar.port == uitext::HostBar::Forward || bar.port == uitext::HostBar::Blocked)
    {
      portColor = led = Colors::amber();
      lit = true;
    }
  }
  else
  {
    std::string hint;
    if (s.hostBusy) hint = m_host->value() ? "Starting the server\xe2\x80\xa6" : "Stopping the server\xe2\x80\xa6";
    else if (s.hostedElsewherePort)
      hint = "Another O11 Session here hosts a room: join localhost:" + std::to_string(s.hostedElsewherePort);
    else hint = "Press Host to start a room here";
    setLabel(m_hostHint, hint);
    setTooltip(m_hostHint, s.hostBusy || s.hostedElsewherePort
                             ? hint
                             : "Host a room in this plug-in: anyone can join at your address without an account, and one "
                               "vote changes the tempo or the interval");
  }
  if (!sameText(m_portText->getText(), port))
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

void O11Editor::updateLicense(const Snapshot &s)
{
  if (s.licensePending && !m_license)
  {
    m_license.reset(new Pane(juce::Colour(0, 0, 0).withAlpha(150.0f / 255.0f)));
    m_license->setInterceptsMouseClicks(true, true); // nothing under it gets the mouse
    m_licenseBox = new Pane(Colors::strip(), 2);
    m_license->addAndMakeVisible(m_licenseBox);
    m_licenseTitle = makeLabel("This server asks you to accept its license", m_fonts.title, Colors::value());
    m_licenseBox->addAndMakeVisible(m_licenseTitle);
    m_licenseText = new TextLinesView(m_fonts.text, m_fonts.valueBold);
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
    l.textColor = Colors::value();
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

void O11Editor::connectOrDisconnect()
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

void O11Editor::copyAddress(AddressBox *box)
{
  if (!box || box->address().empty()) return;
  juce::SystemClipboard::copyTextToClipboard(str(box->address()));
  box->setCopyTitle("Copied");
  setTooltip(box, "Copied " + box->address());
  if (m_copied && m_copied != box) m_copied->setCopyTitle("Copy");
  m_copied = box;
  m_copiedUntil = nowSeconds() + 1.5;
}

} // namespace o11
