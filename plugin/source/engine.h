/*
    O11 Session - engine.h
    Copyright (C) 2026 and onward, O11 Session contributors

    Part of the O11 Session plug-in on JUCE. The licence is decided at release (see README.md).
*/

/*

  One plug-in instance's NINJAM client, on the o11proto engine.

  Threads:
  - the audio thread calls process(), which runs o11proto::engine::Engine::process();
  - the engine's own network thread is the only one that touches the o11proto
    engine otherwise: it runs Engine::run(), carries out what the UI asked for
    (a queue of commands), and publishes a Snapshot of the state;
  - the UI (and the host, for the plug-in's state) only reads snapshots and
    queues commands, so it never waits for the network.

  The network thread runs from start() to stop(), editor open or not.

  The audio path is: input * input volume -> the engine (sent as our channel)
  -> output = the other players + metronome (+ our input when monitoring).
  The metronome is the plug-in's own (click.h), on the beats of the engine's
  intervals; the engine's is muted, and so is its monitoring of our channel.

  Following the host (hostsync.h): while "Sync" is on, process() lines the
  intervals up with the host's bars, and the network thread votes the room to
  the host's tempo when it changes.

  Ableton Link (linksync.h): while "Sync" is on in a room the plug-in is in
  Link, and one instance in the program leads it: process() puts the room's
  tempo and the intervals' beat on Link, so Live (with its Link on) follows
  the room, and while Link has peers it leaves the intervals where they are
  (no BarSync). The network thread still votes the room to the host's tempo
  when it changes, and puts the room's tempo back on Link when the room
  doesn't take it or another app set it (LinkTempoKeeper).

*/

#ifndef _O11_ENGINE_H_
#define _O11_ENGINE_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "click.h"
#include "hostsync.h"
#include "linksync.h"

namespace o11proto { namespace engine { class Engine; } }

namespace o11 {

// the clock of ChatLine::time and RemoteUserView::joinedAt: seconds, steady
double nowSeconds();

struct ChatLine
{
  // System: the plug-in's own notes; Server: what the server says (votes, the room's audio);
  // Host: what the built-in server logs (the port mapper, errors); Join, Leave: who comes and goes;
  // Live: the engine's Live lines (the room went Live, or back on intervals, and why)
  enum Kind { System, Server, Host, Join, Leave, Message, Action, Private, Topic, Error, Live };
  Kind kind = System;
  std::string who; // display name, without the @address
  std::string text;
  double time = 0; // when it came (nowSeconds())
};

struct RemoteChannelView
{
  int index = 0; // the channel's index at its user
  std::string name;
  float volume = 1.0f, pan = 0.0f;
  bool mute = false, solo = false;
  float peak[2] = { 0, 0 };  // what you hear of them: nothing while muted or another channel is soloed
  float level[2] = { 0, 0 }; // what they play, with your volume and pan, muted or not
  long dropouts = 0;         // gaps in their audio since they came in: audio late, cut short or undecodable
  int playerOut = 0; // the plug-in's extra output that plays it alone (1 to Engine::NUM_PLAYER_OUTS), 0 for none
};

struct RemoteUserView
{
  std::string name;        // as the server names the user (name@address)
  std::string displayName; // without the @address
  double joinedAt = 0;     // when they joined the room you're in (nowSeconds()), 0 if they were there first
  int liveHeardMs = -1;    // while their Live stream plays here: how late you hear them, -1 when not known
  int liveFillMs = -1;     // ... and how full its jitter buffer is
  int liveBufferMs = -1;   // ... and the buffer its stream plays with (automatic: 10 ms, or 20 once it fell back)
  bool liveFellBack = false; // its automatic buffer fell back to the longer one
  bool liveDirect = false; // Live goes straight between you (all to all), not through the room's server
  int liveRttMs = -1;      // the round trip of that direct path, -1 when not known
  std::string liveCounters; // what its stream did here since it started, "" before it has one (the `live counters` line)
  std::vector<RemoteChannelView> channels;
};

enum class ConnState { Disconnected, Connecting, Connected, Failed };

struct Snapshot
{
  ConnState state = ConnState::Disconnected;
  std::string status; // one line for the UI
  std::string server; // connected or connecting to
  std::string userName; // the name the server gave us
  float bpm = 0;  // as the server set them
  int bpi = 0;
  float intervalBpm = 0; // the tempo of the interval playing now (the server's bpm from the next interval)
  double roomSince = 0;  // when the plug-in started joining the room it is in (nowSeconds()), 0 while in none
  int intervalPos = 0, intervalLength = 0, intervalBeats = 0; // the interval playing now
  bool audioRunning = false; // the host called process() in the last second
  std::string roomCodec;     // "Opus", "FLAC" or "Vorbis" while connected
  std::string uploadCodec;   // what we send ("OPUr" or "FLAC"), empty when the room has neither
  bool canSend = true;       // false in a Vorbis room (or on a server without Opus): nothing is sent
  int opusKbps = 0;          // your own Opus bitrate (Settings::opusKbps)
  int roomOpusKbps = 0;      // the room's, while connected to a server that sets one; 0: none
  int sendKbps = 0;          // what you send in Opus rooms: the room's, else your own (from the next interval)
  float localPeak[2] = { 0, 0 };
  bool sendPaused = false;   // the host is stopped, so nothing is sent (transmit stays on)

