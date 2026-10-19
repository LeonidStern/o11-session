/*
    O11 Session - processor.cpp
    Copyright (C) 2026 and onward, O11 Session contributors

    Part of the O11 Session plug-in on JUCE. The licence is decided at release (see README.md).
*/

#include "processor.h"

#include "editor.h"

namespace o11 {

static const size_t MAX_STATE_SIZE = 1 << 20;

juce::AudioProcessor::BusesProperties O11Processor::buses()
{
  juce::AudioProcessor::BusesProperties b;
  b.addBus(true, "Input", juce::AudioChannelSet::stereo(), true);
  b.addBus(false, "Output", juce::AudioChannelSet::stereo(), true);
  // a stereo output for each player alone (Engine::PlayerOuts), for recording each on a track
  for (int i = 0; i < Engine::NUM_PLAYER_OUTS; i ++)
    b.addBus(false, "Player " + juce::String(i + 1), juce::AudioChannelSet::stereo(), true);
  return b;
}

O11Processor::O11Processor()
  : juce::AudioProcessor(buses()), m_engine(new Engine)
{
  m_engine->setHostName(juce::PluginHostType().getHostDescription());
  m_engine->start();
}

O11Processor::~O11Processor()
{
  m_engine->stop();
}

void O11Processor::prepareToPlay(double sampleRate, int maxBlockSize)
{
  m_engine->setupProcessing(sampleRate, maxBlockSize);
}

bool O11Processor::isBusesLayoutSupported(const BusesLayout &layouts) const
{
  // a mono or stereo input, a stereo main output; the player outputs stereo or off
  const juce::AudioChannelSet in = layouts.getMainInputChannelSet();
  if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo() && !in.isDisabled()) return false;
  if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
  for (int i = 1; i < layouts.outputBuses.size(); i ++)
  {
    const juce::AudioChannelSet &set = layouts.outputBuses.getReference(i);
    if (set != juce::AudioChannelSet::stereo() && !set.isDisabled()) return false;
  }
  return true;
}

HostPosition O11Processor::hostPosition(const juce::AudioPlayHead::PositionInfo &info)
{
  HostPosition p;
  p.playing = info.getIsPlaying();
  if (const auto tempo = info.getBpm()) p.tempo = *tempo > 0 ? *tempo : 0;
  if (const auto ppq = info.getPpqPosition())
  {
    p.hasPosition = true;
    p.ppq = *ppq;
  }
  if (const auto loop = info.getLoopPoints())
  {
    p.cycleActive = info.getIsLooping();
    p.cycleStart = loop->ppqStart;
    p.cycleEnd = loop->ppqEnd;
  }
  if (const auto sig = info.getTimeSignature())
  {
    p.timeSigNum = sig->numerator;
    p.timeSigDen = sig->denominator;
  }
  return p;
}

void O11Processor::processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &)
{
  juce::ScopedNoDenormals noDenormals;
  const int frames = buffer.getNumSamples();
  if (frames <= 0) return;

  // the buses' channels in the buffer, as getBusBuffer() finds them: the input from channel 0,
  // the outputs one after the other from channel 0; a bus that is off has none
  float *const *channels = buffer.getArrayOfWritePointers();
  const int bufferChannels = buffer.getNumChannels();
  auto busChannels = [this](bool isInput, int bus) {
    const Bus *b = getBus(isInput, bus);
    return b ? b->getNumberOfChannels() : 0;
  };

  // the input, read before the main output is written (they may share buffers: the engine
  // copies its input first, piece by piece, and writes the output of each piece after)
  const int mainOut = std::min(busChannels(false, 0), bufferChannels);
  if (mainOut < 1) return;
  float *in[2] = { nullptr, nullptr };
  int numIn = 0;
  for (int c = 0; c < std::min({ 2, busChannels(true, 0), bufferChannels }); c ++) in[numIn++] = channels[c];
  float *out[2] = { channels[0], mainOut > 1 ? channels[1] : nullptr };

  Engine::HostTiming timing;
  timing.offline = isNonRealtime();
  if (juce::AudioPlayHead *head = getPlayHead())
    if (const auto info = head->getPosition())
    {
      timing.transport = true;
      timing.position = hostPosition(*info);
    }

  Engine::PlayerOuts players;
  float *playerChannels[Engine::NUM_PLAYER_OUTS][2];
  int first = busChannels(false, 0); // the bus's first channel in the buffer
  for (int b = 1; b < getBusCount(false) && b <= Engine::NUM_PLAYER_OUTS; b ++)
  {
    const int busCount = busChannels(false, b);
    const int used = std::min({ 2, busCount, bufferChannels - first });
    if (used >= 1)
    {
      for (int c = 0; c < used; c ++) playerChannels[b - 1][c] = channels[first + c];
      players.bus[b - 1] = playerChannels[b - 1];
      players.channels[b - 1] = used;
    }
    first += busCount;
  }

  m_engine->process(in, numIn, out, mainOut > 1 ? 2 : 1, frames, &timing, &players);
}

juce::AudioProcessorEditor *O11Processor::createEditor()
{
  return new O11Editor(*this);
}

// The state is text (see Settings::serialize), the same format as the plug-in before this one
void O11Processor::getStateInformation(juce::MemoryBlock &dest)
{
  const std::string data = m_engine->getSettings().serialize();
  dest.replaceAll(data.data(), data.size());
}

void O11Processor::setStateInformation(const void *data, int size)
{
  if (!data || size <= 0 || (size_t)size > MAX_STATE_SIZE) return;
  Settings s;
  if (s.deserialize(std::string((const char *)data, (size_t)size))) m_engine->setSettings(s);
}

} // namespace o11

// JUCE's entry point
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter()
{
  return new o11::O11Processor;
}
