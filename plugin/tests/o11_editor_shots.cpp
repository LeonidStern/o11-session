/*
    O11 Session - o11_editor_shots.cpp
    Copyright (C) 2026 and onward, O11 Session contributors

    Part of the O11 Session plug-in on JUCE. The licence is decided at release (see README.md).
*/

/*

  Pictures of the editor, for looking at its layout: plug-in instances in one
  process, each with an audio thread as a host would run it (Live at 120 BPM,
  playing, a tone into the input); one hosts a room on this computer, the
  others join it. Not a test: it needs a display (xvfb-run gives one).

    O11_HOST_PORT=0 O11_PORTMAP=off O11_WHATSMYIP=off xvfb-run -a o11_editor_shots <folder>

  writes, as the hosting instance (Leo) sees it: first-open.png (not in a
  room), alone.png (hosting, nobody else), room.png (four others: one just
  joined, one muted, one silent; a tempo and an interval to come), full.png
  (eight in the room, one too loud), small.png (the smallest window) and
  large.png (a larger one). It takes about a minute.

*/

#include <math.h>
#include <stdio.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "editor.h"
#include "processor.h"

using namespace o11;

static const double SAMPLE_RATE = 48000;
static const int BLOCK = 512;

// the host's play head: Live, playing at 120 BPM in 4/4
class PlayHead : public juce::AudioPlayHead
{
public:
  std::atomic<double> ppq { 0 };
  juce::Optional<PositionInfo> getPosition() const override
  {
    PositionInfo p;
    p.setBpm(120);
    p.setIsPlaying(true);
    p.setPpqPosition(ppq.load());
    p.setTimeSignature(TimeSignature { 4, 4 });
    return p;
  }
};

// an instance, and the audio thread its host would run it on
struct Player
{
  std::unique_ptr<O11Processor> plugin;
  PlayHead head;
  std::thread audio;
  std::atomic<bool> quit { false };
  float level;
  double freq;

  Player(const std::string &name, float lvl, double hz) : plugin(new O11Processor), level(lvl), freq(hz)
  {
    plugin->engine().setHostName("Ableton Live 12 Suite");
    plugin->runCommand("name " + name);
    plugin->setPlayHead(&head);
    plugin->setRateAndBufferSizeDetails(SAMPLE_RATE, BLOCK);
    plugin->prepareToPlay(SAMPLE_RATE, BLOCK);
    audio = std::thread([this]() {
      juce::AudioBuffer<float> buffer(plugin->getTotalNumOutputChannels(), BLOCK);
      juce::MidiBuffer midi;
      double phase = 0;
      auto next = std::chrono::steady_clock::now();
      while (!quit)
      {
        buffer.clear();
        for (int i = 0; i < BLOCK; i ++)
        {
          // a tone that swells and fades, so the meters move
          const float swell = 0.6f + 0.4f * (float)sin(phase * 0.0005);
          const float v = level * swell * (float)sin(phase);
          phase += 2 * M_PI * freq / SAMPLE_RATE;
          buffer.setSample(0, i, v);
          buffer.setSample(1, i, v);
        }
        plugin->processBlock(buffer, midi);
        head.ppq = head.ppq.load() + BLOCK / SAMPLE_RATE * 2.0; // 120 BPM: two quarter notes a second
        next += std::chrono::microseconds((long long)(BLOCK * 1e6 / SAMPLE_RATE));
        std::this_thread::sleep_until(next);
      }
    });
  }
  ~Player()
  {
    quit = true;
    if (audio.joinable()) audio.join();
  }
  Snapshot snapshot()
  {
    Snapshot s;
    plugin->engine().getSnapshot(s);
    return s;
  }
};

static O11Editor *g_editor = nullptr; // kept up to date while waiting

