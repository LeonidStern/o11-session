/*
    O11 Session - editor.h
    Copyright (C) 2026 and onward, O11 Session contributors

    Part of the O11 Session plug-in on JUCE. The licence is decided at release (see README.md).
*/

/*

  The editor, drawn with JUCE inside the host's window, in the look of
  Ableton Live's devices and Max for Live's controls. It is laid out in code
  for 720 x 330 and can be resized: larger, everything scales with the window
  (and the mixer takes what the aspect ratio leaves); down to 640 x 300, the
  layout gets tighter. The size is saved with the plug-in's state.

  From the top: the header (name, server, connect), the transport (Sync with
  the host; the click; the beats of the interval), the mixer (your channel
  first, then the others, each with a short status, and a line at the bottom)
  beside the room panel (the room's tempo, interval, codec and bitrate, which
  change for everyone when picked; the changes the room waits to make; what
  happened in the room), and the host bar (the built-in server and its
  address). Explanations are in the tooltips.

  It reads the Engine's snapshot 30 times a second and sends what the user
  does to the Engine, so it never waits for the network.

*/

#ifndef _O11_EDITOR_H_
#define _O11_EDITOR_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "engine.h"
#include "uitext.h"
#include "views.h"

namespace o11 {

class O11Processor;

class O11Editor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
  explicit O11Editor(O11Processor &plugin);
  ~O11Editor() override;

  void paint(juce::Graphics &g) override;
  void resized() override;

  // the size it is laid out for, the smallest, the largest
  enum { DEFAULT_WIDTH = 720, DEFAULT_HEIGHT = 330, MIN_WIDTH = 640, MIN_HEIGHT = 300, MAX_WIDTH = 3000, MAX_HEIGHT = 2000 };
  // how much a window of this size scales the layout: 1 up to the default size, then as much as fits
  static double zoomFor(double width, double height);

  // what the editor shows now, for tests: a snapshot read and the views brought up to date
  void refresh() { update(); }

private:
  struct Row // one remote channel in the mixer
  {
    std::string user; // name@address
    int channel = 0;
    std::unique_ptr<ui::Strip> container;
    ui::Led *led = nullptr;
    ui::NameCell *name = nullptr;
    ui::FaderBox *volume = nullptr;
    ui::ArcKnob *pan = nullptr;
    ui::FlatButton *mute = nullptr, *solo = nullptr;
    ui::LevelMeter *meter = nullptr;
  };
  struct Columns // where the mixer's columns are in a row of width w
  {
    float led0, led1, name0, name1, vol0, vol1, pan0, pan1, mute0, mute1, solo0, solo1, meter0, meter1;
  };
  static Columns columns(float w);

  void timerCallback() override { update(); }
  void buildViews();
  void layout();
  void layoutHeader(float w);
  void layoutTransport(float w);
  void layoutMixer(juce::Rectangle<float> pane);
  void layoutRow(Row &row, float width);
  void layoutEmpty();
  void layoutRoom(juce::Rectangle<float> pane);
  void layoutHostBar(float w);
  void layoutLicense();
  void update();
  void rebuildRows(const Snapshot &s);
  void updateRows(const Snapshot &s, double now);
  void updateMyRow(const Snapshot &s, double now);
  void updateEmpty(const Snapshot &s, const uitext::HostBar &bar);
  void updateHeader(const Snapshot &s);
  void updateTransport(const Snapshot &s);
  void updateFooter(const Snapshot &s);
  void updateRoom(const Snapshot &s, double now);
  void updateEvents(const Snapshot &s, double now);
  void updateHostBar(const Snapshot &s, const uitext::HostBar &bar);
  void updateLicense(const Snapshot &s);
  void syncSettings();
  void connectOrDisconnect();
  void copyAddress(ui::AddressBox *box);
  void setRemote(size_t row, Engine::RemoteField field, float value);

  Engine &engine();

  O11Processor &m_plugin;
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
  juce::Label *m_beatBox = nullptr;
  ui::Strip *m_sep = nullptr;
  ui::FlatButton *m_click = nullptr;
  ui::ArcKnob *m_clickVolume = nullptr;
  ui::BeatCells *m_beats = nullptr;

  // mixer
  ui::Strip *m_mixer = nullptr;
  ui::Strip *m_myRow = nullptr;
  ui::FlatButton *m_tx = nullptr, *m_in = nullptr;
  ui::NameCell *m_myName = nullptr;
  ui::FaderBox *m_inputVolume = nullptr;
  ui::LevelMeter *m_myMeter = nullptr;
  juce::Viewport *m_players = nullptr;
  juce::Component *m_playersContent = nullptr;
  juce::Label *m_emptyText = nullptr, *m_footer = nullptr;
  ui::AddressBox *m_emptyAddress = nullptr; // your room's address, while nobody else is in it
  std::vector<Row> m_rows;
  struct RowTrack
  {
    double heard = 0;   // when it last had sound
    long dropouts = -1; // its dropouts when last seen (-1: not seen yet)
    double droppedAt = -1; // when they last went up
  };
  std::map<std::string, RowTrack> m_tracks; // by user and channel

  // room panel
  ui::Strip *m_room = nullptr;
  juce::Label *m_roomTitle = nullptr, *m_roomCount = nullptr;
  juce::Label *m_bpmLabel = nullptr, *m_codecLabel = nullptr, *m_kbpsLabel = nullptr, *m_recentLabel = nullptr;
  juce::Label *m_modeLabel = nullptr;
  ui::DragNumberBox *m_bpm = nullptr;
  ui::FlatButton *m_sync = nullptr; // follow the DAW's tempo
  ui::Led *m_syncLed = nullptr;
  juce::Label *m_syncText = nullptr; // the DAW's tempo
  ui::MenuBox *m_codec = nullptr, *m_kbps = nullptr;
  ui::MenuBox *m_mode = nullptr; // the room's: Live, or intervals of one of uitext::BPI_CHOICES beats
  // a tempo and an interval the room waits to change to, and Live: what it waits for
  ui::Chip *m_chips[3] = { nullptr, nullptr, nullptr };
  ui::EventList *m_events = nullptr;

  // host bar
  ui::Strip *m_hostBar = nullptr;
  ui::FlatButton *m_host = nullptr;
  juce::Label *m_hostCount = nullptr, *m_hostHint = nullptr, *m_portText = nullptr;
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
  double m_eventsSince = -1;    // the room the events list was made for (Snapshot::roomSince)
  int m_chipsShown = -1;        // how many chips the room panel was laid out for
  struct Vote
  {
    std::string who; // "You" for you
    int value = 0;
  };
  Vote m_lastVote[2];           // the latest vote the server passed on, for BPM and for the interval
  bool m_emptyShown = false, m_emptyAddressShown = false; // the mixer's empty message, and the address in it
  bool m_hostPending = false;   // the switch was used; wait for the engine
  double m_hostPendingSince = 0;
  bool m_hosting = false;       // what the host bar was laid out for
  bool m_showingOwnServer = false; // the server box shows the server this instance hosts
  ui::AddressBox *m_copied = nullptr; // says "Copied" until m_copiedUntil
  double m_copiedUntil = 0;
  std::string m_lanShown, m_emptyAddressText; // what the address boxes were laid out for (key and text)

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(O11Editor)
};

} // namespace o11

#endif
