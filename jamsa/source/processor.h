/*
    Jamsa - processor.h
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

/*

  The plug-in: a stereo effect whose input is sent to the NINJAM server as
  our channel, and whose output is what the other players play, plus the
  metronome and, if monitoring is on, our own input; and a stereo output for
  each player alone ("Player 1" to "Player 8").

  The plug-in has no automatable parameters: its editor talks to the Engine
  directly, and its state is the Engine's Settings as text.

*/

#ifndef _JAMSA_PROCESSOR_H_
#define _JAMSA_PROCESSOR_H_

#include <memory>
#include <string>

#include <juce_audio_processors/juce_audio_processors.h>

#include "engine.h"

namespace jamsa {

class JamsaProcessor : public juce::AudioProcessor
{
public:
  JamsaProcessor();
  ~JamsaProcessor() override;

  // AudioProcessor
  const juce::String getName() const override { return "Jamsa"; }
  void prepareToPlay(double sampleRate, int maxBlockSize) override;
  void releaseResources() override {}
  bool isBusesLayoutSupported(const BusesLayout &layouts) const override;
  void processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midi) override;
  double getTailLengthSeconds() const override { return std::numeric_limits<double>::infinity(); }
  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  bool isMidiEffect() const override { return false; }
  juce::AudioProcessorEditor *createEditor() override;
  bool hasEditor() const override { return true; }
  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return {}; }
  void changeProgramName(int, const juce::String &) override {}
  void getStateInformation(juce::MemoryBlock &dest) override;
  void setStateInformation(const void *data, int size) override;

  Engine &engine() { return *m_engine; }

  // the text commands of Engine::command(), for tests and automation
  std::string runCommand(const std::string &line) { return m_engine->command(line); }

  // the host's play head as a HostPosition (hostsync.h); public for the tests
  static HostPosition hostPosition(const juce::AudioPlayHead::PositionInfo &info);

private:
  static juce::AudioProcessor::BusesProperties buses();

  std::unique_ptr<Engine> m_engine;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(JamsaProcessor)
};

} // namespace jamsa

#endif
