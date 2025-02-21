/* Copyright 2017 Matt Tytel */

#define NOMINMAX

#include "helm_engine.h"
#include "helm_sequencer.h"
#include "AudioPluginUtil.h"
#include "concurrentqueue.h"

namespace Helm {
  const int MAX_CHARACTERS = 15;
  const int MAX_CHANNELS = 16;
  const int MAX_NOTES = 128;
  const int MAX_MODULATIONS = 16;
  const int VALUES_PER_MODULATION = 3;
  const int MAX_UNITY_CHANNELS = 2;
  const int MAX_UNITY_BUFFER_SIZE = 2048;
  const float MODULATION_RANGE = 1000000.0f;
  const double SIXTEENTHS_PER_BEAT = 4.0;
  const double SECONDS_PER_MINUTE = 60.0;

  const std::map<std::string, std::string> REPLACE_STRINGS = {
    {"stutter_resample", "stutter_resamp"}
  };

  enum Param {
    kChannel,
    kNumParams
  };

  struct EffectData {
    int num_parameters;
    int num_synth_parameters;
    HelmSequencer::Note* sequencer_events[MAX_NOTES];
    mopo::ModulationConnection* modulations[MAX_MODULATIONS];
    moodycamel::ConcurrentQueue<std::pair<float, float>> note_events;
    moodycamel::ConcurrentQueue<std::pair<int, float>> value_events;
    float* parameters;
    mopo::Value** value_lookup;
    std::pair<float, float>* range_lookup;
    int instance_id;
    mopo::HelmEngine synth_engine;
    AudioHelm::Mutex mutex;
    double current_beat;
    double last_global_beat_sync;
    bool active;
    bool silent;
    float send_data[MAX_UNITY_CHANNELS * MAX_UNITY_BUFFER_SIZE];
    int num_send_channels;
  };

  AudioHelm::Mutex instance_mutex;
  int instance_counter = 0;
  double bpm = 120.0;
  double global_beat = 0.0;
  bool global_pause = false;
  std::map<int, EffectData*> instance_map;

  AudioHelm::Mutex sequencer_mutex;
  std::map<HelmSequencer*, bool> sequencer_lookup;

  std::string getValueName(std::string full_name) {
    std::string name = full_name;
    for (auto replace : REPLACE_STRINGS) {
      size_t index = name.find(replace.first);
      if (index != std::string::npos)
        name = name.substr(0, index) + replace.second + name.substr(index + replace.first.length());
    }
    name.erase(std::remove(name.begin(), name.end(), '_'), name.end());
    return name.substr(0, MAX_CHARACTERS);
  }

  int InternalRegisterEffectDefinition(UnityAudioEffectDefinition& definition) {
    std::map<std::string, mopo::ValueDetails> parameters = mopo::Parameters::lookup_.getAllDetails();

    int num_synth_params = parameters.size();
    int num_modulation_params = MAX_MODULATIONS * VALUES_PER_MODULATION;
    int num_plugin_params = kNumParams;
    int total_params = num_synth_params + num_plugin_params + num_modulation_params;

    definition.paramdefs = new UnityAudioParameterDefinition[total_params];
    RegisterParameter(definition, "Channel", "", 0.0f, MAX_CHANNELS, 0.0f, 1.0f, 1.0f, kChannel);

    int index = kNumParams;
    for (auto parameter : parameters) {
      mopo::ValueDetails& details = parameter.second;
      std::string name = getValueName(details.name);
      std::string units = details.display_units.substr(0, MAX_CHARACTERS);
      RegisterParameter(definition, name.c_str(), units.c_str(),
                        details.min, details.max, details.default_value,
                        1.0f, 1.0f, index);
      index++;
    }

    for (int m = 0; m < MAX_MODULATIONS; ++m) {
      std::ostringstream m_str;
      m_str << m;

      std::string name = std::string("mod") + m_str.str();
      std::string source_name = name + "source";
      std::string dest_name = name + "dest";
      std::string value_name = name + "value";
      RegisterParameter(definition, source_name.c_str(), "", 0.0f, MODULATION_RANGE, 0.0f,
                        1.0f, 1.0f, index++);
      RegisterParameter(definition, dest_name.c_str(), "", 0.0f, MODULATION_RANGE, 0.0f,
                        1.0f, 1.0f, index++);
      RegisterParameter(definition, value_name.c_str(), "", -MODULATION_RANGE, MODULATION_RANGE, 0.0f,
                        1.0f, 1.0f, index++);
    }

    return num_synth_params + kNumParams + num_modulation_params;
  }