  // Live: real-time audio, on servers that have it. The whole room plays one way: Live, or
  // intervals while someone's connection isn't good enough (or someone can't play Live).
  enum LiveReason { LiveNoReason, LiveDropouts, LiveCantLive };
  bool liveServer = false;   // the server has Live (its UDP port is open)
  bool liveSwitchOn = false; // the room's Live switch (!live on)
  bool liveRoom = false;     // the room plays Live now
  LiveReason liveReason = LiveNoReason; // while Live, whose trouble suggests switching to intervals
  std::string liveReasonWho; // whose connection or client (display name), "" when no one's
  int liveReasonOthers = 0;  // how many more have the same trouble
  int liveHeardMs = -1;      // about how late you hear the others while Live, -1 when not known
  int liveBufferMs = 0;      // your jitter buffer (Settings::liveBufferMs; 0 = Settings::LIVE_BUFFER_AUTO)
  std::string liveRoute = "none"; // how Live's audio goes in the room: "direct" (all to all), "relay" or "none"
  int liveBufferInUseMs = 20; // where a stream starts: the fixed buffer or the automatic one's 10 ms, raised for the host's long blocks

  std::vector<RemoteUserView> users;
  unsigned int usersRevision = 0; // changes when users or channels come or go (not for levels)

  bool licensePending = false;
  std::string licenseText;

  // following the host (hostsync.h)
  bool followTempo = true;   // the toggle
  std::string hostLabel;     // "Ableton", or the host's name
  std::string hostChip;      // the same, short: "Ableton", "Bitwig", or "DAW" when it doesn't fit
  double nextIntervalPpq = -1; // where the next interval starts in the host (quarter notes), -1 when not in line
  int timeSigNum = 0, timeSigDen = 0; // the host's time signature, 0 when it doesn't say
  double hostTempo = 0;      // 0 when the host doesn't say (or doesn't run the audio)
  BarSync::State barState = BarSync::Off;
  double barErrorMs = 0;     // where the host's interval is, minus ours
  int barFixes = 0;          // intervals made shorter or longer to line up
  int tempoVotes = 0;        // votes for the host's tempo
  std::string syncNote;      // the host's tempo isn't the room's (see hostTempoNote)

  // Ableton Link (linksync.h), while Sync is on in a room
  LinkState linkState = LinkState::Off;
  int linkPeers = 0;         // the other Link apps (Live, with its Link on)
  double linkTempo = 0;      // the Link session's, 0 when not in Link
  bool linkLeads = false;    // this instance leads Link, which has peers: Live's bars follow the intervals
  bool linkAligned = false;  // the intervals start on Link's beat
  double linkErrorMs = 0;    // the interval's beat minus Link's phase
  double linkLatencyMs = 0;  // when the host's blocks are heard, after the plug-in's guess (Calibrator)
  int linkJumps = 0, linkSteps = 0, linkRestores = 0;