static bool waitFor(const std::function<bool()> &done, double seconds)
{
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((long long)(seconds * 1000));
  while (std::chrono::steady_clock::now() < end)
  {
    if (done()) return true;
    if (g_editor) g_editor->refresh();
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
  return done();
}

// time passing for the editor: brought up to date 30 times a second, as its timer would (which
// doesn't run without a message loop)
static void idle(O11Editor &editor, double seconds)
{
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((long long)(seconds * 1000));
  do
  {
    editor.refresh();
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  } while (std::chrono::steady_clock::now() < end);
}

// the editor brought up to date for a while, then painted twice as big
static void shot(O11Editor &editor, const juce::File &file, double settle = 1.0)
{
  idle(editor, settle);
  editor.refresh();
  const juce::Image image = editor.createComponentSnapshot(editor.getLocalBounds(), true, 2.0f);
  file.deleteFile();
  juce::FileOutputStream out(file);
  juce::PNGImageFormat png;
  const bool ok = out.openedOk() && png.writeImageToStream(image, out);
  printf("%s %s\n", ok ? "wrote" : "could not write", file.getFullPathName().toRawUTF8());
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    fprintf(stderr, "usage: o11_editor_shots <folder>\n");
    return 2;
  }
  const juce::File folder = juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]);
  folder.createDirectory();
  juce::ScopedJuceInitialiser_GUI gui;

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    if (!ok)
    {
      fprintf(stderr, "failed: %s\n", what);
      failures++;
    }
  };
  {
    Player leo("Leo", 0.35f, 220);
    std::unique_ptr<O11Editor> editor(dynamic_cast<O11Editor *>(leo.plugin->createEditor()));
    editor->setSize(O11Editor::DEFAULT_WIDTH, O11Editor::DEFAULT_HEIGHT);
    editor->setVisible(true);
    g_editor = editor.get();
    shot(*editor, folder.getChildFile("first-open.png"));

    leo.plugin->runCommand("host on");
    check(waitFor([&]() { return leo.snapshot().state == ConnState::Connected; }, 10), "Leo hosts and joins");
    const int port = leo.snapshot().hostPort;
    shot(*editor, folder.getChildFile("alone.png"), 3.0);

    // four others, Priya silent; Anna half a minute later, so her row still says she just joined
    std::vector<std::unique_ptr<Player>> others;
    const char *names[] = { "Marco", "Sam", "Priya", "Anna", "Yuki", "Diego", "Kai" };
    const float levels[] = { 0.3f, 0.5f, 0.0f, 0.2f, 0.4f, 0.95f, 0.25f };
    const double freqs[] = { 330, 110, 220, 440, 165, 550, 262 };
    auto join = [&](int i) {
      others.emplace_back(new Player(names[i], levels[i], freqs[i]));
      others.back()->plugin->runCommand("connect 127.0.0.1:" + std::to_string(port) + " " + names[i]);
      check(waitFor([&]() { return others.back()->snapshot().state == ConnState::Connected; }, 10), names[i]);
    };
    for (int i = 0; i < 3; i ++) join(i);
    idle(*editor, 31);
    join(3);
    check(waitFor([&]() { return leo.snapshot().users.size() == 4; }, 10), "Leo sees four players");
    // Leo mutes Marco
    for (const RemoteUserView &u : leo.snapshot().users)
      if (u.displayName == "Marco")
        for (const RemoteChannelView &c : u.channels) leo.plugin->engine().setRemote(u.name, c.index, Engine::RemoteMute, 1);
    idle(*editor, 4);
    // Anna asks for 100 BPM and 16 beats just after an interval starts: they come at the next one
    check(waitFor([&]() {
      const Snapshot s = leo.snapshot();
      return s.intervalLength > 0 && s.intervalPos < s.intervalLength / 8;
    }, 10), "an interval starts");
    others[3]->plugin->runCommand("vote bpm 100");
    others[3]->plugin->runCommand("vote bpi 16");
    check(waitFor([&]() {
      const Snapshot s = leo.snapshot();
      return s.bpm == 100 && s.bpi == 16;
    }, 10), "the room takes 100 BPM and 16 beats");
    shot(*editor, folder.getChildFile("room.png"), 0.3);

    // eight in the room
    for (int i = 4; i < 7; i ++) join(i);
    check(waitFor([&]() { return leo.snapshot().users.size() == 7; }, 10), "Leo sees seven players");
    idle(*editor, 20);
    printf("%s", leo.plugin->runCommand("users").c_str());
    shot(*editor, folder.getChildFile("full.png"), 1.0);

    editor->setSize(O11Editor::MIN_WIDTH, O11Editor::MIN_HEIGHT);
    shot(*editor, folder.getChildFile("small.png"), 0.3);
    editor->setSize(1080, 495);
    shot(*editor, folder.getChildFile("large.png"), 0.3);

    g_editor = nullptr;
    editor.reset();
    others.clear();
    leo.plugin->runCommand("host off");
  }
  return failures ? 1 : 0;
}