  void initializeValueLookup(mopo::Value** lookup, std::pair<float, float>* range_lookup,
                             mopo::control_map& controls, int num_params) {
    std::map<std::string, mopo::ValueDetails> parameters = mopo::Parameters::lookup_.getAllDetails();

    for (int i = 0; i < num_params; ++i) {
      lookup[i] = 0;
    }

    int index = kNumParams;
    for (auto parameter : parameters) {
      mopo::ValueDetails& details = parameter.second;
      lookup[index] = controls[details.name];
      range_lookup[index].first = details.min;
      range_lookup[index].second = details.max;
      index++;
    }
  }

  UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK CreateCallback(UnityAudioEffectState* state) {
    EffectData* effect_data = new EffectData;
    memset(effect_data->sequencer_events, 0, sizeof(HelmSequencer::Note*) * MAX_NOTES);

    effect_data->num_synth_parameters = mopo::Parameters::lookup_.getAllDetails().size();
    int num_params = effect_data->num_synth_parameters + kNumParams + MAX_MODULATIONS * VALUES_PER_MODULATION;
    effect_data->num_parameters = num_params;

    effect_data->parameters = new float[num_params];
    InitParametersFromDefinitions(InternalRegisterEffectDefinition, effect_data->parameters);

    effect_data->value_lookup = new mopo::Value*[num_params];
    effect_data->range_lookup = new std::pair<float, float>[num_params];
    mopo::control_map controls = effect_data->synth_engine.getControls();
    initializeValueLookup(effect_data->value_lookup, effect_data->range_lookup, controls, num_params);

    for (int i = 0; i < MAX_MODULATIONS; ++i)
      effect_data->modulations[i] = new mopo::ModulationConnection();

    effect_data->synth_engine.setSampleRate(state->samplerate);
    effect_data->active = false;
    effect_data->silent = false;
    effect_data->current_beat = 0.0;
    effect_data->last_global_beat_sync = 0.0;
    effect_data->num_send_channels = 0;
    memset(effect_data->send_data, 0, MAX_UNITY_CHANNELS * MAX_UNITY_BUFFER_SIZE * sizeof(float));

    state->effectdata = effect_data;
    AudioHelm::MutexScopeLock mutex_instance_lock(instance_mutex);
    effect_data->instance_id = instance_counter;
    instance_map[instance_counter] = effect_data;
    instance_counter++;
    return UNITY_AUDIODSP_OK;
  }

  void clearInstance(int id) {
    instance_map.erase(id);
  }

  UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ReleaseCallback(UnityAudioEffectState* state) {
    EffectData* data = state->GetEffectData<EffectData>();
    data->mutex.Lock();

    AudioHelm::MutexScopeLock mutex_instance_lock(instance_mutex);
    data->synth_engine.allNotesOff();
    clearInstance(data->instance_id);

    data->mutex.Unlock();

    delete[] data->parameters;
    delete[] data->value_lookup;
    delete[] data->range_lookup;

    for (int i = 0; i < MAX_MODULATIONS; ++i) {
      if (data->synth_engine.isModulationActive(data->modulations[i]))
        data->synth_engine.disconnectModulation(data->modulations[i]);
      delete data->modulations[i];
    }

    delete data;

    return UNITY_AUDIODSP_OK;
  }

  UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK SetFloatParameterCallback(
      UnityAudioEffectState* state, int index, float value) {
    EffectData* data = state->GetEffectData<EffectData>();

    if (index < 0 || index >= data->num_parameters)
      return UNITY_AUDIODSP_ERR_UNSUPPORTED;

    data->parameters[index] = value;

    if (data->value_lookup[index])
      data->value_events.enqueue(std::pair<int, float>(index, value));

    int modulation_start = kNumParams + data->num_synth_parameters;
    if (index >= modulation_start) {
      AudioHelm::MutexScopeLock mutex_lock(data->mutex);

      int mod_param = index - modulation_start;
      int mod_index = mod_param / VALUES_PER_MODULATION;
      int mod_type = mod_param % VALUES_PER_MODULATION;

      mopo::ModulationConnection* connection = data->modulations[mod_index];

      if (mod_type == 0) {
        if (data->synth_engine.isModulationActive(connection))
          data->synth_engine.disconnectModulation(connection);

        int source_index = value;
        mopo::output_map sources = data->synth_engine.getModulationSources();
        auto source = sources.begin();
        std::advance(source, source_index);
        connection->source = source->first;
      }
      else if (mod_type == 1) {
        if (data->synth_engine.isModulationActive(connection))
          data->synth_engine.disconnectModulation(connection);

        mopo::output_map monoMods = data->synth_engine.getMonoModulations();
        int dest_index = value;
        if (dest_index < monoMods.size()) {
          auto mod = monoMods.begin();
          std::advance(mod, dest_index);
          connection->destination = mod->first;
        }
        else {
          dest_index -= monoMods.size();
          mopo::output_map polyMods = data->synth_engine.getPolyModulations();
          auto mod = polyMods.begin();
          std::advance(mod, dest_index);
          connection->destination = mod->first;
        }
      }
      else {
        if (value == 0.0f) {
          if (data->synth_engine.isModulationActive(connection))
            data->synth_engine.disconnectModulation(connection);
        }
        else {
          connection->amount.set(value);
          if (!data->synth_engine.isModulationActive(connection))
            data->synth_engine.connectModulation(connection);
        }
      }
    }
    return UNITY_AUDIODSP_OK;
  }

  UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK GetFloatParameterCallback(
      UnityAudioEffectState* state, int index, float* value, char *valuestr) {
    EffectData* data = state->GetEffectData<EffectData>();
    if (index < 0 || index >= data->num_parameters)
      return UNITY_AUDIODSP_ERR_UNSUPPORTED;

    if (value != NULL)
      *value = data->parameters[index];

    if (valuestr != NULL)
      valuestr[0] = 0;

    return UNITY_AUDIODSP_OK;
  }

  int UNITY_AUDIODSP_CALLBACK GetFloatBufferCallback(UnityAudioEffectState* state, const char* name,
                                                     float* buffer, int numsamples) {
    return UNITY_AUDIODSP_OK;
  }

  inline double beatToSixteenth(double beat) {
    return SIXTEENTHS_PER_BEAT * beat;
  }

  inline double timeToBeat(double time, int sample_rate) {
    return (bpm / SECONDS_PER_MINUTE) * time;
  }

  inline double timeToSixteenth(double time, int sample_rate) {
    return beatToSixteenth(timeToBeat(time, sample_rate));
  }

  double wrap(double value, double length, int& num_wraps) {
    num_wraps = value / length;
    return value - num_wraps * length;
  }