  // the built-in server
  bool hosting = false;      // this instance hosts it
  bool hostBusy = false;     // starting or stopping
  int hostPort = 0;
  std::string hostPublicAddress; // host:port on the internet, once the router forwards the port
  std::string hostLocalAddress;  // this machine's address in the local network, with the port
  std::string hostNote;          // why there is no public address (the last thing the port mapper said)
  bool hostPortmapPending = false; // still asking the router
  std::string hostLookedUpAddress; // from a "what's my IP" service, for when the router doesn't say
  bool hostLookupPending = false;  // still asking that service
  bool hostJoinedFromInternet = false; // so the port is open, even if the router didn't say
  int hostUsers = 0;
  int hostMaxUsers = 0;        // how many it lets in
  int hostedElsewherePort = 0; // another instance in this program hosts one

  unsigned long long chatRevision = 0;
  unsigned int settingsRevision = 0;
};

// what the plug-in saves with the host's project
struct Settings
{
  // the Opus bitrates (kbps) to pick from; 510 is libopus's stereo maximum
  static constexpr int OPUS_KBPS[] = { 64, 96, 128, 160, 256, 510 };
  static constexpr int NUM_OPUS_KBPS = 6, DEFAULT_OPUS_KBPS = 510; // the best Opus there is, by default
  static int nearestOpusKbps(int kbps);
  // Live's jitter buffer: automatic by default (each player's stream starts at 10 ms and falls back
  // to 20 ms on dropouts); tests can fix it at one of LIVE_BUFFER_MS instead
  static constexpr int LIVE_BUFFER_MS[] = { 10, 15, 20, 30, 40, 60, 80, 100 };
  static constexpr int NUM_LIVE_BUFFERS = 8, LIVE_BUFFER_AUTO = 0, DEFAULT_LIVE_BUFFER_MS = LIVE_BUFFER_AUTO;
  static int nearestLiveBufferMs(int ms); // LIVE_BUFFER_AUTO for 0 or less

  std::string userName;
  std::string server;
  float inputVolume = 1.0f;
  float metronomeVolume = 0.5f;
  bool metronomeMute = true; // off until you turn it on
  bool monitor = true;
  bool transmit = true;
  int opusKbps = DEFAULT_OPUS_KBPS; // what we send in Opus rooms; FLAC is lossless and ignores it
  bool followHostTempo = true; // Sync: follow the host's tempo (and line up with its bars)
  int liveBufferMs = DEFAULT_LIVE_BUFFER_MS; // Live's jitter buffer: LIVE_BUFFER_AUTO, or fixed at one of LIVE_BUFFER_MS; not saved: only the `live buffer` command and O11_LIVE_BUFFER_MS (tests) change it
  int editorWidth = 0, editorHeight = 0; // the editor's size, 0 for the default

  struct Remote
  {
    float volume = 1.0f, pan = 0.0f;
    bool mute = false;
  };
  std::map<std::string, Remote> remote; // by remoteKey()

  std::string serialize() const;
  bool deserialize(const std::string &data); // false if it isn't a saved state
};

class Engine
{
public:
  Engine();
  ~Engine();

  void start(); // starts the network thread
  void stop();  // disconnects, stops hosting, joins the network thread

  // audio thread
  struct HostTiming // from the host's play head
  {
    bool offline = false; // rendering faster than real time: the intervals are left alone
    bool transport = false; // the host passed its transport (position.playing means something)
    HostPosition position;
  };
  // Besides the main output (everyone, your monitoring and the click), the plug-in has this many
  // stereo outputs, "Player 1" to "Player 8", that each play one player alone. The engine gives
  // each player who joins the least used one, so a ninth player shares one.
  static constexpr int NUM_PLAYER_OUTS = 8;
  struct PlayerOuts
  {
    float **bus[NUM_PLAYER_OUTS] = {}; // nullptr for an output the host doesn't use
    int channels[NUM_PLAYER_OUTS] = {};
  };
  void setupProcessing(double sampleRate, int maxBlockSize); // not while processing
  // While the host says it is stopped, the plug-in sends silence, and nothing from the next
  // interval on; your monitoring goes on.
  void process(float **in, int numIn, float **out, int numOut, int frames, const HostTiming *timing = nullptr,
               const PlayerOuts *playerOuts = nullptr);

