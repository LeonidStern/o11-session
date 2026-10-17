/*
    Jamsa - editor.h
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

/*

  The editor, drawn with JUCE inside the host's window, in the look of
  Ableton Live's devices. It is laid out in code for 720 x 330 and can be
  resized: larger, everything scales with the window (and the panes take what
  the aspect ratio leaves); down to 640 x 300, the layout gets tighter. The
  size is saved with the plug-in's state.

  From the top: the header (name, server, connect), the transport (the room's
  BPM and BPI, which vote when changed; Sync with the host; the click; the
  beats of the interval), the mixer (your channel first, then the others) and
  the chat side by side, and the host bar (the built-in server and its
  address).

  It reads the Engine's snapshot 30 times a second and sends what the user
  does to the Engine, so it never waits for the network.

*/

#ifndef _JAMSA_EDITOR_H_
#define _JAMSA_EDITOR_H_

#include <memory>
#include <string>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "engine.h"
#include "views.h"

namespace jamsa {

class JamsaProcessor;

class JamsaEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
  explicit JamsaEditor(JamsaProcessor &plugin);
  ~JamsaEditor() override;

  void paint(juce::Graphics &g) override;
  void resized() override;

  // the size it is laid out for, the smallest, the largest
  enum { DEFAULT_WIDTH = 720, DEFAULT_HEIGHT = 330, MIN_WIDTH = 640, MIN_HEIGHT = 300, MAX_WIDTH = 3000, MAX_HEIGHT = 2000 };
  // how much a window of this size scales the layout: 1 up to the default size, then as much as fits
  static double zoomFor(double width, double height);

private:
  struct Row // one remote channel in the mixer
  {
    std::string user; // name@address
    int channel = 0;
    std::unique_ptr<ui::Strip> container;
    juce::Label *name = nullptr;
    ui::FaderBox *volume = nullptr;
    ui::ArcKnob *pan = nullptr;
    ui::FlatButton *mute = nullptr, *solo = nullptr;
    ui::SegmentMeter *meter = nullptr;
  };
  struct Columns // where the mixer's columns are in a row of width w
  {
    float name0, name1, vol0, vol1, pan0, pan1, mute0, mute1, solo0, solo1, level0, level1;
  };
  static Columns columns(float w);

  void timerCallback() override { update(); }
  void buildViews();
  void layout();
  void layoutHeader(float w);
  void layoutTransport(float w);
  void layoutMixer(juce::Rectangle<float> pane);
  void layoutRow(Row &row, float width);
  void layoutHostBar(float w);
  void layoutLicense();
  void update();
  void rebuildRows(const Snapshot &s);
  void updateRows(const Snapshot &s);
  void updateChat();
  void updateHeader(const Snapshot &s);
  void updateTransport(const Snapshot &s);
  void updateFooter(const Snapshot &s);
  void updateHostBar(const Snapshot &s);
  void updateLicense(const Snapshot &s);
  void syncSettings();
  void connectOrDisconnect();
  void sendChatLine();
  void copyAddress(ui::AddressBox *box);
  void setRemote(size_t row, Engine::RemoteField field, float value);

  Engine &engine();

  JamsaProcessor &m_plugin;
  double m_zoom = 1.0; // the layout's, from the window's size (zoomFor)

  ui::Fonts m_fonts;
  ui::LookAndFeel m_lookAndFeel;
  juce::TooltipWindow m_tooltips;

  std::unique_ptr<juce::Component> m_root;

  // header
  ui::Strip *m_header = nullptr;
  juce::Label *m_title = nullptr, *m_nameLabel = nullptr, *m_serverLabel = nullptr, *m_connText = nullptr;
  juce::TextEditor *m_nameEdit = nullptr, *m_serverEdit = nullptr;
  ui::FlatButton *m_connect = nullptr;
  ui::Led *m_connLed = nullptr;

  // transport
  ui::Strip *m_transport = nullptr;
  juce::Label *m_bpmLabel = nullptr, *m_bpiLabel = nullptr, *m_syncText = nullptr, *m_beatBox = nullptr;
  ui::DragNumberBox *m_bpm = nullptr;
  ui::MenuBox *m_bpi = nullptr;
  ui::Strip *m_sep[3] = { nullptr, nullptr, nullptr };
  ui::FlatButton *m_sync = nullptr, *m_click = nullptr;
  ui::Led *m_syncLed = nullptr;
  ui::ArcKnob *m_clickVolume = nullptr;
  ui::BeatCells *m_beats = nullptr;

  // mixer
  ui::Strip *m_mixer = nullptr;
  juce::Label *m_colChannel = nullptr, *m_colVolume = nullptr, *m_colPan = nullptr, *m_colLevel = nullptr;
  ui::Strip *m_myRow = nullptr;
  ui::FlatButton *m_tx = nullptr, *m_in = nullptr;
  juce::Label *m_myName = nullptr;
  ui::FaderBox *m_inputVolume = nullptr;
  ui::MenuBox *m_codec = nullptr;
  ui::MenuBox *m_bitrate = nullptr;
  ui::SegmentMeter *m_myMeter = nullptr;
  juce::Viewport *m_players = nullptr;
  juce::Component *m_playersContent = nullptr;
  juce::Label *m_noPlayers = nullptr, *m_footer = nullptr;
  std::vector<Row> m_rows;

  // chat
  ui::Strip *m_chatPane = nullptr;
  ui::TextLinesView *m_chat = nullptr;
  juce::TextEditor *m_chatEdit = nullptr;

  // host bar
  ui::Strip *m_hostBar = nullptr;
  ui::FlatButton *m_host = nullptr;
  juce::Label *m_roomCount = nullptr, *m_hostHint = nullptr, *m_portText = nullptr;
  ui::AddressBox *m_lan = nullptr;
  ui::Led *m_portLed = nullptr;

  // the license, shown over everything while the server waits for an answer
  std::unique_ptr<ui::Strip> m_license;
  ui::Strip *m_licenseBox = nullptr;
  juce::Label *m_licenseTitle = nullptr;
  ui::TextLinesView *m_licenseText = nullptr;
  ui::FlatButton *m_accept = nullptr, *m_decline = nullptr;
  std::string m_licenseShown;

  // what the views show, to update them only on changes
  unsigned int m_usersRevision = ~0u, m_settingsRevision = ~0u;
  unsigned long long m_chatRevision = ~0ull;
  bool m_hostPending = false;   // the switch was used; wait for the engine
  double m_hostPendingSince = 0;
  bool m_refocusChat = false;
  bool m_hosting = false;       // what the host bar was laid out for
  bool m_showingOwnServer = false; // the server box shows the server this instance hosts
  ui::AddressBox *m_copied = nullptr; // says "Copied" until m_copiedUntil
  double m_copiedUntil = 0;
  std::string m_lanShown; // what the address box was laid out for

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(JamsaEditor)
};

} // namespace jamsa

#endif