  void processNotes(EffectData* data, HelmSequencer* sequencer, double current_beat, double end_beat) {
    AudioHelm::MutexScopeLock mutex_lock(sequencer_mutex);
    double sequencer_start_beat = sequencer->start_beat();

    if (sequencer_start_beat >= end_beat)
      return;

    double start_beat = mopo::utils::max(sequencer_start_beat, current_beat);
    double start = beatToSixteenth(start_beat);
    double end = std::max(start, beatToSixteenth(end_beat));
    if (sequencer->loop()) {
      int start_num_wraps = 0;
      int end_num_wraps = 0;
      start = wrap(start, sequencer->length(), start_num_wraps);
      end = wrap(end, sequencer->length(), end_num_wraps);

      if (start_num_wraps == end_num_wraps)
        end = std::max(start, end);
    }

    sequencer->getNoteOffs(data->sequencer_events, start, end);

    for (int i = 0; i < MAX_NOTES && data->sequencer_events[i]; ++i)
      data->synth_engine.noteOff(data->sequencer_events[i]->midi_note);

    sequencer->getNoteOns(data->sequencer_events, start, end);

    for (int i = 0; i < MAX_NOTES && data->sequencer_events[i]; ++i)
      data->synth_engine.noteOn(data->sequencer_events[i]->midi_note, data->sequencer_events[i]->velocity);

    sequencer->updatePosition(end);
  }

  void processSequencerNotes(EffectData* data, double current_beat, double end_beat) {
    for (auto sequencer : sequencer_lookup) {
      if (sequencer.second && sequencer.first->channel() == data->parameters[kChannel])
        processNotes(data, sequencer.first, current_beat, end_beat);
    }
  }

  void processAudio(mopo::HelmEngine& engine,
                    float* in_buffer, float* out_buffer,
                    int in_channels, int out_channels, int samples, int offset) {
    if (engine.getBufferSize() != samples)
      engine.setBufferSize(samples);

    engine.setBpm(bpm);
    engine.process();

    const mopo::mopo_float* engine_output_left = engine.output(0)->buffer;
    const mopo::mopo_float* engine_output_right = engine.output(1)->buffer;
    for (int channel = 0; channel < out_channels; ++channel) {
      const mopo::mopo_float* synth_output = (channel % 2) ? engine_output_right : engine_output_left;
      int in_channel = channel % in_channels;

      for (int i = 0; i < samples; ++i) {
        int sample = i + offset;
        float mult = in_buffer[sample * in_channels + in_channel];
        out_buffer[sample * out_channels + channel] = mult * synth_output[i];
      }
    }
  }

  void processQueuedNotes(EffectData* data) {
    std::pair<float, float> event;
    while (data->note_events.try_dequeue(event)) {
      if (event.second)
        data->synth_engine.noteOn(event.first, event.second);
      else
        data->synth_engine.noteOff(event.first);
    }
  }

  void processQueuedFloatChanges(EffectData* data) {
    std::pair<int, float> event;
    while (data->value_events.try_dequeue(event))
      data->value_lookup[event.first]->set(event.second);
  }

  UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ProcessCallback(
      UnityAudioEffectState* state,
      float* in_buffer, float* out_buffer, unsigned int num_samples,
      int in_channels, int out_channels) {
    EffectData* data = state->GetEffectData<EffectData>();

    double last_beat = data->current_beat;
    double delta_time = (1.0 * num_samples) / state->samplerate;
    double delta_beat = timeToBeat(delta_time, state->samplerate);
    double next_beat = last_beat + delta_beat;
    if (!global_pause) {
      if (data->last_global_beat_sync != global_beat) {
        next_beat = global_beat + delta_beat;
        delta_beat = next_beat - last_beat;
        data->last_global_beat_sync = global_beat;
      }

      data->current_beat = next_beat;
    }

    bool silent = mopo::utils::isSilentf(in_buffer, num_samples * out_channels);
    if (state->flags & UnityAudioEffectStateFlags_IsPaused || silent) {
      data->active = false;
      memset(out_buffer, 0, num_samples * out_channels * sizeof(float));
      return UNITY_AUDIODSP_OK;
    }

    data->active = true;

    int synth_samples = num_samples > mopo::MAX_BUFFER_SIZE ? mopo::MAX_BUFFER_SIZE : num_samples;
    AudioHelm::MutexScopeLock mutex_lock(data->mutex);
    processQueuedFloatChanges(data);

    for (int b = 0; b < num_samples; b += synth_samples) {
      int current_samples = std::min<int>(synth_samples, num_samples - b);

      double start_beat = last_beat + (delta_beat * b) / num_samples;
      double end_beat = last_beat + (delta_beat * (b + current_samples)) / num_samples;
      if (b + synth_samples >= num_samples)
        end_beat = next_beat;

      if (end_beat > start_beat && !global_pause)
        processSequencerNotes(data, start_beat, end_beat);
      processQueuedNotes(data);
      processAudio(data->synth_engine, in_buffer, out_buffer, in_channels, out_channels, current_samples, b);
    }

    data->num_send_channels = out_channels;
    memcpy(data->send_data, out_buffer, num_samples * out_channels * sizeof(float));

    if (data->silent)
      memset(out_buffer, 0, num_samples * out_channels * sizeof(float));

    return UNITY_AUDIODSP_OK;
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void HelmNoteOn(int channel, int note, float velocity) {
    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (((int)data->parameters[kChannel]) == channel && data->active) {
        synth.second->note_events.enqueue(std::pair<float, float>(note, velocity));
      }
    }
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void HelmFrequencyOn(int channel, float frequency,
                                                            float velocity) {
    float note = mopo::utils::frequencyToMidiNote(frequency);
    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (((int)data->parameters[kChannel]) == channel && data->active) {
        synth.second->note_events.enqueue(std::pair<float, float>(note, velocity));
      }
    }
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void HelmNoteOnScheduled(int channel, int note, float velocity,
                                                                double start_time, double end_time) {
    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (((int)data->parameters[kChannel]) == channel && data->active) {
        synth.second->note_events.enqueue(std::pair<float, float>(note, velocity));
      }
    }
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void HelmNoteOff(int channel, int note) {
    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (((int)data->parameters[kChannel]) == channel) {
        synth.second->note_events.enqueue(std::pair<float, float>(note, 0.0f));
      }
    }
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void HelmFrequencyOff(int channel, float frequency) {
    float note = mopo::utils::frequencyToMidiNote(frequency);
    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (((int)data->parameters[kChannel]) == channel && data->active) {
        synth.second->note_events.enqueue(std::pair<float, float>(note, 0.0f));
      }
    }
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void HelmAllNotesOff(int channel) {
    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (((int)data->parameters[kChannel]) == channel) {
        AudioHelm::MutexScopeLock mutex_lock(synth.second->mutex);
        std::pair<float, float> event;

        while (synth.second->note_events.try_dequeue(event))
          ;
        synth.second->synth_engine.allNotesOff();
      }
    }
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void HelmSetPitchWheel(int channel, float value) {
    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (((int)data->parameters[kChannel]) == channel && data->active) {
        synth.second->synth_engine.setPitchWheel(value);
      }
    }
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void HelmSetModWheel(int channel, float value) {
    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (((int)data->parameters[kChannel]) == channel && data->active) {
        synth.second->synth_engine.setModWheel(value);
      }
    }
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void HelmSetAftertouch(int channel, int note, float value) {
    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (((int)data->parameters[kChannel]) == channel && data->active) {
        synth.second->synth_engine.setAftertouch(note, value);
      }
    }
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API bool HelmSetParameterValue(int channel, int index, float value) {
    if (index < kNumParams)
      return false;

    bool success = true;
    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (((int)data->parameters[kChannel]) == channel && data->active) {
        if (index >= data->num_parameters)
          success = false;
        else {
          float clamped_value = mopo::utils::clamp(value, data->range_lookup[index].first,
                                                          data->range_lookup[index].second);
          data->parameters[index] = clamped_value;
          if (data->value_lookup[index])
            data->value_events.enqueue(std::pair<int, float>(index, clamped_value));
        }
      }
    }
    return success;
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void HelmClearModulations(int channel) {
    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (((int)data->parameters[kChannel]) == channel && data->active) {
        AudioHelm::MutexScopeLock mutex_lock(data->mutex);

        for (int i = 0; i < MAX_MODULATIONS; ++i) {
          mopo::ModulationConnection* connection = data->modulations[i];
          if (data->synth_engine.isModulationActive(connection))
            data->synth_engine.disconnectModulation(connection);
        }
      }
    }
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void HelmAddModulation(int channel, int index,
                                                              const char* source,
                                                              const char* dest,
                                                              float amount) {
    if (index < 0 || index >= MAX_MODULATIONS)
      return;

    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (((int)data->parameters[kChannel]) == channel && data->active) {
        AudioHelm::MutexScopeLock mutex_lock(data->mutex);

        mopo::ModulationConnection* connection = data->modulations[index];
        connection->source = source;
        connection->destination = dest;
        connection->amount.set(amount);
        data->synth_engine.connectModulation(connection);
      }
    }
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void HelmSilence(int channel, bool silent) {
    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (((int)data->parameters[kChannel]) == channel)
        data->silent = silent;
    }
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void HelmGetBufferData(int channel, float* buffer, int samples, int channels) {
    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      int send_channels = data->num_send_channels;
      const float* send_buffer = data->send_data;

      if (((int)data->parameters[kChannel]) == channel && data->active && send_channels > 0) {

        if (channels == send_channels)
          memcpy(buffer, data->send_data, samples * channels * sizeof(float));
        else {
          for (int i = 0; i < samples; ++i) {
            for (int c = 0; c < channels; ++c) {
              int send_channel = c % send_channels;
              buffer[i * channels + c] = send_buffer[i * send_channels + send_channel];
            }
          }
        }
        return;
      }
    }
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API float HelmGetParameterMinimum(int index) {
    return mopo::Parameters::lookup_.getDetails(index - 1).min;
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API float HelmGetParameterMaximum(int index) {
    return mopo::Parameters::lookup_.getDetails(index - 1).max;
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API float HelmGetParameterValue(int channel, int index) {
    if (index < kNumParams)
      return 0.0f;

    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (((int)data->parameters[kChannel]) == channel && data->active) {
        if (index < data->num_parameters)
          return data->parameters[index];
      }
    }
    return 0.0f;
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API bool HelmSetParameterPercent(int channel, int index, float percent) {
    if (index < kNumParams)
      return false;

    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (index >= data->num_parameters)
        return false;
      else {
        float range = data->range_lookup[index].second - data->range_lookup[index].first;
        float value = range * mopo::utils::clamp(percent, 0.0f, 1.0f) + data->range_lookup[index].first;
        return HelmSetParameterValue(channel, index, value);
      }
    }
    return false;
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API float HelmGetParameterPercent(int channel, int index) {
    if (index < kNumParams)
      return 0.0f;

    for (auto synth : instance_map) {
      EffectData* data = synth.second;
      if (index >= data->num_parameters)
        return 0.0f;
      else {
        float range = data->range_lookup[index].second - data->range_lookup[index].first;
        float value = HelmGetParameterValue(channel, index);
        float percent = (value - data->range_lookup[index].first) / range;
        return mopo::utils::clamp(percent, 0.0f, 1.0f);
      }
    }
    return 0.0f;
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API HelmSequencer* CreateSequencer() {
    HelmSequencer* sequencer = new HelmSequencer();
    AudioHelm::MutexScopeLock mutex_lock(sequencer_mutex);
    sequencer_lookup[sequencer] = false;
    return sequencer;
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void SetBeatTime(double beat) {
    global_beat = beat;
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void Pause(bool pause) {
    global_pause = pause;
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void DeleteSequencer(HelmSequencer* sequencer) {
    sequencer_mutex.Lock();
    sequencer_lookup.erase(sequencer);
    sequencer_mutex.Unlock();
    delete sequencer;
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void EnableSequencer(HelmSequencer* sequencer, bool enable) {
    AudioHelm::MutexScopeLock mutex_lock(sequencer_mutex);
    sequencer_lookup[sequencer] = enable;
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API HelmSequencer::Note* CreateNote(
      HelmSequencer* sequencer, int note, float velocity, float start, float end) {
    AudioHelm::MutexScopeLock mutex_lock(sequencer_mutex);
    return sequencer->addNote(note, velocity, start, end);
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void DeleteNote(
      HelmSequencer* sequencer, HelmSequencer::Note* note) {
    AudioHelm::MutexScopeLock mutex_lock(sequencer_mutex);
    if (sequencer->isNotePlaying(note))
      HelmNoteOff(sequencer->channel(), note->midi_note);

    sequencer->deleteNote(note);
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void ChangeNoteStart(
      HelmSequencer* sequencer, HelmSequencer::Note* note, float new_start) {
    AudioHelm::MutexScopeLock mutex_lock(sequencer_mutex);
    bool wasPlaying = sequencer->isNotePlaying(note);
    sequencer->changeNoteStart(note, new_start);

    if (wasPlaying && !sequencer->isNotePlaying(note))
      HelmNoteOff(sequencer->channel(), note->midi_note);
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void ChangeNoteEnd(
      HelmSequencer* sequencer, HelmSequencer::Note* note, float new_end) {
    AudioHelm::MutexScopeLock mutex_lock(sequencer_mutex);
    bool wasPlaying = sequencer->isNotePlaying(note);
    sequencer->changeNoteEnd(note, new_end);

    if (wasPlaying && !sequencer->isNotePlaying(note))
      HelmNoteOff(sequencer->channel(), note->midi_note);
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void ChangeNoteValues(
      HelmSequencer* sequencer, HelmSequencer::Note* note,
      int new_midi_key, float new_start, float new_end, float new_velocity) {
    AudioHelm::MutexScopeLock mutex_lock(sequencer_mutex);
    bool wasPlaying = sequencer->isNotePlaying(note);
    sequencer->changeNoteKey(note, new_midi_key);
    sequencer->changeNoteStart(note, new_start);
    sequencer->changeNoteEnd(note, new_end);
    note->velocity = new_velocity;

    if (wasPlaying && !sequencer->isNotePlaying(note))
      HelmNoteOff(sequencer->channel(), note->midi_note);
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void ChangeNoteVelocity(HelmSequencer::Note* note, float new_velocity) {
    note->velocity = new_velocity;
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void ChangeNoteKey(
      HelmSequencer* sequencer, HelmSequencer::Note* note, int midi_key) {
    AudioHelm::MutexScopeLock mutex_lock(sequencer_mutex);
    if (sequencer->isNotePlaying(note))
      HelmNoteOff(sequencer->channel(), note->midi_note);

    sequencer->changeNoteKey(note, midi_key);
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API bool ChangeSequencerChannel(
      HelmSequencer* sequencer, int channel) {
    AudioHelm::MutexScopeLock mutex_lock(sequencer_mutex);
    sequencer->setChannel(channel);

    for (auto sequencer : sequencer_lookup) {
      if (sequencer.first->channel() == channel)
        return false;
    }
    return true;
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void SetSequencerStart(HelmSequencer* sequencer, double start_beat) {
    AudioHelm::MutexScopeLock mutex_lock(sequencer_mutex);
    sequencer->setStartBeat(start_beat);
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void ChangeSequencerLength(HelmSequencer* sequencer, float length) {
    AudioHelm::MutexScopeLock mutex_lock(sequencer_mutex);
    sequencer->setLength(length);
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void LoopSequencer(HelmSequencer* sequencer, bool loop) {
    AudioHelm::MutexScopeLock mutex_lock(sequencer_mutex);
    sequencer->loop(loop);
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API void SetBpm(float new_bpm) {
    bpm = new_bpm;
  }

  extern "C" UNITY_AUDIODSP_EXPORT_API float GetBpm() {
    return bpm;
  }
}