  // any thread
  void getSnapshot(Snapshot &out) const;
  unsigned long long getChat(std::vector<ChatLine> &out, size_t maxLines) const; // returns the revision
  Settings getSettings() const;
  void setSettings(const Settings &s); // a saved state was loaded

  void connect(const std::string &server, const std::string &userName);
  void disconnect();
  void sendChat(const std::string &text);
  void vote(const std::string &what, int value); // "!vote bpm 120" or "!vote bpi 16", sent as chat
  void answerLicense(bool accept);
  void setHosting(bool on);

  void setUserName(const std::string &name);
  void setServerText(const std::string &server);
  void setInputVolume(float v);
  void setTransmit(bool on);
  void setMonitor(bool on);
  void setMetronome(float volume, bool mute);
  // One of Settings::OPUS_KBPS: your own, and in a room whose server sets one for everyone,
  // the room's too (!bitrate N). It applies from the next interval.
  void setOpusKbps(int kbps);
  void setRoomCodec(const std::string &codec); // "opus" or "flac": the room's (!codec), while connected
  void setEditorSize(int width, int height);
  void setFollowHostTempo(bool on); // Sync, and with it Ableton Link
  void setHostName(const std::string &name); // the host's name, for the editor
  void setRoomLive(bool on);       // the room's Live switch (!live on|off), while connected
  void setLiveBuffer(int ms);      // your jitter buffer: Settings::LIVE_BUFFER_AUTO or one of Settings::LIVE_BUFFER_MS

  enum RemoteField { RemoteVolume, RemotePan, RemoteMute, RemoteSolo };
  void setRemote(const std::string &user, int channel, RemoteField field, float value);

  // Text commands, for tests and automation (see processor.h); returns the reply:
  //   connect host:port [name], disconnect, name X, host on|off, chat text (sent as typed),
  //   license accept|decline, transmit on|off, monitor on|off, metronome on|off,
  //   bitrate 64|96|128|160|256|510, codec opus|flac, follow on|off (Sync, with Link), vote bpm|bpi N,
  //   live on|off (the room's switch), live buffer N, live (its state), /live ... (the engine's
  //   Live lines, in the chat log), status, users, chatlog, syncnote, nextinterval, synctip (the
  //   Sync button's tooltip)
  std::string command(const std::string &line);

  static std::string remoteKey(const std::string &user, int channel); // user without @address
  static std::string displayName(const std::string &user); // "alice@1.2.3.4" is "alice", "alice@1.2.3.4.2" "alice.2"
  // the name a new instance starts with: the first name of the computer account's full name
  // ("Leo Stern" is "Leo"), else its login name; "" if neither is known
  static std::string defaultUserName();
  static std::string firstName(const std::string &fullName); // "Leo Stern" is "Leo"

private:
  // one setting, without a copy of the rest (the network thread reads some at each wake)
  template <typename T> T setting(T Settings::*field) const
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    return m_settings.*field;
  }

  void threadProc();
  void post(std::function<void()> fn);
  void runCommands();
  void addChat(ChatLine::Kind kind, const std::string &who, const std::string &text);

  // network thread
  void doConnect(const std::string &server, const std::string &user);
  void doDisconnect(const char *why);
  void doSetHosting(bool on);
  void applyLocalSettings();
  int sendKbps();       // the Opus bitrate to send: the room's, else your own
  void applySendKbps(); // sets the local channel's bitrate to it
  void applyBroadcast(); // sends while transmit is on and the host isn't stopped
  void updateConnection();
  void updateLicense();
  void updateUsers(Snapshot &snap);
  void pollHosting(Snapshot &snap);
  void checkCanSend(bool canSend);
  void followHostTempo();
  void updateLink();
  void releaseLink(); // stop leading Link (it waits for a process() that runs)
  void publish();
  int findUser(const std::string &name) const;
  void onChat(const std::array<std::string, 5> &fields);

  std::unique_ptr<o11proto::engine::Engine> m_client;

  std::thread m_thread;
  std::atomic<bool> m_quit { false };
  bool m_running = false;
  std::mutex m_cmdMutex;
  std::condition_variable m_cmdCv;
  std::deque<std::function<void()>> m_commands;

  mutable std::mutex m_snapMutex;
  Snapshot m_snap;

  mutable std::mutex m_chatMutex;
  std::deque<ChatLine> m_chat;
  unsigned long long m_chatRevision = 0;

  mutable std::mutex m_settingsMutex;
  Settings m_settings;
  unsigned int m_settingsRevision = 0;
  std::string m_hostName; // under m_settingsMutex

  std::atomic<int> m_licenseAnswer { 0 }; // 1 accept, -1 decline
  std::atomic<bool> m_licenseWaiting { false };

  // audio thread
  std::atomic<float> m_inputGain { 1.0f };
  std::atomic<float> m_clickVolume { 0.5f }; // 0 when muted
  Click m_click;
  std::atomic<int> m_sampleRate { 48000 };
  std::atomic<unsigned int> m_processCalls { 0 };
  int m_maxBlock = 0;
  std::vector<float> m_bufL, m_bufR, m_spare;
  std::vector<float> m_monL, m_monR;      // the input, for your monitoring
  std::vector<std::vector<float>> m_mix;  // the engine's outputs: the main pair, then a pair per player output (one the host doesn't use)
  std::atomic<bool> m_monitor { true };
  std::atomic<bool> m_hostStopped { false }; // the host said it is stopped
  float m_roomGain = 1.0f; // audio thread: the room in our outputs, faded to 0 while the host is stopped
  BarSync m_barSync;
  int64_t m_sampleTime = 0; // samples processed since setupProcessing()
  std::unique_ptr<LinkSync> m_link;
  std::atomic<bool> m_follow { true }, m_barReset { false };
  // what the audio thread saw, for the network thread
  std::atomic<double> m_hostTempo { 0 }, m_barErrorMs { 0 };
  std::atomic<double> m_nextIntervalPpq { -1 };
  std::atomic<int> m_timeSig { 0 }; // numerator * 256 + denominator
  std::atomic<int> m_barState { BarSync::Off }, m_barFixes { 0 };

  // network thread only
  ConnState m_state = ConnState::Disconnected;
  std::string m_server, m_statusText;
  double m_connectTime = 0, m_connectedTime = 0;
  double m_licenseDeadline = 0;
  std::set<std::string> m_appliedRemote; // remote channels given their saved settings this session
  unsigned int m_usersRevision = 0;
  bool m_hosting = false, m_hostBusy = false;
  int m_hostPort = 0;
  unsigned long long m_hostLogSerial = 0;
  bool m_warnedCantSend = false; // told the user this room can't hear them
  double m_lastPublish = 0;
  bool m_liveLog = false; // O11_LIVE_LOG: print the `live` line when it changes (benchmarks)
  double m_liveLogAt = 0;
  std::string m_liveLogLine;
  double m_liveCountersAt = 0;
  std::string m_liveCountersLine;
  unsigned int m_seenProcessCalls = 0;
  std::map<std::string, int> m_playerOutSaid; // the player output we last told about, by user name
  std::map<std::string, double> m_joinedAt;   // when each user joined (RemoteUserView::joinedAt), by user name
  double m_lastAudio = -10;
  TempoFollower m_tempoFollower;
  double m_noteSince = -1; // since when the host's tempo isn't the room's
  // Link
  bool m_armPending = false; // joined a room, or Sync went on: vote for the host's tempo, unless Live follows Link
  LinkTempoKeeper m_linkKeeper;
  LinkState m_linkState = LinkState::Off;
  size_t m_linkPeers = 0;
  double m_linkTempo = 0, m_lastLinkPoll = 0;
  bool m_toldLinkHint = false, m_toldContested = false, m_toldLinkLeads = false;
};

} // namespace o11

#endif
